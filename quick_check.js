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
  const cmd = 'echo "=== WAN ===" ; ifconfig eth0.2 | head -5 ; echo "=== igmpproxy ===" ; ps | grep igmpproxy | grep -v grep || echo "未运行" ; echo "=== udpxy ===" ; ps | grep udpxy | grep -v grep || echo "未运行" ; echo "=== igmpproxy config ===" ; cat /etc/config/igmpproxy ; echo "=== udpxy config ===" ; cat /etc/config/udpxy ; echo "=== VIF ===" ; cat /proc/net/ip_mr_vif ; echo "=== log ===" ; logread | grep igmpproxy | tail -3';
  conn.exec(cmd, (err, stream) => {
    if (err) { console.error('Error:', err); conn.end(); return; }
    let data = '';
    stream.on('data', (chunk) => { data += chunk.toString(); });
    stream.on('close', () => { console.log(data); conn.end(); });
  });
}).on('error', (err) => {
  console.error('Connection error:', err.message);
}).connect({
  host: '192.168.2.1',
  port: 22,
  username: 'root',
  password: '19981213',
  readyTimeout: 10000
});
