#!/usr/bin/bash

# 版本发布脚本，请确保在 /home/pi/mdesk/ 目录下执行

PATH=/usr/local/bin:/usr/local/sbin:/bin:/usr/bin:/usr/sbin
PACKAGE_NAME=avm

# major
# 主版本号，不同主版本号的程序集不可互换。
# 例如，这适用于对产品的大量重写，这些重写使得无法实现向后兼容性。
V_MAJOR=$(awk 'BEGIN {FS=" "} {if($2 == "MDESK_VERSION_MAJOR") {print $3}}' version.h)

# minor
# 次版本号，主版本号相同，而次版本号不同，这指示显著增强，但照顾到了向后兼容性。
# 例如，这适用于产品的修正版或完全向后兼容的新版本。
V_MINOR=$(awk 'BEGIN {FS=" "} {if($2 == "MDESK_VERSION_MINOR") {print $3}}' version.h)

# revision
# 修订号，主版本号和次版本号都相同但修订号不同的程序集应是完全可互换的。
# 主要适用于修复以前发布的程序集中的bug、安全漏洞等。
V_REV=$(awk 'BEGIN {FS=" "} {if($2 == "MDESK_PATCHLEVEL") {print $3}}' version.h)

# build
# 内部版本号的不同表示对相同源所作的重新编译。这适合于更改处理器、平台或编译器的情况。

VERSION=${V_MAJOR}.${V_MINOR}.${V_REV}

echo "准备发布 $PACKAGE_NAME "$VERSION

echo "############"
echo "### 打包 ###"
echo "############"
tar zcvf /usr/local/mdesk/release/${PACKAGE_NAME}-${VERSION}.tar.gz -C /home/pi/mdesk/ \
    sucker version.h install.sh config.json runtime.json connect.mp3 Release template/ tools/ music/

echo "############"
echo "### 解包 ###"
echo "############"
mkdir -p /usr/local/mdesk/pool/${PACKAGE_NAME}-${VERSION}/ && \
    tar xvf /usr/local/mdesk/release/${PACKAGE_NAME}-${VERSION}.tar.gz \
        -C /usr/local/mdesk/pool/${PACKAGE_NAME}-${VERSION}/

mkdir -p /usr/local/mdesk/pool/${PACKAGE_NAME}-${VERSION}/log/

echo "############"
echo "### 同步 ###"
echo "############"
rsync -avu /usr/local/mdesk/pool/${PACKAGE_NAME}-${VERSION}/ pi@tvz:/home/pi/mdesk/ --exclude "music/"
rsync -avu /usr/local/mdesk/pool/${PACKAGE_NAME}-${VERSION}/music/ pi@tvz:/home/pi/music/
