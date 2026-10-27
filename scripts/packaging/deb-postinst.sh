#!/bin/sh
set -eu
if [ "$1" = configure ] && [ -w /sys/kernel/security/apparmor/.load ] && command -v aa-enabled >/dev/null 2>&1 && aa-enabled --quiet; then
    apparmor_parser -r /etc/apparmor.d/openxmb-web
fi
