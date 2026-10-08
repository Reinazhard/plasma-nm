// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// The orchestrator: owns netifrc state, the profile store and every D-Bus object.
//
// The D-Bus objects are deliberately dumb. They hold a Backend* and read everything
// through the accessors below, so there is exactly one place where netifrc state and
// NetworkManager-visible state are reconciled.

#pragma once

#include "common/Constants.h"
#include "netifrc/InterfaceWatcher.h"
#include "wifi/WifiScanner.h"

#include <QObject>
#include <QString>

#include <nm-dbus-interface.h>

class QTimer; // global Qt type, so it cannot be declared inside netifrcd below

namespace netifrcd
{

class NetifrcConfig;
class ServiceControl;
class ProfileStore;
struct Profile;

class Backend : public QObject
{
    Q_OBJECT

public:
    // ---------------------------------------------------------------------
    // Records exposed to the D-Bus layer
    // ---------------------------------------------------------------------

    struct DeviceRecord {
        QString ifname;
        int id = 0;
        InterfaceInfo info;
        int state = NM_DEVICE_STATE_UNKNOWN;
        int stateReason = NM_DEVICE_STATE_REASON_NONE;
        QString activeConnectionPath; // empty when not activated
        QStringList apPaths;
        QString activeApPath;
    };

    struct AccessPointRecord {
        int id = 0;
        QString ifname;
        QString path;
        AccessPointInfo info;
    };

    struct ActiveConnectionRecord {
        int id = 0;
        QString path;
        QString profileUuid;
        QString settingsPath;
        QString devicePath;
        QString deviceIfname;
        int state = NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
    };

    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    /// Loads netifrc config and profiles, primes the interface watcher and starts
    /// polling. Does not touch D-Bus.
    bool start(QString *error = nullptr);

    /// Registers every object on the system bus and requests the service name.
    bool registerObjects(QString *error = nullptr);

    /// object path -> interface -> properties, for org.freedesktop.DBus.ObjectManager.
    /// libnm calls this as its first act and refuses to work without it.
    QMap<QDBusObjectPath, QMap<QString, QVariantMap>> managedObjects() const;

    // ---------------------------------------------------------------------
    // Queries used by the D-Bus objects
    // ---------------------------------------------------------------------

    QList<DeviceRecord> devices() const;
    DeviceRecord device(const QString &ifname) const;
    DeviceRecord deviceByPath(const QString &path) const;
    QString devicePathFor(const QString &ifname) const;

    QList<AccessPointRecord> accessPoints(const QString &ifname) const;
    AccessPointRecord accessPointByPath(const QString &path) const;
    QString accessPointPathFor(const QString &ifname, const QString &bssid) const;
    /// Seconds since boot (CLOCK_BOOTTIME) of the last scan on this interface, or -1
    /// if it has never been scanned. This is what NetworkManager's LastScan reports.
    qint64 lastScanTime(const QString &ifname) const;

    QList<ActiveConnectionRecord> activeConnections() const;
    ActiveConnectionRecord activeConnectionByPath(const QString &path) const;
    QString activeConnectionPathFor(const QString &ifname) const;

    QList<Profile> profiles() const;
    Profile profileByUuid(const QString &uuid) const;
    Profile profileBySettingsPath(const QString &path) const;
    QString settingsPathFor(const QString &uuid) const;

    /// Interface this profile should be applied to (explicit binding, or the first
    /// managed interface of a matching type).
    QString targetInterfaceFor(const Profile &profile) const;

    // Manager-level state.
    int managerState() const;
    int connectivity() const;
    bool networkingEnabled() const;
    bool wirelessEnabled() const;
    bool wirelessHardwareEnabled() const;
    bool wwanEnabled() const
    {
        return false;
    }
    bool wwanHardwareEnabled() const
    {
        return false;
    }
    QString primaryConnectionPath() const;
    QString activatingConnectionPath() const;

    // ---------------------------------------------------------------------
    // Operations invoked from D-Bus methods
    // ---------------------------------------------------------------------

    /// Activates the profile at connectionPath on the device at devicePath.
    /// Returns the new active-connection path, or an empty string on failure.
    QString activateConnection(const QString &connectionPath, const QString &devicePath, const QString &specificObject, QString *error);
    QString addAndActivateConnection(const SettingsMap &map, const QString &devicePath, const QString &specificObject, QString *error);
    bool deactivateConnection(const QString &activeConnectionPath, QString *error);

    /// Returns the new Settings.Connection object path.
    QString addConnection(const SettingsMap &map, QString *error);
    bool updateConnection(const QString &settingsPath, const SettingsMap &map, QString *error);
    bool removeConnection(const QString &settingsPath, QString *error);
    SettingsMap settingsForPath(const QString &settingsPath) const;

    bool requestScan(const QString &ifname, QString *error);
    bool setNetworkingEnabled(bool enabled, QString *error);
    bool setWirelessEnabled(bool enabled, QString *error);

    /// True while an OpenRC start/stop is in flight for this interface.
    bool isBusy(const QString &ifname) const;

Q_SIGNALS:
    void deviceAdded(const QString &path);
    void deviceRemoved(const QString &path);
    /// changedProps are NetworkManager property names for the given interface.
    void deviceChanged(const QString &path, const QStringList &changedProps);
    void accessPointListChanged(const QString &ifname);
    void activeConnectionAdded(const QString &path);
    void activeConnectionRemoved(const QString &path);
    void activeConnectionChanged(const QString &path, const QStringList &changedProps);
    void settingsChanged();
    void settingsConnectionChanged(const QString &path);
    void managerChanged(const QStringList &changedProps);

private Q_SLOTS:
    void onInterfacesChanged();
    void onServiceActionFinished(const QString &ifname, int action, bool success, const QString &message);
    /// Scans every usable wireless device. Runs once at startup and then periodically,
    /// so clients see access points without having to call RequestScan themselves.
    void scanWirelessDevices();

private:
    /// Opaque holder for the exported D-Bus objects, defined in Backend.cpp. Keeps the
    /// D-Bus layer out of this header so the records above stay the only contract.
    struct Dbus;

    void reconcile();
    void importNetifrcInterfaces();
    void refreshAccessPoints(const QString &ifname);
    /// Writes profile into netifrcd's managed block for ifname, i.e. claims the
    /// interface. See the definition for why an explicit activation wins over the
    /// administrator's own /etc/conf.d/net entries.
    void claimInterface(const QString &ifname, const Profile &profile);
    /// Re-points (or drops) the managed block for ifname after a profile was deleted.
    /// See the definition.
    void reassignInterface(const QString &ifname, const QString &removedUuid);
    /// First profile other than ignoreUuid that targets ifname, or an invalid Profile.
    Profile firstProfileTargeting(const QString &ifname, const QString &ignoreUuid) const;
    /// Creates/drops the Settings.Connection objects so every profile has a live object
    /// on the bus. libnm resolves ActiveConnection.Connection against these, and the
    /// applet enumerates them, so they must exist whenever a profile does.
    void syncSettingsConnections();
    int nextDeviceId();
    int nextApId();
    int nextActiveConnectionId();

    NetifrcConfig *m_config = nullptr;
    ServiceControl *m_services = nullptr;
    InterfaceWatcher *m_watcher = nullptr;
    ProfileStore *m_profiles = nullptr;
    QTimer *m_scanTimer = nullptr;

    QList<DeviceRecord> m_devices;
    QList<AccessPointRecord> m_accessPoints;
    QList<ActiveConnectionRecord> m_activeConnections;
    QHash<QString, QList<AccessPointInfo>> m_scanCache;
    QHash<QString, qint64> m_lastScan; // ifname -> CLOCK_BOOTTIME seconds of last scan

    QHash<QString, int> m_deviceIds;
    QHash<QString, int> m_activeIds;
    // A Settings.Connection path must stay stable for the lifetime of a connection:
    // clients and the secrets agent cache it, so an id is assigned the first time a
    // uuid is seen and never reused or renumbered. Mutable because the path lookup is
    // const (it is a lazily populated cache, like m_deviceIds above).
    mutable QHash<QString, int> m_settingsIds; // uuid -> Settings.Connection id
    mutable int m_nextSettingsId = 0;
    int m_nextDeviceId = 1;
    int m_nextApId = 1;
    int m_nextActiveId = 1;

    bool m_networkingEnabled = true;
    bool m_wirelessEnabled = true;
    bool m_wirelessHardwareEnabled = true;
    bool m_registered = false;
    Dbus *m_dbus = nullptr;
};

} // namespace netifrcd
