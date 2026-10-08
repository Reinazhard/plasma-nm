// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/AccessPointObject.h"

#include "common/Logging.h"
#include "model/Backend.h"

#include <QtGlobal>

namespace netifrcd
{

namespace
{

using AccessPointRecord = Backend::AccessPointRecord;

} // namespace

AccessPointObject::AccessPointObject(Backend *backend, const QString &ifname, const QString &bssid, QObject *parent)
    : ObjectBase(backend->accessPointPathFor(ifname, bssid), parent)
    , m_backend(backend)
    , m_ifname(ifname)
{
    Q_ASSERT(m_backend);

    m_info = m_backend->accessPointByPath(m_path).info;

    connect(m_backend, &Backend::accessPointListChanged, this, [this](const QString &ifname) {
        if (ifname != m_ifname) {
            return;
        }
        const AccessPointRecord record = m_backend->accessPointByPath(m_path);
        if (record.path.isEmpty()) {
            // The BSSID disappeared; let clients drop their cached values.
            invalidate(QLatin1String(IfaceAccessPoint));
            return;
        }
        const QVariantMap changed = changedMap(m_info, record.info);
        m_info = record.info;
        notify(QLatin1String(IfaceAccessPoint), changed);
    });
}

AccessPointInfo AccessPointObject::currentInfo() const
{
    return m_backend->accessPointByPath(m_path).info;
}

QByteArray AccessPointObject::ssid() const
{
    return currentInfo().ssid.toUtf8();
}

QString AccessPointObject::hwAddress() const
{
    return currentInfo().bssid;
}

uint AccessPointObject::frequency() const
{
    const int frequency = currentInfo().frequency;
    return frequency > 0 ? static_cast<uint>(frequency) : 0;
}

uint AccessPointObject::maxBitrate() const
{
    const int maxBitrate = currentInfo().maxBitrate;
    return maxBitrate > 0 ? static_cast<uint>(maxBitrate) : 0;
}

uint AccessPointObject::mode() const
{
    return static_cast<uint>(currentInfo().mode);
}

uchar AccessPointObject::strength() const
{
    return static_cast<uchar>(qBound(0, currentInfo().strength, 100));
}

int AccessPointObject::lastSeen() const
{
    // The scanner does not track CLOCK_BOOTTIME timestamps; 0 means "just seen".
    return 0;
}

uint AccessPointObject::flags() const
{
    return static_cast<uint>(currentInfo().flags);
}

uint AccessPointObject::wpaFlags() const
{
    return static_cast<uint>(currentInfo().wpaFlags);
}

uint AccessPointObject::rsnFlags() const
{
    return static_cast<uint>(currentInfo().rsnFlags);
}

QVariantMap AccessPointObject::changedMap(const AccessPointInfo &previous, const AccessPointInfo &current) const
{
    QVariantMap changed;
    if (previous.ssid != current.ssid) {
        changed.insert(QStringLiteral("Ssid"), current.ssid.toUtf8());
    }
    if (previous.bssid != current.bssid) {
        changed.insert(QStringLiteral("HwAddress"), current.bssid);
    }
    if (previous.frequency != current.frequency) {
        changed.insert(QStringLiteral("Frequency"), static_cast<uint>(current.frequency > 0 ? current.frequency : 0));
    }
    if (previous.maxBitrate != current.maxBitrate) {
        changed.insert(QStringLiteral("MaxBitrate"), static_cast<uint>(current.maxBitrate > 0 ? current.maxBitrate : 0));
    }
    if (previous.mode != current.mode) {
        changed.insert(QStringLiteral("Mode"), static_cast<uint>(current.mode));
    }
    if (previous.strength != current.strength) {
        changed.insert(QStringLiteral("Strength"), static_cast<uchar>(qBound(0, current.strength, 100)));
    }
    if (previous.flags != current.flags) {
        changed.insert(QStringLiteral("Flags"), static_cast<uint>(current.flags));
    }
    if (previous.wpaFlags != current.wpaFlags) {
        changed.insert(QStringLiteral("WpaFlags"), static_cast<uint>(current.wpaFlags));
    }
    if (previous.rsnFlags != current.rsnFlags) {
        changed.insert(QStringLiteral("RsnFlags"), static_cast<uint>(current.rsnFlags));
    }
    return changed;
}

} // namespace netifrcd
