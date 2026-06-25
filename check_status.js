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
    'echo "=== 1. WAN 口状态 ==="',
    'ifconfig eth0.2',
    'echo "=== 2. igmpproxy 配置 ==="',
    'cat /etc/config/igmpproxy',
    'echo "=== 3. udpxy 配置 ==="',
    'cat /etc/config/udpxy',
    'echo "=== 4. igmpproxy 进程 ==="',
    'ps | grep igmpproxy | grep -v grep || echo "igmpproxy 未运行"',
    'echo "=== 5. udpxy 进程 ==="',
    'ps | grep udpxy | grep -v grep || echo "udpxy 未运行"',
    'echo "=== 6. IGMP 状态 ==="',
    'cat /proc/net/igmp | grep -A 5 "eth0.2"',
    'echo "=== 7. VIF ==="',
    'cat /proc/net/ip_mr_vif',
    'echo "=== 8. 日志 ==="',
    'logread | grep igmpproxy | tail -5',
    'echo "=== 9. 组播路由 ==="',
    'ip mroute show',
    'echo "=== 10. 抓取 IPTV 流量（5秒）==="',
    'tcpdump -i eth0.2 -n "dst net 232.0.0.0/8" -c 10 2>&1'
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
