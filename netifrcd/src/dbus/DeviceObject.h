// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.Device and its type-specific interfaces.
//
// The generic Device interface lives on DeviceObject itself. The per-type
// interfaces (Wired / Wireless / Statistics) are QDBusAbstractAdaptor children, which
// is the only way Qt lets one object expose several D-Bus interfaces. registerObject()
// picks the adaptors up automatically when Backend registers the DeviceObject.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// At global scope: a forward declaration inside namespace netifrcd below would name a
// different type and leave the real QTimer incomplete.
class QTimer;

namespace netifrcd
{

class Backend;

// ---------------------------------------------------------------------------
// Device.StateReason
//
// NetworkManager models this property as the D-Bus struct "(uu)" - the current state
// paired with the reason for the last transition. Qt can only export a struct through
// a type registered with qDBusRegisterMetaType(), so this is the C++ mirror of it; the
// marshalling operators are defined in DeviceObject.cpp and registered in the
// DeviceObject constructor.
// ---------------------------------------------------------------------------

struct DeviceStateReason {
    uint state = NM_DEVICE_STATE_UNKNOWN;
    uint reason = NM_DEVICE_STATE_REASON_NONE;
};

QDBusArgument &operator<<(QDBusArgument &argument, const DeviceStateReason &value);
const QDBusArgument &operator>>(const QDBusArgument &argument, DeviceStateReason &value);

// ---------------------------------------------------------------------------
// org.freedesktop.NetworkManager.Device
// ---------------------------------------------------------------------------

class DeviceObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device")

    Q_PROPERTY(QString Udi READ udi CONSTANT)
    Q_PROPERTY(QString Path READ path CONSTANT)
    Q_PROPERTY(QString Interface READ interface CONSTANT)
    Q_PROPERTY(QString IpInterface READ ipInterface)
    Q_PROPERTY(QString Driver READ driver)
    Q_PROPERTY(QString DriverVersion READ driverVersion)
    Q_PROPERTY(QString FirmwareVersion READ firmwareVersion)
    Q_PROPERTY(uint Capabilities READ capabilities)
    Q_PROPERTY(uint Ip4Address READ ip4Address)
    Q_PROPERTY(uint State READ state)
    Q_PROPERTY(QDBusObjectPath ActiveConnection READ activeConnection)
    Q_PROPERTY(QList<QDBusObjectPath> AvailableConnections READ availableConnections)
    Q_PROPERTY(bool Managed READ managed)
    Q_PROPERTY(bool Autoconnect READ autoconnect)
    Q_PROPERTY(bool FirmwareMissing READ firmwareMissing)
    Q_PROPERTY(bool NmPluginMissing READ nmPluginMissing)
    Q_PROPERTY(uint DeviceType READ deviceType CONSTANT)
    Q_PROPERTY(uint Metered READ metered)
    Q_PROPERTY(QList<QVariantMap> LldpNeighbors READ lldpNeighbors)
    Q_PROPERTY(bool Real READ real)
    Q_PROPERTY(uint Ip4Connectivity READ ip4Connectivity)
    Q_PROPERTY(uint Ip6Connectivity READ ip6Connectivity)
    Q_PROPERTY(uint InterfaceFlags READ interfaceFlags)
    Q_PROPERTY(QString HwAddress READ hwAddress)
    Q_PROPERTY(uint Mtu READ mtu)
    Q_PROPERTY(DeviceStateReason StateReason READ stateReason)

public:
    explicit DeviceObject(Backend *backend, const QString &ifname, QObject *parent = nullptr);

    QString udi() const;
    QString interface() const;
    QString ipInterface() const;
    QString driver() const;
    QString driverVersion() const;
    QString firmwareVersion() const;
    uint capabilities() const;
    uint ip4Address() const;
    uint state() const;
    QDBusObjectPath activeConnection() const;
    QList<QDBusObjectPath> availableConnections() const;
    bool managed() const;
    bool autoconnect() const;
    bool firmwareMissing() const;
    bool nmPluginMissing() const;
    uint deviceType() const;
    uint metered() const;
    QList<QVariantMap> lldpNeighbors() const;
    bool real() const;
    uint ip4Connectivity() const;
    uint ip6Connectivity() const;
    uint interfaceFlags() const;
    QString hwAddress() const;
    uint mtu() const;
    DeviceStateReason stateReason() const;

    QString ifname() const
    {
        return m_ifname;
    }

public Q_SLOTS:
    void Reapply(const SettingsMap &connection, uint versionId, uint flags);
    QVariantMap GetAppliedConnection(uint flags) const;
    void Disconnect();
    void Delete();

Q_SIGNALS:
    void StateChanged(uint newState, uint oldState, uint reason);

private:
    QVariantMap changedMap(const QStringList &props) const;

    Backend *m_backend = nullptr;
    QString m_ifname;
    uint m_lastState = NM_DEVICE_STATE_UNKNOWN;
};

// ---------------------------------------------------------------------------
// org.freedesktop.NetworkManager.Device.Wired
// ---------------------------------------------------------------------------

class DeviceWiredAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device.Wired")

    Q_PROPERTY(QString HwAddress READ hwAddress)
    Q_PROPERTY(QString PermHwAddress READ permHwAddress)
    Q_PROPERTY(uint Speed READ speed)
    Q_PROPERTY(bool Carrier READ carrier)
    Q_PROPERTY(QStringList S390Subchannels READ s390Subchannels)

public:
    DeviceWiredAdaptor(Backend *backend, const QString &ifname, QObject *parent);

    QString hwAddress() const;
    QString permHwAddress() const;
    uint speed() const;
    bool carrier() const;
    QStringList s390Subchannels() const;

private:
    Backend *m_backend = nullptr;
    QString m_ifname;
    QString m_path;
};

// ---------------------------------------------------------------------------
// org.freedesktop.NetworkManager.Device.Wireless
// ---------------------------------------------------------------------------

class DeviceWirelessAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device.Wireless")

    Q_PROPERTY(QString HwAddress READ hwAddress)
    Q_PROPERTY(QString PermHwAddress READ permHwAddress)
    Q_PROPERTY(uint Bitrate READ bitrate)
    Q_PROPERTY(uint Mode READ mode)
    Q_PROPERTY(QList<QDBusObjectPath> AccessPoints READ accessPoints)
    Q_PROPERTY(QDBusObjectPath ActiveAccessPoint READ activeAccessPoint)
    Q_PROPERTY(uint WirelessCapabilities READ wirelessCapabilities)
    Q_PROPERTY(qint64 LastScan READ lastScan)

public:
    DeviceWirelessAdaptor(Backend *backend, const QString &ifname, QObject *parent);

    QString hwAddress() const;
    QString permHwAddress() const;
    uint bitrate() const;
    uint mode() const;
    QList<QDBusObjectPath> accessPoints() const;
    QDBusObjectPath activeAccessPoint() const;
    uint wirelessCapabilities() const;
    qint64 lastScan() const;

public Q_SLOTS:
    QList<QDBusObjectPath> GetAccessPoints() const;
    QList<QDBusObjectPath> GetAllAccessPoints() const;
    void RequestScan(const QVariantMap &options);

Q_SIGNALS:
    void AccessPointAdded(const QDBusObjectPath &accessPoint);
    void AccessPointRemoved(const QDBusObjectPath &accessPoint);

private:
    QVariantMap changedMap(const QStringList &props) const;

    Backend *m_backend = nullptr;
    QString m_ifname;
    QString m_path;
    QStringList m_knownAccessPoints;
};

// ---------------------------------------------------------------------------
// org.freedesktop.NetworkManager.Device.Statistics
// ---------------------------------------------------------------------------
//
// NetworkManager implements this on the device object itself - there is no separate
// "Statistics" object-path property to follow - so it is an adaptor on DeviceObject
// rather than an object of its own.
//
// RefreshRateMs is the polling period the client asks for; NetworkManager only
// guarantees refreshes at that rate "in case the underlying counter has changed too",
// which is why poll() stays silent while the counters are unchanged. A rate of zero
// means no guaranteed refresh at all, so the timer is simply not run.

class DeviceStatisticsAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Device.Statistics")

    Q_PROPERTY(uint RefreshRateMs READ refreshRateMs WRITE setRefreshRateMs)
    Q_PROPERTY(qulonglong RxBytes READ rxBytes)
    Q_PROPERTY(qulonglong TxBytes READ txBytes)

public:
    DeviceStatisticsAdaptor(Backend *backend, const QString &ifname, QObject *parent);

    uint refreshRateMs() const;
    void setRefreshRateMs(uint refreshRate);
    qulonglong rxBytes() const;
    qulonglong txBytes() const;

private:
    void poll();

    QString m_ifname;
    QString m_path;
    QTimer *m_timer = nullptr;
    uint m_refreshRateMs = 0;
    qulonglong m_lastRx = 0; // last value announced, so poll() only speaks on a change
    qulonglong m_lastTx = 0;
};

} // namespace netifrcd

// Q_DECLARE_METATYPE has to sit at global scope (it specializes QMetaTypeId, which
// lives there), hence it is outside the namespace above.
Q_DECLARE_METATYPE(netifrcd::DeviceStateReason)
