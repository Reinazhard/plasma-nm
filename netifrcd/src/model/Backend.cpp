// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "model/Backend.h"

#include "common/Logging.h"
#include "dbus/AccessPointObject.h"
#include "dbus/ActiveConnectionObject.h"
#include "dbus/AgentManagerObject.h"
#include "dbus/ConfigObjects.h"
#include "dbus/DeviceObject.h"
#include "dbus/ManagerObject.h"
#include "dbus/ObjectManagerObject.h"
#include "dbus/SettingsObject.h"
#include "model/Profile.h"
#include "model/ProfileStore.h"
#include "netifrc/NetifrcConfig.h"
#include "netifrc/ServiceControl.h"

#include <QDBusConnection>
#include <QDBusAbstractAdaptor>
#include <QDBusMetaType>
#include <QMetaProperty>
#include <QTimer>

#include <ctime>

namespace netifrcd
{

namespace
{
constexpr int kPollIntervalMs = 3000;
// How often we refresh the access-point list on our own. NetworkManager scans
// periodically too; clients that need a fresh list (the applet when it opens) call
// RequestScan, this is just so the list is never stale/empty to begin with.
constexpr int kScanIntervalMs = 60000;

/// Reads Q_CLASSINFO("D-Bus Interface", ...) off an exported object.
QString interfaceNameOf(const QObject *obj)
{
    const QMetaObject *mo = obj->metaObject();
    const int idx = mo->indexOfClassInfo("D-Bus Interface");
    if (idx < 0) {
        return {};
    }
    return QString::fromUtf8(mo->classInfo(idx).value());
}

/// Reflects an object's Q_PROPERTYs into the a{sv} shape ObjectManager wants. Doing it
/// generically means the property lists live in exactly one place - the class headers.
QVariantMap propertiesOf(const QObject *obj)
{
    QVariantMap map;
    const QMetaObject *mo = obj->metaObject();
    for (int i = 0; i < mo->propertyCount(); ++i) {
        const QMetaProperty prop = mo->property(i);
        if (!prop.isReadable()) {
            continue;
        }
        const QVariant value = prop.read(obj);
        if (!value.isValid()) {
            continue;
        }
        map.insert(QString::fromUtf8(prop.name()), value);
    }
    return map;
}

/// Devices the applet should see. Loopback is deliberately excluded: NetworkManager
/// does not export it as a manageable device either.
bool isExportable(const InterfaceInfo &info)
{
    return !info.loopback;
}
} // namespace

// ---------------------------------------------------------------------------
// Exported object bookkeeping
// ---------------------------------------------------------------------------

struct Backend::Dbus {
    Backend *b = nullptr;
    QDBusConnection bus = QDBusConnection::systemBus();

    ManagerObject *manager = nullptr;
    SettingsObject *settings = nullptr;
    AgentManagerObject *agentManager = nullptr;
    ObjectManagerObject *objectManager = nullptr;

    QHash<QString, DeviceObject *> devices;                          // ifname -> device
    QHash<QString, QPair<Ip4ConfigObject *, Dhcp4ConfigObject *>> configs; // ifname -> ip configs
    QHash<QString, AccessPointObject *> accessPoints;                // ap path -> ap
    QHash<QString, ActiveConnectionObject *> actives;                // active path -> active
    QHash<QString, SettingsConnectionObject *> settingsConnections;  // settings path -> connection

    bool exportObject(const QString &path, QObject *obj)
    {
        // ExportAdaptors is a separate flag and is NOT implied by the ExportAll* set.
        // Without it, the Device.Wired / Device.Wireless adaptors never reach the bus.
        const bool ok = bus.registerObject(path,
                                           obj,
                                           QDBusConnection::ExportAdaptors | QDBusConnection::ExportAllProperties
                                               | QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        if (!ok) {
            qCWarning(NETIFRC) << "failed to export" << path;
        }
        return ok;
    }

    void dropObject(const QString &path)
    {
        bus.unregisterObject(path);
    }

    void syncDevices(const QList<Backend::DeviceRecord> &records)
    {
        QSet<QString> wanted;
        for (const Backend::DeviceRecord &r : records) {
            wanted.insert(r.ifname);
            if (!devices.contains(r.ifname)) {
                auto *dev = new DeviceObject(b, r.ifname, b);
                exportObject(dev->path(), dev);
                devices.insert(r.ifname, dev);
            }
            // IP configuration objects appear once the interface has addresses.
            const bool needsConfig = !r.info.addresses.isEmpty();
            if (needsConfig && !configs.contains(r.ifname)) {
                const QString ipPath = ip4ConfigPath(r.id);
                const QString dhcpPath = dhcp4ConfigPath(r.id);
                auto *ip4 = new Ip4ConfigObject(b, ipPath, r.ifname, b);
                auto *dhcp = new Dhcp4ConfigObject(b, dhcpPath, r.ifname, b);
                exportObject(ipPath, ip4);
                exportObject(dhcpPath, dhcp);
                configs.insert(r.ifname, {ip4, dhcp});
            }
        }

        const auto ifnames = devices.keys();
        for (const QString &ifname : ifnames) {
            if (wanted.contains(ifname)) {
                continue;
            }
            auto *dev = devices.take(ifname);
            dropObject(dev->path());
            dev->deleteLater();
            if (configs.contains(ifname)) {
                const auto pair = configs.take(ifname);
                dropObject(pair.first->path());
                dropObject(pair.second->path());
                pair.first->deleteLater();
                pair.second->deleteLater();
            }
        }
    }

    void syncAccessPoints(const QList<Backend::AccessPointRecord> &records)
    {
        QSet<QString> wanted;
        for (const Backend::AccessPointRecord &r : records) {
            wanted.insert(r.path);
            if (!accessPoints.contains(r.path)) {
                auto *ap = new AccessPointObject(b, r.ifname, r.info.bssid, b);
                exportObject(ap->path(), ap);
                accessPoints.insert(r.path, ap);
            }
        }
        const auto paths = accessPoints.keys();
        for (const QString &path : paths) {
            if (wanted.contains(path)) {
                continue;
            }
            auto *ap = accessPoints.take(path);
            dropObject(ap->path());
            ap->deleteLater();
        }
    }

    void syncActiveConnections(const QList<Backend::ActiveConnectionRecord> &records)
    {
        QSet<QString> wanted;
        for (const Backend::ActiveConnectionRecord &r : records) {
            wanted.insert(r.path);
            if (!actives.contains(r.path)) {
                auto *ac = new ActiveConnectionObject(b, r.path, b);
                exportObject(ac->path(), ac);
                actives.insert(r.path, ac);
            }
        }
        const auto paths = actives.keys();
        for (const QString &path : paths) {
            if (wanted.contains(path)) {
                continue;
            }
            auto *ac = actives.take(path);
            dropObject(ac->path());
            ac->deleteLater();
        }
    }

    void syncSettingsConnections(const QList<Profile> &profiles)
    {
        QSet<QString> wanted;
        for (const Profile &p : profiles) {
            const QString path = b->settingsPathFor(p.uuid);
            if (path.isEmpty()) {
                continue;
            }
            wanted.insert(path);
            if (!settingsConnections.contains(path)) {
                auto *sc = new SettingsConnectionObject(b, path, b);
                exportObject(sc->path(), sc);
                settingsConnections.insert(path, sc);
            }
        }
        const auto paths = settingsConnections.keys();
        for (const QString &path : paths) {
            if (wanted.contains(path)) {
                continue;
            }
            auto *sc = settingsConnections.take(path);
            dropObject(sc->path());
            sc->deleteLater();
        }
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Backend::Backend(QObject *parent)
    : QObject(parent)
    , m_config(new NetifrcConfig())
    , m_services(new ServiceControl(this))
    , m_watcher(new InterfaceWatcher(this))
    , m_profiles(new ProfileStore(this))
{
    connect(m_watcher, &InterfaceWatcher::changed, this, &Backend::onInterfacesChanged);
    connect(m_services,
            &ServiceControl::actionFinished,
            this,
            [this](const QString &ifname, ServiceControl::Action action, bool success, const QString &message) {
                onServiceActionFinished(ifname, static_cast<int>(action), success, message);
            });
    connect(m_profiles, &ProfileStore::changed, this, [this] {
        // Keep the exported Settings.Connection objects in step with the store before
        // announcing the change, so a client reacting to the signal already sees them.
        syncSettingsConnections();
        Q_EMIT settingsChanged();
    });
}

Backend::~Backend()
{
    delete m_dbus;
    delete m_config;
}

bool Backend::start(QString *error)
{
    if (!m_config->load(error)) {
        return false;
    }
    if (!m_profiles->load(error)) {
        return false;
    }

    importNetifrcInterfaces();

    m_watcher->setExtraManaged(m_config->knownInterfaces());
    m_watcher->refresh();
    reconcile();

    auto *poll = new QTimer(this);
    poll->setInterval(kPollIntervalMs);
    connect(poll, &QTimer::timeout, m_watcher, &InterfaceWatcher::refresh);
    poll->start();

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(kScanIntervalMs);
    connect(m_scanTimer, &QTimer::timeout, this, &Backend::scanWirelessDevices);
    m_scanTimer->start();

    // Populate the access-point list once the event loop runs - that is, after
    // registerObjects() has exported the device objects - so clients see the APs
    // without having to call RequestScan first.
    QTimer::singleShot(0, this, &Backend::scanWirelessDevices);

    qCInfo(NETIFRC) << "backend started with" << m_devices.size() << "device(s) and" << m_profiles->profiles().size() << "profile(s)";
    return true;
}

void Backend::importNetifrcInterfaces()
{
    bool added = false;
    for (const QString &ifname : m_config->knownInterfaces()) {
        // Already modelled by a profile?
        bool known = false;
        for (const Profile &p : m_profiles->profiles()) {
            if (p.interfaceName == ifname) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }
        const Profile p = Profile::fromNetifrc(ifname, m_config->configFor(ifname));
        if (p.isValid()) {
            m_profiles->addOrUpdate(p);
            added = true;
            qCInfo(NETIFRC) << "imported netifrc interface" << ifname << "as a profile";
        }
    }
    if (added) {
        m_profiles->save(nullptr);
    }
}

// ---------------------------------------------------------------------------
// D-Bus registration
// ---------------------------------------------------------------------------

bool Backend::registerObjects(QString *error)
{
    if (m_registered) {
        return true;
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        if (error) {
            *error = QStringLiteral("cannot connect to the D-Bus system bus");
        }
        return false;
    }

    if (!bus.registerService(QLatin1String(Service))) {
        if (error) {
            *error = QStringLiteral("cannot own %1: %2 (is NetworkManager still running?)")
                         .arg(QLatin1String(Service), bus.lastError().message());
        }
        return false;
    }

    m_dbus = new Dbus;
    m_dbus->b = this;

    m_dbus->manager = new ManagerObject(this, this);
    m_dbus->exportObject(m_dbus->manager->path(), m_dbus->manager);

    m_dbus->settings = new SettingsObject(this, this);
    m_dbus->exportObject(m_dbus->settings->path(), m_dbus->settings);

    m_dbus->agentManager = new AgentManagerObject(this, this);
    m_dbus->exportObject(m_dbus->agentManager->path(), m_dbus->agentManager);

    // libnm queries this before anything else and gives up if it is missing.
    m_dbus->objectManager = new ObjectManagerObject(this, this);
    m_dbus->exportObject(m_dbus->objectManager->path(), m_dbus->objectManager);

    m_dbus->syncDevices(m_devices);
    m_dbus->syncAccessPoints(m_accessPoints);
    m_dbus->syncActiveConnections(m_activeConnections);
    m_dbus->syncSettingsConnections(m_profiles->profiles());

    m_registered = true;
    qCInfo(NETIFRC) << "registered" << QLatin1String(Service) << "on the system bus";
    return true;
}

void Backend::syncSettingsConnections()
{
    if (!m_dbus) {
        return; // called from the profile store's changed() before D-Bus registration
    }
    m_dbus->syncSettingsConnections(m_profiles->profiles());
}

// ---------------------------------------------------------------------------
// org.freedesktop.DBus.ObjectManager
// ---------------------------------------------------------------------------

QMap<QDBusObjectPath, QMap<QString, QVariantMap>> Backend::managedObjects() const
{
    QMap<QDBusObjectPath, QMap<QString, QVariantMap>> out;
    if (!m_dbus) {
        return out;
    }

    auto add = [&out](QObject *obj, const QString &path) {
        if (!obj || path.isEmpty()) {
            return;
        }
        const QString iface = interfaceNameOf(obj);
        if (iface.isEmpty()) {
            return;
        }
        out[QDBusObjectPath(path)].insert(iface, propertiesOf(obj));
    };

    add(m_dbus->manager, m_dbus->manager->path());
    add(m_dbus->settings, m_dbus->settings->path());
    add(m_dbus->agentManager, m_dbus->agentManager->path());

    for (DeviceObject *device : std::as_const(m_dbus->devices)) {
        add(device, device->path());
        // Device.Wired / Device.Wireless live on the same path as adaptors.
        const auto adaptors = device->findChildren<QDBusAbstractAdaptor *>();
        for (QDBusAbstractAdaptor *adaptor : adaptors) {
            add(adaptor, device->path());
        }
    }

    for (const auto &pair : std::as_const(m_dbus->configs)) {
        add(pair.first, pair.first->path());
        add(pair.second, pair.second->path());
    }

    for (AccessPointObject *ap : std::as_const(m_dbus->accessPoints)) {
        add(ap, ap->path());
    }
    for (ActiveConnectionObject *ac : std::as_const(m_dbus->actives)) {
        add(ac, ac->path());
    }
    for (SettingsConnectionObject *sc : std::as_const(m_dbus->settingsConnections)) {
        add(sc, sc->path());
    }

    return out;
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

void Backend::onInterfacesChanged()
{
    reconcile();
}

void Backend::onServiceActionFinished(const QString &ifname, int action, bool success, const QString &message)
{
    if (!success) {
        qCWarning(NETIFRC) << "netifrc action failed for" << ifname << ":" << message;
    }
    if (action == static_cast<int>(ServiceControl::Action::Stop) && success) {
        // The address is gone; drop the active connection eagerly so the UI reacts now.
        const QString path = activeConnectionPathFor(ifname);
        if (!path.isEmpty()) {
            m_activeConnections.removeIf([&path](const ActiveConnectionRecord &r) {
                return r.path == path;
            });
            Q_EMIT activeConnectionRemoved(path);
        }
    }
    // Give the kernel a moment to settle, then resynchronise.
    QTimer::singleShot(500, this, &Backend::reconcile);
}

void Backend::reconcile()
{
    const QList<InterfaceInfo> ifaces = m_watcher->interfaces();

    QList<DeviceRecord> freshDevices;
    QList<ActiveConnectionRecord> freshActives;

    for (const InterfaceInfo &info : ifaces) {
        if (!isExportable(info)) {
            continue;
        }

        DeviceRecord rec;
        rec.ifname = info.name;
        rec.info = info;
        if (!m_deviceIds.contains(info.name)) {
            m_deviceIds.insert(info.name, m_nextDeviceId++);
        }
        rec.id = m_deviceIds.value(info.name);

        const bool hasAddress = !info.addresses.isEmpty();

        if (!info.up) {
            rec.state = NM_DEVICE_STATE_UNAVAILABLE;
            rec.stateReason = NM_DEVICE_STATE_REASON_NONE;
        } else if (info.deviceType == NM_DEVICE_TYPE_ETHERNET && !info.carrier) {
            rec.state = NM_DEVICE_STATE_UNAVAILABLE;
            rec.stateReason = NM_DEVICE_STATE_REASON_CARRIER;
        } else if (hasAddress) {
            rec.state = NM_DEVICE_STATE_ACTIVATED;
            rec.stateReason = NM_DEVICE_STATE_REASON_NONE;
        } else {
            rec.state = NM_DEVICE_STATE_DISCONNECTED;
            rec.stateReason = NM_DEVICE_STATE_REASON_NONE;
        }

        if (rec.state == NM_DEVICE_STATE_ACTIVATED) {
            if (!m_activeIds.contains(info.name)) {
                m_activeIds.insert(info.name, m_nextActiveId++);
            }
            const int activeId = m_activeIds.value(info.name);

            ActiveConnectionRecord ar;
            ar.id = activeId;
            ar.path = activeConnectionPath(activeId);
            ar.deviceIfname = info.name;
            ar.devicePath = devicePath(rec.id);
            ar.state = NM_ACTIVE_CONNECTION_STATE_ACTIVATED;

            // Attribute the activation to whichever profile targets this interface.
            for (const Profile &p : m_profiles->profiles()) {
                if (targetInterfaceFor(p) == info.name) {
                    ar.profileUuid = p.uuid;
                    ar.settingsPath = settingsPathFor(p.uuid);
                    break;
                }
            }

            rec.activeConnectionPath = ar.path;
            freshActives.append(ar);
        }

        if (info.deviceType == NM_DEVICE_TYPE_WIFI) {
            const QList<AccessPointInfo> aps = m_scanCache.value(info.name);
            for (const AccessPointInfo &ap : aps) {
                rec.apPaths.append(accessPointPathFor(info.name, ap.bssid));
                if (!info.ssid.isEmpty() && ap.ssid == info.ssid) {
                    rec.activeApPath = accessPointPathFor(info.name, ap.bssid);
                }
            }
        }

        freshDevices.append(rec);
    }

    // Work out what changed before publishing the new state.
    QSet<QString> oldPaths;
    for (const DeviceRecord &d : m_devices) {
        oldPaths.insert(devicePath(d.id));
    }
    QSet<QString> newPaths;
    for (const DeviceRecord &d : freshDevices) {
        newPaths.insert(devicePath(d.id));
    }

    const QString oldPrimary = primaryConnectionPath();
    const int oldState = managerState();
    const int oldConnectivity = connectivity();

    m_devices = freshDevices;
    m_activeConnections = freshActives;

    if (m_dbus) {
        m_dbus->syncDevices(m_devices);
        m_dbus->syncActiveConnections(m_activeConnections);
    }

    for (const QString &p : newPaths) {
        if (!oldPaths.contains(p)) {
            Q_EMIT deviceAdded(p);
        }
    }
    for (const QString &p : oldPaths) {
        if (!newPaths.contains(p)) {
            Q_EMIT deviceRemoved(p);
        }
    }
    for (const DeviceRecord &d : m_devices) {
        Q_EMIT deviceChanged(devicePath(d.id),
                             QStringList{QStringLiteral("State"),
                                         QStringLiteral("StateReason"),
                                         QStringLiteral("Ip4Config"),
                                         QStringLiteral("ActiveConnection")});
    }

    if (oldState != managerState() || oldConnectivity != connectivity() || oldPrimary != primaryConnectionPath()) {
        Q_EMIT managerChanged(QStringList{QStringLiteral("State"),
                                          QStringLiteral("Connectivity"),
                                          QStringLiteral("PrimaryConnection"),
                                          QStringLiteral("ActiveConnections"),
                                          QStringLiteral("Devices")});
    }
}

void Backend::scanWirelessDevices()
{
    // refreshAccessPoints() reconciles and announces each device it scans, so this
    // loop only has to pick the devices. m_devices is copied because reconcile()
    // rebuilds it underneath us.
    const QList<DeviceRecord> devices = m_devices;
    for (const DeviceRecord &d : devices) {
        if (d.info.deviceType != NM_DEVICE_TYPE_WIFI) {
            continue;
        }
        // A scan briefly takes the radio off-channel, so never do it on a down
        // interface or while an activation/deactivation is in flight.
        if (!d.info.up || m_services->isBusy(d.ifname)) {
            continue;
        }
        refreshAccessPoints(d.ifname);
    }
}

void Backend::refreshAccessPoints(const QString &ifname)
{
    QString error;
    const QList<AccessPointInfo> aps = WifiScanner::scan(ifname, &error);
    if (!error.isEmpty()) {
        // A failed scan tells us nothing about the neighbourhood, so keep the list we
        // already have rather than blanking it. The periodic auto-scan makes this
        // matter: one transient iw failure must not empty the applet's AP list.
        qCWarning(NETIFRC) << "scan on" << ifname << "failed:" << error;
        return;
    }
    m_scanCache.insert(ifname, aps);

    // NetworkManager reports LastScan as seconds since boot, so record the same clock.
    struct timespec now {
    };
    if (clock_gettime(CLOCK_BOOTTIME, &now) == 0) {
        m_lastScan.insert(ifname, static_cast<qint64>(now.tv_sec));
    }

    QList<AccessPointRecord> records;
    for (const AccessPointInfo &ap : aps) {
        AccessPointRecord r;
        r.ifname = ifname;
        r.info = ap;
        r.path = accessPointPathFor(ifname, ap.bssid);
        // Keep AP ids stable across scans so object paths do not churn.
        bool found = false;
        for (const AccessPointRecord &existing : m_accessPoints) {
            if (existing.path == r.path) {
                r.id = existing.id;
                found = true;
                break;
            }
        }
        if (!found) {
            r.id = m_nextApId++;
        }
        records.append(r);
    }
    m_accessPoints = records;

    if (m_dbus) {
        m_dbus->syncAccessPoints(m_accessPoints);
    }

    // Rebuild the device records BEFORE announcing the change: the
    // accessPointListChanged handler diffs the new paths against the previous
    // apPaths to emit AccessPointAdded/AccessPointRemoved, and apPaths is only
    // recomputed by reconcile(). Announcing first would diff against the stale
    // list and the applet would never hear about a newly discovered AP.
    reconcile();
    Q_EMIT accessPointListChanged(ifname);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

QList<Backend::DeviceRecord> Backend::devices() const
{
    return m_devices;
}

Backend::DeviceRecord Backend::device(const QString &ifname) const
{
    for (const DeviceRecord &d : m_devices) {
        if (d.ifname == ifname) {
            return d;
        }
    }
    return {};
}

Backend::DeviceRecord Backend::deviceByPath(const QString &path) const
{
    for (const DeviceRecord &d : m_devices) {
        if (devicePath(d.id) == path) {
            return d;
        }
    }
    return {};
}

QString Backend::devicePathFor(const QString &ifname) const
{
    const auto it = m_deviceIds.constFind(ifname);
    if (it == m_deviceIds.constEnd()) {
        return {};
    }
    return devicePath(it.value());
}

QList<Backend::AccessPointRecord> Backend::accessPoints(const QString &ifname) const
{
    QList<AccessPointRecord> out;
    for (const AccessPointRecord &r : m_accessPoints) {
        if (ifname.isEmpty() || r.ifname == ifname) {
            out.append(r);
        }
    }
    return out;
}

Backend::AccessPointRecord Backend::accessPointByPath(const QString &path) const
{
    for (const AccessPointRecord &r : m_accessPoints) {
        if (r.path == path) {
            return r;
        }
    }
    return {};
}

QString Backend::accessPointPathFor(const QString &ifname, const QString &bssid) const
{
    for (const AccessPointRecord &r : m_accessPoints) {
        if (r.ifname == ifname && r.info.bssid.compare(bssid, Qt::CaseInsensitive) == 0) {
            return r.path;
        }
    }
    // Not scanned yet: derive a stable path from the device id and a hash of the BSSID.
    const int deviceId = m_deviceIds.value(ifname, 0);
    const int apId = qHash(bssid.toLower()) % 100000;
    Q_UNUSED(deviceId)
    return accessPointPath(apId);
}

qint64 Backend::lastScanTime(const QString &ifname) const
{
    // -1 tells the D-Bus layer "never scanned"; NetworkManager uses the same sentinel.
    return m_lastScan.value(ifname, -1);
}

QList<Backend::ActiveConnectionRecord> Backend::activeConnections() const
{
    return m_activeConnections;
}

Backend::ActiveConnectionRecord Backend::activeConnectionByPath(const QString &path) const
{
    for (const ActiveConnectionRecord &r : m_activeConnections) {
        if (r.path == path) {
            return r;
        }
    }
    return {};
}

QString Backend::activeConnectionPathFor(const QString &ifname) const
{
    for (const ActiveConnectionRecord &r : m_activeConnections) {
        if (r.deviceIfname == ifname) {
            return r.path;
        }
    }
    return {};
}

QList<Profile> Backend::profiles() const
{
    return m_profiles->profiles();
}

Profile Backend::profileByUuid(const QString &uuid) const
{
    return m_profiles->profileByUuid(uuid);
}

Profile Backend::profileBySettingsPath(const QString &path) const
{
    static const QString prefix = QStringLiteral("%1/").arg(QLatin1String(PathSettings));
    if (!path.startsWith(prefix)) {
        return {};
    }
    bool ok = false;
    const int id = path.mid(prefix.size()).toInt(&ok);
    if (!ok) {
        return {};
    }
    for (auto it = m_settingsIds.constBegin(); it != m_settingsIds.constEnd(); ++it) {
        if (it.value() == id) {
            return m_profiles->profileByUuid(it.key());
        }
    }
    return {};
}

QString Backend::settingsPathFor(const QString &uuid) const
{
    if (uuid.isEmpty() || !m_profiles->contains(uuid)) {
        return {};
    }
    // The id is a per-connection identity, not a position in the list: reusing indices
    // would renumber every later connection whenever one is removed, silently moving
    // objects out from under clients that cached their paths.
    auto it = m_settingsIds.constFind(uuid);
    if (it == m_settingsIds.constEnd()) {
        it = m_settingsIds.insert(uuid, m_nextSettingsId++);
    }
    return settingsConnectionPath(it.value());
}

QString Backend::targetInterfaceFor(const Profile &profile) const
{
    if (!profile.interfaceName.isEmpty()) {
        return profile.interfaceName;
    }
    for (const DeviceRecord &d : m_devices) {
        const bool wireless = d.info.deviceType == NM_DEVICE_TYPE_WIFI;
        if (wireless == profile.isWireless()) {
            return d.ifname;
        }
    }
    return {};
}

int Backend::managerState() const
{
    if (!m_networkingEnabled) {
        return NM_STATE_ASLEEP;
    }
    bool activated = false;
    bool withGateway = false;
    for (const DeviceRecord &d : m_devices) {
        if (d.state == NM_DEVICE_STATE_ACTIVATED) {
            activated = true;
            if (!d.info.gateway.isEmpty()) {
                withGateway = true;
            }
        }
    }
    if (withGateway) {
        return NM_STATE_CONNECTED_GLOBAL;
    }
    if (activated) {
        return NM_STATE_CONNECTED_LOCAL;
    }
    for (const DeviceRecord &d : m_devices) {
        if (m_services->isBusy(d.ifname)) {
            return NM_STATE_CONNECTING;
        }
    }
    return NM_STATE_DISCONNECTED;
}

int Backend::connectivity() const
{
    if (!m_networkingEnabled) {
        return NM_CONNECTIVITY_NONE;
    }
    bool activated = false;
    for (const DeviceRecord &d : m_devices) {
        if (d.state != NM_DEVICE_STATE_ACTIVATED) {
            continue;
        }
        activated = true;
        if (!d.info.gateway.isEmpty()) {
            return NM_CONNECTIVITY_FULL;
        }
    }
    return activated ? NM_CONNECTIVITY_LIMITED : NM_CONNECTIVITY_NONE;
}

bool Backend::networkingEnabled() const
{
    return m_networkingEnabled;
}

bool Backend::wirelessEnabled() const
{
    return m_wirelessEnabled;
}

bool Backend::wirelessHardwareEnabled() const
{
    return m_wirelessHardwareEnabled;
}

QString Backend::primaryConnectionPath() const
{
    for (const ActiveConnectionRecord &r : m_activeConnections) {
        const DeviceRecord d = device(r.deviceIfname);
        if (!d.info.gateway.isEmpty()) {
            return r.path;
        }
    }
    return m_activeConnections.isEmpty() ? QString() : m_activeConnections.first().path;
}

QString Backend::activatingConnectionPath() const
{
    for (const DeviceRecord &d : m_devices) {
        if (m_services->isBusy(d.ifname)) {
            return activeConnectionPathFor(d.ifname);
        }
    }
    return {};
}

bool Backend::isBusy(const QString &ifname) const
{
    return m_services->isBusy(ifname);
}

// ---------------------------------------------------------------------------
// netifrc config ownership
// ---------------------------------------------------------------------------

void Backend::claimInterface(const QString &ifname, const Profile &profile)
{
    // Ownership rule: an explicit user action (activating or editing a connection that
    // targets this interface) is the user telling netifrcd to drive it, so we write the
    // interface into our managed block. That block is emitted after the administrator's
    // own lines, so our assignments win for every variable we set; their lines are never
    // rewritten or deleted, and dropping the interface from the block (or deleting the
    // connection, see removeConnection) hands it straight back.
    //
    // The alternative - refusing to touch an interface the administrator configured -
    // was the previous behaviour and it broke the applet's core flow: on a machine with
    // an existing config_wlan0, clicking "connect" wrote nothing to netifrc and silently
    // did nothing. Matching NetworkManager (which manages every device unless listed in
    // unmanaged-devices) is what the applet expects.
    if (!m_config->managedInterfaces().contains(ifname) && m_config->configFor(ifname).configured) {
        // Warn only when we are actually shadowing something: a silent override is
        // otherwise very hard to explain from the applet's point of view.
        qCWarning(NETIFRC) << "taking over" << ifname << "from the administrator's netifrc config in"
                           << NetifrcConfig::defaultPath()
                           << "- the netifrcd managed block now shadows it; remove" << ifname
                           << "from that block to manage the interface by hand again";
    }
    m_config->setInterface(ifname, profile.toNetifrc());
}

Profile Backend::firstProfileTargeting(const QString &ifname, const QString &ignoreUuid) const
{
    for (const Profile &p : m_profiles->profiles()) {
        if (p.uuid != ignoreUuid && targetInterfaceFor(p) == ifname) {
            return p;
        }
    }
    return {};
}

void Backend::reassignInterface(const QString &ifname, const QString &removedUuid)
{
    // The deleted profile's values are what is currently in the managed block, so it has
    // to be re-pointed at whatever still targets the interface - several profiles can
    // share one interface (two saved Wi-Fi networks, or a user profile alongside the one
    // netifrcd imported from the administrator's own config). Leaving the block alone
    // would keep the deleted profile's SSID/addresses live; dropping it outright (the
    // old behaviour) would wipe config a surviving profile still needs. Only when
    // nothing targets the interface any more do we hand it back to the administrator.
    const Profile survivor = firstProfileTargeting(ifname, removedUuid);
    if (survivor.isValid()) {
        m_config->setInterface(ifname, survivor.toNetifrc());
    } else {
        m_config->removeInterface(ifname);
    }
    m_config->save(nullptr);
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

QString Backend::activateConnection(const QString &connectionPath, const QString &devicePathIn, const QString &specificObject, QString *error)
{
    Profile profile = profileBySettingsPath(connectionPath);
    if (!profile.isValid()) {
        if (error) {
            *error = QStringLiteral("unknown connection %1").arg(connectionPath);
        }
        return {};
    }

    QString ifname = profile.interfaceName;
    if (!devicePathIn.isEmpty()) {
        const DeviceRecord d = deviceByPath(devicePathIn);
        if (!d.ifname.isEmpty()) {
            ifname = d.ifname;
        }
    }
    if (ifname.isEmpty()) {
        ifname = targetInterfaceFor(profile);
    }
    if (ifname.isEmpty()) {
        if (error) {
            *error = QStringLiteral("no suitable interface for %1").arg(profile.id);
        }
        return {};
    }
    profile.interfaceName = ifname;

    // Connecting to a specific access point: take the SSID from the scan result.
    if (profile.isWireless() && !specificObject.isEmpty()) {
        const AccessPointRecord ap = accessPointByPath(specificObject);
        if (!ap.info.bssid.isEmpty()) {
            profile.ssid = ap.info.ssid;
        }
    }

    claimInterface(ifname, profile);
    QString saveError;
    if (!m_config->save(&saveError)) {
        if (error) {
            *error = saveError;
        }
        return {};
    }

    if (profile.isWireless() && !profile.ssid.isEmpty()) {
        QString wifiError;
        if (!WifiScanner::writeNetwork(ifname, profile.ssid, profile.psk, profile.hidden, &wifiError)) {
            if (error) {
                *error = wifiError;
            }
            return {};
        }
    }

    m_profiles->addOrUpdate(profile);
    m_profiles->save(nullptr);

    // Publish an activating active-connection immediately so the applet shows progress.
    if (!m_activeIds.contains(ifname)) {
        m_activeIds.insert(ifname, m_nextActiveId++);
    }
    ActiveConnectionRecord ar;
    ar.id = m_activeIds.value(ifname);
    ar.path = activeConnectionPath(ar.id);
    ar.profileUuid = profile.uuid;
    ar.settingsPath = settingsPathFor(profile.uuid);
    ar.deviceIfname = ifname;
    const DeviceRecord dev = device(ifname);
    ar.devicePath = devicePath(dev.id);
    ar.state = NM_ACTIVE_CONNECTION_STATE_ACTIVATING;

    bool existed = false;
    for (ActiveConnectionRecord &existing : m_activeConnections) {
        if (existing.path == ar.path) {
            existing = ar;
            existed = true;
            break;
        }
    }
    if (!existed) {
        m_activeConnections.append(ar);
        if (m_dbus) {
            m_dbus->syncActiveConnections(m_activeConnections);
        }
        Q_EMIT activeConnectionAdded(ar.path);
    }

    m_services->perform(ServiceControl::Action::Start, ifname);
    return ar.path;
}

QString Backend::addAndActivateConnection(const SettingsMap &map, const QString &devicePathIn, const QString &specificObject, QString *error)
{
    Profile profile = Profile::fromSettingsMap(map);
    if (!profile.isValid()) {
        if (error) {
            *error = QStringLiteral("invalid connection settings");
        }
        return {};
    }

    if (!devicePathIn.isEmpty()) {
        const DeviceRecord d = deviceByPath(devicePathIn);
        if (!d.ifname.isEmpty()) {
            profile.interfaceName = d.ifname;
        }
    }
    if (profile.isWireless() && !specificObject.isEmpty()) {
        const AccessPointRecord ap = accessPointByPath(specificObject);
        if (!ap.info.bssid.isEmpty()) {
            profile.ssid = ap.info.ssid;
        }
    }

    m_profiles->addOrUpdate(profile);
    m_profiles->save(nullptr);
    Q_EMIT settingsChanged();

    return activateConnection(settingsPathFor(profile.uuid), devicePathIn, specificObject, error);
}

bool Backend::deactivateConnection(const QString &activeConnectionPathIn, QString *error)
{
    const ActiveConnectionRecord ar = activeConnectionByPath(activeConnectionPathIn);
    if (ar.deviceIfname.isEmpty()) {
        if (error) {
            *error = QStringLiteral("unknown active connection %1").arg(activeConnectionPathIn);
        }
        return false;
    }
    m_services->perform(ServiceControl::Action::Stop, ar.deviceIfname);
    return true;
}

QString Backend::addConnection(const SettingsMap &map, QString *error)
{
    Profile profile = Profile::fromSettingsMap(map);
    if (!profile.isValid()) {
        if (error) {
            *error = QStringLiteral("invalid connection settings");
        }
        return {};
    }
    m_profiles->addOrUpdate(profile);
    if (!m_profiles->save(error)) {
        return {};
    }
    Q_EMIT settingsChanged();
    return settingsPathFor(profile.uuid);
}

bool Backend::updateConnection(const QString &settingsPath, const SettingsMap &map, QString *error)
{
    Profile existing = profileBySettingsPath(settingsPath);
    if (!existing.isValid()) {
        if (error) {
            *error = QStringLiteral("unknown connection %1").arg(settingsPath);
        }
        return false;
    }
    Profile updated = Profile::fromSettingsMap(map);
    updated.uuid = existing.uuid; // the uuid is the identity, never rewritten by the editor
    m_profiles->addOrUpdate(updated);
    if (!m_profiles->save(error)) {
        return false;
    }

    // If the profile is bound to a live interface, push the change into netifrc too.
    // Editing a connection is as explicit as activating one, so it claims the interface
    // the same way.
    const QString ifname = targetInterfaceFor(updated);
    if (!ifname.isEmpty()) {
        claimInterface(ifname, updated);
        m_config->save(nullptr);
    }

    Q_EMIT settingsConnectionChanged(settingsPath);
    return true;
}

bool Backend::removeConnection(const QString &settingsPath, QString *error)
{
    const Profile profile = profileBySettingsPath(settingsPath);
    if (!profile.isValid()) {
        if (error) {
            *error = QStringLiteral("unknown connection %1").arg(settingsPath);
        }
        return false;
    }

    if (!profile.interfaceName.isEmpty() && profile.isWireless()) {
        WifiScanner::removeNetwork(profile.interfaceName, profile.ssid, nullptr);
    }

    m_profiles->remove(profile.uuid);
    if (!m_profiles->save(error)) {
        return false;
    }

    if (!profile.interfaceName.isEmpty()) {
        reassignInterface(profile.interfaceName, profile.uuid);
    }

    Q_EMIT settingsChanged();
    return true;
}

SettingsMap Backend::settingsForPath(const QString &settingsPath) const
{
    const Profile p = profileBySettingsPath(settingsPath);
    return p.isValid() ? p.toSettingsMap() : SettingsMap{};
}

bool Backend::requestScan(const QString &ifnameIn, QString *error)
{
    QString ifname = ifnameIn;
    if (ifname.isEmpty()) {
        for (const DeviceRecord &d : m_devices) {
            if (d.info.deviceType == NM_DEVICE_TYPE_WIFI) {
                ifname = d.ifname;
                break;
            }
        }
    }
    if (ifname.isEmpty()) {
        if (error) {
            *error = QStringLiteral("no wireless interface available");
        }
        return false;
    }
    // refreshAccessPoints() reconciles and announces the new list itself.
    refreshAccessPoints(ifname);
    return true;
}

bool Backend::setNetworkingEnabled(bool enabled, QString *error)
{
    Q_UNUSED(error)
    if (m_networkingEnabled == enabled) {
        return true;
    }
    m_networkingEnabled = enabled;
    if (!enabled) {
        for (const DeviceRecord &d : m_devices) {
            if (d.state == NM_DEVICE_STATE_ACTIVATED) {
                m_services->perform(ServiceControl::Action::Stop, d.ifname);
            }
        }
    }
    Q_EMIT managerChanged(QStringList{QStringLiteral("NetworkingEnabled"), QStringLiteral("State")});
    return true;
}

bool Backend::setWirelessEnabled(bool enabled, QString *error)
{
    Q_UNUSED(error)
    if (m_wirelessEnabled == enabled) {
        return true;
    }
    m_wirelessEnabled = enabled;
    if (!enabled) {
        for (const DeviceRecord &d : m_devices) {
            if (d.info.deviceType == NM_DEVICE_TYPE_WIFI && d.state == NM_DEVICE_STATE_ACTIVATED) {
                m_services->perform(ServiceControl::Action::Stop, d.ifname);
            }
        }
    }
    Q_EMIT managerChanged(QStringList{QStringLiteral("WirelessEnabled")});
    return true;
}

} // namespace netifrcd
