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
- **Web 管理界面** — 内置状态页面和频道列表
- **M3U 播放列表** — 自动生成和自定义频道列表
- **会话缓存** — 断开后短暂保持组播连接
- **IGMP 嗅探模式** — 自动发现 IPTV 频道，机顶盒换台即捕获

## 快速开始

### 下载预编译二进制

从 [GitHub Releases](https://github.com/laishouchao/multicast-proxy/releases) 下载最新版本：

```bash
# OpenWrt 路由器 (mipsel_24kc 架构)
wget https://github.com/laishouchao/multicast-proxy/releases/download/v1.2.0/multicast_proxy-v1.2.0-openwrt-mipsel.tar.gz

# 解压
tar xzf multicast_proxy-v1.2.0-openwrt-mipsel.tar.gz
```

### 部署到路由器

```bash
# 复制到路由器
scp multicast_proxy root@192.168.2.1:/usr/bin/
scp multicast_proxy.init root@192.168.2.1:/etc/init.d/multicast_proxy
scp multicast_proxy.uci root@192.168.2.1:/etc/config/multicast_proxy
scp example.conf root@192.168.2.1:/etc/multicast_proxy.conf

# 启用并启动
ssh root@192.168.2.1 "chmod +x /usr/bin/multicast_proxy /etc/init.d/multicast_proxy"
ssh root@192.168.2.1 "/etc/init.d/multicast_proxy enable && /etc/init.d/multicast_proxy start"
```

### 本地编译

```bash
# 在 OpenWrt 路由器上本地编译
gcc -O2 -Wall -Wextra -pthread -o multicast_proxy multicast_proxy.c

# 交叉编译 (需要 OpenWrt SDK)
# 下载 LEDE 17.01.7 SDK:
# https://archive.openwrt.org/releases/17.01.7/targets/ramips/mt7621/lede-sdk-17.01.7-ramips-mt7621_gcc-5.4.0_musl-1.1.16.Linux-x86_64.tar.xz
make CROSS_COMPILE=mipsel-openwrt-linux-musl- LDFLAGS="-pthread -static"
```

## 使用方法

### HTTP API 端点

| 端点 | 说明 |
|------|------|
| `GET /udp/GROUP:PORT` | 播放组播频道 |
| `GET /rtp/GROUP:PORT` | 播放 RTP 频道 |
| `GET /status` | Web 状态页面 |
| `GET /playlist` | 下载 M3U 播放列表 |
| `GET /api/status` | JSON 状态信息 |
| `GET /api/channels` | JSON 频道列表 |
| `GET /health` | 健康检查 |
| `GET /discovered` | 已发现频道列表 (嗅探模式) |

### 使用示例

```bash
# 播放频道 (在播放器中打开)
http://192.168.2.1:8888/udp/232.0.1.210:1234

# 查看状态页面
http://192.168.2.1:8888/status

# 下载播放列表
http://192.168.2.1:8888/playlist

# SSM 模式 (指定源地址)
multicast_proxy -p 8888 -i eth0.2 -s 10.253.18.18

# 加载自定义频道列表
multicast_proxy -f /path/to/channels.m3u

# 守护进程模式 + 详细日志
multicast_proxy -d -v
```

## IGMP 嗅探模式

嗅探模式可以自动发现 IPTV 频道。将机顶盒连接在 OpenWrt 路由器下游，开启嗅探模式后，机顶盒每次换台时，程序会自动嗅探 IGMP Join 报文，记录组播组地址，并尝试解析频道名。

### 工作原理

```
机顶盒换台 → 发送 IGMP Join → 路由器嗅探捕获 → 记录组播组地址
                                         ↓
                                 定时扫描验证频道活跃
                                         ↓
                                 尝试从 TS 流解析频道名 (SDT)
                                         ↓
                                 自动生成 discovered.m3u
```

### 启用嗅探模式

```bash
# 命令行方式
multicast_proxy -S -i eth0.2 -v

# 配置文件方式 (/etc/multicast_proxy.conf)
sniff_mode=1
mcast_iface=br-lan    # 可选，默认使用upstream接口

# UCI 方式
uci set multicast_proxy.@proxy[0].sniff_mode=1
uci commit multicast_proxy
/etc/init.d/multicast_proxy restart
```

### 嗅探输出

发现的频道会自动写入：
- **M3U 文件**: `/etc/multicast_proxy.m3u.discovered`
- **API**: `GET /discovered` (JSON 格式)
- **Web 界面**: 状态页面会显示已发现频道

### 使用场景

1. **自动构建播放列表**: 无需手动配置频道，机顶盒换台即可自动发现
2. **频道监控**: 实时监控哪些频道正在被观看
3. **网络调试**: 查看 IPTV 组播流量情况

### 嗅探日志示例

```
[SNIFF] IGMP 嗅探已启动 (接口: eth0.2)
[SNIFF] ★ 发现新频道! 组播组: 232.0.1.210 (来自: 192.168.2.100)
[SNIFF] 频道名更新: 232.0.1.210 -> CCTV-1 综合
[SNIFF] 已更新播放列表: 5 个活跃频道 -> /etc/multicast_proxy.m3u.discovered
```

## 命令行参数

```bash
Multicast Proxy v1.3.0 - 智能IPTV组播代理

用法: multicast_proxy [选项]

选项:
  -p PORT      HTTP监听端口 (默认: 8888)
  -i IFACE     上游组播接口 (默认: eth0.2)
  -c FILE      配置文件路径 (默认: /etc/multicast_proxy.conf)
  -f FILE      频道列表文件 (M3U/CSV格式)
  -C MAX       最大客户端数 (默认: 200)
  -t SEC       会话缓存秒数 (默认: 5)
  -s SOURCE    SSM源地址
  -S           嗅探模式 (自动发现IPTV频道)
  -d           守护进程模式
  -v           详细日志输出
  -h           显示帮助信息
```

## 配置文件

### 命令行配置 (/etc/multicast_proxy.conf)

```ini
# 基础配置
port=8888                    # HTTP监听端口
iface=eth0.2                 # 上游组播接口
max_clients=200              # 最大并发客户端数
rcvbuf=1048576               # 接收缓冲区大小 (字节)
grace_sec=5                  # 会话缓存时间 (秒)

# SSM配置 (可选)
ssm_source=10.253.18.18     # SSM源地址 (启用后自动切换SSM模式)

# 文件路径
m3u_file=/etc/multicast_proxy.m3u  # M3U播放列表文件路径

# 调试
verbose=0                    # 详细日志 (0=关闭, 1=开启)

# 嗅探模式 (可选)
sniff_mode=0                 # 启用嗅探模式 (0=关闭, 1=开启)
mcast_iface=                 # 嗅探接口 (默认使用upstream接口)
```

### UCI 配置 (/etc/config/multicast_proxy)

```uci
config multicast_proxy 'main'
    option enabled '1'           # 启用服务
    option port '8888'           # HTTP端口
    option iface 'eth0.2'        # 上游接口
    option max_clients '200'     # 最大客户端数
    option rcvbuf '1048576'      # 接收缓冲区
    option grace_sec '5'         # 会话缓存时间
    # option ssm_source ''       # SSM源地址 (可选)
    # option m3u_file '/etc/multicast_proxy.m3u'  # M3U文件路径
    # option verbose '0'         # 详细日志
    # option sniff_mode '0'      # 嗅探模式
    # option mcast_iface ''      # 嗅探接口
```

### UCI 命令行配置

```bash
uci set multicast_proxy.@proxy[0].enabled=1
uci set multicast_proxy.@proxy[0].port=8888
uci set multicast_proxy.@proxy[0].iface=eth0.2
uci set multicast_proxy.@proxy[0].max_clients=200
uci set multicast_proxy.@proxy[0].rcvbuf=1048576
uci set multicast_proxy.@proxy[0].grace_sec=5
uci set multicast_proxy.@proxy[0].ssm_source=10.253.18.18  # 可选
uci commit multicast_proxy
```

## 频道列表

### M3U 格式

在路由器上创建 `/etc/multicast_proxy.m3u`:

```m3u
#EXTM3U
#EXTINF:-1 tvg-name="CCTV1" group-name="央视频道",CCTV-1 综合
/udp/232.0.1.210:1234
#EXTINF:-1 tvg-name="CCTV2" group-name="央视频道",CCTV-2 财经
/udp/232.0.3.2:1234
#EXTINF:-1 tvg-name="CCTV5" group-name="央视频道",CCTV-5 体育
/udp/232.0.6.5:1234
```

### CSV 格式

```csv
CCTV-1 综合,央视频道,232.0.1.210,1234
CCTV-2 财经,央视频道,232.0.3.2,1234
CCTV-5 体育,央视频道,232.0.6.5,1234
```

### 默认内置频道

如果未配置频道文件，程序会自动加载18个央视频道（CCTV-1到CCTV-17），端口均为1234。

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
| Web界面 | 无 | 内置 |
| M3U生成 | 需要额外配置 | 自动生成 |
| 频道嗅探 | 不支持 | IGMP 嗅探模式 |

## 环境要求

- OpenWrt 17.01+ (LEDE) 或更高版本
- Linux Kernel 3.x+
- 支持架构: mipsel_24kc (WR1200JS / MT7621), x86_64
- 无其他依赖

## 支持的路由器

已测试的路由器型号：
- **WR1200JS** (MediaTek MT7621, mipsel_24kc) - 主要开发平台
- 其他 OpenWrt 支持的路由器理论上兼容

## 故障排除

### 常见问题

1. **无法播放频道**
   - 检查上游接口 `iface` 是否正确
   - 确认组播源地址和端口
   - 检查防火墙是否允许组播流量

2. **IGMP 加入失败**
   - 确认内核支持 IGMP
   - 检查接口是否已获取 IP 地址

3. **连接数过多**
   - 调整 `max_clients` 参数
   - 检查 `rcvbuf` 缓冲区大小

### 日志调试

```bash
# 启用详细日志
multicast_proxy -v

# 查看系统日志
logread | grep multicast_proxy
```

## 版本历史

### v1.3.0 (当前版本)
- 新增 IGMP 嗅探模式 (`-S` 参数)
- 自动发现 IPTV 频道 (监听 IGMP Join 报文)
- TS 流频道名解析 (SDT 表)
- 自动生成 `discovered.m3u` 播放列表
- 新增 `/discovered` API 端点
- 配置文件支持 `sniff_mode` 和 `mcast_iface`

### v1.2.0
- 修复 CI: 恢复 OpenWrt SDK 交叉编译
- 主要目标: mipsel_24kc (WR1200JS / MT7621)
- 静态链接: 二进制不依赖路由器共享库
- 代码改进: 所有 strncpy 替换为 snprintf (null-safe)

### v1.1.1
- 修复 CI 编译错误
- 添加 XSS 防护
- 修复 fork() 共享状态问题
- 添加 /health 端点

### v1.1.0
- 初始版本发布
- 基本组播转发功能
- Web 状态页面
- procd/UCI 集成

## 贡献

欢迎提交 Issue 和 Pull Request！

## 许可证

MIT License

## 链接

- [GitHub 仓库](https://github.com/laishouchao/multicast-proxy)
- [Releases](https://github.com/laishouchao/multicast-proxy/releases)
- [Issues](https://github.com/laishouchao/multicast-proxy/issues)
