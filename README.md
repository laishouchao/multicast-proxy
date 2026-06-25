# Multicast Proxy

All-in-One IPTV proxy for OpenWrt — 替代 igmpproxy + udpxy 的一体化方案。

## 核心原理

```
传统方案:  STB发IGMP → igmpproxy转发 → ISP发组播 → udpxy接收 → HTTP客户端
本方案:    HTTP请求 → 内核IP_ADD_MEMBERSHIP → 内核自动发IGMP → ISP发组播 → HTTP转发
```

使用 Linux 内核标准的 `setsockopt(IP_ADD_MEMBERSHIP)` 让内核自动处理 IGMP 协议，
完全不需要 igmp_raw socket、辅助IP、手工注入、或任何额外的守护进程。

## 特性

- **单一二进制** — 一个程序替代 igmpproxy + udpxy
- **零配置 IGMP** — 内核自动处理 IGMPv1/v2/v3 join/leave
- **广泛协议兼容** — ASM / SSM / PIM-SSM，组播地址范围 224.0.0.0 - 239.255.255.255
- **HTTP API** — `/udp/GROUP:PORT` 播放，`/status` 状态页，`/playlist` 播放列表
- **procd 集成** — OpenWrt 原生服务管理，开机自启
- **UCI 配置** — 标准 OpenWrt 配置方式
- **并发支持** — 多客户端同时观看同一频道
- **守护进程模式** — 支持 `-d` 后台运行

## 快速开始

### 编译

```bash
# 在 OpenWrt 路由器上本地编译 (推荐)
gcc -O2 -Wall -o multicast_proxy multicast_proxy.c

# 交叉编译 (需要 OpenWrt SDK)
make CROSS_COMPILE=/path/to/openwrt/staging_dir/toolchain-.../bin/mipsel-openwrt-linux-musl-
```

### 部署

```bash
# 复制到路由器
scp multicast_proxy root@192.168.2.1:/usr/bin/
ssh root@192.168.2.1 "chmod +x /usr/bin/multicast_proxy"

# 安装启动脚本
scp multicast_proxy.init root@192.168.2.1:/etc/init.d/multicast_proxy
ssh root@192.168.2.1 "chmod +x /etc/init.d/multicast_proxy"

# 安装UCI配置
scp multicast_proxy.uci root@192.168.2.1:/etc/config/multicast_proxy

# 启用并启动
ssh root@192.168.2.1 "/etc/init.d/multicast_proxy enable && /etc/init.d/multicast_proxy start"
```

### 一键部署 (Node.js)

```bash
npm install ssh2
node deploy_final.js
```

## 使用方法

```bash
# 播放频道
http://192.168.2.1:8888/udp/232.0.1.210:1234

# 查看状态
http://192.168.2.1:8888/status

# 下载播放列表
http://192.168.2.1:8888/playlist

# SSM 模式 (指定源地址)
multicast_proxy -p 8888 -i eth0.2 -s 10.253.18.18
```

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p PORT` | HTTP 监听端口 | 8888 |
| `-i IFACE` | 上游组播接口 | eth0.2 |
| `-c MAX` | 最大客户端数 | 100 |
| `-t SEC` | 客户端超时 (0=无) | 0 |
| `-s IP` | SSM 源地址 | (ASM模式) |
| `-b SIZE` | 接收缓冲区 (bytes) | 524288 |
| `-d` | 守护进程模式 | 否 |
| `-v` | 详细日志 | 否 |

## UCI 配置

```bash
uci set multicast_proxy.@proxy[0].enabled=1
uci set multicast_proxy.@proxy[0].port=8888
uci set multicast_proxy.@proxy[0].upstream=eth0.2
uci set multicast_proxy.@proxy[0].max_clients=100
uci set multicast_proxy.@proxy[0].ssm_source=10.253.18.18  # 可选
uci commit multicast_proxy
```

## 频道列表

在路由器上创建 `/etc/multicast_proxy.m3u`:

```m3u
#EXTM3U
#EXTINF:-1 tvg-name="CCTV1" group-name="央视频道",CCTV-1 综合
/udp/232.0.1.210:1234
#EXTINF:-1 tvg-name="CCTV2" group-name="央视频道",CCTV-2 财经
/udp/232.0.3.2:1234
```

## 架构

```
┌──────────────────────────────────────────────┐
│              multicast_proxy                  │
│                                              │
│  ┌─────────┐    ┌──────────┐    ┌─────────┐ │
│  │  HTTP   │    │  IGMP    │    │  Data   │ │
│  │ Server  │───▶│ Manager  │───▶│Forwarder│ │
│  │ :8888   │    │ (kernel) │    │         │ │
│  └─────────┘    └──────────┘    └─────────┘ │
│       │                            │        │
│  HTTP请求                     组播数据流     │
│       │                            │        │
└───────┼────────────────────────────┼────────┘
        │                            │
   LAN (br-lan)              WAN (eth0.2)
   手机/电脑/电视              ISP组播源
```

## 与传统方案对比

| 特性 | igmpproxy + udpxy | multicast_proxy |
|------|-------------------|-----------------|
| 进程数 | 2+ | 1 |
| IGMP处理 | 用户态raw socket | 内核自动 |
| 辅助IP | 需要 | 不需要 |
| 配置复杂度 | 高 | 低 |
| IGMPv3支持 | 取决于版本 | 内核版本 |
| SSM支持 | 不支持 | 原生支持 |
| 错误恢复 | 手动 | 自动 |

## 环境要求

- OpenWrt 17.01+ (LEDE)
- Linux Kernel 3.x+
- gcc (路由器上编译或交叉编译)
- 无其他依赖

## License

MIT
