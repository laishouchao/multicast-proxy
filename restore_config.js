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
  console.log('SSH Connected - 恢复昨天的工作配置');
  
  // Step 1: Kill processes and configure network
  const step1 = 'killall -9 igmpproxy udpxy 2>/dev/null ; sleep 2 ; echo "进程已停止" ; uci set network.wan.macaddr=A8:F4:70:A5:F2:CE ; uci set network.wan.proto=static ; uci set network.wan.ipaddr=192.168.1.216 ; uci set network.wan.netmask=255.255.255.0 ; uci set network.wan.gateway=192.168.1.1 ; uci set network.wan.dns=192.168.1.1 ; uci delete network.vlan1000 2>/dev/null ; uci delete network.iptv 2>/dev/null ; uci commit network ; echo "网络配置已恢复"';
  
  conn.exec(step1, (err, stream) => {
    if (err) { console.error('Error:', err); conn.end(); return; }
    let data = '';
    stream.on('data', (chunk) => { data += chunk.toString(); });
    stream.on('close', () => {
      console.log('Step 1:', data);
      
      // Step 2: Configure igmpproxy
      const step2 = 'uci delete igmpproxy.@phyint[0] 2>/dev/null ; uci delete igmpproxy.@phyint[0] 2>/dev/null ; uci set igmpproxy.@phyint[0]=phyint ; uci set igmpproxy.@phyint[0].network=wan ; uci set igmpproxy.@phyint[0].direction=upstream ; uci add_list igmpproxy.@phyint[0].altnetwork=0.0.0.0/0 ; uci add_list igmpproxy.@phyint[0].altnetwork=10.0.0.0/8 ; uci add_list igmpproxy.@phyint[0].altnetwork=172.16.0.0/12 ; uci add_list igmpproxy.@phyint[0].altnetwork=192.168.0.0/16 ; uci set igmpproxy.@phyint[1]=phyint ; uci set igmpproxy.@phyint[1].network=lan ; uci set igmpproxy.@phyint[1].direction=downstream ; uci commit igmpproxy ; echo "igmpproxy 配置已恢复" ; cat /etc/config/igmpproxy';
      
      conn.exec(step2, (err2, stream2) => {
        if (err2) { console.error('Error:', err2); conn.end(); return; }
        let data2 = '';
        stream2.on('data', (chunk) => { data2 += chunk.toString(); });
        stream2.on('close', () => {
          console.log('Step 2:', data2);
          
          // Step 3: Configure udpxy and restart network
          const step3 = 'uci set udpxy.@udpxy[0].source=eth0.2 ; uci commit udpxy ; echo "udpxy 配置已恢复" ; /etc/init.d/network restart ; sleep 5 ; echo "网络已重启"';
          
          conn.exec(step3, (err3, stream3) => {
            if (err3) { console.error('Error:', err3); conn.end(); return; }
            let data3 = '';
            stream3.on('data', (chunk) => { data3 += chunk.toString(); });
            stream3.on('close', () => {
              console.log('Step 3:', data3);
              
              // Step 4: Manual WAN config and start services
              const step4 = 'ifconfig eth0.2 down ; ifconfig eth0.2 hw ether A8:F4:70:A5:F2:CE ; ifconfig eth0.2 192.168.1.216 netmask 255.255.255.0 up ; echo "WAN 口已手动配置" ; ifconfig eth0.2';
              
              conn.exec(step4, (err4, stream4) => {
                if (err4) { console.error('Error:', err4); conn.end(); return; }
                let data4 = '';
                stream4.on('data', (chunk) => { data4 += chunk.toString(); });
                stream4.on('close', () => {
                  console.log('Step 4:', data4);
                  
                  // Step 5: Generate runtime config and start igmpproxy
                  const step5 = 'mkdir -p /var/etc ; echo -e "quickleave\\n\\nphyint eth0.2 upstream ratelimit 0 threshold 1\\n    altnet 0.0.0.0/0\\n    altnet 10.0.0.0/8\\n    altnet 172.16.0.0/12\\n    altnet 192.168.0.0/16\\n\\nphyint br-lan downstream ratelimit 0 threshold 1" > /var/etc/igmpproxy.conf ; echo "运行时配置已生成" ; cat /var/etc/igmpproxy.conf ; /usr/sbin/igmpproxy /var/etc/igmpproxy.conf & ; sleep 3 ; ps | grep igmpproxy | grep -v grep || echo "igmpproxy 未运行"';
                  
                  conn.exec(step5, (err5, stream5) => {
                    if (err5) { console.error('Error:', err5); conn.end(); return; }
                    let data5 = '';
                    stream5.on('data', (chunk) => { data5 += chunk.toString(); });
                    stream5.on('close', () => {
                      console.log('Step 5:', data5);
                      
                      // Step 6: Start udpxy and check status
                      const step6 = '/etc/init.d/udpxy restart ; sleep 2 ; ps | grep udpxy | grep -v grep || echo "udpxy 未运行" ; echo "=== IGMP 状态 ===" ; cat /proc/net/igmp | grep -A 5 "eth0.2" ; echo "=== VIF ===" ; cat /proc/net/ip_mr_vif ; echo "=== 日志 ===" ; logread | grep igmpproxy | tail -5';
                      
                      conn.exec(step6, (err6, stream6) => {
                        if (err6) { console.error('Error:', err6); conn.end(); return; }
                        let data6 = '';
                        stream6.on('data', (chunk) => { data6 += chunk.toString(); });
                        stream6.on('close', () => {
                          console.log('Step 6:', data6);
                          conn.end();
                        });
                      });
                    });
                  });
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
  readyTimeout: 15000
});
