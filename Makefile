# Makefile for multicast_proxy
# 支持本地编译和OpenWrt交叉编译

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Werror
LDFLAGS ?= -pthread

# 目标
TARGET = multicast_proxy

# 源文件
SRCS = multicast_proxy.c
OBJS = $(SRCS:.c=.o)

# 安装路径
DESTDIR ?=
PREFIX ?= /usr
BINDIR = $(DESTDIR)$(PREFIX)/bin

# OpenWrt 交叉编译 (在OpenWrt SDK目录下执行)
# make CROSS_COMPILE=mipsel-openwrt-linux-musl-
CROSS_COMPILE ?=

ifdef CROSS_COMPILE
    CC = $(CROSS_COMPILE)gcc
    STRIP = $(CROSS_COMPILE)strip
else
    STRIP = strip
endif

.PHONY: all clean install uninstall static

all: $(TARGET)

static: LDFLAGS += -static
static: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Build complete: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed to $(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(BINDIR)/$(TARGET)

# 交叉编译示例 (需要OpenWrt SDK)
# make CROSS_COMPILE=/path/to/openwrt/staging_dir/toolchain-mipsel_24kc_gcc-5.4.0_musl-1.1.16/bin/mipsel-openwrt-linux-musl-
# make CROSS_COMPILE=/path/to/openwrt/staging_dir/toolchain-mipsel_24kc_gcc-5.4.0_musl-1.1.16/bin/mipsel-openwrt-linux-musl- strip

# 在路由器上本地编译
# make CC=gcc

# 打包
package: $(TARGET)
	$(STRIP) $(TARGET)
	tar czf multicast_proxy.tar.gz $(TARGET) multicast_proxy.init multicast_proxy.uci
	@echo "Package: multicast_proxy.tar.gz"
