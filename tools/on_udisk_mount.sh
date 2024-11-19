#!/usr/bin/bash

PIPE_NAME="/tmp/avm_pipe"
CUR_TIME=`date +'%Y-%m-%d %H:%M:%S'`
LOGFILE="/home/pi/mdesk/log/mount.log"

# create nonexist name pipe
[ -p "$PIPE_NAME" ] || mkfifo "$PIPE_NAME"

pids=`ps aux | grep '[s]ucker' |  awk '{print $2}'`

if [ -n "$pids" ]; then
    echo "notify $pids" >> $LOGFILE

    kill -s SIGUSR1 $pids >> $LOGFILE # MUST be done BEFORE pipe write
    echo "{'module': 'hardware', 'command': 'stick_on', 'data': 'udisk'}" > $PIPE_NAME
else
    echo "nobody to talk" >> $LOGFILE
fi
