// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Wi-Fi scanning and association for netifrc-managed interfaces.
//
// Association is delegated to netifrc itself rather than driven directly: netifrc's
// wpa_supplicant module looks for /etc/wpa_supplicant/wpa_supplicant-<iface>.conf first
// (see wpa_supplicant_pre_start in /lib/netifrc/net/wpa_supplicant.sh), so netifrcd
// writes a wpa_supplicant network block there and lets `rc-service net.<iface> start`
// do the rest. netifrc therefore stays the single owner of the wireless lifecycle.
//
// Scanning prefers wpa_cli when a supplicant control socket exists, and falls back to
// `iw dev <iface> scan` for interfaces that are up but not yet associated.

#pragma once

#include <QList>
#include <QString>

namespace netifrcd
{

struct AccessPointInfo {
    QString ssid;
    QString bssid;
    int frequency = 0;    // MHz
    int strength = 0;     // 0-100, NetworkManager's scale
    int maxBitrate = 54000; // kbit/s
    int mode = 0;         // NM80211Mode
    int flags = 0;        // NM80211ApFlags
    int wpaFlags = 0;     // NM80211ApSecurityFlags
    int rsnFlags = 0;     // NM80211ApSecurityFlags
};

class WifiScanner
{
public:
    /// Scans and returns the visible access points. Returns an empty list on failure,
    /// with *error set.
    static QList<AccessPointInfo> scan(const QString &ifname, QString *error = nullptr);

    /// Path netifrc will use for this interface's supplicant configuration.
    static QString wpaConfigPath(const QString &ifname);

    /// Writes (or replaces) a wpa_supplicant network block for ssid/psk.
    /// A psk of QString() produces an open (no-key) network.
    static bool writeNetwork(const QString &ifname, const QString &ssid, const QString &psk, bool hidden, QString *error = nullptr);

    /// Removes any network block matching ssid.
    static bool removeNetwork(const QString &ifname, const QString &ssid, QString *error = nullptr);

    static bool wpaSupplicantAvailable();
};

} // namespace netifrcd
