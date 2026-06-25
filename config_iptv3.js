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
    'killall -9 igmpproxy udpxy 2>/dev/null',
    'sleep 2',
    
    'echo "=== 1. 检查交换机 VLAN 配置 ==="',
    'swconfig dev switch0 show 2>/dev/null | grep -E "VLAN|port"',
    
    'echo "=== 2. 修复 igmpproxy 配置（添加下游接口）==="',
    'uci delete igmpproxy.@phyint[0] 2>/dev/null',
    'uci delete igmpproxy.@phyint[0] 2>/dev/null',
    'uci set igmpproxy.@phyint[0]=phyint',
    'uci set igmpproxy.@phyint[0].network=iptv',
    'uci set igmpproxy.@phyint[0].direction=upstream',
    'uci set igmpproxy.@phyint[1]=phyint',
    'uci set igmpproxy.@phyint[1].network=lan',
    'uci set igmpproxy.@phyint[1].direction=downstream',
    'uci commit igmpproxy',
    'cat /etc/config/igmpproxy',
    
    'echo "=== 3. 启动 igmpproxy ==="',
    '/etc/init.d/igmpproxy restart',
    'sleep 3',
    'ps | grep igmpproxy | grep -v grep || echo "igmpproxy 未运行"',
    
    'echo "=== 4. 启动 udpxy ==="',
    '/etc/init.d/udpxy restart',
    'sleep 2',
    'ps | grep udpxy | grep -v grep || echo "udpxy 未运行"',
    
    'echo "=== 5. 检查 VIF ==="',
    'cat /proc/net/ip_mr_vif',
    
    'echo "=== 6. 检查 eth0.1000 流量 ==="',
    'ifconfig eth0.1000 | grep -E "RX packets"',
    
    'echo "=== 7. 手动创建 VLAN 1000 子接口 ==="',
    'ip link add link eth0 name eth0.1000 type vlan id 1000 2>/dev/null && echo "创建成功" || echo "已存在"',
    'ip link set eth0.1000 up',
    'ifconfig eth0.1000',
    
    'echo "=== 8. 检查日志 ==="',
    'logread | grep igmpproxy | tail -5',
    
    'echo "=== 9. 抓取 eth0.1000 流量（5秒）==="',
    'tcpdump -i eth0.1000 -n -c 10 2>&1',
    
    'echo "=== 10. 同时抓取 eth0 上的 VLAN 1000 流量 ==="',
    'tcpdump -i eth0 -n "vlan 1000" -c 10 2>&1'
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
