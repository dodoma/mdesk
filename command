$ sudo timedatectl set-timezone Asia/Shanghai
$ sudo apt-get install pmount
$ sudo udevadm control --reload
$ sudo mkdir /media

$ sudo apt-get install samba samba-common smbclient
$ sudo smbpasswd -a pi

# 普通用户可以配置samba
$ sudo chgrp adm /etc/samba/smb.conf
$ sudo chmod 664 /etc/samba/smb.conf
$ sudo cp tools/50-samba-reload.rules /etc/polkit-1/rules.d
$ sudo systemctl restart polkit
