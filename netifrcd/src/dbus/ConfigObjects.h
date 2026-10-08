// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.IP4Config and ...DHCP4Config.
//
// A device exposes one IP4Config while it is activated. Modern clients (including
// NetworkManagerQt) read the AddressData/NameserverData "aa{sv}" properties; the
// older Addresses/Nameservers "aau"/"au" properties are still populated as a
// fallback for anything that has not been updated.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace netifrcd
{

class Backend;

/// /org/freedesktop/NetworkManager/IP4Config/<id>
class Ip4ConfigObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.IP4Config")

    Q_PROPERTY(QList<QVariantMap> AddressData READ addressData)
    Q_PROPERTY(QList<QList<uint>> Addresses READ addresses)
    Q_PROPERTY(QString Gateway READ gateway)
    Q_PROPERTY(QList<QList<uint>> Routes READ routes)
    Q_PROPERTY(QList<QVariantMap> RouteData READ routeData)
    Q_PROPERTY(QList<QVariantMap> NameserverData READ nameserverData)
    Q_PROPERTY(QList<uint> Nameservers READ nameservers)
    Q_PROPERTY(QStringList Domains READ domains)
    Q_PROPERTY(QStringList Searches READ searches)
    Q_PROPERTY(QStringList DnsOptions READ dnsOptions)
    Q_PROPERTY(int DnsPriority READ dnsPriority)
    Q_PROPERTY(QStringList WinsServerData READ winsServerData)

public:
    explicit Ip4ConfigObject(Backend *backend, const QString &path, const QString &ifname, QObject *parent = nullptr);

    QList<QVariantMap> addressData() const;
    QList<QList<uint>> addresses() const;
    QString gateway() const;
    QList<QList<uint>> routes() const;
    QList<QVariantMap> routeData() const;
    QList<QVariantMap> nameserverData() const;
    QList<uint> nameservers() const;
    QStringList domains() const;
    QStringList searches() const;
    QStringList dnsOptions() const;
    int dnsPriority() const;
    QStringList winsServerData() const;

private Q_SLOTS:
    void onDeviceChanged(const QString &path, const QStringList &changedProps);

private:
    /// Re-reads the interface's addresses/gateway/DNS from the Backend and returns
    /// the PropertiesChanged map, updating the cached members in place.
    QVariantMap refresh();

    Backend *m_backend = nullptr;
    QString m_ifname;

    QList<QVariantMap> m_addressData;
    QList<QList<uint>> m_addresses;
    QString m_gateway;
    QList<QVariantMap> m_nameserverData;
};

/// /org/freedesktop/NetworkManager/DHCP4Config/<id>
class Dhcp4ConfigObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.DHCP4Config")

    Q_PROPERTY(QVariantMap Options READ options)

public:
    explicit Dhcp4ConfigObject(Backend *backend, const QString &path, const QString &ifname, QObject *parent = nullptr);

    QVariantMap options() const;

private:
    Backend *m_backend = nullptr;
    QString m_ifname;
};

} // namespace netifrcd
