### Nagle vs Delayed ACK
互联网刚新起之时，链路上绝对的主力是什么？是 Telnet 和 NCP 这类实时交互协议。它们对网络传输的实时性要求极为苛刻，发送端每一个待发送的数据包都必须被立即送出去，接收端必须对每一个数据包进行响应，即使这些数据包的负载非常小。

当时间来到二十世纪八十年代，情况发生了转变。TCP/IP 协议栈占据了整个网络的绝对主导地位，这个时候人们发现如果继续沿用上面的机制，网络将会变得异常拥塞，网络传输也极为低效。试想一下，当网络上每个数据包的负载都只有1个字节，而数据包的报头却有40个字节(20个字节 TCP 报头及20个字节 IP 报头)，这类数据包有一个专有名词 tinygram。当网络上充斥着大量 tinygram，网络的利用率该有多低？

当时很多人就优化 TCP/IP 做出了巨大贡献，其中 John Nagle 提出了 Nagle 算法，极大的减少了发送端发送 tinygram 的概率。其核心思想是将多个等待发送的 tinygram 合并成一个更大的数据包，然后在发送到网络中去。

借用 ExtraHop 给出的例子：当搬家时，你聘请搬家公司来为你搬运家具，你是希望每辆运输车装好一个家具就立即出发，还是希望将运输车装满然后再出发呢？毫无疑问，我们都会选择后者，因为前者非常低效，并可能会造成公路拥堵。

Nagle 算法的核心思想是：当应用有数据需要发送，但是之前有发送的数据包还没有被 ACK，那么就先缓存本次数据。算法描述如下：[minshal-nagle](https://www.ietf.org/archive/id/draft-minshall-nagle-01.txt)

```
If a TCP has less than a full-sized packet to transmit,
and if any previous packet has not yet been acknowledged,
do not transmit a packet.

// pseudo-code
if ((packet.size < Eff.snd.MSS) && (snd.nxt > snd.una)) {
    do not send the packet;
}
```

- snd.nxt: a TCP variable which names the next byte of data to be transmitted
- snd.una: a TCP variable which names the next byte of data to be acknowledged, if equals to snd.nxt, then all previous packets have been acknowledged
- Eff.snd.MSS: the largest TCP payload (user data) that can be transmitted in one packet
- snd.sml: a TCP variable which names the last byte of data in the most recently transmitted small packet

正常情况下，Nagle 算法的使用能够极大提升网络的整体利用率。然而，几乎同时另一拨人提出了 Delayed ACK 算法，其算法思想和 Nagle 算法类似，但是作用于接收端。为了避免网络上充斥大量的 ACK 数据包，Delayed ACK 算法限制接收端不需要每次收到数据包时都立即响应 ACK，而是等待一段时间，如果稍后接收端也有数据发送，正好就可以捎上这个 ACK 数据包。如果接收端没有数据包发送，则需要满足以下两个条件之一：

- 定时器超时：Delayed ACK 算法维护一个定时器，当收到一个数据包后，必须在定时器超时时会送 ACK，定时器一般不超过200ms，最大限制500ms；
- 每收到两个 full-size 数据包，就必须会送一个 ACK。

```
This process, known as ``delayed ACKing'' [RFC1122],
typically causes an ACK to be generated for every other received
(full-sized) data packet.  In the case of an ``isolated'' TCP
packet (i.e., where a second TCP packet is not going to arrive
anytime soon), the delayed ACK policy causes an acknowledgement for
the data in the isolated packet to be delayed up to 200
milliseconds of the receipt of the isolated packet (the actual
maximum time the acknowledgement can be delayed is 500ms [RFC1122],
but most systems implement a maximum of 200ms, and we shall assume
that number in this document).
```

这两个算法相互独立使用都不会造成问题，但是当两个算法一起使用时，问题就出现了：
- 发送端一次数据发送被拆成两个数据包，第二个数据包大小小于 MSS。发送端会把第一个数据包发送出去，并等待 ACK（Nagle 算法）
- 接收端接收到第一个数据包，但是不会立即发送 ACK，最多等待200ms后，才发送 ACK（Delayed ACK 算法）
- 发送端接收到 ACK，发送第二个数据包

注意到没，两次发送之间有一个 Delay。上面的场景并不少见，那如何解决呢？

1999年，Minshall 对 Nagle 算法做了一点点改动，大幅减少了 Nagle 和 Delayed ACK 算法共用的问题：

```
If a TCP has less than a full-sized packet to transmit,
and if any previously transmitted less than full-sized
packet has not yet been acknowledged, do not transmit
a packet.

// pseudo-code
if (packet.size < Eff.snd.MSS) {
    if (snd.sml > snd.una)) {
        do not send the packet;
    } else {
        snd.sml = snd.nxt+packet.size;
        send the packet;
    }
}
```

在改进版本的 Nagle 算法中，仅针对小数据包进行限制：当之前发送的一个小数据包还未被 ACK，当前发送的小数据包则会被延迟发送。而原始 Nagle 算法在发送小数据包时，如果存在未被 ACK 的数据包，不论其大小，都会被延迟发送。

需要注意的是，改进版 Nagle 算法并没有完全消除延迟发送的可能，而是更加严格的限制了其发生的条件。

举个栗子，当我们发送一个 HTTP 请求时，当 Header 和 Body 被合在一个数据包中发送时，如果其大小不是 MSS 的整数倍：
- 使用原始 Nagle 算法会延迟发送最后一个数据包
- 使用改进 Nagle 算法不存在这个问题

而当 Header 和 Body 被拆成两个数据包发送时，若 Header 和 Body 大小都小于 MSS，两种算法都会延迟发送 Body。

那么真正避免数据包延迟发送的方法是什么呢？正是这两个算法一起使用才导致的这个问题，如果我们禁用任何一端（禁止发送端使用 Nagle，或者禁止接收端使用 Delayed ACK），问题都迎刃而解。

Linux针对这两个算法都提供一个开关，用于禁用算法：
- TCP_NODELAY：发送端禁用 Nagle 算法
- TCP_QUICKACK：接收端禁用 Delayed ACK 算法
调用 setsockopt 可以为 socket 设置对应的选项。

最后，我们总结下应用在什么模式下才会出现延迟发送：
- write-read-write-read 模式：这种模式下不会出现上面的问题，因为接收端在发送数据的时候回捎上 ACK
- write-write-read 模式：如果发送的第二个是小数据包，旧版 Nagle 算法存在问题，但是新版不存在

### Silly Window Syndrome (SWS)
SWS 昵称傻逼窗口综合征^_^。其病状就是发送端持续发送 tinygram，或者是接收端只能接收少量的字节。

总所周知，TCP 协议的接收端在接收到数据时，需要会送一个 ACK 数据包，其中包含一个关键字段：Window Size，表示其当前所有接收的最大字节数。发送端用该窗口大小，减去未被 ACK 的字节数，就可计算出可用窗口大小，发送端发送的数据量不能超过这个大小。这就是经典的 TCP 流量控制算法。

那么什么情况下会出现 SWS 呢？主要是一下两种情况：

第一种，发送端产生数据的速度足够慢。试想发送端每次就产生一个字节的数据，若不加任何控制，网络上就会充斥着这种 tinygram。其解决方案非常简单，借助 Nagle 算法，将多次产生的小负载合并成一个数据包。这里需要做一个权衡，如果应用对实时性要求非常严格，Nagle 算法可能会引起其他的问题。

第二种，接收端处理数据的速度足够慢。试想发送端每次都全量（MSS）发送，但是接收端每次只能处理一个字节，这会导致接收端的可用窗口越来越小，最后会减小到一个字节大小，与之对应的就是发送端调整发送速度，最后还是会变成每次发送一个字节。其解决方案则有以下两种：
- 当可用窗口足够小时，停止向发送端传播其窗口大小，而是等到其能够完整容纳一个 MSS 数据包，或者是其缓冲区空闲过半时，才告知发送端窗口大小
- 延迟发送 ACK，也就是延迟一段时间才向发送端发送窗口大小。这里需要注意的是，需要控制好这个延迟的时间，一定要避免发送端因为超时导致数据包重发

现实中，这两种情况都有可能会发生，因此当我们遇到 SWS 问题时，首先要定位是哪一端引起的问题，然后再调整策略。

### Reference
1. https://www.ietf.org/archive/id/draft-minshall-nagle-01.txt
2. https://www.extrahop.com/company/blog/2016/tcp-nodelay-nagle-quickack-best-practices/