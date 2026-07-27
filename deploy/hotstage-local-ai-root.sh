#!/bin/busybox sh
set -eu

stage=/tmp/libreecho-local-ai-stage
backup=/data/libreecho/hotstage/local-ai-backup

[ -x "$stage/libreecho-web" ]
[ -x "$stage/libreecho-agentd" ]
[ -x "$stage/libreecho-airplayd" ]
[ -x "$stage/libreecho-wyomingd" ]
[ -x "$stage/libreecho-sttd-wyoming" ]
[ -x "$stage/libreecho-ttsd-wyoming" ]
[ -f "$stage/libreecho-sttd.init" ]
[ -f "$stage/libreecho-ttsd.init" ]
[ -f "$stage/libreecho-airplayd.init" ]
[ -f "$stage/index.html" ]
[ -f "$stage/integrations-ui.js" ]
[ -f "$stage/privacy-ui.js" ]

mkdir -p "$backup/sbin" "$backup/init" "$backup/web/js" /etc/default

backup_once() {
    source=$1
    target=$2
    if [ -e "$source" ] && [ ! -e "$target" ]; then
        cp -p "$source" "$target"
    fi
}

replace_executable() {
    source=$1
    target=$2
    cp "$source" "$target.new"
    chmod 0755 "$target.new"
    mv "$target.new" "$target"
}

backup_once /usr/local/sbin/libreecho-web "$backup/sbin/libreecho-web"
backup_once /usr/local/sbin/libreecho-agentd "$backup/sbin/libreecho-agentd"
backup_once /usr/local/sbin/libreecho-airplayd "$backup/sbin/libreecho-airplayd"
backup_once /usr/local/sbin/libreecho-wyomingd "$backup/sbin/libreecho-wyomingd"
backup_once /etc/init.d/libreecho-sttd.init "$backup/init/libreecho-sttd.init"
backup_once /etc/init.d/libreecho-ttsd.init "$backup/init/libreecho-ttsd.init"
backup_once /etc/init.d/libreecho-airplayd.init "$backup/init/libreecho-airplayd.init"
backup_once /usr/local/share/libreecho/web/index.html "$backup/web/index.html"
backup_once /usr/local/share/libreecho/web/js/integrations-ui.js \
    "$backup/web/js/integrations-ui.js"
backup_once /usr/local/share/libreecho/web/js/privacy-ui.js \
    "$backup/web/js/privacy-ui.js"

/etc/init.d/libreecho-agentd.init stop || true
/etc/init.d/libreecho-sttd.init stop || true
/etc/init.d/libreecho-ttsd.init stop || true
/etc/init.d/libreecho-airplayd.init stop || true
/etc/init.d/libreecho-web.init stop || true

replace_executable "$stage/libreecho-web" /usr/local/sbin/libreecho-web
replace_executable "$stage/libreecho-agentd" /usr/local/sbin/libreecho-agentd
replace_executable "$stage/libreecho-airplayd" /usr/local/sbin/libreecho-airplayd
replace_executable "$stage/libreecho-wyomingd" /usr/local/sbin/libreecho-wyomingd
replace_executable "$stage/libreecho-sttd-wyoming" \
    /usr/local/sbin/libreecho-sttd-wyoming
replace_executable "$stage/libreecho-ttsd-wyoming" \
    /usr/local/sbin/libreecho-ttsd-wyoming

cp "$stage/libreecho-sttd.init" /etc/init.d/libreecho-sttd.init.new
chmod 0755 /etc/init.d/libreecho-sttd.init.new
mv /etc/init.d/libreecho-sttd.init.new /etc/init.d/libreecho-sttd.init
cp "$stage/libreecho-ttsd.init" /etc/init.d/libreecho-ttsd.init.new
chmod 0755 /etc/init.d/libreecho-ttsd.init.new
mv /etc/init.d/libreecho-ttsd.init.new /etc/init.d/libreecho-ttsd.init
cp "$stage/libreecho-airplayd.init" /etc/init.d/libreecho-airplayd.init.new
chmod 0755 /etc/init.d/libreecho-airplayd.init.new
mv /etc/init.d/libreecho-airplayd.init.new /etc/init.d/libreecho-airplayd.init

cp "$stage/index.html" /usr/local/share/libreecho/web/index.html
cp "$stage/integrations-ui.js" \
    /usr/local/share/libreecho/web/js/integrations-ui.js
cp "$stage/privacy-ui.js" /usr/local/share/libreecho/web/js/privacy-ui.js
chmod 0644 /usr/local/share/libreecho/web/index.html \
    /usr/local/share/libreecho/web/js/integrations-ui.js \
    /usr/local/share/libreecho/web/js/privacy-ui.js

printf '%s\n' 'DAEMON=/usr/local/sbin/libreecho-agentd' \
    >/etc/default/libreecho-agentd
chmod 0644 /etc/default/libreecho-agentd

/etc/init.d/libreecho-web.init start
/etc/init.d/libreecho-sttd.init start
/etc/init.d/libreecho-ttsd.init start
/etc/init.d/libreecho-agentd.init start
/etc/init.d/libreecho-airplayd.init start
sleep 1
/etc/init.d/libreecho-web.init status

sha256sum /usr/local/sbin/libreecho-web \
    /usr/local/sbin/libreecho-agentd \
    /usr/local/sbin/libreecho-airplayd \
    /usr/local/sbin/libreecho-wyomingd \
    /usr/local/sbin/libreecho-sttd-wyoming \
    /usr/local/sbin/libreecho-ttsd-wyoming
