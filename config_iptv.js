const { Client } = require('ssh2');

const conn = new Client({
  hostAlgorithm: 'ssh-rsa',
  algorithms: {
    kex: ['diffie-hellman-group14-sha1', 'diffie-hellman-group1-sha1', 'diffie-hellman-group-exchange-sha1'],
    cipher: ['aes128-ctr', 'aes192-ctr', 'aes256-ctr', 'aes128-cbc', 'aes192-cbc', 'aes256-cbc', '3des-cbc'],
    hmac: ['hmac-sha1', 'hmac-sha2-256', 'hmac-md5'],
    hostAlgorithms: ['ssh-rsa']
  }
});

conn.on('ready', () => {
  console.log('SSH Connected');
  const commands = [
    'echo "=== 1. 检查 eth0.1000 接口 ==="',
    'ifconfig eth0.1000',
    'echo "=== 2. 抓取 eth0.1000 上的 IGMP 包（10秒）==="',
    'tcpdump -i eth0.1000 -n igmp -c 20 2>&1',
    'echo "=== 3. 抓取 eth0.1000 上的组播 UDP（10秒）==="',
    'tcpdump -i eth0.1000 -n "udp and dst net 224.0.0.0/4" -c 30 2>&1',
    'echo "=== 4. 配置 igmpproxy 使用 eth0.1000 作为上游 ==="',
    'killall -9 igmpproxy 2>/dev/null',
    'uci delete igmpproxy.@phyint[0] 2>/dev/null',
    'uci set igmpproxy.@phyint[0]=phyint',
    'uci set igmpproxy.@phyint[0].network=iptv',
    'uci set igmpproxy.@phyint[0].direction=upstream',
    'uci set igmpproxy.@phyint[0].altnetwork="0.0.0.0/0"',
    'uci commit igmpproxy',
    'echo "=== 5. 查看 igmpproxy 配置 ==="',
    'cat /etc/config/igmpproxy',
    'echo "=== 6. 配置 udpxy 使用 eth0.1000 ==="',
    'killall -9 udpxy 2>/dev/null',
    'uci set udpxy.@udpxy[0].source=eth0.1000',
    'uci commit udpxy',
    'echo "=== 7. 查看 udpxy 配置 ==="',
    'cat /etc/config/udpxy',
    'echo "=== 8. 重启 igmpproxy ==="',
    '/etc/init.d/igmpproxy restart',
    'sleep 3',
    'ps | grep igmpproxy | grep -v grep',
    'echo "=== 9. 重启 udpxy ==="',
    '/etc/init.d/udpxy restart',
    'sleep 2',
    'ps | grep udpxy | grep -v grep',
    'echo "=== 10. 检查 IGMP 状态 ==="',
    'cat /proc/net/igmp | grep -A 5 "eth0.1000"',
    'echo "=== 11. 检查 VIF ==="',
    'cat /proc/net/ip_mr_vif',
    'echo "=== 12. 检查日志 ==="',
    'logread | grep -E "(igmpproxy|udpxy)" | tail -10'
  ];
  const cmdStr = commands.join('\n');
  conn.exec(cmdStr, (err, stream) => {
    if (err) { console.error('Exec error:', err); conn.end(); return; }
    let data = '';
    stream.on('data', (chunk) => { data += chunk.toString(); });
    stream.on('close', () => {
      console.log(data);
      conn.end();
    });
  });
}).on('error', (err) => {
  console.error('Connection error:', err.message);
}).connect({
  host: '192.168.2.1',
  port: 22,
  username: 'root',
  password: '19981213',
  readyTimeout: 15000
});
