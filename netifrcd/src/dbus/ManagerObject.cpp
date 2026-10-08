// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/ManagerObject.h"

#include "common/Logging.h"
#include "model/Backend.h"
#include "model/Profile.h"

#include <QVariant>

namespace netifrcd
{

namespace
{

using ActiveConnectionRecord = Backend::ActiveConnectionRecord;
using DeviceRecord = Backend::DeviceRecord;

/// NetworkManager's permission names. Values are "yes"/"no"/"auth"/"unknown";
/// netifrcd runs with full authority, so everything is granted.
QStringList permissionNames()
{
    return {
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-network"),
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-wifi"),
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-wwan"),
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-wimax"),
        QStringLiteral("org.freedesktop.NetworkManager.sleep-allow"),
        QStringLiteral("org.freedesktop.NetworkManager.network-control"),
        QStringLiteral("org.freedesktop.NetworkManager.wifi-share-protected"),
        QStringLiteral("org.freedesktop.NetworkManager.wifi-share-open"),
        QStringLiteral("org.freedesktop.NetworkManager.settings-modify-system"),
        QStringLiteral("org.freedesktop.NetworkManager.settings-modify-own"),
        QStringLiteral("org.freedesktop.NetworkManager.settings-modify-hostname"),
        QStringLiteral("org.freedesktop.NetworkManager.settings-modify-global-dns"),
        QStringLiteral("org.freedesktop.NetworkManager.reload"),
        QStringLiteral("org.freedesktop.NetworkManager.checkpoint-rollback"),
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-statistics"),
        QStringLiteral("org.freedesktop.NetworkManager.enable-disable-connectivity-check"),
        QStringLiteral("org.freedesktop.NetworkManager.wifi-scan"),
    };
}

} // namespace

ManagerObject::ManagerObject(Backend *backend, QObject *parent)
    : ObjectBase(QLatin1String(PathManager), parent)
    , m_backend(backend)
{
    Q_ASSERT(m_backend);

    connect(m_backend, &Backend::managerChanged, this, [this](const QStringList &props) {
        if (props.contains(QLatin1String("State"))) {
            Q_EMIT StateChanged(state());
        }
        notify(QLatin1String(IfaceManager), changedMap(props));
    });

    connect(m_backend, &Backend::deviceAdded, this, [this](const QString &path) {
        Q_EMIT DeviceAdded(toPath(path));
        const QVariantMap changed{
            {QStringLiteral("Devices"), QVariant::fromValue(devices())},
            {QStringLiteral("AllDevices"), QVariant::fromValue(allDevices())},
        };
        notify(QLatin1String(IfaceManager), changed);
    });

    connect(m_backend, &Backend::deviceRemoved, this, [this](const QString &path) {
        Q_EMIT DeviceRemoved(toPath(path));
        const QVariantMap changed{
            {QStringLiteral("Devices"), QVariant::fromValue(devices())},
            {QStringLiteral("AllDevices"), QVariant::fromValue(allDevices())},
        };
        notify(QLatin1String(IfaceManager), changed);
    });

    const auto activeConnectionsChanged = [this]() {
        notify(QLatin1String(IfaceManager), {{QStringLiteral("ActiveConnections"), QVariant::fromValue(activeConnections())}});
    };
    connect(m_backend, &Backend::activeConnectionAdded, this, [activeConnectionsChanged](const QString &) {
        activeConnectionsChanged();
    });
    connect(m_backend, &Backend::activeConnectionRemoved, this, [activeConnectionsChanged](const QString &) {
        activeConnectionsChanged();
    });
}

QString ManagerObject::version() const
{
#ifdef NETIFRCD_NM_VERSION
    return QStringLiteral(NETIFRCD_NM_VERSION);
#else
    return QStringLiteral("1.52.0");
#endif
}

uint ManagerObject::versionInfo() const
{
    return 0;
}

uint ManagerObject::capabilities() const
{
    return 0;
}

uint ManagerObject::state() const
{
    return static_cast<uint>(m_backend->managerState());
}

bool ManagerObject::startup() const
{
    return false;
}

bool ManagerObject::networkingEnabled() const
{
    return m_backend->networkingEnabled();
}

bool ManagerObject::wirelessEnabled() const
{
    return m_backend->wirelessEnabled();
}

void ManagerObject::setWirelessEnabled(bool enabled)
{
    QString error;
    if (!m_backend->setWirelessEnabled(enabled, &error)) {
        qCWarning(NETIFRC) << "failed to set WirelessEnabled:" << error;
    }
}

bool ManagerObject::wirelessHardwareEnabled() const
{
    return m_backend->wirelessHardwareEnabled();
}

bool ManagerObject::wwanEnabled() const
{
    return m_backend->wwanEnabled();
}

bool ManagerObject::wwanHardwareEnabled() const
{
    return m_backend->wwanHardwareEnabled();
}

uint ManagerObject::radioFlags() const
{
    return 0;
}

QList<QDBusObjectPath> ManagerObject::activeConnections() const
{
    const QList<ActiveConnectionRecord> records = m_backend->activeConnections();
    QList<QDBusObjectPath> paths;
    paths.reserve(records.size());
    for (const ActiveConnectionRecord &record : records) {
        paths.append(QDBusObjectPath(record.path));
    }
    return paths;
}

QDBusObjectPath ManagerObject::primaryConnection() const
{
    return toPath(m_backend->primaryConnectionPath());
}

QString ManagerObject::primaryConnectionType() const
{
    const QString primary = m_backend->primaryConnectionPath();
    if (primary.isEmpty()) {
        return QString();
    }
    const ActiveConnectionRecord record = m_backend->activeConnectionByPath(primary);
    if (record.profileUuid.isEmpty()) {
        return QString();
    }
    return m_backend->profileByUuid(record.profileUuid).type;
}

QDBusObjectPath ManagerObject::activatingConnection() const
{
    return toPath(m_backend->activatingConnectionPath());
}

QList<QDBusObjectPath> ManagerObject::devices() const
{
    const QList<DeviceRecord> records = m_backend->devices();
    QList<QDBusObjectPath> paths;
    paths.reserve(records.size());
    for (const DeviceRecord &record : records) {
        paths.append(toPath(m_backend->devicePathFor(record.ifname)));
    }
    return paths;
}

QList<QDBusObjectPath> ManagerObject::allDevices() const
{
    return devices();
}

uint ManagerObject::metered() const
{
    // Backend has no metered accessor yet; NM_METERED_UNKNOWN is the honest answer.
    return NM_METERED_UNKNOWN;
}

uint ManagerObject::connectivity() const
{
    return static_cast<uint>(m_backend->connectivity());
}

bool ManagerObject::connectivityCheckAvailable() const
{
    return false;
}

bool ManagerObject::connectivityCheckEnabled() const
{
    // Backend has no connectivity-check accessor; netifrcd does not run one.
    return false;
}

QString ManagerObject::connectivityCheckUri() const
{
    return QString();
}

QVariantMap ManagerObject::globalDnsConfiguration() const
{
    return QVariantMap();
}

QList<QDBusObjectPath> ManagerObject::GetDevices() const
{
    return devices();
}

QList<QDBusObjectPath> ManagerObject::GetAllDevices() const
{
    return allDevices();
}

QDBusObjectPath ManagerObject::GetDeviceByIpIface(const QString &iface) const
{
    return toPath(m_backend->devicePathFor(iface));
}

QDBusObjectPath ManagerObject::ActivateConnection(const QDBusObjectPath &connection,
                                                  const QDBusObjectPath &device,
                                                  const QDBusObjectPath &specificObject)
{
    QString error;
    const QString path = m_backend->activateConnection(connection.path(), device.path(), specificObject.path(), &error);
    if (path.isEmpty()) {
        qCWarning(NETIFRC) << "ActivateConnection" << connection.path() << "on" << device.path() << "failed:" << error;
        return nonePath();
    }
    return toPath(path);
}

QDBusObjectPath ManagerObject::AddAndActivateConnection(const SettingsMap &settings,
                                                        const QDBusObjectPath &device,
                                                        const QDBusObjectPath &specificObject,
                                                        QDBusObjectPath &activeConnection)
{
    QString error;
    const QString activePath = m_backend->addAndActivateConnection(settings, device.path(), specificObject.path(), &error);
    if (activePath.isEmpty()) {
        qCWarning(NETIFRC) << "AddAndActivateConnection on" << device.path() << "failed:" << error;
        activeConnection = nonePath();
        return nonePath();
    }
    activeConnection = QDBusObjectPath(activePath);
    return toPath(m_backend->activeConnectionByPath(activePath).settingsPath);
}

void ManagerObject::DeactivateConnection(const QDBusObjectPath &activeConnection)
{
    QString error;
    if (!m_backend->deactivateConnection(activeConnection.path(), &error)) {
        qCWarning(NETIFRC) << "DeactivateConnection" << activeConnection.path() << "failed:" << error;
    }
}

void ManagerObject::Enable(bool enabled)
{
    QString error;
    if (!m_backend->setNetworkingEnabled(enabled, &error)) {
        qCWarning(NETIFRC) << "failed to set NetworkingEnabled:" << error;
    }
}

void ManagerObject::Sleep(bool sleeping)
{
    qCInfo(NETIFRC) << "Sleep(" << sleeping << ") ignored; netifrc has no sleep state";
}

QVariantMap ManagerObject::GetPermissions() const
{
    QVariantMap permissions;
    const QStringList names = permissionNames();
    for (const QString &name : names) {
        permissions.insert(name, QStringLiteral("yes"));
    }
    return permissions;
}

uint ManagerObject::CheckConnectivity() const
{
    return connectivity();
}

void ManagerObject::SetLogging(const QString &level, const QString &domains)
{
    qCInfo(NETIFRC) << "SetLogging ignored:" << level << domains;
}

void ManagerObject::Reload(uint flags)
{
    qCInfo(NETIFRC) << "Reload(" << flags << ") ignored";
}

QDBusObjectPath ManagerObject::CheckpointCreate(const QList<QDBusObjectPath> &devicePaths, uint rollbackTimeout)
{
    Q_UNUSED(devicePaths);
    Q_UNUSED(rollbackTimeout);
    return nonePath();
}

void ManagerObject::CheckpointDestroy(const QDBusObjectPath &checkpoint)
{
    Q_UNUSED(checkpoint);
}

QVariantMap ManagerObject::CheckpointRollback(const QDBusObjectPath &checkpoint)
{
    Q_UNUSED(checkpoint);
    return QVariantMap();
}

QVariantMap ManagerObject::changedMap(const QStringList &props) const
{
    QVariantMap changed;
    for (const QString &prop : props) {
        if (prop == QLatin1String("State")) {
            changed.insert(prop, state());
        } else if (prop == QLatin1String("NetworkingEnabled")) {
            changed.insert(prop, networkingEnabled());
        } else if (prop == QLatin1String("WirelessEnabled")) {
            changed.insert(prop, wirelessEnabled());
        } else if (prop == QLatin1String("WirelessHardwareEnabled")) {
            changed.insert(prop, wirelessHardwareEnabled());
        } else if (prop == QLatin1String("Connectivity")) {
            changed.insert(prop, connectivity());
        } else if (prop == QLatin1String("PrimaryConnection")) {
            changed.insert(prop, QVariant::fromValue(primaryConnection()));
        } else if (prop == QLatin1String("PrimaryConnectionType")) {
            changed.insert(prop, primaryConnectionType());
        } else if (prop == QLatin1String("ActivatingConnection")) {
            changed.insert(prop, QVariant::fromValue(activatingConnection()));
        } else if (prop == QLatin1String("Devices") || prop == QLatin1String("AllDevices")) {
            changed.insert(prop, QVariant::fromValue(devices()));
        } else if (prop == QLatin1String("ActiveConnections")) {
            changed.insert(prop, QVariant::fromValue(activeConnections()));
        } else if (prop == QLatin1String("Metered")) {
            changed.insert(prop, metered());
        }
    }
    return changed;
}

} // namespace netifrcd
