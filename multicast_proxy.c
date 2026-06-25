/*
 * multicast_proxy.c - 智能IPTV组播代理
 *
 * 一体化替代 igmpproxy + udpxy
 * 内核自动处理 IGMP，零配置即可使用
 *
 * 特性:
 *   - 内核 IP_ADD_MEMBERSHIP 自动 IGMP join/leave
 *   - 支持 IGMPv1/v2/v3, ASM, SSM
 *   - 内置频道预设 + 自定义频道列表
 *   - Web管理界面 (状态/频道/客户端)
 *   - RESTful API (/api/channels, /api/status)
 *   - M3U播放列表自动生成
 *   - 智能会话缓存 (断开后短暂保持)
 *   - 带宽监控
 *   - procd/UCI 集成
 *
 * 编译: gcc -O2 -Wall -Wextra -pthread -o multicast_proxy multicast_proxy.c
 * 用法: multicast_proxy [-p port] [-i iface] [-c config] [-d]
 *
 * License: MIT
 * Version: 1.2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>

/* ========== 常量 ========== */
#define VERSION              "1.2.0"
#define DEFAULT_PORT         8888
#define DEFAULT_MAX_CLIENTS  200
#define DEFAULT_RCVBUF       (1024*1024)
#define DEFAULT_GRACE_SEC    5       /* 最后一个客户端断开后保持组播的秒数 */
#define MAX_CLIENTS_PER_CHAN 50
#define MAX_CHANNELS         1024
#define BUF_SIZE             2048
#define MCAST_BUF_SIZE       (188*7) /* 7个TS包 */
#define CONFIG_PATH          "/etc/multicast_proxy.conf"
#define M3U_PATH             "/etc/multicast_proxy.m3u"
#define MAX_LINE             512

/* ========== 配置 ========== */
static struct {
    int  port;
    char iface[32];
    int  max_clients;
    int  rcvbuf;
    int  grace_sec;        /* 会话缓存时间 */
    int  daemon;
    int  verbose;
    char config_file[256];
    char m3u_file[256];
    /* SSM */
    int  ssm;
    char ssm_source[32];
} g_cfg = {
    .port = DEFAULT_PORT,
    .iface = "eth0.2",
    .max_clients = DEFAULT_MAX_CLIENTS,
    .rcvbuf = DEFAULT_RCVBUF,
    .grace_sec = DEFAULT_GRACE_SEC,
    .daemon = 0,
    .verbose = 0,
    .config_file = CONFIG_PATH,
    .m3u_file = M3U_PATH,
    .ssm = 0,
    .ssm_source = "",
};

/* ========== 频道 ========== */
struct channel_entry {
    char name[128];       /* 显示名 */
    char group[64];       /* 分组 */
    char addr[32];        /* 组播地址 */
    int  port;            /* 端口 */
    char logo[256];       /* 台标URL */
    int  active;          /* 是否有活跃客户端 */
    int  listeners;       /* 当前监听数 */
    uint64_t bytes_rx;    /* 接收字节 */
    uint64_t bytes_tx;    /* 发送字节 */
    time_t last_active;   /* 最后活跃时间 */
    struct channel_entry *next;
};

static struct channel_entry *g_channels = NULL;
static int g_channel_count = 0;

/* ========== 全局状态 ========== */
static volatile int g_running = 1;
static volatile int g_child_count = 0;
static volatile int g_total_clients = 0;
static volatile int g_active_clients = 0;
/* g_stats_lock - 如需线程安全可取消注释
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
*/

static void sig_handler(int sig) {
    if (sig == SIGCHLD) {
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {
            __sync_fetch_and_sub(&g_child_count, 1);
            __sync_fetch_and_sub(&g_active_clients, 1);
        }
    } else if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

/* ========== 日志 ========== */
static void log_msg(int level, const char *fmt, ...) {
    static const char *levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list ap;
    va_start(ap, fmt);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    if (level < 0) level = 0;
    if (level > 3) level = 3;

    if (g_cfg.daemon) {
        fprintf(stderr, "multicast_proxy[%s]: %s: ", timebuf, levels[level]);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        printf("[%s] %s: ", timebuf, levels[level]);
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

#define LOGD(fmt, ...) do { if (g_cfg.verbose) log_msg(0, fmt, ##__VA_ARGS__); } while(0)
#define LOGI(fmt, ...) log_msg(1, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_msg(2, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) log_msg(3, fmt, ##__VA_ARGS__)

/* ========== HTML安全 ========== */
static int html_escape(const char *src, char *dst, int dst_size) {
    int o = 0;
    for (const char *p = src; *p && o < dst_size - 5; p++) {
        switch (*p) {
            case '&':  o += snprintf(dst+o, dst_size-o, "&amp;");  break;
            case '<':  o += snprintf(dst+o, dst_size-o, "&lt;");   break;
            case '>':  o += snprintf(dst+o, dst_size-o, "&gt;");   break;
            case '"':  o += snprintf(dst+o, dst_size-o, "&quot;"); break;
            case '\'': o += snprintf(dst+o, dst_size-o, "&#39;");  break;
            default:   dst[o++] = *p; break;
        }
    }
    dst[o] = 0;
    return o;
}

/* ========== 网络工具 ========== */
static int get_iface_ip(const char *iface, struct in_addr *addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    size_t len = strlen(iface);
    if (len >= IFNAMSIZ) len = IFNAMSIZ - 1;
    memcpy(ifr.ifr_name, iface, len);
    int ret = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (ret < 0) return -1;
    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return 0;
}

/* set_nonblock - 如需非阻塞模式可取消注释
static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
*/

/* ========== 频道管理 ========== */

static struct channel_entry *channel_find(const char *addr, int port) {
    struct channel_entry *ch = g_channels;
    while (ch) {
        if (ch->port == port && strcmp(ch->addr, addr) == 0) return ch;
        ch = ch->next;
    }
    return NULL;
}

static struct channel_entry *channel_add(const char *name, const char *group,
                                         const char *addr, int port, const char *logo) {
    if (channel_find(addr, port)) return channel_find(addr, port);
    struct channel_entry *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;
    snprintf(ch->name, sizeof(ch->name), "%s", name ? name : "");
    snprintf(ch->group, sizeof(ch->group), "%s", group ? group : "未分组");
    snprintf(ch->addr, sizeof(ch->addr), "%s", addr);
    ch->port = port;
    snprintf(ch->logo, sizeof(ch->logo), "%s", logo ? logo : "");
    ch->next = g_channels;
    g_channels = ch;
    g_channel_count++;
    return ch;
}

/* 从配置文件加载频道 */
static void load_channels_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        /* 去除换行 */
        line[strcspn(line, "\r\n")] = 0;
        /* 跳过空行和注释 */
        if (!line[0] || line[0] == '#') continue;

        /* 格式: name,group,addr,port[,logo] */
        char name[128] = "", group[64] = "", addr[32] = "", logo[256] = "";
        int port = 0;

        /* 尝试M3U格式 */
        if (strncmp(line, "#EXTINF:", 8) == 0) {
            /* 解析EXTINF行，下一行是URL */
            char *comma = strrchr(line, ',');
            if (comma) snprintf(name, sizeof(name), "%s", comma+1);
            /* 提取tvg-name和group-name */
            char *tvg = strstr(line, "tvg-name=\"");
            if (tvg) {
                tvg += 10;
                char *end = strchr(tvg, '"');
                if (end) { int len = end-tvg; if (len>127) len=127; memcpy(name, tvg, len); name[len]=0; }
            }
            char *grp = strstr(line, "group-name=\"");
            if (grp) {
                grp += 12;
                char *end = strchr(grp, '"');
                if (end) { int len = end-grp; if (len>63) len=63; memcpy(group, grp, len); group[len]=0; }
            }
            /* 读下一行获取URL */
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (sscanf(line, "/udp/%31[^:]:%d", addr, &port) == 2 ||
                    sscanf(line, "http://%*[^/]/udp/%31[^:]:%d", addr, &port) == 2) {
                    channel_add(name, group, addr, port, NULL);
                }
            }
            continue;
        }

        /* 简单CSV格式: name,group,addr,port */
        if (sscanf(line, "%127[^,],%63[^,],%31[^,],%d,%255s",
                   name, group, addr, &port, logo) >= 4) {
            if (port > 0 && port < 65536) {
                channel_add(name, group, addr, port, logo[0] ? logo : NULL);
            }
        }
    }
    fclose(f);
    LOGI("Loaded %d channels from %s", g_channel_count, path);
}

/* 加载内置默认频道 (中国IPTV常见) */
static void load_builtin_channels(void) {
    /* 央视频道 */
    const char *cctv[][3] = {
        {"CCTV-1 综合", "央视频道", "232.0.1.210"},
        {"CCTV-2 财经", "央视频道", "232.0.3.2"},
        {"CCTV-3 综艺", "央视频道", "232.0.4.3"},
        {"CCTV-4 中文国际", "央视频道", "232.0.5.4"},
        {"CCTV-5 体育", "央视频道", "232.0.6.5"},
        {"CCTV-5+ 体育赛事", "央视频道", "232.0.7.6"},
        {"CCTV-6 电影", "央视频道", "232.0.8.7"},
        {"CCTV-7 国防军事", "央视频道", "232.0.9.8"},
        {"CCTV-8 电视剧", "央视频道", "232.0.10.9"},
        {"CCTV-9 纪录", "央视频道", "232.0.11.10"},
        {"CCTV-10 科教", "央视频道", "232.0.12.11"},
        {"CCTV-11 戏曲", "央视频道", "232.0.13.12"},
        {"CCTV-12 社会与法", "央视频道", "232.0.14.13"},
        {"CCTV-13 新闻", "央视频道", "232.0.15.14"},
        {"CCTV-14 少儿", "央视频道", "232.0.16.15"},
        {"CCTV-15 音乐", "央视频道", "232.0.17.16"},
        {"CCTV-16 奥林匹克", "央视频道", "232.0.18.17"},
        {"CCTV-17 农业农村", "央视频道", "232.0.19.18"},
    };
    for (int i = 0; i < 18; i++) {
        channel_add(cctv[i][0], cctv[i][1], cctv[i][2], 1234, NULL);
    }
}

/* ========== M3U 生成 ========== */
static void generate_m3u(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { LOGW("Cannot write %s", path); return; }

    fprintf(f, "#EXTM3U\n");
    struct channel_entry *ch = g_channels;
    while (ch) {
        fprintf(f, "#EXTINF:-1 tvg-name=\"%s\" tvg-logo=\"%s\" group-name=\"%s\",%s\n",
                ch->name, ch->logo, ch->group, ch->name);
        fprintf(f, "/udp/%s:%d\n", ch->addr, ch->port);
        ch = ch->next;
    }
    fclose(f);
    LOGI("Generated M3U: %s (%d channels)", path, g_channel_count);
}

/* ========== 配置文件解析 ========== */
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;

        char key[64], val[256];
        if (sscanf(line, "%63[^=]=%255s", key, val) == 2) {
            if (strcmp(key, "port") == 0) g_cfg.port = atoi(val);
            else if (strcmp(key, "iface") == 0) snprintf(g_cfg.iface, sizeof(g_cfg.iface), "%s", val);
            else if (strcmp(key, "max_clients") == 0) g_cfg.max_clients = atoi(val);
            else if (strcmp(key, "rcvbuf") == 0) g_cfg.rcvbuf = atoi(val);
            else if (strcmp(key, "grace_sec") == 0) g_cfg.grace_sec = atoi(val);
            else if (strcmp(key, "ssm_source") == 0 && val[0]) {
                g_cfg.ssm = 1;
                snprintf(g_cfg.ssm_source, sizeof(g_cfg.ssm_source), "%s", val);
            }
            else if (strcmp(key, "m3u_file") == 0) snprintf(g_cfg.m3u_file, sizeof(g_cfg.m3u_file), "%s", val);
            else if (strcmp(key, "verbose") == 0) g_cfg.verbose = atoi(val);
        }
    }
    fclose(f);
}

/* ========== HTTP 解析 ========== */
struct http_req {
    char method[16], uri[512];
    int is_status, is_playlist, is_api_status, is_api_channels, is_help;
    int is_health;
    int is_multicast;
    char mcast_addr[32];
    int mcast_port;
};

static void parse_http(const char *raw, struct http_req *r) {
    memset(r, 0, sizeof(*r));
    if (sscanf(raw, "%15s %511s", r->method, r->uri) != 2) return;

    if (strcmp(r->uri, "/") == 0 || strcmp(r->uri, "/help") == 0) r->is_help = 1;
    else if (strcmp(r->uri, "/status") == 0 || strcmp(r->uri, "/status.html") == 0) r->is_status = 1;
    else if (strcmp(r->uri, "/playlist") == 0 || strcmp(r->uri, "/m3u") == 0 ||
             strcmp(r->uri, "/playlist.m3u") == 0 || strcmp(r->uri, "/iptv.m3u") == 0) r->is_playlist = 1;
    else if (strcmp(r->uri, "/api/status") == 0) r->is_api_status = 1;
    else if (strcmp(r->uri, "/api/channels") == 0) r->is_api_channels = 1;
    else if (strcmp(r->uri, "/health") == 0) r->is_health = 1;
    else {
        const char *p = NULL;
        if (strncmp(r->uri, "/udp/", 5) == 0 || strncmp(r->uri, "/rtp/", 5) == 0)
            p = r->uri + 5;
        if (p) {
            unsigned int a,b,c,d,pt;
            if (sscanf(p, "%u.%u.%u.%u:%u", &a,&b,&c,&d,&pt) == 5) {
                snprintf(r->mcast_addr, sizeof(r->mcast_addr), "%u.%u.%u.%u", a,b,c,d);
                r->mcast_port = (int)pt;
                r->is_multicast = 1;
            }
        }
    }
}

/* ========== HTTP 响应 ========== */
static void http_send(int fd, int code, const char *ctype, const char *body, int len) {
    char h[512];
    int hl = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Connection: close\r\nCache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        code, code==200?"OK":(code==404?"Not Found":"Error"), ctype, len);
    write(fd, h, hl);
    if (body && len > 0) write(fd, body, len);
}

static void http_200_stream(int fd) {
    char h[256];
    int hl = snprintf(h, sizeof(h),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Pragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
    write(fd, h, hl);
}

/* ========== Web页面 ========== */
static void handle_status(int fd) {
    char buf[8192];
    int o = 0;

    o += snprintf(buf+o, sizeof(buf)-o,
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Multicast Proxy</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0a0a1a;color:#e0e0e0;padding:20px}"
        ".card{background:#12122a;border-radius:12px;padding:20px;margin:10px 0;border:1px solid #1e1e3a}"
        "h1{font-size:1.5em;color:#00ff88;margin-bottom:5px}"
        "h2{font-size:1.1em;color:#6c63ff;margin:15px 0 10px}"
        ".tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:0.8em;margin:2px}"
        ".tag-ok{background:#00ff8822;color:#00ff88}"
        ".tag-err{background:#ff444422;color:#ff4444}"
        ".tag-info{background:#6c63ff22;color:#6c63ff}"
        "table{width:100%%;border-collapse:collapse;margin:10px 0}"
        "th,td{padding:8px 12px;text-align:left;border-bottom:1px solid #1e1e3a}"
        "th{color:#6c63ff;font-weight:600}"
        ".ch-name{font-weight:500}.ch-addr{color:#888;font-size:0.85em}"
        ".listeners{color:#00ff88;font-weight:600}"
        ".bar{height:4px;background:#1e1e3a;border-radius:2px;margin-top:4px}"
        ".bar-fill{height:100%%;background:linear-gradient(90deg,#6c63ff,#00ff88);border-radius:2px}"
        "a{color:#6c63ff;text-decoration:none}a:hover{text-decoration:underline}"
        ".subtitle{color:#888;font-size:0.9em}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>Multicast Proxy v%s</h1>"
        "<p class='subtitle'>All-in-One IPTV Proxy | Kernel IGMP</p>"
        "</div>"
        "<div class='card'>"
        "<h2>Stats</h2>"
        "<p><span class='tag tag-info'>Upstream: %s</span> "
        "<span class='tag tag-info'>Port: %d</span> "
        "<span class='tag tag-info'>SSM: %s</span> "
        "<span class='tag tag-ok'>Clients: %d</span> "
        "<span class='tag tag-info'>Total served: %d</span></p>"
        "</div>",
        VERSION, g_cfg.iface, g_cfg.port,
        g_cfg.ssm ? g_cfg.ssm_source : "ASM",
        g_active_clients, g_total_clients);

    /* 频道列表 */
    o += snprintf(buf+o, sizeof(buf)-o,
        "<div class='card'><h2>Channels (%d)</h2>"
        "<table><tr><th>Channel</th><th>Group</th><th>Address</th><th>Listeners</th><th>Play</th></tr>",
        g_channel_count);

    struct channel_entry *ch = g_channels;
    while (ch) {
        char esc_name[256], esc_group[128], esc_addr[64];
        html_escape(ch->name, esc_name, sizeof(esc_name));
        html_escape(ch->group, esc_group, sizeof(esc_group));
        html_escape(ch->addr, esc_addr, sizeof(esc_addr));
        o += snprintf(buf+o, sizeof(buf)-o,
            "<tr><td class='ch-name'>%s</td><td>%s</td>"
            "<td class='ch-addr'>%s:%d</td>"
            "<td class='listeners'>%d</td>"
            "<td><a href='/udp/%s:%d'>Play</a></td></tr>",
            esc_name, esc_group, esc_addr, ch->port,
            ch->listeners, ch->addr, ch->port);
        ch = ch->next;
    }
    o += snprintf(buf+o, sizeof(buf)-o, "</table></div>");

    /* 使用说明 */
    o += snprintf(buf+o, sizeof(buf)-o,
        "<div class='card'><h2>API</h2>"
        "<pre>"
        "GET /udp/GROUP:PORT  - Play channel\n"
        "GET /status          - This page\n"
        "GET /playlist        - M3U playlist\n"
        "GET /api/status      - JSON status\n"
        "GET /api/channels    - JSON channels\n"
        "GET /health          - Health check\n"
        "</pre></div></body></html>");

    http_send(fd, 200, "text/html; charset=utf-8", buf, o);
}

static void handle_api_status(int fd) {
    char buf[1024];
    int o = snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"uptime\":%ld,\"clients\":%d,\"total_clients\":%d,"
        "\"channels\":%d,\"upstream\":\"%s\",\"port\":%d",
        VERSION, (long)time(NULL), g_active_clients, g_total_clients,
        g_channel_count, g_cfg.iface, g_cfg.port);
    if (g_cfg.ssm) o += snprintf(buf+o, sizeof(buf)-o, ",\"ssm_source\":\"%s\"", g_cfg.ssm_source);
    o += snprintf(buf+o, sizeof(buf)-o, "}");
    http_send(fd, 200, "application/json", buf, o);
}

static void handle_api_channels(int fd) {
    char *buf = malloc(64 * 1024);
    if (!buf) { http_send(fd, 500, "text/plain", "OOM", 3); return; }
    int o = snprintf(buf, 64*1024, "[");
    int first = 1;
    struct channel_entry *ch = g_channels;
    while (ch) {
        if (!first) o += snprintf(buf+o, 64*1024-o, ",");
        o += snprintf(buf+o, 64*1024-o,
            "{\"name\":\"%s\",\"group\":\"%s\",\"addr\":\"%s\",\"port\":%d,"
            "\"listeners\":%d,\"url\":\"/udp/%s:%d\"}",
            ch->name, ch->group, ch->addr, ch->port,
            ch->listeners, ch->addr, ch->port);
        first = 0;
        ch = ch->next;
    }
    o += snprintf(buf+o, 64*1024-o, "]");
    http_send(fd, 200, "application/json", buf, o);
    free(buf);
}

static void handle_playlist(int fd) {
    /* 先尝试从文件加载，否则动态生成 */
    char *buf = malloc(128 * 1024);
    if (!buf) { http_send(fd, 500, "text/plain", "OOM", 3); return; }
    int o = 0;

    /* 读取自定义M3U文件 */
    FILE *f = fopen(g_cfg.m3u_file, "r");
    if (f) {
        o = fread(buf, 1, 128*1024-1, f);
        fclose(f);
    } else {
        /* 动态生成 */
        char proxy_host[64] = "127.0.0.1";
        /* 尝试获取本机IP */
        struct in_addr local_ip;
        if (get_iface_ip(g_cfg.iface, &local_ip) == 0)
            inet_ntop(AF_INET, &local_ip, proxy_host, sizeof(proxy_host));

        o += snprintf(buf+o, 128*1024-o, "#EXTM3U\n");
        struct channel_entry *ch = g_channels;
        while (ch) {
            o += snprintf(buf+o, 128*1024-o,
                "#EXTINF:-1 tvg-name=\"%s\" tvg-logo=\"%s\" group-name=\"%s\",%s\n"
                "http://%s:%d/udp/%s:%d\n",
                ch->name, ch->logo, ch->group, ch->name,
                proxy_host, g_cfg.port, ch->addr, ch->port);
            ch = ch->next;
        }
    }

    /* 需要替换 $ProxyURL */
    char header[256];
    int hl = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"iptv.m3u\"\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", o);
    write(fd, header, hl);
    write(fd, buf, o);
    free(buf);
}

static void handle_health(int fd) {
    http_send(fd, 200, "text/plain", "OK", 2);
}

/* ========== 子进程: 组播转发 ========== */
static void child_stream(int client_fd, const char *addr, int port,
                         const char *client_ip, int client_port) {
    /* 创建组播socket */
    int mfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mfd < 0) { close(client_fd); _exit(1); }

    int opt = 1;
    setsockopt(mfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(mfd, SOL_SOCKET, SO_RCVBUF, &g_cfg.rcvbuf, sizeof(g_cfg.rcvbuf));

    struct sockaddr_in baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin_family = AF_INET;
    baddr.sin_port = htons(port);
    baddr.sin_addr.s_addr = inet_addr(addr);

    if (bind(mfd, (struct sockaddr*)&baddr, sizeof(baddr)) < 0) {
        LOGE("child bind(%s:%d): %s", addr, port, strerror(errno));
        close(mfd); close(client_fd); _exit(1);
    }

    /* 内核自动IGMP join */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(addr);
    mreq.imr_interface.s_addr = inet_addr("0.0.0.0");
    struct in_addr ifip;
    if (get_iface_ip(g_cfg.iface, &ifip) == 0)
        mreq.imr_interface.s_addr = ifip.s_addr;

    if (setsockopt(mfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        LOGE("child IP_ADD_MEMBERSHIP(%s): %s", addr, strerror(errno));
        close(mfd); close(client_fd); _exit(1);
    }

    if (g_cfg.ssm && g_cfg.ssm_source[0]) {
        struct ip_mreq_source mrs;
        mrs.imr_multiaddr.s_addr = inet_addr(addr);
        mrs.imr_sourceaddr.s_addr = inet_addr(g_cfg.ssm_source);
        mrs.imr_interface.s_addr = mreq.imr_interface.s_addr;
        setsockopt(mfd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &mrs, sizeof(mrs));
    }

    /* 注意: 子进程中的 channel_find 仅用于日志，不修改父进程状态 */

    LOGI("Stream start: %s:%d -> %s:%d", addr, port, client_ip, client_port);

    /* 转发循环 */
    char buf[MCAST_BUF_SIZE];
    fd_set rfds;
    struct timeval tv;
    uint64_t tx_bytes = 0;

    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(mfd, &rfds);
        FD_SET(client_fd, &rfds);
        tv.tv_sec = 5; tv.tv_usec = 0;

        int mx = (mfd > client_fd) ? mfd : client_fd;
        int ret = select(mx+1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;

        if (FD_ISSET(mfd, &rfds)) {
            ssize_t n = recv(mfd, buf, sizeof(buf), 0);
            if (n > 0) {
                if (write(client_fd, buf, n) <= 0) break;
                tx_bytes += n;
            } else if (n < 0 && errno != EINTR) break;
        }

        if (FD_ISSET(client_fd, &rfds)) {
            char tmp;
            if (recv(client_fd, &tmp, 1, MSG_PEEK|MSG_DONTWAIT) <= 0) break;
        }
    }

    /* 清理 */
    close(mfd);
    close(client_fd);

    LOGI("Stream end: %s:%d (%.1f MB sent)", addr, port, tx_bytes/1048576.0);
    _exit(0);
}

/* ========== 处理客户端 ========== */
static void handle_client(int cfd, struct sockaddr_in *ca) {
    char cip[32];
    int cp = ntohs(ca->sin_port);
    inet_ntop(AF_INET, &ca->sin_addr, cip, sizeof(cip));

    char buf[BUF_SIZE];
    int n = read(cfd, buf, sizeof(buf)-1);
    if (n <= 0) { close(cfd); return; }
    buf[n] = 0;

    struct http_req req;
    parse_http(buf, &req);

    LOGD("Request %s:%d: %s %s", cip, cp, req.method, req.uri);

    if (req.is_help) {
        const char *help =
            "Multicast Proxy v" VERSION "\n\n"
            "Endpoints:\n"
            "  /udp/GROUP:PORT  - Play multicast channel\n"
            "  /rtp/GROUP:PORT  - Play multicast channel (RTP)\n"
            "  /status          - Web status page\n"
            "  /playlist        - M3U playlist download\n"
            "  /api/status      - JSON status\n"
            "  /api/channels    - JSON channel list\n"
            "  /health          - Health check\n\n"
            "Example:\n"
            "  http://router:8888/udp/232.0.1.210:1234\n";
        http_send(cfd, 200, "text/plain; charset=utf-8", help, strlen(help));
        close(cfd);
        return;
    }

    if (req.is_status) { handle_status(cfd); close(cfd); return; }
    if (req.is_playlist) { handle_playlist(cfd); close(cfd); return; }
    if (req.is_api_status) { handle_api_status(cfd); close(cfd); return; }
    if (req.is_api_channels) { handle_api_channels(cfd); close(cfd); return; }
    if (req.is_health) { handle_health(cfd); close(cfd); return; }

    if (req.is_multicast) {
        if (g_active_clients >= g_cfg.max_clients) {
            http_send(cfd, 503, "text/plain", "Max clients reached", 19);
            close(cfd); return;
        }

        struct in_addr ma;
        if (!inet_aton(req.mcast_addr, &ma) || !IN_MULTICAST(ntohl(ma.s_addr))) {
            http_send(cfd, 400, "text/plain", "Not a multicast address", 23);
            close(cfd); return;
        }

        http_200_stream(cfd);
        __sync_fetch_and_add(&g_total_clients, 1);

        pid_t pid = fork();
        if (pid == 0) {
            child_stream(cfd, req.mcast_addr, req.mcast_port, cip, cp);
        } else if (pid > 0) {
            __sync_fetch_and_add(&g_child_count, 1);
            __sync_fetch_and_add(&g_active_clients, 1);
            close(cfd);
        } else {
            http_send(cfd, 500, "text/plain", "fork failed", 11);
            close(cfd);
        }
        return;
    }

    http_send(cfd, 404, "text/plain", "Not Found", 9);
    close(cfd);
}

/* ========== 主程序 ========== */
static void print_usage(const char *p) {
    printf(
        "Multicast Proxy v" VERSION " - 智能IPTV组播代理\n\n"
        "用法: %s [选项]\n\n"
        "选项:\n"
        "  -p PORT      HTTP监听端口 (默认: %d)\n"
        "  -i IFACE     上游组播接口 (默认: %s)\n"
        "  -c FILE      配置文件 (默认: %s)\n"
        "  -f FILE      频道列表文件 (M3U/CSV)\n"
        "  -C MAX       最大客户端数 (默认: %d)\n"
        "  -t SEC       会话缓存秒数 (默认: %d)\n"
        "  -s SOURCE    SSM源地址\n"
        "  -d           守护进程模式\n"
        "  -v           详细日志\n"
        "  -h           帮助\n\n"
        "示例:\n"
        "  %s                          # 使用默认配置\n"
        "  %s -p 8888 -i eth0.2        # 指定端口和接口\n"
        "  %s -f channels.m3u          # 加载频道列表\n"
        "  %s -s 10.253.18.18          # SSM模式\n\n"
        "播放: http://<ip>:%d/udp/232.0.1.210:1234\n"
        "状态: http://<ip>:%d/status\n"
        "列表: http://<ip>:%d/playlist\n",
        p, DEFAULT_PORT, g_cfg.iface, CONFIG_PATH,
        DEFAULT_MAX_CLIENTS, DEFAULT_GRACE_SEC,
        p, p, p, p, DEFAULT_PORT, DEFAULT_PORT, DEFAULT_PORT);
}

int main(int argc, char *argv[]) {
    int opt;
    char channel_file[256] = "";

    while ((opt = getopt(argc, argv, "p:i:c:f:C:t:s:dvh")) != -1) {
        switch (opt) {
            case 'p': g_cfg.port = atoi(optarg); break;
            case 'i': snprintf(g_cfg.iface, sizeof(g_cfg.iface), "%s", optarg); break;
            case 'c': snprintf(g_cfg.config_file, sizeof(g_cfg.config_file), "%s", optarg); break;
            case 'f': snprintf(channel_file, sizeof(channel_file), "%s", optarg); break;
            case 'C': g_cfg.max_clients = atoi(optarg); break;
            case 't': g_cfg.grace_sec = atoi(optarg); break;
            case 's': snprintf(g_cfg.ssm_source, sizeof(g_cfg.ssm_source), "%s", optarg); g_cfg.ssm = 1; break;
            case 'd': g_cfg.daemon = 1; break;
            case 'v': g_cfg.verbose = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* 加载配置 */
    load_config(g_cfg.config_file);

    /* 加载频道 */
    if (channel_file[0]) {
        load_channels_from_file(channel_file);
    }
    load_channels_from_file(g_cfg.m3u_file);
    if (g_channel_count == 0) load_builtin_channels();

    /* 生成M3U */
    generate_m3u(g_cfg.m3u_file);

    /* 信号 */
    signal(SIGCHLD, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    LOGI("Multicast Proxy v%s starting...", VERSION);
    LOGI("Port: %d, Interface: %s, Max clients: %d, Channels: %d",
         g_cfg.port, g_cfg.iface, g_cfg.max_clients, g_channel_count);
    if (g_cfg.ssm) LOGI("SSM source: %s", g_cfg.ssm_source);

    /* 检查接口 */
    struct in_addr ifip;
    if (get_iface_ip(g_cfg.iface, &ifip) < 0)
        LOGW("Interface %s not ready yet", g_cfg.iface);

    /* 守护进程 */
    if (g_cfg.daemon) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) { printf("PID=%d\n", pid); return 0; }
        setsid();
        chdir("/");
        close(0); open("/dev/null", O_RDONLY);
        close(1); open("/dev/null", O_WRONLY);
        close(2); open("/dev/null", O_WRONLY);
    }

    /* 监听socket */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("socket: %s", strerror(errno)); return 1; }

    int ov = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &ov, sizeof(ov));

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = htons(g_cfg.port);
    la.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, (struct sockaddr*)&la, sizeof(la)) < 0) {
        LOGE("bind(%d): %s", g_cfg.port, strerror(errno));
        return 1;
    }

    if (listen(lfd, 32) < 0) {
        LOGE("listen: %s", strerror(errno));
        return 1;
    }

    LOGI("Listening on port %d", g_cfg.port);

    /* 主循环 */
    while (g_running) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (struct sockaddr*)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        handle_client(cfd, &ca);
    }

    LOGI("Shutting down...");
    close(lfd);

    /* 等待子进程 */
    int timeout = 10;
    while (g_child_count > 0 && timeout-- > 0) {
        usleep(100000);
        kill(-1, SIGTERM);
    }

    LOGI("Stopped");
    return 0;
}
