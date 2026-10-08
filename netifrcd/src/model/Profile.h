// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// A connection profile.
//
// This is netifrcd's own model, deliberately small. It converts two ways:
//   * to/from the NetworkManager settings map (a{sa{sv}}), so the unmodified Plasma
//     applet, KCM and connection editor can read and write it, and
//   * to/from netifrc's /etc/conf.d/net variables, which is what actually gets applied.

#pragma once

#include "common/Constants.h"
#include "netifrc/NetifrcConfig.h"

#include <QString>
#include <QStringList>

namespace netifrcd
{

struct Profile {
    enum class IpMethod { Auto, Manual, Disabled };

    QString uuid;
    QString id;            // human readable connection name
    QString type;          // TypeWired / TypeWireless
    QString interfaceName; // bound interface; empty means "any of this type"
    bool autoconnect = true;
    quint64 timestamp = 0;

    IpMethod ipv4Method = IpMethod::Auto;
    QStringList addresses; // CIDR
    QString gateway;
    QStringList dns;

    QString ssid;
    QString psk;
    bool hidden = false;

    bool isValid() const
    {
        return !uuid.isEmpty() && !type.isEmpty();
    }

    bool isWireless() const
    {
        return type == QLatin1String(TypeWireless);
    }

    /// NetworkManager settings map, as returned by Settings.Connection.GetSettings.
    SettingsMap toSettingsMap() const;

    /// Parses a NetworkManager settings map. Unknown keys are ignored rather than
    /// rejected, so profiles written by the KDE editor round-trip safely.
    static Profile fromSettingsMap(const SettingsMap &map);

    /// Builds a profile from an existing netifrc interface stanza, so an interface the
    /// administrator configured by hand still shows up in the applet.
    static Profile fromNetifrc(const QString &ifname, const NetifrcConfig::InterfaceConfig &cfg);

    /// The netifrc stanza this profile implies.
    NetifrcConfig::InterfaceConfig toNetifrc() const;

    /// Generates a UUID in the same format NetworkManager uses.
    static QString newUuid();
};

} // namespace netifrcd
