// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Kernel-side view of the network interfaces netifrc manages.
//
// We read state from `ip -j` (iproute2's JSON output) rather than asking OpenRC,
// because OpenRC has no cheap "is it up?" query and forking rc-service on a poll timer
// would be wasteful. `ip -j` is a single fast fork and gives us flags, carrier,
// addresses and routes in one shot.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace netifrcd
{

struct InterfaceInfo {
    QString name;
    int ifindex = 0;
    QString mac;
    int mtu = 0;
    bool up = false;      // IFF_UP
    bool carrier = false; // IFF_LOWER_UP / IFF_RUNNING
    bool loopback = false;
    int deviceType = 0; // NMDeviceType
    QString driver;
    QStringList addresses; // CIDR, IPv4 only for now
    QString gateway;
    QStringList dns;
    QString ssid;    // current SSID for wireless interfaces
    int bitrate = 0; // kbit/s, wireless
};

class InterfaceWatcher : public QObject
{
    Q_OBJECT

public:
    explicit InterfaceWatcher(QObject *parent = nullptr);

    /// Interfaces netifrcd considers manageable: real hardware (has a
    /// /sys/class/net/<if>/device link) plus anything named in netifrc's config.
    /// Docker/bridge/veth plumbing is deliberately excluded.
    QList<InterfaceInfo> interfaces() const;

    InterfaceInfo info(const QString &ifname) const;

    /// Interfaces named in netifrc's configuration are always managed, even if they
    /// are virtual (bridges, vlans, ...).
    void setExtraManaged(const QStringList &ifnames);

    /// Forces a re-read and emits changed() if anything differs.
    void refresh();

    static bool isWireless(const QString &ifname);
    static QString driverFor(const QString &ifname);
    static bool isHardwareBacked(const QString &ifname);

Q_SIGNALS:
    void changed();

private:
    static InterfaceInfo parseLink(const QJsonObject &obj);
    void readRoutes(QHash<QString, QString> *gateways);
    void readDns(QHash<QString, QStringList> *dns);

    QList<InterfaceInfo> m_interfaces;
    QStringList m_extraManaged;
    bool m_primed = false;
};

} // namespace netifrcd
