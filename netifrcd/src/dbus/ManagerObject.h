// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager - the manager object.
//
// This is the object the Plasma applet talks to first: it enumerates devices and
// active connections and exposes the global switches (networking / wireless).
// Every value is read through Backend, never cached here, so there is exactly one
// owner of the reconciled state.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace netifrcd
{

class Backend;

class ManagerObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager")

    Q_PROPERTY(QString Version READ version CONSTANT)
    Q_PROPERTY(uint VersionInfo READ versionInfo CONSTANT)
    Q_PROPERTY(uint Capabilities READ capabilities CONSTANT)
    Q_PROPERTY(uint State READ state)
    Q_PROPERTY(bool Startup READ startup)
    Q_PROPERTY(bool NetworkingEnabled READ networkingEnabled)
    Q_PROPERTY(bool WirelessEnabled READ wirelessEnabled WRITE setWirelessEnabled)
    Q_PROPERTY(bool WirelessHardwareEnabled READ wirelessHardwareEnabled)
    Q_PROPERTY(bool WwanEnabled READ wwanEnabled)
    Q_PROPERTY(bool WwanHardwareEnabled READ wwanHardwareEnabled)
    Q_PROPERTY(uint RadioFlags READ radioFlags)
    Q_PROPERTY(QList<QDBusObjectPath> ActiveConnections READ activeConnections)
    Q_PROPERTY(QDBusObjectPath PrimaryConnection READ primaryConnection)
    Q_PROPERTY(QString PrimaryConnectionType READ primaryConnectionType)
    Q_PROPERTY(QDBusObjectPath ActivatingConnection READ activatingConnection)
    Q_PROPERTY(QList<QDBusObjectPath> Devices READ devices)
    Q_PROPERTY(QList<QDBusObjectPath> AllDevices READ allDevices)
    Q_PROPERTY(uint Metered READ metered)
    Q_PROPERTY(uint Connectivity READ connectivity)
    Q_PROPERTY(bool ConnectivityCheckAvailable READ connectivityCheckAvailable)
    Q_PROPERTY(bool ConnectivityCheckEnabled READ connectivityCheckEnabled)
    Q_PROPERTY(QString ConnectivityCheckUri READ connectivityCheckUri)
    Q_PROPERTY(QVariantMap GlobalDnsConfiguration READ globalDnsConfiguration)

public:
    explicit ManagerObject(Backend *backend, QObject *parent = nullptr);

    // org.freedesktop.NetworkManager properties
    QString version() const;
    uint versionInfo() const;
    uint capabilities() const;
    uint state() const;
    bool startup() const;
    bool networkingEnabled() const;
    bool wirelessEnabled() const;
    void setWirelessEnabled(bool enabled);
    bool wirelessHardwareEnabled() const;
    bool wwanEnabled() const;
    bool wwanHardwareEnabled() const;
    uint radioFlags() const;
    QList<QDBusObjectPath> activeConnections() const;
    QDBusObjectPath primaryConnection() const;
    QString primaryConnectionType() const;
    QDBusObjectPath activatingConnection() const;
    QList<QDBusObjectPath> devices() const;
    QList<QDBusObjectPath> allDevices() const;
    uint metered() const;
    uint connectivity() const;
    bool connectivityCheckAvailable() const;
    bool connectivityCheckEnabled() const;
    QString connectivityCheckUri() const;
    QVariantMap globalDnsConfiguration() const;

public Q_SLOTS:
    QList<QDBusObjectPath> GetDevices() const;
    QList<QDBusObjectPath> GetAllDevices() const;
    QDBusObjectPath GetDeviceByIpIface(const QString &iface) const;

    QDBusObjectPath ActivateConnection(const QDBusObjectPath &connection,
                                       const QDBusObjectPath &device,
                                       const QDBusObjectPath &specificObject);

    /// NetworkManager returns two object paths: the Settings.Connection and the
    /// new ActiveConnection. Qt puts the return value first and any trailing
    /// non-const reference after it, so the Settings path is the return value and
    /// activeConnection is the trailing output.
    QDBusObjectPath AddAndActivateConnection(const SettingsMap &settings,
                                             const QDBusObjectPath &device,
                                             const QDBusObjectPath &specificObject,
                                             QDBusObjectPath &activeConnection);

    void DeactivateConnection(const QDBusObjectPath &activeConnection);
    void Enable(bool enabled);
    void Sleep(bool sleeping);
    QVariantMap GetPermissions() const;
    uint CheckConnectivity() const;
    void SetLogging(const QString &level, const QString &domains);
    void Reload(uint flags);

    // Checkpoints are not implemented; these exist so clients that introspect or
    // opportunistically call them get a well-formed (if empty) answer.
    QDBusObjectPath CheckpointCreate(const QList<QDBusObjectPath> &devices, uint rollbackTimeout);
    void CheckpointDestroy(const QDBusObjectPath &checkpoint);
    QVariantMap CheckpointRollback(const QDBusObjectPath &checkpoint);

Q_SIGNALS:
    void StateChanged(uint state);
    void DeviceAdded(const QDBusObjectPath &device);
    void DeviceRemoved(const QDBusObjectPath &device);
    void CheckPermissions();

private:
    /// Translates the property names reported by Backend::managerChanged into the
    /// current values, so PropertiesChanged only carries what actually changed.
    QVariantMap changedMap(const QStringList &props) const;

    Backend *m_backend = nullptr;
};

} // namespace netifrcd
