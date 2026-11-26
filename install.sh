set -eu

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run make install from the project directory.' >&2
    exit 77
fi

project=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=/Library/PrivilegedHelperTools/local.fanpilot
configuration='/Library/Application Support/Fanpilot/config.plist'
service=/Library/LaunchDaemons/local.fanpilot.plist
rotation=/etc/newsyslog.d/local.fanpilot.conf

"$project/build/fanpilot" validate "$project/config/fanpilot.plist"
/usr/bin/codesign --verify --strict "$project/build/fanpilot"
for directory in '/Library/Application Support/Fanpilot' /var/db/fanpilot /var/run/fanpilot; do
    if [ -L "$directory" ]; then
        printf 'Refusing symbolic link: %s\n' "$directory" >&2
        exit 1
    fi
    if [ -e "$directory" ] && [ "$(stat -f '%u' "$directory")" -ne 0 ]; then
        printf 'Directory must belong to root: %s\n' "$directory" >&2
        exit 1
    fi
done
for file in "$binary" "$configuration" "$service" "$rotation" /var/log/fanpilot.log; do
    if [ -L "$file" ]; then
        printf 'Refusing symbolic link: %s\n' "$file" >&2
        exit 1
    fi
done
if [ -e "$configuration" ]; then
    "$project/build/fanpilot" validate "$configuration"
fi

backup=$(mktemp -d /private/tmp/fanpilot-install.XXXXXX)
active=0
if launchctl print system/local.fanpilot >/dev/null 2>&1; then active=1; fi
for item in binary configuration service rotation; do
    eval "file=\$$item"
    if [ -f "$file" ]; then cp -p "$file" "$backup/$item"; fi
done

rollback() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$result" -ne 0 ]; then
        launchctl bootout system/local.fanpilot >/dev/null 2>&1 || true
        for item in binary configuration service rotation; do
            eval "file=\$$item"
            if [ -f "$backup/$item" ]; then
                cp -p "$backup/$item" "$file"
            else
                rm -f "$file"
            fi
        done
        if [ "$active" -eq 1 ]; then launchctl bootstrap system "$service" || true; fi
        printf '%s\n' 'Installation failed; previous installation restored.' >&2
    fi
    rm -rf "$backup"
    exit "$result"
}
trap rollback EXIT
trap 'exit 1' HUP INT TERM

if [ "$active" -eq 1 ]; then launchctl bootout system/local.fanpilot; fi
"$project/build/fanpilot" verify
install -d -o root -g wheel -m 0755 '/Library/Application Support/Fanpilot' /var/run/fanpilot
install -d -o root -g wheel -m 0700 /var/db/fanpilot
install -o root -g wheel -m 0755 "$project/build/fanpilot" "$binary"
if [ ! -f "$configuration" ]; then
    install -o root -g wheel -m 0644 "$project/config/fanpilot.plist" "$configuration"
fi
chown root:wheel "$configuration"
chmod 0644 "$configuration"
install -o root -g wheel -m 0644 "$project/config/local.fanpilot.plist" "$service"
install -o root -g wheel -m 0644 "$project/config/local.fanpilot.conf" "$rotation"
touch /var/log/fanpilot.log
chown root:wheel /var/log/fanpilot.log
chmod 0640 /var/log/fanpilot.log
launchctl enable system/local.fanpilot
launchctl bootstrap system "$service"
attempt=0
until "$binary" status; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 10 ]; then exit 1; fi
    sleep 1
done
printf '%s\n' 'Fanpilot installed and running. Boot persistence enabled.'
