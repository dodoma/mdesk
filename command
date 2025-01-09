#开发环境
$ sudo apt-get install libasound2-dev git xtail vim tig libmagic-dev uchardet libuchardet-dev

$ sudo dpkg-reconfigure locales
$ sudo timedatectl set-timezone Asia/Shanghai
$ sudo systemctl enable ssh
$ sudo systemctl start ssh
$ sudo raspi-config => 5 => L4

# samba
$ sudo apt-get install samba samba-common smbclient
$ sudo smbpasswd -a pi

$ sudo chgrp adm /etc/samba/smb.conf
$ sudo chmod 664 /etc/samba/smb.conf

# wpa_supplicant
$ sudo chgrp adm /etc/wpa_supplicant/wpa_supplicant.conf
$ sudo chmod 660 /etc/wpa_supplicant/wpa_supplicant.conf

# 热点模式
$ sudo apt-get install hostapd dnsmasq ifupdown
$ sudo systemctl unmask hostapd
$ sudo systemctl disable hostapd dnsmasq

# 文件拷贝
$ cd /home/pi/mdesk/
$ sudo cp tools/99-avm.rules /etc/udev/rules.d/
$ sudo udevadm control --reload

$ sudo cp tools/50-samba-reload.rules /etc/polkit-1/rules.d/
$ sudo systemctl restart polkit

$ sudo cp tools/hostapd.conf /etc/hostapd/
$ sudo cp tools/dnsmasq.conf /etc/

$ sudo cp tools/interfaces /etc/network/
$ sudo cp tools/interfaces.master /etc/network/
$ sudo systemctl edit network
加入
[Service]
TimeoutStartSec=20sec

# systemd service
$ cd /home/pi/mdesk/tools/
$ sudo cp *.service /lib/systemd/system/
$ cd /lib/systemd/system
$ sudo systemctl enable avm.service
$ sudo systemctl enable on-udisk-mount.service
$ sudo systemctl enable switchAP.service

#镜像制作
1. 烧录官方 lite 镜像
2. 去掉 /dev/sdb1 cmdline.txt  init=/usr/lib/raspberrypi-sys-mods/firstboot
3. 启动后 sudo resize_root_part.sh
   再执行上面这些安装
4. sudo chmod +x /etc/init.d/resize2fs_once
   sudo systemctl enable resize2fs_once
5. mount sd卡修复cmdline.txt
6. umount, sudo fdisk -l /dev/sdb 记下 count-1
   sudo dd if=/dev/sdb of=/home/ml/avm-0.2.0.img  count=5894142 status=progress


#版本迭代
1. 烧录 avmxxx.img
2. 去掉 /dev/sdb1 cmdline.txt  init=/usr/lib/raspberrypi-sys-mods/firstboot
3. 开发环境release
4. sudo cp tools/resize2fs_once /etc/init.d/
5. sudo systemctl enable resize2fs_once
6. mount sd卡修复cmdline.txt
7. umount, sudo fdisk -l /dev/sdb 记下 count-1
   sudo dd if=/dev/sdb of=/home/ml/avm-0.2.0.img  count=5894142 status=progress
