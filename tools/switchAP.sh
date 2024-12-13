#!/usr/bin/bash

# 设备PING不通服务器时，wifi切换到热点模式，等待客户端配置网络参数
# 1. dhcpcd   DHCP 的客户端守护进程，用于自动获取IP等
# 2. dnsmasq  轻量级DNS服务器，提供本地DNS解析服务，类似BIND
# 3. hostapd  热点管理服务
# 正常联网时，开启服务1
# 热点模式下，开启服务2、3

CUR_TIME=`date +'%Y-%m-%d %H:%M:%S'`
#MOCSERVER="120.76.206.21"
MOCSERVER="mbox.net.cn"
logfile=/home/pi/mdesk/log/switchAP.log
#logfile="/dev/stdout"

echo "$CUR_TIME" >> $logfile

ap_do()
{
    echo "网络不通，切换到热点模式" >> $logfile

    ifdown wlan0 >> $logfile 2>&1
    sleep 1
    ifup wlan0 -i /etc/network/interfaces.master >> $logfile 2>&1
    sleep 1
    systemctl start hostapd >> $logfile 2>&1
    systemctl start dnsmasq >> $logfile 2>&1
}

RETRY_COUNT=0
until ping -W 5 -nq -c3 $MOCSERVER >> $logfile 2>&1; do
    ((RETRY_COUNT++))

    if [ $RETRY_COUNT -eq 2 ]; then
        ap_do
        exit 0
    fi

    echo "ping 不通，重试... $RETRY_COUNT" >> $logfile
    sleep 3
done

echo "网络畅通，设备在线 $RETRY_COUNT" >> $logfile
