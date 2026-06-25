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
    'echo "=== 1. 抓取 eth0 上所有 VLAN 流量（10秒）==="',
    'tcpdump -i eth0 -n -e -c 20 2>&1',
    'echo "=== 2. 抓取 eth0 上的 IGMP 流量（10秒）==="',
    'tcpdump -i eth0 -n igmp -c 10 2>&1',
    'echo "=== 3. 抓取 eth0 上的组播流量（10秒）==="',
    'tcpdump -i eth0 -n "multicast" -c 20 2>&1',
    'echo "=== 4. 检查 eth0 接口统计 ==="',
    'ifconfig eth0 | grep -E "RX|TX"',
    'echo "=== 5. 检查所有 VLAN 接口 ==="',
    'ip link show | grep eth0'
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
