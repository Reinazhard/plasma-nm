#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# netifrcd Device.Statistics test.
#
# NetworkManager implements org.freedesktop.NetworkManager.Device.Statistics on the
# *device object itself* (there is no separate "Statistics" object path to follow),
# with three properties:
#
#   RefreshRateMs  u   read/write   polling period the client asks for; 0 = no polling
#   RxBytes        t   read         bytes received
#   TxBytes        t   read         bytes transmitted
#
# netifrcd mirrors that with DeviceStatisticsAdaptor: an adaptor on DeviceObject, so
# it lands on the device path, counters read from /sys/class/net/<if>/statistics, and
# a timer that only emits PropertiesChanged when a counter actually changed (which is
# what NetworkManager promises for a non-zero RefreshRateMs).
#
# This test is NON-DESTRUCTIVE and does NOT need root: it runs netifrcd on a private
# D-Bus with a scratch netifrc config, so the live NetworkManager (if any) is left
# completely alone. It reads the real interface's counters.
#
# Usage:
#   ./statistics-test.sh                    # interface with the default route
#   IFACE=eth0 ./statistics-test.sh         # a specific interface
#   NETIFRCD=/path/to/netifrcd ./statistics-test.sh

set -u

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)

NETIFRCD=${NETIFRCD:-$REPO_DIR/build-netifrcd/netifrcd}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
c_ok=$'\033[1;32m'; c_bad=$'\033[1;31m'; c_info=$'\033[1;34m'; c_warn=$'\033[1;33m'; c_off=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$c_info" "$c_off" "$*"; }
warn() { printf '%s!!!%s %s\n' "$c_warn" "$c_off" "$*" >&2; }
die()  { printf '%sXXX%s %s\n' "$c_bad" "$c_off" "$*" >&2; exit 1; }
pass() { printf '%sPASS%s %s\n' "$c_ok" "$c_off" "$*"; pass_n=$((pass_n + 1)); }
fail() { printf '%sFAIL%s %s\n' "$c_bad" "$c_off" "$*"; fail_n=$((fail_n + 1)); }
skip() { printf '%sSKIP%s %s\n' "$c_warn" "$c_off" "$*"; }
pass_n=0; fail_n=0

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
for tool in dbus-daemon gdbus ip sed awk; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done
[ -x "$NETIFRCD" ] || die "netifrcd binary not found at $NETIFRCD
    build it first:  cmake -S '$REPO_DIR/netifrcd' -B '$REPO_DIR/build-netifrcd' && cmake --build '$REPO_DIR/build-netifrcd'"

detect_iface() {
    local dev
    dev=$(ip -o route show default 2>/dev/null \
        | awk '{for (i = 1; i <= NF; i++) if ($i == "dev") print $(i + 1)}' | head -1)
    if [ -n "$dev" ] && [ -e "/sys/class/net/$dev/statistics/rx_bytes" ]; then
        echo "$dev"; return
    fi
    for p in /sys/class/net/*; do
        dev=${p##*/}
        [ "$dev" = lo ] && continue
        [ -e "$p/statistics/rx_bytes" ] && { echo "$dev"; return; }
    done
}

IFACE=${IFACE:-$(detect_iface)}
[ -n "$IFACE" ] || die "could not pick an interface; pass IFACE=..."
[ -e "/sys/class/net/$IFACE/statistics/rx_bytes" ] \
    || die "$IFACE has no /sys/class/net/$IFACE/statistics/rx_bytes"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/netifrcd-statistics.XXXXXX")
BUS_SOCK=$WORK/bus
BUS_ADDR=unix:path=$BUS_SOCK
LOGF=$WORK/netifrcd.log

cleanup() {
    [ -f "$WORK/netifrcd.pid" ] && kill "$(cat "$WORK/netifrcd.pid")" 2>/dev/null
    [ -f "$WORK/dbus.pid" ] && kill "$(cat "$WORK/dbus.pid")" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Ground truth: real NetworkManager, if it happens to be running.
# ---------------------------------------------------------------------------
REAL_DEV=""
for d in $(busctl --system call org.freedesktop.NetworkManager /org/freedesktop/NetworkManager \
        org.freedesktop.NetworkManager GetDevices 2>/dev/null \
        | grep -oE '/org/freedesktop/NetworkManager/Devices/[0-9]+'); do
    if [ "$(busctl --system get-property org.freedesktop.NetworkManager "$d" \
            org.freedesktop.NetworkManager.Device Interface 2>/dev/null | sed 's/^s //')" = "\"$IFACE\"" ]; then
        REAL_DEV=$d; break
    fi
done

# ---------------------------------------------------------------------------
# Start netifrcd on a private bus, pointed at scratch files.
# ---------------------------------------------------------------------------
log "interface: $IFACE (sysfs rx=$(cat "/sys/class/net/$IFACE/statistics/rx_bytes") tx=$(cat "/sys/class/net/$IFACE/statistics/tx_bytes"))"
log "starting a private D-Bus and netifrcd"
dbus-daemon --session --fork --address="$BUS_ADDR" --print-pid=1 > "$WORK/dbus.pid" 2>/dev/null \
    || die "could not start a private dbus-daemon"
for _ in $(seq 50); do [ -S "$BUS_SOCK" ] && break; sleep 0.1; done

NETIFRCD_ALLOW_NONROOT=1 \
NETIFRCD_NETIFRC_CONF=$WORK/net \
NETIFRCD_PROFILE_STORE=$WORK/profiles.json \
DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDR" \
    "$NETIFRCD" > "$LOGF" 2>&1 &
echo $! > "$WORK/netifrcd.pid"
daemon_alive() { kill -0 "$(cat "$WORK/netifrcd.pid" 2>/dev/null)" 2>/dev/null; }

owned=0
for _ in $(seq 100); do
    if gdbus call --address "$BUS_ADDR" --dest org.freedesktop.DBus \
            --object-path /org/freedesktop/DBus \
            --method org.freedesktop.DBus.NameHasOwner org.freedesktop.NetworkManager 2>/dev/null | grep -q true; then
        owned=1; break
    fi
    sleep 0.2
done
[ "$owned" = 1 ] || { cat "$LOGF"; die "netifrcd did not claim org.freedesktop.NetworkManager"; }
log "netifrcd is up (pid $(cat "$WORK/netifrcd.pid"))"

nm_get() { # nm_get <object-path> <interface> <property>
    gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path "$1" --method org.freedesktop.DBus.Properties.Get "$2" "$3" 2>/dev/null
}

# Device ids depend on enumeration order, so find ours by Interface.
DEV=""
for d in $(gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path /org/freedesktop/NetworkManager \
        --method org.freedesktop.NetworkManager.GetDevices 2>/dev/null \
        | grep -oE '/org/freedesktop/NetworkManager/Devices/[0-9]+'); do
    # gdbus renders a string as ('wlan0',); anchor on the quoted name.
    if nm_get "$d" org.freedesktop.NetworkManager.Device Interface | grep -q "'$IFACE'"; then
        DEV=$d; break
    fi
done
[ -n "$DEV" ] || { cat "$LOGF"; die "netifrcd did not export a device for $IFACE"; }

STATS_IFACE=org.freedesktop.NetworkManager.Device.Statistics
INTRO=$(gdbus introspect --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path "$DEV" 2>/dev/null)

# ---------------------------------------------------------------------------
# 1. The interface is on the device path, not a path of its own.
# ---------------------------------------------------------------------------
if echo "$INTRO" | grep -q "interface $STATS_IFACE"; then
    pass "Device.Statistics is exported on the device path ($DEV)"
else
    fail "Device.Statistics is missing on $DEV"
fi

# ---------------------------------------------------------------------------
# 2. Property names and D-Bus types match real NetworkManager exactly.
# ---------------------------------------------------------------------------
shape_gdbus() {
    sed -nE 's/^[[:space:]]*(readonly|readwrite)[[:space:]]+([a-z]+)[[:space:]]+([A-Za-z0-9_]+).*/\3 \2/p' | sort
}
MINE_SHAPE=$(echo "$INTRO" | sed -n '/Device.Statistics/,/^  };/p' | shape_gdbus)
if [ -n "$REAL_DEV" ]; then
    REAL_SHAPE=$(busctl --system introspect org.freedesktop.NetworkManager "$REAL_DEV" "$STATS_IFACE" 2>/dev/null \
        | sed -nE 's/^\.([A-Za-z0-9_]+)[[:space:]]+property[[:space:]]+([a-z]+).*/\1 \2/p' | sort)
    printf '    real NM  : %s\n' "$(echo "$REAL_SHAPE" | tr '\n' ' ')"
    printf '    netifrcd : %s\n' "$(echo "$MINE_SHAPE" | tr '\n' ' ')"
    if [ -n "$REAL_SHAPE" ] && [ "$REAL_SHAPE" = "$MINE_SHAPE" ]; then
        pass "property names and D-Bus types match real NetworkManager"
    else
        fail "property shape differs from real NetworkManager"
    fi
else
    printf '    netifrcd : %s\n' "$(echo "$MINE_SHAPE" | tr '\n' ' ')"
    skip "real NetworkManager is not running; cannot compare the shape against it"
fi

# ---------------------------------------------------------------------------
# 3. The counters are the kernel's own.
# ---------------------------------------------------------------------------
raw_get() { # gdbus renders these as (<uint64 86862220>,); anchor on the type tag,
            # because a bare [0-9]+ also matches the "64" inside "uint64".
    nm_get "$DEV" "$STATS_IFACE" "$1" | grep -oE 'uint(32|64) [0-9]+' | awk '{print $2}' | head -1
}
SYS_RX=$(cat "/sys/class/net/$IFACE/statistics/rx_bytes")
SYS_TX=$(cat "/sys/class/net/$IFACE/statistics/tx_bytes")
NM_RX=$(raw_get RxBytes)
NM_TX=$(raw_get TxBytes)
printf '    sysfs rx=%s tx=%s | netifrcd RxBytes=%s TxBytes=%s\n' "$SYS_RX" "$SYS_TX" "$NM_RX" "$NM_TX"
# The counter keeps ticking between the two reads, so allow a little drift.
if [ -n "$NM_RX" ] && [ "$NM_RX" -ge "$SYS_RX" ] && [ $((NM_RX - SYS_RX)) -lt 200000 ]; then
    pass "RxBytes tracks /sys/class/net/$IFACE/statistics/rx_bytes"
else
    fail "RxBytes ($NM_RX) does not match sysfs ($SYS_RX)"
fi
if [ -n "$NM_TX" ] && [ "$NM_TX" -ge "$SYS_TX" ] && [ $((NM_TX - SYS_TX)) -lt 200000 ]; then
    pass "TxBytes tracks /sys/class/net/$IFACE/statistics/tx_bytes"
else
    fail "TxBytes ($NM_TX) does not match sysfs ($SYS_TX)"
fi

# ---------------------------------------------------------------------------
# 4. GetManagedObjects lists it (libnm enumerates through this).
# ---------------------------------------------------------------------------
if gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path /org/freedesktop --method org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null \
        | grep -q "$STATS_IFACE"; then
    pass "GetManagedObjects lists Device.Statistics"
else
    fail "GetManagedObjects does not list Device.Statistics"
fi

# ---------------------------------------------------------------------------
# 5. RefreshRateMs is writable and drives updates when the counter moves.
# ---------------------------------------------------------------------------
set_rate() { # set_rate <ms>
    local out
    out=$(gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path "$DEV" --method org.freedesktop.DBus.Properties.Set \
        "$STATS_IFACE" RefreshRateMs "<uint32 $1>" 2>&1)
    # gdbus prints "()" for a successful void method, not an empty string.
    [ "$out" = "()" ]
}

if set_rate 1000; then
    pass "RefreshRateMs accepted a Set (1000 ms)"
else
    fail "RefreshRateMs rejected a Set"
fi
RATE=$(raw_get RefreshRateMs)
if [ "$RATE" = "1000" ]; then
    pass "RefreshRateMs reads back as 1000"
else
    fail "RefreshRateMs reads back as '$RATE', expected 1000"
fi

# Watch the device while generating traffic on it.
gdbus monitor --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager --object-path "$DEV" \
    > "$WORK/monitor.log" 2>&1 &
MON=$!
sleep 1
GATEWAY=$(ip -o route show default 2>/dev/null \
    | awk '{for (i = 1; i <= NF; i++) if ($i == "via") print $(i + 1)}' | head -1)
PINGED=0
if [ -n "$GATEWAY" ] && ping -c 6 -i 0.3 -W 2 "$GATEWAY" >/dev/null 2>&1; then
    PINGED=1
elif ping -c 6 -i 0.3 -W 2 1.1.1.1 >/dev/null 2>&1; then
    PINGED=1
fi
sleep 2
kill "$MON" 2>/dev/null
sleep 0.3

if grep -q "RxBytes\|TxBytes" "$WORK/monitor.log"; then
    printf '    %s\n' "$(grep -oE "(Rx|Tx)Bytes': <uint64 [0-9]+>" "$WORK/monitor.log" | head -4 | tr '\n' ' ')"
    pass "polling emitted PropertiesChanged for the byte counters"
elif [ "$PINGED" = 0 ]; then
    skip "no traffic could be generated on $IFACE, so no counter could change"
else
    fail "no PropertiesChanged for RxBytes/TxBytes while RefreshRateMs=1000"
fi

if set_rate 0 && [ "$(raw_get RefreshRateMs)" = "0" ]; then
    pass "RefreshRateMs back to 0 (polling stopped)"
else
    fail "RefreshRateMs did not return to 0"
fi

# A rate of zero means "no guaranteed refresh", so the timer must really be off:
# heavy traffic must produce no counter notifications at all.
gdbus monitor --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager --object-path "$DEV" \
    > "$WORK/monitor-idle.log" 2>&1 &
MON=$!
sleep 1
[ -n "$GATEWAY" ] && ping -c 8 -i 0.2 -W 2 "$GATEWAY" >/dev/null 2>&1
sleep 2
kill "$MON" 2>/dev/null
sleep 0.3
if grep -q "RxBytes\|TxBytes" "$WORK/monitor-idle.log"; then
    printf '    %s\n' "$(grep -oE "(Rx|Tx)Bytes': <uint64 [0-9]+>" "$WORK/monitor-idle.log" | head -4 | tr '\n' ' ')"
    fail "counter notifications were emitted while RefreshRateMs=0"
else
    pass "no counter notifications while RefreshRateMs=0 (timer is stopped)"
fi

# ---------------------------------------------------------------------------
daemon_alive || fail "netifrcd died during the run"

echo
if [ "$fail_n" = 0 ]; then
    printf '%sRESULT: %d passed, %d failed - Device.Statistics matches NetworkManager.%s\n' \
        "$c_ok" "$pass_n" "$fail_n" "$c_off"
else
    printf '%sRESULT: %d passed, %d failed.%s\n' "$c_bad" "$pass_n" "$fail_n" "$c_off"
    echo "--- netifrcd log ---"
    tail -30 "$LOGF"
fi
exit $((fail_n > 0))
