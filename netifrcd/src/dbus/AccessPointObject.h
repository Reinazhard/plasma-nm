// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.AccessPoint.
//
// One object per visible BSSID. The object is keyed by interface + BSSID and its
// values come straight from the AccessPointRecord the Backend keeps in sync with
// the scanner.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"
#include "wifi/WifiScanner.h"

#include <QByteArray>
#include <QObject>
#include <QString>

namespace netifrcd
{

class Backend;

class AccessPointObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.AccessPoint")

    Q_PROPERTY(QByteArray Ssid READ ssid)
    Q_PROPERTY(QString HwAddress READ hwAddress)
    Q_PROPERTY(uint Frequency READ frequency)
    Q_PROPERTY(uint MaxBitrate READ maxBitrate)
    Q_PROPERTY(uint Mode READ mode)
    Q_PROPERTY(uchar Strength READ strength)
    Q_PROPERTY(int LastSeen READ lastSeen)
    Q_PROPERTY(uint Flags READ flags)
    Q_PROPERTY(uint WpaFlags READ wpaFlags)
    Q_PROPERTY(uint RsnFlags READ rsnFlags)

public:
    AccessPointObject(Backend *backend, const QString &ifname, const QString &bssid, QObject *parent = nullptr);

    QByteArray ssid() const;
    QString hwAddress() const;
    uint frequency() const;
    uint maxBitrate() const;
    uint mode() const;
    uchar strength() const;
    int lastSeen() const;
    uint flags() const;
    uint wpaFlags() const;
    uint rsnFlags() const;

private:
    AccessPointInfo currentInfo() const;
    QVariantMap changedMap(const AccessPointInfo &previous, const AccessPointInfo &current) const;

    Backend *m_backend = nullptr;
    QString m_ifname;
    AccessPointInfo m_info;
};

} // namespace netifrcd
