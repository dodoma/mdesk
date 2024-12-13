#开发环境
$ sudo apt-get install libasound2-dev git xtail

$ sudo dpkg-reconfigure locales
$ sudo timedatectl set-timezone Asia/Shanghai
$ sudo systemctl enable ssh
$ sudo systemctl start ssh
$ sudo mkdir /media

# systemd service
$ cd /home/pi/mdesk/tools/
$ sudo cp *.service /lib/systemd/system/
$ cd /lib/systemd/system
$ sudo systemctl enable avm.service
$ sudo systemctl enable on-udisk-mount.service
$ sudo systemctl enable switchAP.service

# samba
$ sudo apt-get install samba samba-common smbclient
$ sudo smbpasswd -a pi

$ sudo chgrp adm /etc/samba/smb.conf
$ sudo chmod 664 /etc/samba/smb.conf

$ cd /home/pi/mdesk/
$ sudo cp tools/99-avm.rules /etc/udev/rules.d/
$ sudo udevadm control --reload

$ sudo cp tools/50-samba-reload.rules /etc/polkit-1/rules.d/
$ sudo systemctl restart polkit

# wpa_supplicant
$ sudo chgrp adm /etc/wpa_supplicant/wpa_supplicant.conf
$ sudo chmod 660 /etc/wpa_supplicant/wpa_supplicant.conf


# 热点模式
$ sudo apt-get install hostapd dnsmasq
$ sudo systemctl unmask hostapd

$ sudo cp tools/hostapd.conf /etc/hostapd/
$ sudo cp tools/dnsmasq.conf /etc/
