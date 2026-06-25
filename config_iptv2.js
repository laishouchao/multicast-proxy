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
  console.log('SSH Connected - 配置 IPTV');
  
  const step1 = [
    'killall -9 igmpproxy udpxy tcpdump 2>/dev/null',
    'sleep 2',
    
    'echo "=== 1. 配置 igmpproxy 上游为 eth0.1000 ==="',
    'uci delete igmpproxy.@phyint[0] 2>/dev/null',
    'uci set igmpproxy.@phyint[0]=phyint',
    'uci set igmpproxy.@phyint[0].network=iptv',
    'uci set igmpproxy.@phyint[0].direction=upstream',
    'uci commit igmpproxy',
    'cat /etc/config/igmpproxy',
    
    'echo "=== 2. 配置 udpxy 源为 eth0.1000 ==="',
    'uci set udpxy.@udpxy[0].source=eth0.1000',
    'uci commit udpxy',
    'cat /etc/config/udpxy',
    
    'echo "=== 3. 启动 igmpproxy ==="',
    '/etc/init.d/igmpproxy restart',
    'sleep 3',
    'ps | grep igmpproxy | grep -v grep || echo "igmpproxy 未运行"',
    
    'echo "=== 4. 启动 udpxy ==="',
    '/etc/init.d/udpxy restart',
    'sleep 2',
    'ps | grep udpxy | grep -v grep || echo "udpxy 未运行"',
    
    'echo "=== 5. 检查 eth0.1000 状态 ==="',
    'ifconfig eth0.1000',
    
    'echo "=== 6. 检查 IGMP ==="',
    'cat /proc/net/igmp | grep -A 5 "eth0.1000" || echo "无 eth0.1000 IGMP"',
    
    'echo "=== 7. 检查 VIF ==="',
    'cat /proc/net/ip_mr_vif',
    
    'echo "=== 8. 检查日志 ==="',
    'logread | grep -E "(igmpproxy|udpxy)" | tail -10'
  ].join(' ; ');
  
  conn.exec(step1, (err, stream) => {
    if (err) { console.error('Error:', err); conn.end(); return; }
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
