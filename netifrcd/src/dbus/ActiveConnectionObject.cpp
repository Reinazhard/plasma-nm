// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/ActiveConnectionObject.h"

#include "common/Logging.h"
#include "model/Backend.h"
#include "model/Profile.h"

#include <QVariant>

namespace netifrcd
{

namespace
{

using ActiveConnectionRecord = Backend::ActiveConnectionRecord;

bool isActivated(uint state)
{
    return state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
}

} // namespace

ActiveConnectionObject::ActiveConnectionObject(Backend *backend, const QString &activePath, QObject *parent)
    : ObjectBase(activePath, parent)
    , m_backend(backend)
{
    Q_ASSERT(m_backend);

    m_lastState = static_cast<uint>(m_backend->activeConnectionByPath(m_path).state);

    connect(m_backend, &Backend::activeConnectionChanged, this, [this](const QString &path, const QStringList &props) {
        if (path != m_path) {
            return;
        }
        if (props.contains(QLatin1String("State"))) {
            const uint newState = state();
            Q_EMIT StateChanged(newState, NM_ACTIVE_CONNECTION_STATE_REASON_NONE);
            m_lastState = newState;
        }
        notify(QLatin1String(IfaceActiveConnection), changedMap(props));
    });
}

QDBusObjectPath ActiveConnectionObject::connection() const
{
    return toPath(m_backend->activeConnectionByPath(m_path).settingsPath);
}

QDBusObjectPath ActiveConnectionObject::specificObject() const
{
    // ActiveConnectionRecord does not carry a specific object (e.g. the AP path).
    return nonePath();
}

QString ActiveConnectionObject::id() const
{
    const ActiveConnectionRecord record = m_backend->activeConnectionByPath(m_path);
    if (record.profileUuid.isEmpty()) {
        return QString();
    }
    return m_backend->profileByUuid(record.profileUuid).id;
}

QString ActiveConnectionObject::uuid() const
{
    return m_backend->activeConnectionByPath(m_path).profileUuid;
}

QString ActiveConnectionObject::type() const
{
    const ActiveConnectionRecord record = m_backend->activeConnectionByPath(m_path);
    if (record.profileUuid.isEmpty()) {
        return QString();
    }
    return m_backend->profileByUuid(record.profileUuid).type;
}

QList<QDBusObjectPath> ActiveConnectionObject::devices() const
{
    const QString devicePath = m_backend->activeConnectionByPath(m_path).devicePath;
    if (devicePath.isEmpty()) {
        return {};
    }
    return {toPath(devicePath)};
}

uint ActiveConnectionObject::state() const
{
    return static_cast<uint>(m_backend->activeConnectionByPath(m_path).state);
}

uint ActiveConnectionObject::stateFlags() const
{
    if (!isActivated(state())) {
        return NM_ACTIVATION_STATE_FLAG_NONE;
    }
    return NM_ACTIVATION_STATE_FLAG_LAYER2_READY | NM_ACTIVATION_STATE_FLAG_IP4_READY | NM_ACTIVATION_STATE_FLAG_IP6_READY;
}

bool ActiveConnectionObject::isDefault() const
{
    const QString primary = m_backend->primaryConnectionPath();
    return !primary.isEmpty() && primary == m_path;
}

bool ActiveConnectionObject::default6() const
{
    return false;
}

QDBusObjectPath ActiveConnectionObject::ip4Config() const
{
    const ActiveConnectionRecord record = m_backend->activeConnectionByPath(m_path);
    if (!isActivated(static_cast<uint>(record.state))) {
        return nonePath();
    }
    return QDBusObjectPath(ip4ConfigPath(record.id));
}

QDBusObjectPath ActiveConnectionObject::dhcp4Config() const
{
    const ActiveConnectionRecord record = m_backend->activeConnectionByPath(m_path);
    if (!isActivated(static_cast<uint>(record.state))) {
        return nonePath();
    }
    return QDBusObjectPath(dhcp4ConfigPath(record.id));
}

QDBusObjectPath ActiveConnectionObject::ip6Config() const
{
    return nonePath();
}

QDBusObjectPath ActiveConnectionObject::dhcp6Config() const
{
    return nonePath();
}

bool ActiveConnectionObject::vpn() const
{
    return false;
}

QDBusObjectPath ActiveConnectionObject::controller() const
{
    return nonePath();
}

QList<QDBusObjectPath> ActiveConnectionObject::ports() const
{
    return {};
}

QVariantMap ActiveConnectionObject::changedMap(const QStringList &props) const
{
    QVariantMap changed;
    for (const QString &prop : props) {
        if (prop == QLatin1String("Connection")) {
            changed.insert(prop, QVariant::fromValue(connection()));
        } else if (prop == QLatin1String("SpecificObject")) {
            changed.insert(prop, QVariant::fromValue(specificObject()));
        } else if (prop == QLatin1String("Id")) {
            changed.insert(prop, id());
        } else if (prop == QLatin1String("Uuid")) {
            changed.insert(prop, uuid());
        } else if (prop == QLatin1String("Type")) {
            changed.insert(prop, type());
        } else if (prop == QLatin1String("Devices")) {
            changed.insert(prop, QVariant::fromValue(devices()));
        } else if (prop == QLatin1String("State")) {
            changed.insert(prop, state());
        } else if (prop == QLatin1String("StateFlags")) {
            changed.insert(prop, stateFlags());
        } else if (prop == QLatin1String("Default")) {
            changed.insert(prop, isDefault());
        } else if (prop == QLatin1String("Default6")) {
            changed.insert(prop, default6());
        } else if (prop == QLatin1String("Ip4Config")) {
            changed.insert(prop, QVariant::fromValue(ip4Config()));
        } else if (prop == QLatin1String("Dhcp4Config")) {
            changed.insert(prop, QVariant::fromValue(dhcp4Config()));
        } else if (prop == QLatin1String("Vpn")) {
            changed.insert(prop, vpn());
        }
    }
    return changed;
}

} // namespace netifrcd
