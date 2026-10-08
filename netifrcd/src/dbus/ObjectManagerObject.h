// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.DBus.ObjectManager at /org/freedesktop.
//
// This is not part of the NetworkManager API proper, but NetworkManager implements it
// and modern clients depend on it: libnm's NMClient calls GetManagedObjects() as its
// very first act, and concludes "NetworkManager is not running" if the call fails.
// NetworkManagerQt does not need it, but implementing it costs little and makes
// netifrcd usable from nmcli, nmtui and any libnm-based tool.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace netifrcd
{

class Backend;

/// One interface's properties: { "org.freedesktop.NetworkManager.Device": {...} }
using InterfacePropertiesMap = QMap<QString, QVariantMap>;

/// The ObjectManager reply shape: object path -> interface -> properties.
using ManagedObjectsMap = QMap<QDBusObjectPath, InterfacePropertiesMap>;

class ObjectManagerObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")

public:
    explicit ObjectManagerObject(Backend *backend, QObject *parent = nullptr);

public Q_SLOTS:
    /// Builds the map from every object currently exported by the Backend.
    /// Must be a slot: Q_INVOKABLE methods are not picked up by ExportAllSlots.
    netifrcd::ManagedObjectsMap GetManagedObjects() const;

Q_SIGNALS:
    void InterfacesAdded(const QDBusObjectPath &objectPath, const netifrcd::InterfacePropertiesMap &interfacesAndProperties);
    void InterfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces);

private:
    Backend *m_backend = nullptr;
};

} // namespace netifrcd
