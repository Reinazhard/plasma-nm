// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.Connection.Active.
//
// One object per activation. netifrc has no notion of a multi-step activation, so
// these objects mostly mirror the ActiveConnectionRecord the Backend keeps, plus
// the profile the record points at.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QString>

namespace netifrcd
{

class Backend;

class ActiveConnectionObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Connection.Active")

    Q_PROPERTY(QDBusObjectPath Connection READ connection)
    Q_PROPERTY(QDBusObjectPath SpecificObject READ specificObject)
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(QString Uuid READ uuid)
    Q_PROPERTY(QString Type READ type)
    Q_PROPERTY(QList<QDBusObjectPath> Devices READ devices)
    Q_PROPERTY(uint State READ state)
    Q_PROPERTY(uint StateFlags READ stateFlags)
    Q_PROPERTY(bool Default READ isDefault)
    Q_PROPERTY(bool Default6 READ default6)
    Q_PROPERTY(QDBusObjectPath Ip4Config READ ip4Config)
    Q_PROPERTY(QDBusObjectPath Dhcp4Config READ dhcp4Config)
    Q_PROPERTY(QDBusObjectPath Ip6Config READ ip6Config)
    Q_PROPERTY(QDBusObjectPath Dhcp6Config READ dhcp6Config)
    Q_PROPERTY(bool Vpn READ vpn)
    Q_PROPERTY(QDBusObjectPath Controller READ controller)
    Q_PROPERTY(QList<QDBusObjectPath> Ports READ ports)

public:
    explicit ActiveConnectionObject(Backend *backend, const QString &activePath, QObject *parent = nullptr);

    QDBusObjectPath connection() const;
    QDBusObjectPath specificObject() const;
    QString id() const;
    QString uuid() const;
    QString type() const;
    QList<QDBusObjectPath> devices() const;
    uint state() const;
    uint stateFlags() const;
    bool isDefault() const;
    bool default6() const;
    QDBusObjectPath ip4Config() const;
    QDBusObjectPath dhcp4Config() const;
    QDBusObjectPath ip6Config() const;
    QDBusObjectPath dhcp6Config() const;
    bool vpn() const;
    QDBusObjectPath controller() const;
    QList<QDBusObjectPath> ports() const;

Q_SIGNALS:
    void StateChanged(uint state, uint reason);

private:
    QVariantMap changedMap(const QStringList &props) const;

    Backend *m_backend = nullptr;
    uint m_lastState = NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
};

} // namespace netifrcd
