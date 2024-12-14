#!/usr/bin/bash

# 安装 avm，请使用 sudo 执行

cp tools/99-avm.rules /etc/udev/rules.d/
udevadm control --reload

cp tools/50-samba-reload.rules /etc/polkit-1/rules.d/
systemctl restart polkit

cp tools/hostapd.conf /etc/hostapd/
cp tools/dnsmasq.conf /etc/

cp tools/avm.service tools/on-udisk-mount.service tools/switchAP.service /lib/systemd/system/
systemctl daemon-reload
systemctl enable avm.service
systemctl enable on-udisk-mount.service
systemctl enable switchAP.service
