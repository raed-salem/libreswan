ipsec auto --up  westnet-eastnet-ipv4-psk-ikev2
ping -n -c 4 -I 192.0.1.254 192.0.2.254
ipsec look
# sleep for 30s to run a few liveness cycles
sleep 15
sleep 15
# kill pluto; host may send ICMP unreachble. with iptables it won't 
kill -9 pluto
