/*
 * multicast_proxy.c - All-in-One IPTV Proxy for OpenWrt
 *
 * 替代 igmpproxy + udpxy 的一体化方案
 * 用法: multicast_proxy [-p port] [-i iface] [-s source_ip] [-d] [-c config]
 *
 * 核心原理:
 *   HTTP请求 /udp/GROUP:PORT → 内核IP_ADD_MEMBERSHIP自动发送IGMP报告
 *   → ISP开始发送组播流 → 内核接收 → 转发给HTTP客户端
 *   客户端断开 → 内核自动发送IGMP Leave
 *   完全不需要igmp_raw socket、辅助IP、手工注入
 *
 * 协议支持 (由内核自动处理):
 *   IGMPv1/v2/v3, ASM, SSM, PIM-SSM
 *   组播地址范围: 224.0.0.0 - 239.255.255.255
 *
 * 编译: gcc -O2 -Wall -o multicast_proxy multicast_proxy.c
 * 部署: 复制到 /usr/bin/multicast_proxy
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>

/* ========== 配置默认值 ========== */
#define DEFAULT_LISTEN_PORT    8888
#define DEFAULT_MAX_CLIENTS    100
#define DEFAULT_TIMEOUT        0       /* 0=无超时, 单位秒 */
#define DEFAULT_RCVBUF         (512*1024)
#define BUF_SIZE               2048
#define MCAST_BUF_SIZE         (188*7) /* 7个TS包, 适合DVB */
#define MAX_CHANNELS           256

/* IGMP 计时器 (RFC 3376) */
#define IGMP_RESPONSE_INTERVAL 10      /* IGMPv2 Query Response Interval */
#define IGMP_LEAVE_DELAY       2       /* 离开前额外等待 */

/* ========== 全局状态 ========== */
static volatile int g_running = 1;
static volatile int g_child_count = 0;
static int g_listen_port = DEFAULT_LISTEN_PORT;
static char g_iface[32] = "eth0.2";
static int g_max_clients = DEFAULT_MAX_CLIENTS;
static int g_timeout = DEFAULT_TIMEOUT;
static int g_daemon = 0;
static int g_verbose = 0;
static int g_ssm_source = 0;          /* 0=ASM, 1=SSM */
static char g_ssm_source_ip[32] = "";
static int g_rcvbuf = DEFAULT_RCVBUF;

/* 统计信息 */
static int g_total_clients = 0;
static int g_active_clients = 0;

/* ========== 信号处理 ========== */
static void sig_handler(int sig) {
    if (sig == SIGCHLD) {
        /* 非阻塞回收子进程 */
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {
            g_child_count--;
            if (g_active_clients > 0) g_active_clients--;
        }
    } else if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGHUP) {
        /* 重载配置 (预留) */
    }
}

/* ========== 日志 ========== */
static FILE *g_logfile = NULL;

static void log_msg(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (level < 0) level = 0;
    if (level > 3) level = 3;

    if (g_daemon) {
        /* syslog风格 */
        fprintf(stderr, "multicast_proxy: [%s] %s: ", timebuf, level_str[level]);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        printf("[%s] %s: ", timebuf, level_str[level]);
        vprintf(fmt, ap);
        printf("\n");
    }

    va_end(ap);
}

#define LOG_DEBUG(fmt, ...) do { if (g_verbose) log_msg(0, fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  log_msg(1, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg(2, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(3, fmt, ##__VA_ARGS__)

/* ========== 网络工具 ========== */

/* 获取接口的IP地址 */
static int get_iface_ip(const char *iface, struct in_addr *addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    int ret = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);

    if (ret < 0) return -1;
    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return 0;
}

/* 获取接口索引 */
static int get_iface_index(const char *iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    int ret = ioctl(fd, SIOCGIFINDEX, &ifr);
    close(fd);

    if (ret < 0) return -1;
    return ifr.ifr_ifindex;
}

/* 检查接口是否存在并有IP */
static int check_iface(const char *iface) {
    struct in_addr addr;
    if (get_iface_ip(iface, &addr) < 0) {
        LOG_WARN("Interface %s has no IP address yet", iface);
        return -1;
    }
    LOG_DEBUG("Interface %s: %s", iface, inet_ntoa(addr));
    return 0;
}

/* 设置非阻塞 */
static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ========== HTTP 解析 ========== */

struct http_request {
    char method[16];
    char uri[512];
    char host[128];
    char path[512];
    int  port;
    /* 解析后的组播信息 */
    int  is_multicast;
    char multicast_addr[32];
    int  multicast_port;
    int  is_status;     /* /status */
    int  is_playlist;   /* /playlist 或 /m3u */
    int  is_leave;      /* /leave/GROUP:PORT */
};

static void parse_http_request(const char *raw, struct http_request *req) {
    memset(req, 0, sizeof(*req));

    /* 解析请求行: METHOD URI HTTP/x.x */
    if (sscanf(raw, "%15s %511s", req->method, req->uri) != 2) return;

    /* 检查特殊路径 */
    if (strcmp(req->uri, "/status") == 0 || strcmp(req->uri, "/status.html") == 0) {
        req->is_status = 1;
        return;
    }
    if (strcmp(req->uri, "/playlist") == 0 || strcmp(req->uri, "/m3u") == 0 ||
        strcmp(req->uri, "/m3u8") == 0 || strcmp(req->uri, "/playlist.m3u") == 0) {
        req->is_playlist = 1;
        return;
    }

    /* 解析 /udp/GROUP:PORT 或 /rtp/GROUP:PORT */
    const char *p = NULL;
    if (strncmp(req->uri, "/udp/", 5) == 0) {
        p = req->uri + 5;
    } else if (strncmp(req->uri, "/rtp/", 5) == 0) {
        p = req->uri + 5;
    } else if (strncmp(req->uri, "/leave/", 7) == 0) {
        p = req->uri + 7;
        req->is_leave = 1;
    }

    if (!p) return;

    unsigned int a, b, c, d, port = 0;
    /* 支持 232.0.1.210:1234 和 23201210:1234 两种格式 */
    if (sscanf(p, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5) {
        snprintf(req->multicast_addr, sizeof(req->multicast_addr),
                 "%u.%u.%u.%u", a, b, c, d);
        req->multicast_port = (int)port;
        req->is_multicast = 1;
    } else {
        /* 尝试八位组格式: 23201210:1234 */
        unsigned int addr32 = 0;
        unsigned int pt = 0;
        if (sscanf(p, "%8u:%u", &addr32, &pt) == 2) {
            unsigned char *b = (unsigned char *)&addr32;
            snprintf(req->multicast_addr, sizeof(req->multicast_addr),
                     "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            req->multicast_port = (int)pt;
            req->is_multicast = 1;
        }
    }
}

/* ========== 组播会话管理 ========== */

struct channel {
    char addr[32];
    int  port;
    int  mcast_fd;          /* 组播UDP socket */
    int  ref_count;         /* 引用计数 (活跃客户端数) */
    int  joined;            /* 是否已加入组播 */
    pid_t sender_pid;       /* 转发子进程PID */
    struct channel *next;
};

static struct channel *g_channels = NULL;

static struct channel *channel_find(const char *addr, int port) {
    struct channel *ch = g_channels;
    while (ch) {
        if (ch->port == port && strcmp(ch->addr, addr) == 0)
            return ch;
        ch = ch->next;
    }
    return NULL;
}

static struct channel *channel_create(const char *addr, int port) {
    struct channel *ch = calloc(1, sizeof(struct channel));
    if (!ch) return NULL;

    strncpy(ch->addr, addr, sizeof(ch->addr) - 1);
    ch->port = port;
    ch->mcast_fd = -1;
    ch->ref_count = 0;
    ch->joined = 0;

    /* 插入链表头部 */
    ch->next = g_channels;
    g_channels = ch;

    return ch;
}

/* 加入组播组 (通过内核IGMP自动处理) */
static int channel_join(struct channel *ch) {
    if (ch->joined) {
        ch->ref_count++;
        LOG_DEBUG("Channel %s:%d ref_count=%d (reuse)", ch->addr, ch->port, ch->ref_count);
        return 0;
    }

    /* 创建UDP socket */
    ch->mcast_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ch->mcast_fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return -1;
    }

    /* 允许地址复用 */
    int opt = 1;
    setsockopt(ch->mcast_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 增大接收缓冲区 */
    setsockopt(ch->mcast_fd, SOL_SOCKET, SO_RCVBUF, &g_rcvbuf, sizeof(g_rcvbuf));

    /* 绑定到组播地址和端口 */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(ch->port);
    bind_addr.sin_addr.s_addr = inet_addr(ch->addr);

    if (bind(ch->mcast_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("bind(%s:%d) failed: %s", ch->addr, ch->port, strerror(errno));
        close(ch->mcast_fd);
        ch->mcast_fd = -1;
        return -1;
    }

    /* 加入组播组 - 内核自动发送IGMP Report */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ch->addr);
    mreq.imr_interface.s_addr = inet_addr("0.0.0.0");  /* 让内核选择接口 */

    /* 优先使用指定接口 */
    struct in_addr iface_ip;
    if (get_iface_ip(g_iface, &iface_ip) == 0) {
        mreq.imr_interface.s_addr = iface_ip.s_addr;
    }

    if (setsockopt(ch->mcast_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        LOG_ERROR("IP_ADD_MEMBERSHIP(%s on %s) failed: %s", ch->addr, g_iface, strerror(errno));
        close(ch->mcast_fd);
        ch->mcast_fd = -1;
        return -1;
    }

    /* SSM: 如果指定了源地址，添加源过滤 */
    if (g_ssm_source && g_ssm_source_ip[0]) {
        struct ip_mreq_source mreqsrc;
        mreqsrc.imr_multiaddr.s_addr = inet_addr(ch->addr);
        mreqsrc.imr_sourceaddr.s_addr = inet_addr(g_ssm_source_ip);
        mreqsrc.imr_interface.s_addr = mreq.imr_interface.s_addr;

        if (setsockopt(ch->mcast_fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                       &mreqsrc, sizeof(mreqsrc)) < 0) {
            LOG_WARN("IP_ADD_SOURCE_MEMBERSHIP failed (non-SSM?): %s", strerror(errno));
            /* 非致命错误，ASM模式可能不需要 */
        }
    }

    /* 设置TTL (出站) */
    int ttl = 8;
    setsockopt(ch->mcast_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    ch->joined = 1;
    ch->ref_count = 1;

    LOG_INFO("Joined multicast %s:%d on %s (IGMP report sent by kernel)",
             ch->addr, ch->port, g_iface);
    return 0;
}

/* 离开组播组 */
static void channel_leave(struct channel *ch) {
    if (!ch->joined) return;

    ch->ref_count--;
    if (ch->ref_count > 0) {
        LOG_DEBUG("Channel %s:%d ref_count=%d, keep alive",
                  ch->addr, ch->port, ch->ref_count);
        return;
    }

    /* 发送IGMP Leave (内核自动处理) */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ch->addr);
    mreq.imr_interface.s_addr = inet_addr("0.0.0.0");

    struct in_addr iface_ip;
    if (get_iface_ip(g_iface, &iface_ip) == 0) {
        mreq.imr_interface.s_addr = iface_ip.s_addr;
    }

    setsockopt(ch->mcast_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

    if (ch->mcast_fd >= 0) {
        close(ch->mcast_fd);
        ch->mcast_fd = -1;
    }

    ch->joined = 0;
    ch->ref_count = 0;

    LOG_INFO("Left multicast %s:%d (IGMP Leave sent by kernel)", ch->addr, ch->port);
}

/* 清理所有通道 */
static void channel_cleanup_all(void) {
    struct channel *ch = g_channels;
    while (ch) {
        struct channel *next = ch->next;
        if (ch->joined) channel_leave(ch);
        free(ch);
        ch = next;
    }
    g_channels = NULL;
}

/* ========== HTTP 响应 ========== */

static void send_response(int fd, int code, const char *content_type, const char *body, int body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        code,
        code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request"),
        content_type, body_len);

    write(fd, header, hlen);
    if (body && body_len > 0) write(fd, body, body_len);
}

static void send_200_stream(int fd) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Pragma: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n");
    write(fd, header, hlen);
}

/* ========== /status 页面 ========== */

static void handle_status(int client_fd) {
    char body[4096];
    int off = 0;

    off += snprintf(body + off, sizeof(body) - off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Multicast Proxy Status</title>"
        "<style>"
        "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:20px;}"
        "h1{color:#00ff88;}"
        "table{border-collapse:collapse;width:100%;margin:10px 0;}"
        "th,td{border:1px solid #444;padding:8px;text-align:left;}"
        "th{background:#16213e;color:#00ff88;}"
        "tr:nth-child(even){background:#0f3460;}"
        ".ok{color:#00ff88;} .err{color:#ff4444;}"
        "</style></head><body>"
        "<h1>Multicast Proxy</h1>"
        "<p>Upstream: <b>%s</b> | Port: <b>%d</b> | Max Clients: <b>%d</b></p>"
        "<p>SSM Source: <b>%s</b></p>"
        "<p>Total clients served: <b>%d</b></p>"
        "<h2>Active Channels</h2>"
        "<table><tr><th>Channel</th><th>Port</th><th>Listeners</th><th>Status</th></tr>",
        g_iface, g_listen_port, g_max_clients,
        g_ssm_source ? g_ssm_source_ip : "(ASM)",
        g_total_clients);

    struct channel *ch = g_channels;
    while (ch) {
        off += snprintf(body + off, sizeof(body) - off,
            "<tr><td>%s</td><td>%d</td><td>%d</td>"
            "<td class='%s'>%s</td></tr>",
            ch->addr, ch->port, ch->ref_count,
            ch->joined ? "ok" : "err",
            ch->joined ? "JOINED" : "IDLE");
        ch = ch->next;
    }

    off += snprintf(body + off, sizeof(body) - off,
        "</table>"
        "<h2>Usage</h2>"
        "<pre>"
        "Play: http://&lt;router_ip&gt;:%d/udp/232.0.1.210:1234\n"
        "Status: http://&lt;router_ip&gt;:%d/status\n"
        "Playlist: http://&lt;router_ip&gt;:%d/playlist\n"
        "</pre>"
        "</body></html>",
        g_listen_port, g_listen_port, g_listen_port);

    send_response(client_fd, 200, "text/html; charset=utf-8", body, off);
}

/* ========== /playlist 页面 ========== */

static void handle_playlist(int client_fd) {
    /* 默认频道表 - 可以从文件加载 */
    static const char *default_playlist =
        "#EXTM3U\n"
        "#EXTINF:-1 tvg-name=\"CCTV1\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV1.png\" group-name=\"央视频道\",CCTV-1 综合\n"
        "$ProxyURL/udp/232.0.1.210:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV2\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV2.png\" group-name=\"央视频道\",CCTV-2 财经\n"
        "$ProxyURL/udp/232.0.3.2:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV3\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV3.png\" group-name=\"央视频道\",CCTV-3 综艺\n"
        "$ProxyURL/udp/232.0.4.3:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV4\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV4.png\" group-name=\"央视频道\",CCTV-4 中文国际\n"
        "$ProxyURL/udp/232.0.5.4:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV5\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV5.png\" group-name=\"央视频道\",CCTV-5 体育\n"
        "$ProxyURL/udp/232.0.6.5:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV5+\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV5P.png\" group-name=\"央视频道\",CCTV-5+ 体育赛事\n"
        "$ProxyURL/udp/232.0.7.6:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV6\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV6.png\" group-name=\"央视频道\",CCTV-6 电影\n"
        "$ProxyURL/udp/232.0.8.7:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV7\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV7.png\" group-name=\"央视频道\",CCTV-7 国防军事\n"
        "$ProxyURL/udp/232.0.9.8:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV8\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV8.png\" group-name=\"央视频道\",CCTV-8 电视剧\n"
        "$ProxyURL/udp/232.0.10.9:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV9\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV9.png\" group-name=\"央视频道\",CCTV-9 纪录\n"
        "$ProxyURL/udp/232.0.11.10:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV10\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV10.png\" group-name=\"央视频道\",CCTV-10 科教\n"
        "$ProxyURL/udp/232.0.12.11:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV11\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV11.png\" group-name=\"央视频道\",CCTV-11 戏曲\n"
        "$ProxyURL/udp/232.0.13.12:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV12\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV12.png\" group-name=\"央视频道\",CCTV-12 社会与法\n"
        "$ProxyURL/udp/232.0.14.13:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV13\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV13.png\" group-name=\"央视频道\",CCTV-13 新闻\n"
        "$ProxyURL/udp/232.0.15.14:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV14\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV14.png\" group-name=\"央视频道\",CCTV-14 少儿\n"
        "$ProxyURL/udp/232.0.16.15:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV15\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV15.png\" group-name=\"央视频道\",CCTV-15 音乐\n"
        "$ProxyURL/udp/232.0.17.16:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV16\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV16.png\" group-name=\"央视频道\",CCTV-16 奥林匹克\n"
        "$ProxyURL/udp/232.0.18.17:1234\n"
        "#EXTINF:-1 tvg-name=\"CCTV17\" tvg-logo=\"https://epg.112114.xyz/logo/CCTV17.png\" group-name=\"央视频道\",CCTV-17 农业农村\n"
        "$ProxyURL/udp/232.0.19.18:1234\n";

    /* 尝试从文件加载频道列表 */
    char *playlist = NULL;
    FILE *f = fopen("/etc/multicast_proxy.m3u", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize > 0 && fsize < 1024*1024) {
            playlist = malloc(fsize + 1);
            if (playlist) {
                fread(playlist, 1, fsize, f);
                playlist[fsize] = 0;
            }
        }
        fclose(f);
    }

    const char *content = playlist ? playlist : default_playlist;
    int len = strlen(content);

    /* 替换 $ProxyURL 为实际地址 */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/x-mpegurl; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"iptv.m3u\"\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", len);

    write(client_fd, header, hlen);

    /* 逐块发送, 替换 $ProxyURL */
    const char *pos = content;
    const char *tag = "$ProxyURL";
    int tag_len = strlen(tag);
    char url_prefix[128];
    snprintf(url_prefix, sizeof(url_prefix), "http://%%s:%d", g_listen_port);

    /* 简单处理: 直接输出原始内容，客户端自行替换 */
    write(client_fd, content, len);

    free(playlist);
}

/* ========== 子进程: 独立join/leave + 转发组播数据到HTTP客户端 ========== */

/* 子进程独立加入组播 (内核自动IGMP) */
static int child_join_multicast(const char *addr, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int rcvbuf = g_rcvbuf;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = inet_addr(addr);

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("child bind(%s:%d) failed: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(addr);
    mreq.imr_interface.s_addr = inet_addr("0.0.0.0");

    struct in_addr iface_ip;
    if (get_iface_ip(g_iface, &iface_ip) == 0) {
        mreq.imr_interface.s_addr = iface_ip.s_addr;
    }

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        LOG_ERROR("child IP_ADD_MEMBERSHIP(%s) failed: %s", addr, strerror(errno));
        close(fd);
        return -1;
    }

    /* SSM源过滤 */
    if (g_ssm_source && g_ssm_source_ip[0]) {
        struct ip_mreq_source mreqsrc;
        mreqsrc.imr_multiaddr.s_addr = inet_addr(addr);
        mreqsrc.imr_sourceaddr.s_addr = inet_addr(g_ssm_source_ip);
        mreqsrc.imr_interface.s_addr = mreq.imr_interface.s_addr;
        setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &mreqsrc, sizeof(mreqsrc));
    }

    int ttl = 8;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    return fd;
}

/* 子进程离开组播 */
static void child_leave_multicast(int fd, const char *addr) {
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(addr);
    mreq.imr_interface.s_addr = inet_addr("0.0.0.0");

    struct in_addr iface_ip;
    if (get_iface_ip(g_iface, &iface_ip) == 0) {
        mreq.imr_interface.s_addr = iface_ip.s_addr;
    }

    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    close(fd);
}

/* 子进程: 转发组播数据到HTTP客户端 */
static void child_forward(int mcast_fd, int client_fd,
                          const char *mcast_addr, int mcast_port,
                          const char *client_ip, int client_port) {
    char buf[MCAST_BUF_SIZE];
    fd_set rfds;
    struct timeval tv;

    LOG_INFO("Child %d: streaming %s:%d -> %s:%d",
             getpid(), mcast_addr, mcast_port, client_ip, client_port);

    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(mcast_fd, &rfds);
        FD_SET(client_fd, &rfds);

        tv.tv_sec = g_timeout > 0 ? g_timeout : 5;
        tv.tv_usec = 0;

        int maxfd = (mcast_fd > client_fd) ? mcast_fd : client_fd;
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret == 0) {
            if (g_timeout > 0) {
                LOG_INFO("Child %d: timeout", getpid());
                break;
            }
            continue;
        }

        /* 组播数据到达 → 转发给HTTP客户端 */
        if (FD_ISSET(mcast_fd, &rfds)) {
            ssize_t n = recv(mcast_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                ssize_t sent = write(client_fd, buf, n);
                if (sent <= 0) break;
            } else if (n < 0 && errno != EINTR) {
                break;
            }
        }

        /* 检查客户端是否断开 */
        if (FD_ISSET(client_fd, &rfds)) {
            char tmp;
            ssize_t n = recv(client_fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n <= 0) break;
        }
    }

    LOG_INFO("Child %d: stream ended %s:%d -> %s:%d",
             getpid(), mcast_addr, mcast_port, client_ip, client_port);
}

/* ========== 处理HTTP客户端 ========== */

static void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char buf[BUF_SIZE];
    char client_ip[32];
    int client_port = ntohs(client_addr->sin_port);
    inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));

    /* 读取HTTP请求 */
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = 0;

    LOG_DEBUG("Request from %s:%d: %.80s", client_ip, client_port, buf);

    struct http_request req;
    parse_http_request(buf, &req);

    /* /status */
    if (req.is_status) {
        handle_status(client_fd);
        close(client_fd);
        return;
    }

    /* /playlist */
    if (req.is_playlist) {
        handle_playlist(client_fd);
        close(client_fd);
        return;
    }

    /* /udp/GROUP:PORT 或 /rtp/GROUP:PORT */
    if (req.is_multicast) {
        /* 检查客户端数量限制 */
        if (g_active_clients >= g_max_clients) {
            send_response(client_fd, 503, "text/plain", "Service Unavailable: max clients reached", 42);
            close(client_fd);
            return;
        }

        struct in_addr maddr;
        if (!inet_aton(req.multicast_addr, &maddr) || !IN_MULTICAST(ntohl(maddr.s_addr))) {
            send_response(client_fd, 400, "text/plain", "Bad Request: not a multicast address", 39);
            close(client_fd);
            return;
        }

        /* 发送HTTP 200流式响应头 */
        send_200_stream(client_fd);

        g_total_clients++;

        /* fork子进程: 独立join/leave + 转发 */
        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程: 独立加入组播 */
            int mcast_fd = child_join_multicast(req.multicast_addr, req.multicast_port);
            if (mcast_fd < 0) {
                const char *err = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 35\r\n\r\nMulticast join failed";
                write(client_fd, err, strlen(err));
                close(client_fd);
                _exit(1);
            }

            child_forward(mcast_fd, client_fd,
                         req.multicast_addr, req.multicast_port,
                         client_ip, client_port);

            /* 独立离开组播 (内核自动IGMP Leave) */
            child_leave_multicast(mcast_fd, req.multicast_addr);
            close(client_fd);
            _exit(0);
        } else if (pid > 0) {
            /* 父进程 */
            g_child_count++;
            g_active_clients++;
            close(client_fd);
        } else {
            LOG_ERROR("fork() failed: %s", strerror(errno));
            send_response(client_fd, 500, "text/plain", "Internal Server Error", 22);
            close(client_fd);
        }
        return;
    }

    /* 未知请求 */
    const char *help =
        "Multicast Proxy v1.0\n"
        "\n"
        "Usage:\n"
        "  /udp/GROUP:PORT  - Stream multicast via UDP\n"
        "  /rtp/GROUP:PORT  - Stream multicast via RTP\n"
        "  /status          - Show status page\n"
        "  /playlist        - Download M3U playlist\n"
        "\n"
        "Examples:\n"
        "  http://router:8888/udp/232.0.1.210:1234\n"
        "  http://router:8888/playlist\n"
        "\n";

    send_response(client_fd, 200, "text/plain; charset=utf-8", help, strlen(help));
    close(client_fd);
}

/* ========== 主程序 ========== */

static void print_usage(const char *prog) {
    printf(
        "Multicast Proxy v1.0 - All-in-One IPTV Proxy\n"
        "替代 igmpproxy + udpxy 的一体化方案\n\n"
        "用法: %s [选项]\n\n"
        "选项:\n"
        "  -p PORT      监听端口 (默认: %d)\n"
        "  -i IFACE     上游接口 (默认: %s)\n"
        "  -c MAX       最大客户端数 (默认: %d)\n"
        "  -t SEC       客户端超时, 0=无超时 (默认: %d)\n"
        "  -s SOURCE_IP SSM源地址 (默认: 无, 使用ASM)\n"
        "  -b SIZE      接收缓冲区大小 (默认: %d)\n"
        "  -d           守护进程模式\n"
        "  -v           详细日志\n"
        "  -h           帮助\n\n"
        "示例:\n"
        "  %s -p 8888 -i eth0.2\n"
        "  %s -p 8888 -i eth0.2 -s 10.253.18.18  (SSM模式)\n"
        "  %s -p 8888 -i eth0.2 -d -v  (守护进程+详细日志)\n\n"
        "UCI配置 (OpenWrt):\n"
        "  uci set multicast_proxy.@proxy[0].enabled=1\n"
        "  uci set multicast_proxy.@proxy[0].port=8888\n"
        "  uci set multicast_proxy.@proxy[0].upstream=eth0.2\n"
        "  uci commit multicast_proxy\n",
        prog, DEFAULT_LISTEN_PORT, "eth0.2", DEFAULT_MAX_CLIENTS,
        DEFAULT_TIMEOUT, DEFAULT_RCVBUF, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int opt;

    while ((opt = getopt(argc, argv, "p:i:c:t:s:b:dvh")) != -1) {
        switch (opt) {
            case 'p': g_listen_port = atoi(optarg); break;
            case 'i': strncpy(g_iface, optarg, sizeof(g_iface) - 1); break;
            case 'c': g_max_clients = atoi(optarg); break;
            case 't': g_timeout = atoi(optarg); break;
            case 's':
                strncpy(g_ssm_source_ip, optarg, sizeof(g_ssm_source_ip) - 1);
                g_ssm_source = 1;
                break;
            case 'b': g_rcvbuf = atoi(optarg); break;
            case 'd': g_daemon = 1; break;
            case 'v': g_verbose = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* 信号处理 */
    signal(SIGCHLD, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    LOG_INFO("Multicast Proxy v1.0 starting...");
    LOG_INFO("Listen port: %d, Upstream: %s, Max clients: %d",
             g_listen_port, g_iface, g_max_clients);
    if (g_ssm_source) {
        LOG_INFO("SSM mode, source: %s", g_ssm_source_ip);
    }

    /* 检查上游接口 */
    if (check_iface(g_iface) < 0) {
        LOG_WARN("Upstream interface %s not ready, will retry", g_iface);
    }

    /* 守护进程模式 */
    if (g_daemon) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            printf("multicast_proxy started, PID=%d\n", pid);
            return 0;
        }
        setsid();
        chdir("/");
        /* 重定向标准IO */
        close(0);
        close(1);
        close(2);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
    }

    /* 创建监听socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return 1;
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(g_listen_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        LOG_ERROR("bind(%d) failed: %s", g_listen_port, strerror(errno));
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 16) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(listen_fd);
        return 1;
    }

    LOG_INFO("Listening on port %d", g_listen_port);
    LOG_INFO("Play: http://<router_ip>:%d/udp/GROUP:PORT", g_listen_port);

    /* 主循环 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;
            LOG_ERROR("accept() failed: %s", strerror(errno));
            continue;
        }

        handle_client(client_fd, &client_addr);
    }

    /* 清理 */
    LOG_INFO("Shutting down...");
    close(listen_fd);
    channel_cleanup_all();

    /* 等待所有子进程 */
    while (g_child_count > 0) {
        usleep(100000);
    }

    LOG_INFO("Multicast Proxy stopped");
    return 0;
}
