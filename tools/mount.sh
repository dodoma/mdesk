#!/usr/bin/bash

MOUNT="/usr/bin/systemd-mount"
CUR_TIME=`date +'%Y-%m-%d %H:%M:%S'`
LOGFILE="/home/pi/mdesk/log/mount.log"

echo "" >> $LOGFILE
echo "$CUR_TIME $DEVNAME $ACTION $ID_FS_TYPE" >> $LOGFILE

if [ "$ACTION" = "add" ] && [ -n "$DEVNAME" ] && [ -n "$ID_FS_TYPE" ]; then
    if [[ "${DEVNAME:0:12}" != "/dev/mmcblk0" ]]; then
        # 由于没法使用模板单元（template unit) 在启动 media-@sda1.mount 后，自动启动 usb-notify@sda1.service，
        # 暂时只支持单个U盘分区挂载
        mkdir -p /media/udisk

        # https://wiki.archlinux.org/title/Udev#Mounting_drives_in_rules
        grep -q "/media/udisk" /proc/mounts || $MOUNT --options=iocharset=utf8 --no-block --collect $DEVNAME /media/udisk >> $LOGFILE 2>&1
    fi
fi
