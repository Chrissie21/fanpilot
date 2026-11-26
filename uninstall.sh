set -eu

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run make uninstall from the project directory.' >&2
    exit 77
fi

if launchctl print system/local.fanpilot >/dev/null 2>&1; then
    launchctl bootout system/local.fanpilot
fi
if [ -f /var/db/fanpilot/baseline.plist ]; then
    /Library/PrivilegedHelperTools/local.fanpilot restore
fi
rm -f /Library/LaunchDaemons/local.fanpilot.plist
rm -f /Library/PrivilegedHelperTools/local.fanpilot
rm -f /etc/newsyslog.d/local.fanpilot.conf
rm -f '/Library/Application Support/Fanpilot/config.plist'
rm -f /var/run/fanpilot/status.json /var/run/fanpilot/lock
rmdir '/Library/Application Support/Fanpilot' /var/run/fanpilot 2>/dev/null || true
printf '%s\n' 'Fanpilot removed. Original fan minimums restored; baseline retained in /var/db/fanpilot.'
