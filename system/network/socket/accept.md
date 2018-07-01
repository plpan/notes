### 1. socket连接
说起socket编程，闹钟浮现的肯定是下面这张图：
```
+----------------------------------------+               +----------------------------------------+
|                client                  |               |                server                  |
|                                        |               |                                        |
|      +------------------------+        |               |      +------------------------+        |
|      | create a stream socket |        |               |      | create a stream socket |        |
|      |        socket()        |        |               |      |        socket()        |        |
|      +------------|-----------+        |               |      +------------|-----------+        |
|                   |                    |               |                   |                    |
|                   |                    |               |                   |                    |
|      +------------v-----------+        |               |      +------------v-----------+        |
|      | bind to local address  |        |               |      | bind to local address  |        |
|      |    bind() [optional]   |        |               |      |         bind()         |        |
|      +------------|-----------+        |               |      +------------|-----------+        |
|                   |                    |               |                   |                    |
|                   |                    |               |                   |                    |
|                   |                    |               |      +------------v-----------+        |
|                   |                    |               |      | waiting to accept conn |        |
|                   |                    |               |      |        listen()        |        |
|      +------------v-----------+        |               |      +------------|-----------+        |
|      |   connect to server    |        |             --------------------->|<--------------+    |
|      |       connect()        ----------------------/  |                   |               |    |
|      +------------|-----------+        |               |      +------------v-----------+   |    |
|                   |                    |               |      |  accept a connection   |   |    |
|                   |                    |               |      |        accept()        |   |    |
|                   |                    |               |      +------------|-----------+   |    |
|                   |                    |               |                   |               |    |
|                   |                    |               |                   |               |    |
|      +------------v-----------+        |               |      +------------v-----------+   |    |
|      | communicate with server|        |               |      | communicate with client|   |    |
|      |     write()/read()     <------------------------------->     read()/write()     |   |    |
|      +------------|-----------+        |               |      +------------|-----------+   |    |
|                   |                    |               |                   |               |    |
|                   |                    |               |                   |               |    |
|      +------------v-----------+        |               |      +------------v-----------+   |    |
|      |  close the connection  |        |               |      |    close the socket    |   |    |
|      |         close()        |        |               |      |        close()         ----+    |
|      +------------------------+        |               |      +------------------------+        |
|                                        |               |                                        |
+----------------------------------------+               +----------------------------------------+
```
这张图展现了socket的工作流程，以及socket函数的功能。

本文中，我们并非要介绍socket的整体工作机制，而是将关注点集中在accept函数，深入学习其内核实现，并了解最初的实现所带来的性能问题，及其改进方案。

### 2. accept内核实现简述[1]
当线程想要监听一个tcp连接请求，那么它只需创建一个socket，并调用accept系统调用即可。accept系统调用会最终会调用tcp协议提供的tcp_accept处理相关工作，涉及到的数据结构主要有：线程状态、socket等待队列。其中线程状态由task_struct中的state字段来描述，线程的状态集合包含：运行中、睡眠中、等待事件中等。

如果一个线程调用了accept，则将其线程状态从TASK_RUNNIGN设置为TASK_INTERRUPTIBLE，并将线程描述符放到对应socket的等待队列中。结果就是线程进入了睡眠状态。如果有多个线程都执行了如上操作，则依次将多个线程添加到socket等待队列中。注意，只有在同一个socket上调用accept函数，才会将所有线程加到一个队列。

而当有客户端发起一个请求时，网卡驱动会将数据包传递至内核，并交由tcp_v4_recv函数来处理。函数发现该请求想要和一个正在accept的socket建立连接，它就会调用wake_up_interruptible函数，唤醒等待队列中的线程，并由该线程去处理这个连接。

这个唤醒策略有多种实现，在老版本linux内核中，wake_up_interruptible函数会唤醒等待队列中的所有线程。然而，问题是仅会有一个线程负责和客户端建立连接并进行通信，其他线程紧接着又会进入TASK_INTERRUPTIBLE状态，并被重新放回至socket的等待队列中。很明显，这种唤醒所有线程的做法是非常低效的。这也是linux系统中一个非常著名的问题：惊群（herd）。

想要理解tcp协议栈如何唤醒线程，我们需要稍微了解socket数据结构。socket数据结构中有六个钩子函数，初始创建socket时，这六个钩子函数分别指向通用处理函数，不同协议簇分别提供了对应的实现函数。tcp协议簇覆盖了其中几个方法入下：
```
sock->state_change
	(pointer to sock_def_wakeup) -> tcp_wakeup

sock->data_ready
	(pointer to sock_def_readable) -> tcp_data_ready

sock->write_space
	(pointer to tcp_write_space) -> tcp_write_space

sock->error_report
	(pointer to sock_def_error_report) -> sock_def_error_report
```
这几个函数内部都会调用wake_up_interruptible函数，也即每次只要其中一个函数被调用，老linux内核都会发生一次惊群效应。那么这个问题该如何解决呢？

### 3. 改进方案
1） 方案一：task exclusive
本方案需要新增一个线程状态TASK_EXCLUSIVE，并调整等待队列的处理方式：
- 新增方法add_wait_queue_exclusive，该方法会将线程添加到等待队列的尾部，而普通方法add_wait_queue依然会将线程添加到等待队列头部
- 当调用__wake_up()方法（wake_up_interruptible内部调用）时，它会首先唤醒所有共享状态的线程，并只唤醒一个非共享状态的线程

注意，所有非共享状态的线程都会一次添加到socket等待队列的尾部，而共享状态的线程则会被依次添加到等待队列的头部，因此只需从头开始遍历等待队列，直到碰到一个非共享状态的线程即可。

在一些极端情况下，仍然需要特殊处理去唤醒所有线程，如连接异常中断。

2） 方案二：wake one
本方法则更加直接，直接为tcp协议簇添加一个新方法wake_one_interruptible。该方案无需新增线程状态，并且将唤醒一个线程还是唤醒所有线程这个选择提前了，并非是等到唤醒时刻去遍历等待队列，而是对唤醒策略进行了修改。

本方案最大的好处是，仅对tcp协议簇有影响。等验证wake_one_interruptible方法对所有协议有效之后，其实可以使用wake_one_interruptible方法替代wake_up_interruptible，并将其作为默认默认方法即可。

经实验验证[1]，以上两种方案都能解决惊群问题，并使得accept性能大幅提升。

### reference
[1]. accept() scalability on linux