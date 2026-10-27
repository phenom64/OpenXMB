#!/bin/sh
set -eu
case "$1" in
    remove|purge)
        if command -v aa-enabled >/dev/null 2>&1 && aa-enabled --quiet; then
            printf 'profile openxmb-web { }\n' | apparmor_parser -R 2>/dev/null || true
        fi
        ;;
esac
