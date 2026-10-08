// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/DeviceObject.h"

#include "common/Logging.h"
#include "model/Backend.h"
#include "model/Profile.h"

#include <QDBusMetaType>
#include <QFile>
#include <QTimer>
#include <QVariant>

namespace netifrcd
{

namespace
{

using ActiveConnectionRecord = Backend::ActiveConnectionRecord;
using DeviceRecord = Backend::DeviceRecord;

InterfaceInfo deviceInfo(Backend *backend, const QString &ifname)
{
    return backend->device(ifname).info;
}

QList<QDBusObjectPath> accessPointPaths(const QStringList &apPaths)
{
    QList<QDBusObjectPath> paths;
    paths.reserve(apPaths.size());
    for (const QString &path : apPaths) {
        paths.append(QDBusObjectPath(path));
    }
    return paths;
}

/// qDBusRegisterMetaType() has to run once, before any Device object is exported.
void ensureDeviceStateReasonRegistered()
{
    static const bool once = [] {
        qDBusRegisterMetaType<DeviceStateReason>();
        return true;
    }();
    Q_UNUSED(once);
}

/// One of the kernel's per-interface counters. NetworkManager reports the same numbers
/// over netlink; reading them from sysfs keeps this independent of netifrc, which has
/// no notion of byte counters at all. A missing file (interface gone) reads as zero.
qulonglong readInterfaceCounter(const QString &ifname, const char *counter)
{
    QFile file(QStringLiteral("/sys/class/net/%1/statistics/%2").arg(ifname, QLatin1String(counter)));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    bool ok = false;
    const qulonglong value = file.readAll().trimmed().toULongLong(&ok);
    return ok ? value : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Device.StateReason marshalling
//
// The D-Bus signature is "(uu)": the state and the reason for the last transition.
// The operators must live in namespace netifrcd so ADL finds them when Qt marshals a
// QVariant holding a DeviceStateReason.
// ---------------------------------------------------------------------------

QDBusArgument &operator<<(QDBusArgument &argument, const DeviceStateReason &value)
{
    argument.beginStructure();
    argument << value.state << value.reason;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, DeviceStateReason &value)
{
    argument.beginStructure();
    argument >> value.state >> value.reason;
    argument.endStructure();
    return argument;
}

// ---------------------------------------------------------------------------
// DeviceObject
// ---------------------------------------------------------------------------

DeviceObject::DeviceObject(Backend *backend, const QString &ifname, QObject *parent)
    : ObjectBase(backend->devicePathFor(ifname), parent)
    , m_backend(backend)
    , m_ifname(ifname)
{
    Q_ASSERT(m_backend);

    ensureDeviceStateReasonRegistered();

    const DeviceRecord record = m_backend->device(ifname);
    m_lastState = static_cast<uint>(record.state);

    if (record.info.deviceType == NM_DEVICE_TYPE_WIFI) {
        new DeviceWirelessAdaptor(m_backend, m_ifname, this);
    } else if (record.info.deviceType == NM_DEVICE_TYPE_ETHERNET) {
        new DeviceWiredAdaptor(m_backend, m_ifname, this);
    }

    // NetworkManager exports Device.Statistics on every device, not just the ones with
    // a type-specific interface, so this is unconditional.
    new DeviceStatisticsAdaptor(m_backend, m_ifname, this);

    qCDebug(NETIFRC) << "DeviceObject" << ifname << "deviceType" << record.info.deviceType << "adaptors"
                     << findChildren<QDBusAbstractAdaptor *>().size();

    connect(m_backend, &Backend::deviceChanged, this, [this](const QString &path, const QStringList &props) {
        if (path != m_path) {
            return;
        }
        if (props.contains(QLatin1String("State"))) {
            const uint newState = state();
            Q_EMIT StateChanged(newState, m_lastState, stateReason().reason);
            m_lastState = newState;
        }
        notify(QLatin1String(IfaceDevice), changedMap(props));
    });
}

QString DeviceObject::udi() const
{
    return QStringLiteral("/sys/class/net/%1").arg(m_ifname);
}

QString DeviceObject::interface() const
{
    return m_ifname;
}

QString DeviceObject::ipInterface() const
{
    const InterfaceInfo info = deviceInfo(m_backend, m_ifname);
    return info.name.isEmpty() ? m_ifname : info.name;
}

QString DeviceObject::driver() const
{
    return deviceInfo(m_backend, m_ifname).driver;
}

QString DeviceObject::driverVersion() const
{
    return QString();
}

QString DeviceObject::firmwareVersion() const
{
    return QString();
}

uint DeviceObject::capabilities() const
{
    return NM_DEVICE_CAP_NM_SUPPORTED | NM_DEVICE_CAP_CARRIER_DETECT;
}

uint DeviceObject::ip4Address() const
{
    return 0;
}

uint DeviceObject::state() const
{
    return static_cast<uint>(m_backend->device(m_ifname).state);
}

QDBusObjectPath DeviceObject::activeConnection() const
{
    return toPath(m_backend->device(m_ifname).activeConnectionPath);
}

QList<QDBusObjectPath> DeviceObject::availableConnections() const
{
    QList<QDBusObjectPath> paths;
    const QList<Profile> profiles = m_backend->profiles();
    for (const Profile &profile : profiles) {
        if (m_backend->targetInterfaceFor(profile) != m_ifname) {
            continue;
        }
        const QString settingsPath = m_backend->settingsPathFor(profile.uuid);
        if (!settingsPath.isEmpty()) {
            paths.append(toPath(settingsPath));
        }
    }
    return paths;
}

bool DeviceObject::managed() const
{
    return true;
}

bool DeviceObject::autoconnect() const
{
    return true;
}

bool DeviceObject::firmwareMissing() const
{
    return false;
}

bool DeviceObject::nmPluginMissing() const
{
    return false;
}

uint DeviceObject::deviceType() const
{
    return static_cast<uint>(deviceInfo(m_backend, m_ifname).deviceType);
}

uint DeviceObject::metered() const
{
    // Backend has no per-device metered state; report "unknown" rather than guess.
    return NM_METERED_UNKNOWN;
}

QList<QVariantMap> DeviceObject::lldpNeighbors() const
{
    return {};
}

bool DeviceObject::real() const
{
    return true;
}

uint DeviceObject::ip4Connectivity() const
{
    if (state() != NM_DEVICE_STATE_ACTIVATED) {
        return NM_CONNECTIVITY_UNKNOWN;
    }
    return static_cast<uint>(m_backend->connectivity());
}

uint DeviceObject::ip6Connectivity() const
{
    return NM_CONNECTIVITY_UNKNOWN;
}

uint DeviceObject::interfaceFlags() const
{
    const InterfaceInfo info = deviceInfo(m_backend, m_ifname);
    uint flags = NM_DEVICE_INTERFACE_FLAG_NONE;
    if (info.up) {
        flags |= NM_DEVICE_INTERFACE_FLAG_UP;
    }
    if (info.carrier) {
        flags |= NM_DEVICE_INTERFACE_FLAG_LOWER_UP | NM_DEVICE_INTERFACE_FLAG_CARRIER;
    }
    return flags;
}

QString DeviceObject::hwAddress() const
{
    return deviceInfo(m_backend, m_ifname).mac;
}

uint DeviceObject::mtu() const
{
    const int mtu = deviceInfo(m_backend, m_ifname).mtu;
    return mtu > 0 ? static_cast<uint>(mtu) : 0;
}

DeviceStateReason DeviceObject::stateReason() const
{
    const DeviceRecord record = m_backend->device(m_ifname);
    return {static_cast<uint>(record.state), static_cast<uint>(record.stateReason)};
}

void DeviceObject::Reapply(const SettingsMap &connection, uint versionId, uint flags)
{
    Q_UNUSED(connection);
    Q_UNUSED(versionId);
    Q_UNUSED(flags);
    qCInfo(NETIFRC) << "Reapply on" << m_ifname << "ignored; reactivation is required";
}

QVariantMap DeviceObject::GetAppliedConnection(uint flags) const
{
    Q_UNUSED(flags);

    const DeviceRecord record = m_backend->device(m_ifname);
    if (record.activeConnectionPath.isEmpty()) {
        return QVariantMap();
    }
    const ActiveConnectionRecord active = m_backend->activeConnectionByPath(record.activeConnectionPath);
    if (active.settingsPath.isEmpty()) {
        return QVariantMap();
    }

    // NetworkManager returns a{sa{sv}} here. We flatten the settings map into a
    // single QVariantMap, which is what the D-Bus signature below describes.
    const SettingsMap settings = m_backend->settingsForPath(active.settingsPath);
    QVariantMap flattened;
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        flattened.insert(it.key(), it.value());
    }
    return flattened;
}

void DeviceObject::Disconnect()
{
    const DeviceRecord record = m_backend->device(m_ifname);
    if (record.activeConnectionPath.isEmpty()) {
        return;
    }
    QString error;
    if (!m_backend->deactivateConnection(record.activeConnectionPath, &error)) {
        qCWarning(NETIFRC) << "Disconnect on" << m_ifname << "failed:" << error;
    }
}

void DeviceObject::Delete()
{
    qCInfo(NETIFRC) << "Delete on" << m_ifname << "ignored; netifrcd does not remove hardware devices";
}

QVariantMap DeviceObject::changedMap(const QStringList &props) const
{
    QVariantMap changed;
    for (const QString &prop : props) {
        if (prop == QLatin1String("State")) {
            changed.insert(prop, state());
        } else if (prop == QLatin1String("StateReason")) {
            changed.insert(prop, QVariant::fromValue(stateReason()));
        } else if (prop == QLatin1String("ActiveConnection")) {
            changed.insert(prop, QVariant::fromValue(activeConnection()));
        } else if (prop == QLatin1String("InterfaceFlags")) {
            changed.insert(prop, interfaceFlags());
        } else if (prop == QLatin1String("HwAddress")) {
            changed.insert(prop, hwAddress());
        } else if (prop == QLatin1String("Mtu")) {
            changed.insert(prop, mtu());
        } else if (prop == QLatin1String("Ip4Connectivity")) {
            changed.insert(prop, ip4Connectivity());
        } else if (prop == QLatin1String("Ip6Connectivity")) {
            changed.insert(prop, ip6Connectivity());
        } else if (prop == QLatin1String("AvailableConnections")) {
            changed.insert(prop, QVariant::fromValue(availableConnections()));
        } else if (prop == QLatin1String("Metered")) {
            changed.insert(prop, metered());
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// DeviceWiredAdaptor
// ---------------------------------------------------------------------------

DeviceWiredAdaptor::DeviceWiredAdaptor(Backend *backend, const QString &ifname, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_backend(backend)
    , m_ifname(ifname)
    , m_path(backend->devicePathFor(ifname))
{
    connect(backend, &Backend::deviceChanged, this, [this](const QString &path, const QStringList &props) {
        if (path != m_path) {
            return;
        }
        QVariantMap changed;
        for (const QString &prop : props) {
            if (prop == QLatin1String("HwAddress")) {
                changed.insert(prop, hwAddress());
            } else if (prop == QLatin1String("Carrier")) {
                changed.insert(prop, carrier());
            }
        }
        if (!changed.isEmpty()) {
            ObjectBase::sendPropertiesChanged(m_path, QLatin1String(IfaceDeviceWired), changed);
        }
    });
}

QString DeviceWiredAdaptor::hwAddress() const
{
    return deviceInfo(m_backend, m_ifname).mac;
}

QString DeviceWiredAdaptor::permHwAddress() const
{
    // Only the current MAC is available from iproute2; the permanent address is
    // not exposed by InterfaceWatcher.
    return deviceInfo(m_backend, m_ifname).mac;
}

uint DeviceWiredAdaptor::speed() const
{
    // Link speed is not tracked by InterfaceWatcher.
    return 0;
}

bool DeviceWiredAdaptor::carrier() const
{
    return deviceInfo(m_backend, m_ifname).carrier;
}

QStringList DeviceWiredAdaptor::s390Subchannels() const
{
    return {};
}

// ---------------------------------------------------------------------------
// DeviceWirelessAdaptor
// ---------------------------------------------------------------------------

DeviceWirelessAdaptor::DeviceWirelessAdaptor(Backend *backend, const QString &ifname, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_backend(backend)
    , m_ifname(ifname)
    , m_path(backend->devicePathFor(ifname))
    , m_knownAccessPoints(backend->device(ifname).apPaths)
{
    connect(backend, &Backend::deviceChanged, this, [this](const QString &path, const QStringList &props) {
        if (path != m_path) {
            return;
        }
        const QVariantMap changed = changedMap(props);
        if (!changed.isEmpty()) {
            ObjectBase::sendPropertiesChanged(m_path, QLatin1String(IfaceDeviceWireless), changed);
        }
    });

    connect(backend, &Backend::accessPointListChanged, this, [this](const QString &ifname) {
        if (ifname != m_ifname) {
            return;
        }
        const QStringList current = m_backend->device(m_ifname).apPaths;
        for (const QString &path : current) {
            if (!m_knownAccessPoints.contains(path)) {
                Q_EMIT AccessPointAdded(QDBusObjectPath(path));
            }
        }
        for (const QString &path : m_knownAccessPoints) {
            if (!current.contains(path)) {
                Q_EMIT AccessPointRemoved(QDBusObjectPath(path));
            }
        }
        m_knownAccessPoints = current;

        QVariantMap changed;
        changed.insert(QStringLiteral("AccessPoints"), QVariant::fromValue(accessPoints()));
        changed.insert(QStringLiteral("ActiveAccessPoint"), QVariant::fromValue(activeAccessPoint()));
        changed.insert(QStringLiteral("LastScan"), lastScan());
        ObjectBase::sendPropertiesChanged(m_path, QLatin1String(IfaceDeviceWireless), changed);
    });
}

QString DeviceWirelessAdaptor::hwAddress() const
{
    return deviceInfo(m_backend, m_ifname).mac;
}

QString DeviceWirelessAdaptor::permHwAddress() const
{
    return deviceInfo(m_backend, m_ifname).mac;
}

uint DeviceWirelessAdaptor::bitrate() const
{
    const int bitrate = deviceInfo(m_backend, m_ifname).bitrate;
    return bitrate > 0 ? static_cast<uint>(bitrate) : 0;
}

uint DeviceWirelessAdaptor::mode() const
{
    return NM_802_11_MODE_INFRA;
}

QList<QDBusObjectPath> DeviceWirelessAdaptor::accessPoints() const
{
    return accessPointPaths(m_backend->device(m_ifname).apPaths);
}

QDBusObjectPath DeviceWirelessAdaptor::activeAccessPoint() const
{
    return toPath(m_backend->device(m_ifname).activeApPath);
}

uint DeviceWirelessAdaptor::wirelessCapabilities() const
{
    return NM_WIFI_DEVICE_CAP_CIPHER_WEP40 | NM_WIFI_DEVICE_CAP_CIPHER_WEP104 | NM_WIFI_DEVICE_CAP_CIPHER_TKIP
        | NM_WIFI_DEVICE_CAP_CIPHER_CCMP | NM_WIFI_DEVICE_CAP_WPA | NM_WIFI_DEVICE_CAP_RSN | NM_WIFI_DEVICE_CAP_FREQ_VALID
        | NM_WIFI_DEVICE_CAP_FREQ_2GHZ | NM_WIFI_DEVICE_CAP_FREQ_5GHZ;
}

qint64 DeviceWirelessAdaptor::lastScan() const
{
    // Seconds since boot of the last completed scan; -1 when the device has never
    // been scanned. Backend records this every time refreshAccessPoints() runs.
    return m_backend->lastScanTime(m_ifname);
}

QList<QDBusObjectPath> DeviceWirelessAdaptor::GetAccessPoints() const
{
    return accessPoints();
}

QList<QDBusObjectPath> DeviceWirelessAdaptor::GetAllAccessPoints() const
{
    return accessPoints();
}

void DeviceWirelessAdaptor::RequestScan(const QVariantMap &options)
{
    Q_UNUSED(options);
    QString error;
    if (!m_backend->requestScan(m_ifname, &error)) {
        qCWarning(NETIFRC) << "RequestScan on" << m_ifname << "failed:" << error;
    }
}

QVariantMap DeviceWirelessAdaptor::changedMap(const QStringList &props) const
{
    QVariantMap changed;
    for (const QString &prop : props) {
        if (prop == QLatin1String("HwAddress")) {
            changed.insert(prop, hwAddress());
        } else if (prop == QLatin1String("Bitrate")) {
            changed.insert(prop, bitrate());
        } else if (prop == QLatin1String("Mode")) {
            changed.insert(prop, mode());
        } else if (prop == QLatin1String("AccessPoints")) {
            changed.insert(prop, QVariant::fromValue(accessPoints()));
        } else if (prop == QLatin1String("ActiveAccessPoint")) {
            changed.insert(prop, QVariant::fromValue(activeAccessPoint()));
        } else if (prop == QLatin1String("WirelessCapabilities")) {
            changed.insert(prop, wirelessCapabilities());
        } else if (prop == QLatin1String("LastScan")) {
            changed.insert(prop, lastScan());
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// DeviceStatisticsAdaptor
// ---------------------------------------------------------------------------

DeviceStatisticsAdaptor::DeviceStatisticsAdaptor(Backend *backend, const QString &ifname, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_ifname(ifname)
    , m_path(backend->devicePathFor(ifname))
    , m_timer(new QTimer(this))
    // Seeded with the current counters so the first poll only announces real movement.
    , m_lastRx(readInterfaceCounter(ifname, "rx_bytes"))
    , m_lastTx(readInterfaceCounter(ifname, "tx_bytes"))
{
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &DeviceStatisticsAdaptor::poll);
}

uint DeviceStatisticsAdaptor::refreshRateMs() const
{
    return m_refreshRateMs;
}

void DeviceStatisticsAdaptor::setRefreshRateMs(uint refreshRate)
{
    if (m_refreshRateMs == refreshRate) {
        return;
    }
    m_refreshRateMs = refreshRate;

    if (refreshRate == 0) {
        m_timer->stop();
    } else {
        m_timer->start(static_cast<int>(refreshRate));
    }

    QVariantMap changed;
    changed.insert(QStringLiteral("RefreshRateMs"), refreshRate);
    ObjectBase::sendPropertiesChanged(m_path, QLatin1String(IfaceDeviceStatistics), changed);
}

qulonglong DeviceStatisticsAdaptor::rxBytes() const
{
    return readInterfaceCounter(m_ifname, "rx_bytes");
}

qulonglong DeviceStatisticsAdaptor::txBytes() const
{
    return readInterfaceCounter(m_ifname, "tx_bytes");
}

void DeviceStatisticsAdaptor::poll()
{
    const qulonglong rx = rxBytes();
    const qulonglong tx = txBytes();

    QVariantMap changed;
    if (rx != m_lastRx) {
        m_lastRx = rx;
        changed.insert(QStringLiteral("RxBytes"), rx);
    }
    if (tx != m_lastTx) {
        m_lastTx = tx;
        changed.insert(QStringLiteral("TxBytes"), tx);
    }
    if (!changed.isEmpty()) {
        ObjectBase::sendPropertiesChanged(m_path, QLatin1String(IfaceDeviceStatistics), changed);
    }
}

} // namespace netifrcd
