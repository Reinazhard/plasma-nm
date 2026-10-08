#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# netifrcd real-hardware wireless auto-scan test.
#
# Verifies that the *real* netifrcd daemon populates Device.Wireless.AccessPoints
# on its own - no client ever calls RequestScan. netifrcd scans every usable
# wireless device once at startup and then on a timer (Backend::scanWirelessDevices),
# and records the time in Device.Wireless.LastScan.
#
#   netifrcd start()
#     -> QTimer::singleShot(0, scanWirelessDevices)   (initial scan)
#     -> m_scanTimer (60s) -> scanWirelessDevices     (periodic scan)
#     -> WifiScanner::scan() -> wpa_cli scan_results or `iw dev <if> scan`
#     -> refreshAccessPoints() -> LastScan = CLOCK_BOOTTIME seconds
#
# It is DESTRUCTIVE in the same way as wifi-associate-test.sh: it stops
# NetworkManager and iwd and takes the wireless interface over, so run it from a
# LOCAL console. Everything it touches is restored on exit, including on failure
# or Ctrl-C.
#
# Usage:
#   sudo ./autoscan-test.sh                       # wlan0
#   sudo IFACE=wlp3s0 ./autoscan-test.sh          # a different interface
#   sudo AUTOSCAN_YES=1 ./autoscan-test.sh        # skip the confirmation prompt
#   sudo AUTOSCAN_PERIODIC=1 ./autoscan-test.sh   # also wait ~70s for the 2nd scan

set -u

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)

IFACE=${IFACE:-wlan0}
NETIFRCD=${NETIFRCD:-$REPO_DIR/build-netifrcd/netifrcd}

# How long to wait for the startup scan to land in the D-Bus property.
STARTUP_TIMEOUT=${STARTUP_TIMEOUT:-30}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
c_ok=$'\033[1;32m'; c_bad=$'\033[1;31m'; c_info=$'\033[1;34m'; c_warn=$'\033[1;33m'; c_off=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$c_info" "$c_off" "$*"; }
warn() { printf '%s!!!%s %s\n' "$c_warn" "$c_off" "$*" >&2; }
die()  { printf '%sXXX%s %s\n' "$c_bad" "$c_off" "$*" >&2; exit 1; }
pass() { printf '%sPASS%s %s\n' "$c_ok" "$c_off" "$*"; }
fail() { printf '%sFAIL%s %s\n' "$c_bad" "$c_off" "$*"; }

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "must run as root (it stops NetworkManager/iwd and scans the radio)"

for tool in iw rc-service dbus-daemon gdbus ip sed awk; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done
[ -x "$NETIFRCD" ] || die "netifrcd binary not found at $NETIFRCD
    build it first:  cmake -S '$REPO_DIR/netifrcd' -B '$REPO_DIR/build-netifrcd' && cmake --build '$REPO_DIR/build-netifrcd'"
[ -d "/sys/class/net/$IFACE/wireless" ] || [ -d "/sys/class/net/$IFACE/phy80211" ] \
    || die "$IFACE does not look like a wireless interface"

if [ -n "${SSH_CONNECTION:-}" ] && [ "${AUTOSCAN_YES:-0}" != 1 ]; then
    die "running over SSH (SSH_CONNECTION set): stopping the network will drop your session.
    Re-run from a local console, or set AUTOSCAN_YES=1 if you really mean it."
fi

warn "NetworkManager and iwd will be STOPPED and $IFACE taken over."
warn "Your network drops for the duration of the test (restored on exit)."

if [ "${AUTOSCAN_YES:-0}" != 1 ]; then
    [ -t 0 ] || die "not a TTY and AUTOSCAN_YES is not set; refusing to proceed blindly"
    printf '%sContinue? [y/N] %s' "$c_warn" "$c_off"
    read -r reply
    case "$reply" in y|Y|yes|YES) ;; *) die "aborted" ;; esac
fi

# ---------------------------------------------------------------------------
# Backup + cleanup
# ---------------------------------------------------------------------------
WORK=$(mktemp -d /run/nftest.XXXXXX)
BUS_SOCK=$WORK/bus
BUS_ADDR="unix:path=$BUS_SOCK"
LOGF=$WORK/netifrcd.log

cleanup() {
    set +e
    echo
    log "cleaning up"

    # Stop our private daemons first so nothing keeps scanning the radio.
    [ -f "$WORK/netifrcd.pid" ] && kill "$(cat "$WORK/netifrcd.pid")" 2>/dev/null
    [ -f "$WORK/dbus.pid" ]     && kill "$(cat "$WORK/dbus.pid")" 2>/dev/null
    sleep 1
    [ -f "$WORK/netifrcd.pid" ] && kill -9 "$(cat "$WORK/netifrcd.pid")" 2>/dev/null

    # Any supplicant netifrc may have left behind still owns the radio.
    pkill -f "wpa_supplicant.*-i *$IFACE" 2>/dev/null

    # Give the interface back.
    ip addr flush dev "$IFACE" 2>/dev/null
    ip link set "$IFACE" down 2>/dev/null

    log "restarting iwd and NetworkManager"
    rc-service iwd start >/dev/null 2>&1
    rc-service NetworkManager start >/dev/null 2>&1
    for _ in $(seq 40); do
        iw dev "$IFACE" link 2>/dev/null | grep -q 'Connected to' && break
        sleep 1
    done
    log "post-cleanup state of $IFACE:"
    iw dev "$IFACE" link 2>/dev/null | sed 's/^/    /'
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Take the interface over
# ---------------------------------------------------------------------------
log "stopping NetworkManager and iwd"
PHY_NUM=$(iw dev "$IFACE" info 2>/dev/null | sed -n 's/^[[:space:]]*wiphy \([0-9][0-9]*\)$/\1/p' | head -1)
log "interfaces before takeover: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"

rc-service NetworkManager stop >/dev/null 2>&1
rc-service iwd stop >/dev/null 2>&1
sleep 1

pkill -f "wpa_supplicant.*-i *$IFACE" 2>/dev/null

# On some setups the wireless netdev is created/owned by iwd or NetworkManager and
# disappears when they stop; recreate it on the same phy so the scan has a radio.
if [ ! -e "/sys/class/net/$IFACE" ]; then
    warn "$IFACE disappeared when iwd/NetworkManager stopped; recreating it"
    if [ -n "$PHY_NUM" ]; then
        phy="phy$PHY_NUM"
    else
        phy=$(iw phy 2>/dev/null | sed -n 's/^Wiphy \(phy[0-9]*\)$/\1/p' | head -1)
    fi
    [ -n "$phy" ] || die "$IFACE is gone and no wireless phy is available (driver unloaded?)"
    log "recreating $IFACE on $phy"
    iw phy "$phy" interface add "$IFACE" type managed 2>/dev/null \
        || die "could not recreate $IFACE on $phy"
    for _ in $(seq 30); do [ -e "/sys/class/net/$IFACE" ] && break; sleep 0.1; done
    [ -e "/sys/class/net/$IFACE" ] || die "recreated $IFACE on $phy but it did not appear"
fi
log "$IFACE is present (ifindex $(cat "/sys/class/net/$IFACE/ifindex" 2>/dev/null))"

# A scan needs the interface administratively up. No association is required: the
# auto-scan path is independent of activation, which is exactly what we are testing.
ip addr flush dev "$IFACE" 2>/dev/null
ip link set "$IFACE" up 2>/dev/null
for _ in $(seq 30); do
    [ "$(cat "/sys/class/net/$IFACE/operstate" 2>/dev/null)" != down ] && break
    sleep 0.2
done

log "starting a private D-Bus and netifrcd"
dbus-daemon --session --fork --address="$BUS_ADDR" --print-pid=1 > "$WORK/dbus.pid" 2>/dev/null \
    || die "could not start a private dbus-daemon"
for _ in $(seq 50); do [ -S "$BUS_SOCK" ] && break; sleep 0.1; done

DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDR" QT_LOGGING_RULES="netifrcd.debug=true" \
    "$NETIFRCD" > "$LOGF" 2>&1 &
echo $! > "$WORK/netifrcd.pid"

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
log "netifrcd is up"

# ---------------------------------------------------------------------------
# Find the Device object for $IFACE (ids depend on enumeration order)
# ---------------------------------------------------------------------------
nm_get() { # nm_get <object-path> <interface> <property>
    gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path "$1" --method org.freedesktop.DBus.Properties.Get "$2" "$3" 2>/dev/null
}

# gdbus prints an int64 property as "(<int64 165607>,)". Extract the number by
# anchoring on the "int64 " tag: a plain "greedy .* then capture digits" sed grabs
# only the LAST digit (164790 -> 0, -1 -> 1), which silently breaks comparisons.
nm_get_int() { # nm_get_int <object-path> <interface> <property>
    nm_get "$1" "$2" "$3" | grep -oE 'int64 -?[0-9]+' | grep -oE -- '-?[0-9]+$' | head -1
}

# True while the daemon under test is still running.
daemon_alive() { kill -0 "$(cat "$WORK/netifrcd.pid" 2>/dev/null)" 2>/dev/null; }

dev_path=""
for _ in $(seq 50); do
    devices=$(gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
                --object-path /org/freedesktop/NetworkManager \
                --method org.freedesktop.DBus.Properties.Get \
                org.freedesktop.NetworkManager Devices 2>/dev/null \
              | grep -o "/org/freedesktop/NetworkManager/Devices/[0-9]*")
    for p in $devices; do
        iface=$(nm_get "$p" org.freedesktop.NetworkManager.Device Interface | sed -n "s/.*'\(.*\)'.*/\1/p")
        if [ "$iface" = "$IFACE" ]; then dev_path=$p; break; fi
    done
    [ -n "$dev_path" ] && break
    sleep 0.2
done
[ -n "$dev_path" ] || { cat "$LOGF"; die "no Device object for $IFACE on the bus"; }
log "device object is $dev_path"

# ---------------------------------------------------------------------------
# Watch the access-point list fill in - without ever calling RequestScan
# ---------------------------------------------------------------------------
log "waiting up to ${STARTUP_TIMEOUT}s for the startup auto-scan (RequestScan is never called)"
aps=""
lastscan=-1
for _ in $(seq "$STARTUP_TIMEOUT"); do
    aps=$(nm_get "$dev_path" org.freedesktop.NetworkManager.Device.Wireless AccessPoints)
    lastscan=$(nm_get_int "$dev_path" org.freedesktop.NetworkManager.Device.Wireless LastScan)
    [ -n "$lastscan" ] || lastscan=-1
    # AccessPoints is "ao" (array of object paths); a non-empty list has at least one.
    if printf '%s' "$aps" | grep -q "AccessPoint/[0-9]"; then
        break
    fi
    sleep 1
done

ap_count=$(printf '%s' "$aps" | grep -o "AccessPoint/[0-9]*" | sort -u | wc -l)
log "AccessPoints = $aps"
log "LastScan     = $lastscan"
log "distinct access points: $ap_count"

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
echo "===================== artifacts ====================="
echo "--- netifrcd log ---"
sed 's/psk=.*/psk=<redacted>/' "$LOGF" 2>/dev/null

echo
echo "===================== results ======================="
rc=0

# 1. The daemon scanned on its own: LastScan must be a real CLOCK_BOOTTIME value.
if [ "$lastscan" -gt 0 ] 2>/dev/null; then
    pass "netifrcd scanned without any RequestScan (LastScan = ${lastscan}s since boot)"
else
    fail "netifrcd never recorded a scan (LastScan = ${lastscan:-<none>}); the auto-scan did not run or failed"
    rc=1
fi

# 2. The scan produced access points, so the applet has something to show.
if [ "$ap_count" -gt 0 ]; then
    pass "Device.Wireless.AccessPoints is populated ($ap_count AP(s)) with no client scan"
else
    fail "Device.Wireless.AccessPoints is still empty after the auto-scan"
    rc=1
fi

# 3. The access points are reachable as objects (the applet introspects each one).
if [ "$ap_count" -gt 0 ]; then
    first_ap=$(printf '%s' "$aps" | grep -o "/org/freedesktop/NetworkManager/AccessPoint/[0-9]*" | head -1)
    if [ -n "$first_ap" ] && nm_get "$first_ap" org.freedesktop.NetworkManager.AccessPoint Ssid >/dev/null; then
        pass "access point objects are exported and readable ($first_ap)"
    else
        fail "access point paths are listed but the objects are not readable"
        rc=1
    fi
fi

# 4. Optional: the periodic timer rescans, so LastScan advances on its own.
if [ "${AUTOSCAN_PERIODIC:-0}" = 1 ]; then
    # The timer interval is 60s, so 80s is enough to see exactly one rescan.
    log "waiting up to 80s for the periodic scan (interval 60s) to advance LastScan"
    before=$lastscan
    advanced=0
    died=0
    for _ in $(seq 80); do
        daemon_alive || { died=1; break; }
        now=$(nm_get_int "$dev_path" org.freedesktop.NetworkManager.Device.Wireless LastScan)
        if [ -n "$now" ] && [ "$now" -gt "$before" ] 2>/dev/null; then advanced=1; lastscan=$now; break; fi
        sleep 1
    done
    if [ "$advanced" = 1 ]; then
        pass "the periodic timer rescanned on its own (LastScan ${before} -> ${lastscan})"
    elif [ "$died" = 1 ]; then
        fail "netifrcd exited during the periodic wait (crashed?); LastScan stalled at ${before}"
        rc=1
    else
        fail "LastScan did not advance past ${before}; the periodic scan timer did not fire"
        rc=1
    fi
fi

echo
if [ "$rc" = 0 ]; then
    printf '%sRESULT: automatic scanning populates access points on real hardware.%s\n' "$c_ok" "$c_off"
else
    printf '%sRESULT: auto-scan FAILED - see the log above.%s\n' "$c_bad" "$c_off"
fi
exit "$rc"
