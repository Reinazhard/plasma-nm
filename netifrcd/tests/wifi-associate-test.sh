#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# netifrcd real-hardware Wi-Fi association test.
#
# This drives the *real* netifrcd daemon (not a re-implementation) through its
# org.freedesktop.NetworkManager D-Bus API and checks that an interface actually
# associates to a WPA2 access point via netifrc + wpa_supplicant:
#
#   netifrcd ActivateConnection
#     -> Profile::toNetifrc()      (writes config_<if> / modules_<if>="wpa_supplicant")
#     -> WifiScanner::writeNetwork (writes /etc/wpa_supplicant/wpa_supplicant-<if>.conf)
#     -> ServiceControl            (rc-service net.<if> start)
#     -> netifrc wpa_supplicant.sh (associates)
#     -> /etc/wpa_supplicant/wpa_cli.sh on CONNECTED (re-runs the service for DHCP)
#
# It is DESTRUCTIVE: it stops NetworkManager and iwd and takes over the wireless
# interface, so run it from a LOCAL console, not over SSH. Everything it touches
# is backed up first and restored on exit, including on failure or Ctrl-C.
#
# Usage:
#   sudo ./wifi-associate-test.sh                    # wlan0 -> Alfarozy1
#   sudo SSID=traumerei ./wifi-associate-test.sh     # a different SSID
#   sudo IFACE=wlp3s0 ./wifi-associate-test.sh       # a different interface
#   sudo NFTEST_YES=1 ./wifi-associate-test.sh       # skip the confirmation prompt
#   sudo PSK='...' ./wifi-associate-test.sh          # supply the key explicitly
#
# The PSK is read from iwd's store (/var/lib/iwd/<ssid>.psk) or from
# /etc/wpa_supplicant/wpa_supplicant.conf. Override with PSK=... if neither has it.

set -u
umask 077 # the seeded profile store holds the PSK

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)

IFACE=${IFACE:-wlan0}
SSID=${SSID:-Alfarozy1}
NETIFRCD=${NETIFRCD:-$REPO_DIR/build-netifrcd/netifrcd}

CONF=/etc/conf.d/net
STORE_DIR=/var/lib/netifrcd
STORE=$STORE_DIR/profiles.json
UUID=6f9a1c62-0000-4000-8000-00000000a551

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
c_ok=$'\033[1;32m'; c_bad=$'\033[1;31m'; c_info=$'\033[1;34m'; c_warn=$'\033[1;33m'; c_off=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$c_info" "$c_off" "$*"; }
warn() { printf '%s!!!%s %s\n' "$c_warn" "$c_off" "$*" >&2; }
die()  { printf '%sXXX%s %s\n' "$c_bad" "$c_off" "$*" >&2; exit 1; }
pass() { printf '%sPASS%s %s\n' "$c_ok" "$c_off" "$*"; }
fail() { printf '%sFAIL%s %s\n' "$c_bad" "$c_off" "$*"; }

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

extract_psk() {
    local ssid=$1 f psk
    for f in "/var/lib/iwd/$ssid.psk" "/var/lib/iwd/$ssid.8021x"; do
        [ -f "$f" ] || continue
        psk=$(sed -n 's/^[[:space:]]*PreSharedKey=//p' "$f" | head -1)
        [ -n "$psk" ] && { printf '%s' "$psk"; return 0; }
        psk=$(sed -n 's/^[[:space:]]*Passphrase=//p' "$f" | head -1)
        [ -n "$psk" ] && { printf '%s' "$psk"; return 0; }
    done
    if [ -f /etc/wpa_supplicant/wpa_supplicant.conf ]; then
        psk=$(awk -v want="$ssid" '
            /network[[:space:]]*=/ { inb=1; ss=""; ps=""; next }
            inb && /^[[:space:]]*ssid=/ { s=$0; sub(/^[[:space:]]*ssid=[[:space:]]*/,"",s); gsub(/^"|"[[:space:]]*$/,"",s); ss=s }
            inb && /^[[:space:]]*psk=/  { p=$0; sub(/^[[:space:]]*psk=[[:space:]]*/,"",p);  gsub(/^"|"[[:space:]]*$/,"",p); ps=p }
            inb && /}/ { if (ss==want && ps!="") { print ps; exit } inb=0 }
        ' /etc/wpa_supplicant/wpa_supplicant.conf)
        [ -n "$psk" ] && { printf '%s' "$psk"; return 0; }
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "must run as root (it stops NetworkManager/iwd and drives rc-service)"

for tool in iw wpa_cli wpa_supplicant rc-service dbus-daemon gdbus ip sed awk; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done
[ -x "$NETIFRCD" ] || die "netifrcd binary not found at $NETIFRCD
    build it first:  cmake -S '$REPO_DIR/netifrcd' -B '$REPO_DIR/build-netifrcd' && cmake --build '$REPO_DIR/build-netifrcd'"
[ -d "/sys/class/net/$IFACE/wireless" ] || [ -d "/sys/class/net/$IFACE/phy80211" ] \
    || die "$IFACE does not look like a wireless interface"

if [ -n "${SSH_CONNECTION:-}" ] && [ "${NFTEST_YES:-0}" != 1 ]; then
    die "running over SSH (SSH_CONNECTION set): stopping the network will drop your session.
    Re-run from a local console, or set NFTEST_YES=1 if you really mean it."
fi

warn "NetworkManager and iwd will be STOPPED and $IFACE taken over."
warn "Your network drops for the duration of the test (restored on exit)."

if [ "${NFTEST_YES:-0}" != 1 ]; then
    [ -t 0 ] || die "not a TTY and NFTEST_YES is not set; refusing to proceed blindly"
    printf '%sContinue? [y/N] %s' "$c_warn" "$c_off"
    read -r reply
    case "$reply" in y|Y|yes|YES) ;; *) die "aborted" ;; esac
fi

# ---------------------------------------------------------------------------
# PSK
# ---------------------------------------------------------------------------
PSK=${PSK:-}
if [ -z "$PSK" ]; then
    PSK=$(extract_psk "$SSID" || true)
fi
if [ -z "$PSK" ] && [ -t 0 ]; then
    printf '%sWPA passphrase for "%s": %s' "$c_warn" "$SSID" "$c_off"
    read -r -s PSK
    echo
fi
[ -n "$PSK" ] || die "no PSK for '$SSID'. Pass PSK=... or store the network in iwd/wpa_supplicant first."

# ---------------------------------------------------------------------------
# Backup + cleanup
# ---------------------------------------------------------------------------
WORK=$(mktemp -d /run/nftest.XXXXXX)
BUS_SOCK=$WORK/bus
BUS_ADDR="unix:path=$BUS_SOCK"
WPACONF=/etc/wpa_supplicant/wpa_supplicant-$IFACE.conf
LOGF=$WORK/netifrcd.log

[ -f "$CONF" ]    && cp -a "$CONF"  "$WORK/conf.d.net"
[ -f "$STORE" ]   && cp -a "$STORE" "$WORK/profiles.json"
[ -e "$WPACONF" ] && cp -a "$WPACONF" "$WORK/wpa.conf"

cleanup() {
    set +e
    echo
    log "cleaning up"

    # Kill the supplicant and its wpa_cli action-file daemon FIRST. wpa_cli runs
    # /etc/wpa_supplicant/wpa_cli.sh as a persistent daemon, and that script runs
    # `net.<if> start` on CONNECTED - so a still-live one can re-mark the service
    # "started" right after we stop it, breaking the next run.
    for pf in "/run/wpa_cli-$IFACE.pid" "/run/wpa_supplicant-$IFACE.pid"; do
        [ -f "$pf" ] && kill "$(cat "$pf")" 2>/dev/null
    done
    pkill -f "wpa_cli.*-i *$IFACE" 2>/dev/null
    pkill -f "wpa_supplicant.*-i *$IFACE" 2>/dev/null

    # Now stop whatever netifrc started for the interface.
    rc-service "net.$IFACE" stop >/dev/null 2>&1

    # Stop our private daemons.
    [ -f "$WORK/netifrcd.pid" ] && kill "$(cat "$WORK/netifrcd.pid")" 2>/dev/null
    [ -f "$WORK/dbus.pid" ]     && kill "$(cat "$WORK/dbus.pid")" 2>/dev/null
    sleep 1
    [ -f "$WORK/netifrcd.pid" ] && kill -9 "$(cat "$WORK/netifrcd.pid")" 2>/dev/null

    # Restore every file we touched.
    if [ -f "$WORK/conf.d.net" ]; then cp -a "$WORK/conf.d.net" "$CONF"; fi
    if [ -f "$WORK/profiles.json" ]; then
        cp -a "$WORK/profiles.json" "$STORE"
    else
        rm -f "$STORE"
    fi
    if [ -f "$WORK/wpa.conf" ]; then cp -a "$WORK/wpa.conf" "$WPACONF"; else rm -f "$WPACONF"; fi

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
# Remember which phy the interface lives on, so we can put the netdev back if
# stopping the daemons removes it (see the recreate block below).
PHY_NUM=$(iw dev "$IFACE" info 2>/dev/null | sed -n 's/^[[:space:]]*wiphy \([0-9][0-9]*\)$/\1/p' | head -1)
log "interfaces before takeover: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"

rc-service NetworkManager stop >/dev/null 2>&1
rc-service iwd stop >/dev/null 2>&1
sleep 1

# Any supplicant left over from NetworkManager would still own the radio.
pkill -f "wpa_supplicant.*-i *$IFACE" 2>/dev/null

# netifrc can only configure an interface that exists. On some setups the wireless
# netdev is created/owned by iwd or NetworkManager and disappears when they stop;
# if that happens, recreate it on the same phy so netifrcd + netifrc have something
# to drive. (Without this, net.wlan0 fails with "interface wlan0 does not exist".)
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
    log "$IFACE is back (ifindex $(cat "/sys/class/net/$IFACE/ifindex" 2>/dev/null))"
else
    log "$IFACE survived the takeover (ifindex $(cat "/sys/class/net/$IFACE/ifindex" 2>/dev/null))"
fi

ip addr flush dev "$IFACE" 2>/dev/null
ip link set "$IFACE" up 2>/dev/null

# net.<if> lives in the default runlevel, and a previous run can leave it marked
# "started" even though nothing is connected (its wpa_cli action file runs
# `net.<if> start` on CONNECTED and never stops it on DISCONNECTED). netifrc's start
# would then no-op with "has already been started" and nothing would associate, so
# clear any non-stopped state before handing the interface to netifrcd.
svc_state=$(rc-service "net.$IFACE" status 2>/dev/null | sed -n 's/.*status:[[:space:]]*//p' | head -1)
if [ -n "$svc_state" ] && [ "$svc_state" != "stopped" ]; then
    log "resetting net.$IFACE (was '$svc_state') so netifrcd's start actually runs"
    rc-service "net.$IFACE" stop >/dev/null 2>&1
fi

# netifrcd only rewrites an interface's netifrc config when the interface is not
# configured yet, or when it already appears in its own managed block
# (Backend::activateConnection). $IFACE is pre-configured in /etc/conf.d/net on
# this machine, so neutralise config_<if> for the duration of the test (restored
# by cleanup) to represent "netifrcd owns this interface".
if grep -qE "^config_$IFACE=" "$CONF" 2>/dev/null; then
    log "temporarily disabling config_$IFACE in $CONF so netifrcd will manage $IFACE"
    sed -i "s|^config_$IFACE=.*|# nftest: temporarily disabled|" "$CONF"
fi

log "starting a private D-Bus and netifrcd"
dbus-daemon --session --fork --address="$BUS_ADDR" --print-pid=1 > "$WORK/dbus.pid" 2>/dev/null \
    || die "could not start a private dbus-daemon"
for _ in $(seq 50); do [ -S "$BUS_SOCK" ] && break; sleep 0.1; done

mkdir -p "$STORE_DIR"
cat > "$STORE" <<EOF
{
  "version": 1,
  "profiles": [
    {
      "uuid": "$UUID",
      "id": "nftest-$SSID",
      "type": "802-11-wireless",
      "interfaceName": "$IFACE",
      "autoconnect": true,
      "timestamp": 0,
      "ipv4Method": "auto",
      "addresses": [],
      "gateway": "",
      "dns": [],
      "ssid": "$SSID",
      "psk": "$(json_escape "$PSK")",
      "hidden": false
    }
  ]
}
EOF

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
# Activate
# ---------------------------------------------------------------------------
log "resolving the connection object for uuid $UUID"
OUT=$(gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
        --object-path /org/freedesktop/NetworkManager/Settings \
        --method org.freedesktop.NetworkManager.Settings.GetConnectionByUuid "$UUID") \
    || die "GetConnectionByUuid failed"
CONN=$(printf '%s' "$OUT" | sed -n "s/.*'\(.*\)'.*/\1/p")
[ -n "$CONN" ] || die "could not parse a connection path out of: $OUT"
log "connection is $CONN"

log "calling ActivateConnection($CONN, /, /)"
gdbus call --address "$BUS_ADDR" --dest org.freedesktop.NetworkManager \
    --object-path /org/freedesktop/NetworkManager \
    --method org.freedesktop.NetworkManager.ActivateConnection "$CONN" "/" "/" \
    || warn "ActivateConnection returned an error (see the log below)"

# ---------------------------------------------------------------------------
# Wait for association, then for the lease
# ---------------------------------------------------------------------------
log "waiting for association (up to 90s)"
state=""
for _ in $(seq 90); do
    state=$(wpa_cli -i "$IFACE" status 2>/dev/null | sed -n 's/^wpa_state=//p')
    [ "$state" = COMPLETED ] && break
    sleep 1
done

# Association and DHCP are separate steps: netifrc's wpa_supplicant module returns as
# soon as the supplicant is up, and its wpa_cli action file re-runs the service on
# CONNECTED to do DHCP. Wait for the lease so we do not report a false negative.
addr=""
if [ "$state" = COMPLETED ]; then
    log "associated; waiting for a DHCP lease (up to 30s)"
    for _ in $(seq 30); do
        addr=$(ip -4 -o addr show dev "$IFACE" 2>/dev/null | awk '{print $4; exit}')
        [ -n "$addr" ] && break
        sleep 1
    done
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
echo "===================== artifacts ====================="
echo "--- /etc/conf.d/net (netifrcd managed block) ---"
sed -n '/netifrcd managed block >>>/,/netifrcd managed block <<</p' "$CONF" 2>/dev/null || echo "(no managed block written)"
echo "--- $WPACONF ---"
sed 's/psk=.*/psk=<redacted>/' "$WPACONF" 2>/dev/null || echo "(not written)"
echo "--- netifrcd log ---"
sed 's/psk=.*/psk=<redacted>/' "$LOGF" 2>/dev/null

echo
echo "===================== results ======================="
echo "--- wpa_cli -i $IFACE status ---"
wpa_cli -i "$IFACE" status 2>/dev/null | sed 's/^/    /'
echo "--- iw dev $IFACE link ---"
iw dev "$IFACE" link 2>/dev/null | sed 's/^/    /'
echo "--- ip addr / route ---"
ip -4 addr show dev "$IFACE" 2>/dev/null | sed 's/^/    /'
ip -4 route show dev "$IFACE" 2>/dev/null | sed 's/^/    /'

echo
rc=0
if sed -n '/netifrcd managed block >>>/,/netifrcd managed block <<</p' "$CONF" 2>/dev/null | grep -q 'modules_.*wpa_supplicant'; then
    pass "netifrcd wrote the wireless module into /etc/conf.d/net"
else
    fail "netifrcd did not enable netifrc's wireless module (modules_<if>=wpa_supplicant)"
    rc=1
fi
if [ "$state" = COMPLETED ]; then
    pass "wpa_supplicant associated ($IFACE -> $SSID)"
else
    fail "wpa_supplicant did not associate (wpa_state='${state:-<none>}')"
    rc=1
fi
if iw dev "$IFACE" link 2>/dev/null | grep -q 'Connected to'; then
    pass "kernel reports an association on $IFACE"
else
    fail "kernel reports no association on $IFACE"
    rc=1
fi

if [ -n "$addr" ]; then
    gw=$(ip -4 route show default dev "$IFACE" 2>/dev/null | awk '{print $3; exit}')
    if [ -n "$gw" ] && ping -c 2 -W 3 "$gw" >/dev/null 2>&1; then
        pass "DHCP lease $addr obtained and the gateway $gw answers"
    else
        pass "DHCP lease $addr obtained"
        [ -n "$gw" ] && warn "got a default route via $gw but it does not answer ping"
    fi
else
    warn "no DHCP lease on $IFACE (association still counts as success)"
fi

echo
if [ "$rc" = 0 ]; then
    printf '%sRESULT: Wi-Fi association works on real hardware.%s\n' "$c_ok" "$c_off"
else
    printf '%sRESULT: association FAILED - see the log above.%s\n' "$c_bad" "$c_off"
fi
exit "$rc"
