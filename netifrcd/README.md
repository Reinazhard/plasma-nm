# netifrcd

`netifrcd` is a NetworkManager-compatible D-Bus daemon backed by **netifrc** —
Gentoo/OpenRC's shell-based networking (`/etc/conf.d/net` plus
`/etc/init.d/net.*`). It implements the `org.freedesktop.NetworkManager` D-Bus
API on the **system bus** so that unmodified NetworkManager clients work on a
netifrc/OpenRC machine instead of NetworkManager.

## Architecture in one line

> netifrcd reimplements the NetworkManager D-Bus API on top of netifrc, so
> NetworkManager clients (the KDE Plasma applet) work unchanged.

**Why this is possible:** NetworkManagerQt — the library the Plasma
NetworkManager applet is built on — is a *pure D-Bus client*. It does not link
against NetworkManager's internals or talk to any private socket; it only speaks
the documented `org.freedesktop.NetworkManager` D-Bus interfaces over the system
bus. As long as a process owns that bus name and answers those interfaces
correctly, the applet cannot tell whether the backend is NetworkManager or
netifrcd. netifrcd answers those interfaces by driving netifrc:

- Interface discovery maps to netifrc interface services (`net.eth0`, …).
- Connect/disconnect maps to starting/stopping the matching `net.*` service.
- Configuration is written into netifrc's own files, not into a private store.
- Wi-Fi scanning/association goes through netifrc's `wpa_supplicant` module.

## Build

`netifrcd` is a standalone CMake project. From the repository root:

```sh
cmake -S netifrcd -B build-netifrcd
cmake --build build-netifrcd
```

Requirements: a C++20 compiler, Qt6 (`Core`, `DBus`, `Network`), and libnm
development headers (for the authoritative `nm-dbus-interface.h`). It can also
be built as part of plasma-nm with `-DBUILD_NETIFRCD=ON`.

## Switching over from NetworkManager

Both NetworkManager and netifrcd claim the same bus name
(`org.freedesktop.NetworkManager`), and only one process can own it. Stop and
disable NetworkManager (and `iwd`, which may also be managing the radio) before
starting netifrcd.

```sh
# Stop and disable the services netifrcd replaces.
rc-service NetworkManager stop
rc-update del NetworkManager default

rc-service iwd stop
rc-update del iwd default

# Install the service and start netifrcd.
rc-update add netifrcd default
rc-service netifrcd start
```

Verify that netifrcd now owns the name and answers the API:

```sh
busctl --system list | grep NetworkManager
nmcli general status
```

The first command should show `org.freedesktop.NetworkManager` owned by
`netifrcd` (root); the second should report status through netifrcd rather than
failing to connect.

## Supported / not supported

**Supported**

- Wired interfaces: DHCP and static addressing.
- Wi-Fi scanning and WPA-PSK association through netifrc's `wpa_supplicant`
  module.
- Connect and disconnect of the above.

**Not supported**

- VPN.
- Modem / WWAN.
- 802.1X (enterprise Wi-Fi).
- Hotspot / AP mode.
- Checkpoints.
- A secrets agent.

Wi-Fi PSKs are stored by the `wpa_supplicant` module in
`/etc/wpa_supplicant/wpa_supplicant-<iface>.conf`.

## How netifrcd writes configuration

netifrcd edits only its own delimited region and never touches anything else.

- **`/etc/conf.d/net`** — netifrcd owns ONLY the block between

  ```
  # >>> netifrcd managed block >>>
  # <<< netifrcd managed block <<<
  ```

  Everything outside those markers is left untouched. Hand-written netifrc
  settings outside the block are preserved and continue to apply.

- **`/etc/wpa_supplicant/wpa_supplicant-<iface>.conf`** — per-interface Wi-Fi
  configuration, written via netifrc's `wpa_supplicant` module.

- **`/var/lib/netifrcd/profiles.json`** — the profile store (connection
  definitions as seen through the NetworkManager Settings API).

## Troubleshooting

- **Service status:** `rc-service netifrcd status`.
- **System logs:** netifrcd logs to the system log; inspect it with
  `rc-service netifrcd status` plus your logger (`syslog-ng`, `rsyslog`, or
  `journalctl -t netifrcd` if present).
- **Applet shows no networks / stale list:** force a rescan, e.g. restart the
  service with `rc-service netifrcd restart`, or rescan from the applet. Confirm
  the Wi-Fi radio is not blocked with `rfkill list`.
- **netifrcd fails to start:** it must run as **root** (it owns a system-bus
  name and starts `net.*` services). Also make sure NetworkManager is stopped —
  the init script refuses to start while NetworkManager is running, because both
  claim `org.freedesktop.NetworkManager`.
- **Name already owned:** check with `busctl --system list | grep NetworkManager`
  and stop whatever other process owns the name.

## Files installed by this directory

| File | Install destination |
| --- | --- |
| `org.freedesktop.NetworkManager.netifrcd.conf` | `/usr/share/dbus-1/system.d/` |
| `netifrcd.initd` | `/etc/init.d/netifrcd` |
| `org.freedesktop.NetworkManager.service` | `/usr/share/dbus-1/system-services/` |
