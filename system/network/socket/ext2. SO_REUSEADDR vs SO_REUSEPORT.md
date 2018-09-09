### socket鼻祖
目前公认的socket鼻祖就是BSD实现的socket，其他的socket或多或少都参考了BSD的实现。

### 连接
一个tcp/udp连接由如下5个部分组成：
```
{<protocol>, <src addr>, <src port>, <dst addr>, <dst port>}
```
- protocol字段是在调用socket()时指定
- src addr和src port字段是在调用bind()时绑定
- dst addr和dst port字段是在调用connect()时指定

针对udp协议，应用程序可以选择不调用connect函数，但是操作系统底层会在应用程序首次发送数据时自动绑定一个地址（IP地址+随机分配一个空闲端口号）。当然，udp也可以选择先调用connect函数，然后再发送数据。

调用bind函数时，应用程序可以将端口号指定为0，这样操作系统就会随机分配一个未被使用的端口号。此外，还可以将IP地址指定为”0.0.0.0“，它表示绑定至任意地址，并接收任意地址发送过来的连接请求。注意，当随后对端连接请求到达时，操作系统会根据对端地址及路由表信息选择一个本地地址进行重绑定，这是因为一个链接的源地址和目的地址必须明确。

默认情况下，当需要对多个socket进行绑定时，ip地址和端口号不能同时相同，也即多个socket不能同时绑定到一个ip+端口号上。那么当一个socket被绑定至”0.0.0.0:21“时，21端口号将不能再被任何其它socket绑定。在这种情况下，BSD操作系统提供了两个选项，用来解决这个问题。

### SO_REUSEADDR
如果socket开启了SO_REUSEADDR选项，除非两个socket绑定到完全一模一样的地址，否则两个socket都能绑定成功。举个栗子，当socketA先绑定到了”0.0.0.0:21“，当socketB想要绑定到”10.0.0.1:21”时，如果socketB未开启SO_REUSEADDR，那么socketB的bind就会报错；而如果socketB开启了SO_REUSEADDR，那么socketB的bind就会成功。这是因为“0.0.0.0”和“10.0.0.1”并非同一个IP地址。

下面这张表展示了SO_REUSEADDR选项开启对地址绑定起到的作用：

```
SO_REUSEADDR       socketA        socketB       Result
---------------------------------------------------------------------
  ON/OFF       192.168.0.1:21   192.168.0.1:21    Error (EADDRINUSE)
  ON/OFF       192.168.0.1:21      10.0.0.1:21    OK
  ON/OFF          10.0.0.1:21   192.168.0.1:21    OK
   OFF             0.0.0.0:21   192.168.1.0:21    Error (EADDRINUSE)
   OFF         192.168.1.0:21       0.0.0.0:21    Error (EADDRINUSE)
   ON              0.0.0.0:21   192.168.1.0:21    OK
   ON          192.168.1.0:21       0.0.0.0:21    OK
  ON/OFF           0.0.0.0:21       0.0.0.0:21    Error (EADDRINUSE)

```
这张表的含义是：socketA已经被绑定成功，SO_REUSEADDR开启与否，当绑定socketB时返回的结果。

除了上面这个作用，SO_REUSEADDR还有一个非常著名的应用。针对tcp应用，当一个socket调用send发送数据时，它并非立即就会将数据发送出去，而是将数据写入缓冲区，并告知应用程序send已经发送成功。这其实是一个假象，缓冲区中的数据可能会等待一段时间之后才会被发送出去，因为当应用数据比较少时，tcp协议有一个优化，必须等缓冲区中的数据足够多或者超时发生时，才会将缓冲区中的数据统一发送出去。当应用程序读取read返回的成功状态之后，立即关闭连接，如果底层连接立即被释放，那么这些所有都为发送出去的数据都将丢失，这显然有悖于tcp连接的可靠传输。

当tcp连接的一端调用close函数发送FIN报文，并接受到对端的FIN+ACK报文之后，它就会进入TIME_WAIT状态，并持续等待一段时间。在这期间，它还能够处理来自对端的延迟报文。

操作系统实现的这个等待时间被称为Linger Time，一般为2分钟。无论是否真的还有数据发送，操作系统都会等待这么多时间。当然，应用程序也能够调整这个时间，使用SO_LINGER就能配置这个等待时间，甚至能够将其设置为0，也就表示不等待，这其实是非常危险的一个操作，它不仅有丢失数据的风险，还可能会导致对端异常退出(RST)。还需要注意的是，当配置了SO_LINGER选项，必须主动关闭socket，否则（如exit之前没有调用close）操作系统还是会使用默认的等待时间。

而SO_REUSEADDR所起的作用就和TIME_WAIT状态息息相关。正常情况下，绑定socket到一个正处于TIME_WAIT的地址会失败。但是如果socket被设置了SO_REUSEADDR选项，那么绑定时就会成功。

注意在第一个场景中，我们描述了当两个socket绑定的地址完全相同时，第二个socket绑定过程会报错。但是在第二个场景中，如果第一个socket已经处于TIME_WAIT状态，那么第二个socket绑定过程将会成功。这里不可避免的会带来副作用：丢数据+RST对端。

两个场景下，只需要第二个socket设置了SO_REUSEADDR即可，第一个socket无需进行任何操作。

### SO_REUSEPORT
SO_REUSEADDR处理两个socket绑定的问题，只要地址不完全相同，或者前一个已经处于TIME_WAIT状态。那么，如果要将多个socket绑定至完全相同（ip和port都相同）的地址呢？这就是SO_REUSEPORT提供的能力。

只要socket设置了SO_REUSEPORT选项，任意多个socket都能绑定至完全相同的地址。这里要求所有的socket都设置了SO_REUSEPORT选项，如果第一个socket没有设置该选项，那么后续的绑定都将失败。

借助SO_REUSEPORT，我们有能力将多个socket绑定至完全相同的地址，如果我们将多个socket同时connect到一个相同对端时，会发生什么呢？第二个及之后的socket执行connect将返回EADDRINUSE错误。这是因为每一个连接必须由唯一的五元组标识。

### linux
- 内核版本 < 3.9
	- 仅有SO_REUSEADDR选项，并且含义有所区别：
		- 针对服务端，两个socket如要监听相同的端口号，则IP地址必须是不相同的本地地址，不能是“0.0.0.0”
		- 针对客户端，两个socket如果要绑定至相同IP+PORT，则它们都需要设置SO_REUSEADDR选项
- 内核版本 >= 3.9
	- 新增SO_REUSEPORT选项，含义也有所区别
		- 如果多个socket要绑定至相同IP+PORT，则它们所属进程的用户ID必须相同
		- 多个socket监听相同IP+PORT，内核实现了简单负载均衡

### reference
https://stackoverflow.com/questions/14388706/socket-options-so-reuseaddr-and-so-reuseport-how-do-they-differ-do-they-mean-t