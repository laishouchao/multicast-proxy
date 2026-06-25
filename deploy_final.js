/**
 * deploy_final.js - 部署 multicast_proxy 到 OpenWrt 路由器
 *
 * 步骤:
 *   1. 上传 multicast_proxy.c 到路由器
 *   2. 在路由器上用gcc编译 (避免交叉编译)
 *   3. 安装启动脚本和UCI配置
 *   4. 停止旧的 igmpproxy + udpxy
 *   5. 启动 multicast_proxy
 *   6. 测试播放
 */

const { ssh, runCmds } = require('./ssh_tool');
const fs = require('fs');
const path = require('path');
const { Client } = require('ssh2');

const SSH_CONFIG = {
    host: '192.168.2.1',
    port: 22,
    username: 'root',
    password: '19981213',
    readyTimeout: 15000,
};

function sshExec(config, cmd) {
    return new Promise((resolve, reject) => {
        const conn = new Client();
        let stdout = '', stderr = '';
        conn.on('ready', () => {
            conn.exec(cmd, (err, stream) => {
                if (err) { conn.end(); reject(err); return; }
                stream.on('data', d => stdout += d.toString());
                stream.stderr.on('data', d => stderr += d.toString());
                stream.on('close', (code) => { conn.end(); resolve({ stdout, stderr, code }); });
            });
        }).on('error', reject).connect(config);
    });
}

async function uploadFile(localPath, remotePath) {
    const content = fs.readFileSync(localPath);
    const b64 = content.toString('base64');
    const CHUNK = 1000;
    const chunks = [];
    for (let i = 0; i < b64.length; i += CHUNK) {
        chunks.push(b64.substring(i, i + CHUNK));
    }
    console.log(`  ${content.length} bytes -> ${chunks.length} chunks`);
    for (let i = 0; i < chunks.length; i++) {
        const op = i === 0 ? '>' : '>>';
        const cmd = `echo -n '${chunks[i]}' | base64 -d ${op} ${remotePath}`;
        process.stdout.write(`  chunk ${i+1}/${chunks.length}...`);
        const r = await sshExec(SSH_CONFIG, cmd);
        if (r.code !== 0) throw new Error(`chunk ${i+1} failed: ${r.stderr}`);
        console.log(' OK');
    }
    const v = await ssh(`wc -c < ${remotePath}`);
    console.log(`  remote: ${v.stdout.trim()} bytes`);
}

async function safeSSH(cmd) {
    try {
        return await ssh(cmd);
    } catch(e) {
        return { stdout: '', stderr: e.message, code: -1 };
    }
}

async function main() {
    console.log('========================================');
    console.log('  部署 multicast_proxy');
    console.log('========================================\n');

    // 等待SSH
    console.log('等待路由器SSH...');
    for (let i = 0; i < 30; i++) {
        try {
            const r = await ssh('echo ok');
            if (r.stdout.includes('ok')) { console.log('连接成功\n'); break; }
        } catch(e) {
            process.stdout.write('.');
            await new Promise(r => setTimeout(r, 3000));
        }
    }

    try {
        // === 第1步: 上传源码 ===
        console.log('=== 第1步: 上传 multicast_proxy.c ===');
        await uploadFile(path.join(__dirname, 'multicast_proxy.c'), '/tmp/multicast_proxy.c');

        // === 第2步: 编译 ===
        console.log('\n=== 第2步: 编译 ===');
        let r = await ssh('gcc -O2 -Wall -o /usr/bin/multicast_proxy /tmp/multicast_proxy.c 2>&1; echo EXIT:$?');
        if (!r.stdout.includes('EXIT:0')) {
            console.error('编译失败:', r.stdout);
            return;
        }
        console.log('编译成功');
        r = await ssh('ls -la /usr/bin/multicast_proxy');
        console.log(r.stdout);

        // === 第3步: 安装配置 ===
        console.log('\n=== 第3步: 安装配置 ===');

        // 启动脚本
        const initScript = fs.readFileSync(path.join(__dirname, 'multicast_proxy.init'), 'utf8');
        const initB64 = Buffer.from(initScript).toString('base64');
        await ssh(`echo -n '${initB64}' | base64 -d > /etc/init.d/multicast_proxy`);
        await ssh('chmod +x /etc/init.d/multicast_proxy');
        await ssh('/etc/init.d/multicast_proxy enable');
        console.log('  启动脚本已安装');

        // UCI配置
        const uciConf = fs.readFileSync(path.join(__dirname, 'multicast_proxy.uci'), 'utf8');
        const uciB64 = Buffer.from(uciConf).toString('base64');
        await ssh(`echo -n '${uciB64}' | base64 -d > /etc/config/multicast_proxy`);
        console.log('  UCI配置已安装');

        // === 第4步: 停止旧服务 ===
        console.log('\n=== 第4步: 停止旧服务 ===');
        await ssh('killall -9 igmpproxy udpxy igmp_inject igmp_proxy igmp_server.sh 2>/dev/null');
        await ssh('/etc/init.d/igmpproxy stop 2>/dev/null');
        await ssh('/etc/init.d/udpxy stop 2>/dev/null');
        await ssh('/etc/init.d/igmpproxy disable 2>/dev/null');
        await ssh('/etc/init.d/udpxy disable 2>/dev/null');
        console.log('  旧服务已停止');

        // === 第5步: 网络配置 ===
        console.log('\n=== 第5步: 网络配置 ===');
        // WAN口MAC克隆
        await ssh('ifconfig eth0.2 down 2>/dev/null');
        await ssh('ifconfig eth0.2 hw ether A8:F4:70:A5:F2:CE 2>/dev/null');
        await ssh('ifconfig eth0.2 192.168.1.216 netmask 255.255.255.0 up 2>/dev/null');
        await ssh('uci set network.wan.proto=static');
        await ssh('uci set network.wan.ipaddr=192.168.1.216');
        await ssh('uci set network.wan.netmask=255.255.255.0');
        await ssh('uci set network.wan.gateway=192.168.1.1');
        await ssh('uci set network.wan.dns=192.168.1.1');
        await ssh('uci set network.wan.macaddr=A8:F4:70:A5:F2:CE');
        await ssh('uci commit network');
        r = await ssh('ifconfig eth0.2 2>/dev/null | grep -E "inet addr|ether"');
        console.log('  WAN:', r.stdout.trim() || 'eth0.2不存在');

        // 防火墙
        await ssh('uci add firewall rule 2>/dev/null; uci set firewall.@rule[-1].name=Allow-IGMP; uci set firewall.@rule[-1].src=wan; uci set firewall.@rule[-1].proto=igmp; uci set firewall.@rule[-1].target=ACCEPT');
        await ssh('uci add firewall rule 2>/dev/null; uci set firewall.@rule[-1].name=Allow-Multicast-UDP; uci set firewall.@rule[-1].src=wan; uci set firewall.@rule[-1].dest=lan; uci set firewall.@rule[-1].proto=udp; uci set firewall.@rule[-1].dest_ip=224.0.0.0/4; uci set firewall.@rule[-1].target=ACCEPT');
        await ssh('uci commit firewall');
        await ssh('/etc/init.d/firewall restart 2>/dev/null');
        console.log('  防火墙规则已添加');

        // IGMP snooping
        await ssh('echo 0 > /sys/devices/virtual/net/br-lan/bridge/multicast_snooping 2>/dev/null');
        console.log('  IGMP snooping已关闭');

        // rc.local持久化
        await ssh("sed -i '/multicast_proxy/d' /etc/rc.local 2>/dev/null");
        await ssh("sed -i '/exit 0/i ifconfig eth0.2 hw ether A8:F4:70:A5:F2:CE 2>/dev/null' /etc/rc.local");

        // === 第6步: 启动 ===
        console.log('\n=== 第6步: 启动 multicast_proxy ===');
        await ssh('/etc/init.d/multicast_proxy start');
        await new Promise(r => setTimeout(r, 2000));
        r = await ssh('ps | grep multicast_proxy | grep -v grep');
        console.log('  服务:', r.stdout.trim() || '启动失败!');

        // === 第7步: 验证 ===
        console.log('\n=== 第7步: 验证 ===');
        const verify = await runCmds([
            { label: '进程', cmd: 'ps | grep multicast_proxy | grep -v grep' },
            { label: '网络', cmd: 'ifconfig eth0.2 2>/dev/null | grep -E "inet addr|ether"' },
            { label: '防火墙', cmd: 'iptables -L zone_wan_input -n | grep -E "igmp|224"' },
            { label: '端口监听', cmd: 'netstat -tlnp 2>/dev/null | grep 8888 || cat /proc/net/tcp | grep 2288' },
            { label: '启动项', cmd: 'ls -la /etc/rc.d/S90multicast_proxy 2>/dev/null' },
        ]);
        for (const v of verify) {
            console.log(`\n--- ${v.label} ---`);
            console.log(v.stdout || '(empty)');
        }

        // === 第8步: 测试 ===
        console.log('\n=== 第8步: 播放测试 ===');
        r = await ssh("wget -O /dev/null --timeout=5 'http://127.0.0.1:8888/udp/232.0.1.210:1234' 2>&1 | tail -3");
        console.log('wget 8888:', r.stdout);

        r = await ssh("wget -O /dev/null --timeout=3 'http://127.0.0.1:8888/status' 2>&1 | head -5");
        console.log('status:', r.stdout.substring(0, 100));

        console.log('\n========================================');
        console.log('  部署完成!');
        console.log('');
        console.log('  播放地址: http://192.168.2.1:8888/udp/232.0.x.x:1234');
        console.log('  状态页面: http://192.168.2.1:8888/status');
        console.log('  频道列表: http://192.168.2.1:8888/playlist');
        console.log('');
        console.log('  无需 igmpproxy, 无需 udpxy');
        console.log('  IGMP join/leave 由内核自动处理');
        console.log('========================================');

    } catch (e) {
        console.error('\n错误:', e.message);
    }
}

main();
