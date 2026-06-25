# 济南广电 IPTV OpenWrt 代理配置工作总结

## 问题背景

用户需要在 OpenWrt WR1200JS 路由器上实现：
- **机顶盒**通过 igmpproxy 正常观看 IPTV（已有，不能受影响）
- **手机/电脑**通过 udpxy 在局域网内观看 IPTV 频道

拓扑：光猫 LAN2 → OpenWrt WAN(eth0.2) | OpenWrt LAN1 → 机顶盒 | 手机连 OpenWrt WiFi

## 根因分析

### 为什么 udpxy 无法播放

udpxy 运行在路由器上，加入组播组时使用的源 IP 是 **192.168.2.1**（br-lan 的接口地址）。igmpproxy 收到这个 IGMP 报告后，执行了以下检查：

```c
// igmpproxy 源码 request.c
if(sourceVif->InAdr.s_addr == src) {
    my_log(LOG_NOTICE, 0, "The IGMP message was from myself. Ignoring.");
    return;  // 直接忽略！
}
```

由于 192.168.2.1 正是 br-lan 的 IP，igmpproxy 认为"这是我自己的 IGMP 报告"，**直接忽略了**。因此不会向上游发送 IGMP 加入请求，ISP 不会发送组播数据，udpxy 收不到任何数据。

机顶盒（192.168.2.197）不受影响，因为它的 IP 不等于 br-lan 的接口 IP。

### 关键技术点

1. **SSM 组播**：济南广电 IPTV 使用 SSM（Source-Specific Multicast），组播地址 232.0.0.0/8，源地址 10.253.18.18
2. **MRT_MISS_UPCALL**：当组播数据到达但没有匹配的 mroute 条目时，内核会发送 MRT_MISS_UPCALL 给 MRT_INIT 持有者（igmpproxy），携带正确的源地址。igmpproxy 据此创建正确的 (10.253.18.18, G) mroute 条目
3. **udpxy 必须使用 -m br-lan**：Linux 内核在 mroute 转发时，只有下游接口（br-lan）上的本地 socket 能收到数据，上游接口（eth0.2）上的不行

## 解决方案

编写了一个轻量级 C 程序 **igmp_inject**，通过在 br-lan 上添加辅助 IP **192.168.2.254**，从该 IP 注入 IGMP 报告，绕过 igmpproxy 的"自身检测"。

### 工作流程

```
udpxy 请求 232.0.1.58
    ↓
内核在 br-lan 发送 IGMP (源=192.168.2.1)
    ↓
igmpproxy 收到 → "是自己的，忽略" → 不处理
    ↓
igmp_inject 守护进程监控 /proc/net/igmp
    ↓ 发现 br-lan 上有 232.x.x.x 组
从 192.168.2.254 注入 IGMP 报告
    ↓
igmpproxy 收到 → 不是自己的 → 处理！
    ↓
向上游 eth0.2 发送 IGMP 加入
    ↓
ISP 开始发送组播数据 (源=10.253.18.18)
    ↓
内核 MRT_MISS_UPCALL → igmpproxy 创建 mroute
    ↓ (10.253.18.18, 232.0.1.58) eth0.2 → br-lan
udpxy 在 br-lan 上收到数据 → 转发给手机
```

## 最终配置

| 组件 | 配置 |
|------|------|
| igmpproxy | upstream=eth0.2(wan), downstream=br-lan(lan), altnet=0.0.0.0/0 |
| udpxy | interface=lan, source=br-lan, port=8888 |
| igmp_inject | 辅助IP=192.168.2.254, 监控间隔=5秒, 守护模式 |
| 防火墙 | Allow-IGMP (wan→igmp), Allow-Multicast-UDP (wan→lan) |
| IGMP snooping | 已关闭 (br-lan) |

## 使用方式

- 机顶盒：正常观看，不受影响
- 手机/IPTV Pro/VLC：`http://192.168.2.1:8888/udp/232.0.x.x:1234`
- M3U 播放列表：`c:\Users\18375\Documents\Trae\机顶盒代理调试\iptv_channels.m3u`

## 持久化

- igmpproxy / udpxy：OpenWrt 标准服务，重启自动启动
- igmp_inject：已创建 `/etc/init.d/igmp_inject` 启动脚本，已设置自启
- 辅助 IP 192.168.2.254：已添加到 `/etc/rc.local`

## 文件清单

| 文件 | 位置 | 用途 |
|------|------|------|
| igmp_inject.c | 路由器 /usr/bin/igmp_inject | IGMP 注入守护进程（已编译安装） |
| igmp_inject.init | 路由器 /etc/init.d/igmp_inject | 启动脚本 |
| iptv_channels.m3u | 本机工作目录 | 130 个频道的 M3U 播放列表 |
