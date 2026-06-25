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
  console.log('SSH Connected - 修复所有问题');
  
  // Step 1: Clone STB MAC to WAN port
  const step1 = 'ifconfig eth0.2 down ; ifconfig eth0.2 hw ether A8:F4:70:A5:F2:CE ; ifconfig eth0.2 192.168.1.216 netmask 255.255.255.0 up ; echo "WAN MAC 已克隆" ; ifconfig eth0.2 | head -3';
  
  conn.exec(step1, (err, stream) => {
    if (err) { console.error('Error:', err); conn.end(); return; }
    let data = '';
    stream.on('data', (chunk) => { data += chunk.toString(); });
    stream.on('close', () => {
      console.log('Step 1:', data);
      
      // Step 2: Restore igmpproxy config
      const step2 = 'uci delete igmpproxy.@phyint[0] 2>/dev/null ; uci delete igmpproxy.@phyint[0] 2>/dev/null ; uci set igmpproxy.@phyint[0]=phyint ; uci set igmpproxy.@phyint[0].network=wan ; uci set igmpproxy.@phyint[0].direction=upstream ; uci add_list igmpproxy.@phyint[0].altnetwork=0.0.0.0/0 ; uci add_list igmpproxy.@phyint[0].altnetwork=10.0.0.0/8 ; uci add_list igmpproxy.@phyint[0].altnetwork=172.16.0.0/12 ; uci add_list igmpproxy.@phyint[0].altnetwork=192.168.0.0/16 ; uci set igmpproxy.@phyint[1]=phyint ; uci set igmpproxy.@phyint[1].network=lan ; uci set igmpproxy.@phyint[1].direction=downstream ; uci commit igmpproxy ; echo "igmpproxy 配置已恢复" ; cat /etc/config/igmpproxy';
      
      conn.exec(step2, (err2, stream2) => {
        if (err2) { console.error('Error:', err2); conn.end(); return; }
        let data2 = '';
        stream2.on('data', (chunk) => { data2 += chunk.toString(); });
        stream2.on('close', () => {
          console.log('Step 2:', data2);
          
          // Step 3: Generate runtime config and start igmpproxy
          const step3 = 'mkdir -p /var/etc ; echo -e "quickleave\\n\\nphyint eth0.2 upstream ratelimit 0 threshold 1\\n    altnet 0.0.0.0/0\\n    altnet 10.0.0.0/8\\n    altnet 172.16.0.0/12\\n    altnet 192.168.0.0/16\\n\\nphyint br-lan downstream ratelimit 0 threshold 1" > /var/etc/igmpproxy.conf ; cat /var/etc/igmpproxy.conf ; /usr/sbin/igmpproxy /var/etc/igmpproxy.conf & ; sleep 3 ; ps | grep igmpproxy | grep -v grep || echo "igmpproxy 启动失败"';
          
          conn.exec(step3, (err3, stream3) => {
            if (err3) { console.error('Error:', err3); conn.end(); return; }
            let data3 = '';
            stream3.on('data', (chunk) => { data3 += chunk.toString(); });
            stream3.on('close', () => {
              console.log('Step 3:', data3);
              
              // Step 4: Start udpxy
              const step4 = '/etc/init.d/udpxy restart ; sleep 2 ; ps | grep udpxy | grep -v grep || echo "udpxy 启动失败" ; echo "=== IGMP ===" ; cat /proc/net/igmp | grep -A 5 "eth0.2" ; echo "=== VIF ===" ; cat /proc/net/ip_mr_vif';
              
              conn.exec(step4, (err4, stream4) => {
                if (err4) { console.error('Error:', err4); conn.end(); return; }
                let data4 = '';
                stream4.on('data', (chunk) => { data4 += chunk.toString(); });
                stream4.on('close', () => {
                  console.log('Step 4:', data4);
                  conn.end();
                });
              });
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
  readyTimeout: 10000
});
