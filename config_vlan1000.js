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
  console.log('SSH Connected - 配置 IPTV VLAN 1000');
  
  const step1 = [
    'echo "=== 1. 查看当前交换机配置 ==="',
    'uci show network | grep switch',
    'echo "=== 2. 查看交换机端口 ==="',
    'swconfig dev switch0 show 2>/dev/null | grep -E "VLAN|port" | head -20'
  ].join(' ; ');
  
  conn.exec(step1, (err, stream) => {
    if (err) { console.error('Error:', err); conn.end(); return; }
    let data = '';
    stream.on('data', (chunk) => { data += chunk.toString(); });
    stream.on('close', () => {
      console.log(data);
      
      // Step 2: Add VLAN 1000 and configure IPTV interface
      const step2 = [
        'echo "=== 3. 添加 VLAN 1000 ==="',
        // Add VLAN 1000 to switch (port 0 = WAN port, port 6 = CPU)
        'uci set network.vlan1000=switch_vlan',
        'uci set network.vlan1000.device=switch0',
        'uci set network.vlan1000.vlan=1000',
        'uci set network.vlan1000.ports="0t 6t"',
        'echo "VLAN 1000 已添加"',
        
        'echo "=== 4. 创建 IPTV 网络接口 ==="',
        'uci set network.iptv=interface',
        'uci set network.iptv.ifname=eth0.1000',
        'uci set network.iptv.proto=dhcp',
        'uci set network.iptv.auto=1',
        'echo "IPTV 接口已创建"',
        
        'echo "=== 5. 提交网络配置 ==="',
        'uci commit network',
        'echo "网络配置已提交"',
        
        'echo "=== 6. 查看完整配置 ==="',
        'uci show network | grep -E "(vlan1000|iptv)"'
      ].join(' ; ');
      
      conn.exec(step2, (err2, stream2) => {
        if (err2) { console.error('Error:', err2); conn.end(); return; }
        let data2 = '';
        stream2.on('data', (chunk) => { data2 += chunk.toString(); });
        stream2.on('close', () => {
          console.log(data2);
          
          // Step 3: Restart network and bring up IPTV interface
          const step3 = [
            'echo "=== 7. 重启网络服务 ==="',
            '/etc/init.d/network restart',
            'sleep 5',
            'echo "=== 8. 检查 IPTV 接口 ==="',
            'ifconfig eth0.1000 2>/dev/null || echo "接口未就绪"',
            'ip addr show eth0.1000 2>/dev/null || echo "接口未就绪"',
            'echo "=== 9. 检查所有接口 ==="',
            'ip link show | grep -E "eth0|br-lan"'
          ].join(' ; ');
          
          conn.exec(step3, (err3, stream3) => {
            if (err3) { console.error('Error:', err3); conn.end(); return; }
            let data3 = '';
            stream3.on('data', (chunk) => { data3 += chunk.toString(); });
            stream3.on('close', () => {
              console.log(data3);
              conn.end();
            });
          });
        });
      });
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
