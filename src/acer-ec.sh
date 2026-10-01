#!/bin/bash
# acer-ec — EC-only fan/profile CLI (reads /sys/kernel/acer_fanctl).
# Installed by install.sh to /usr/local/bin/acer-ec.
set -euo pipefail

SYS=/sys/kernel/acer_fanctl

err() { echo "acer-ec: $1" >&2; exit 1; }

profile_name() {
    case "$1" in
        1) echo "quiet" ;;
        2) echo "balanced" ;;
        3) echo "performance" ;;
        4) echo "gaming" ;;
        *) echo "unknown" ;;
    esac
}

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

[ -e "$SYS/profile" ] || err "sysfs not found at $SYS — modules not loaded (run sudo ./install.sh from the acer-ec repo)"

cmd_status() {
    cat "$SYS/all"
}

cmd_profile_show() {
    local cur name
    cur=$(cat "$SYS/profile" 2>/dev/null || echo 0)
    name=$(profile_name "$cur")
    echo "Current profile: $name ($cur)"
    echo "Profiles: 1=quiet 2=balanced 3=performance 4=gaming"
    echo "Usage: acer-ec profile <1-4|name>"
}

cmd_profile_set() {
    local val want got name
    case "$1" in
        1|quiet)      val=1 ;;
        2|balanced)   val=2 ;;
        3|performance) val=3 ;;
        4|gaming)     val=4 ;;
        *)            err "Invalid profile. Use 1-4 or: quiet balanced performance gaming" ;;
    esac
    echo "$val" > "$SYS/profile"
    want="$val"
    got=$(cat "$SYS/profile" 2>/dev/null || echo 0)
    if [ "$got" != "$want" ]; then
        err "Profile write failed to land in EC (wanted $want, EC reports $got)"
    fi
    name=$(profile_name "$val")
    echo "Profile set to $name ($val)"
}

case "${1:-status}" in
    status|st)
        cmd_status
        ;;
    profile)
        if [ -z "${2:-}" ]; then
            cmd_profile_show
        else
            cmd_profile_set "$2"
        fi
        ;;
    help|--help|-h)
        echo "Usage: acer-ec [command]"
        echo ""
        echo "  acer-ec              Show status (default)"
        echo "  acer-ec status       Show fan/temp values"
        echo "  acer-ec profile      Show profile help"
        echo "  acer-ec profile 4    Set profile by number"
        echo "  acer-ec profile gaming  Set profile by name"
        echo "  acer-ec help         This message"
        ;;
    *)
        err "Unknown command: $1. Use: status, profile, help"
        ;;
esac
