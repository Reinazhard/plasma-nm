// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/ConfigObjects.h"

#include "common/Logging.h"
#include "model/Backend.h"

#include <QDBusMetaType>
#include <QHostAddress>

namespace netifrcd
{

namespace
{
/// Qt does not know the D-Bus signature of a nested container until it is
/// registered, and it silently omits an unregistered Q_PROPERTY from
/// org.freedesktop.DBus.Properties instead of failing. Registering these two once
/// is what makes AddressData ("aa{sv}") and Addresses ("aau") appear at all.
void registerDbusContainerTypes()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<QList<QVariantMap>>();
        qDBusRegisterMetaType<QList<QList<uint>>>();
        return true;
    }();
    Q_UNUSED(registered);
}

/// "10.0.0.5/24" -> {"10.0.0.5", 24}; a bare address is treated as a /32 host route.
QPair<QString, uint> splitCidr(const QString &cidr)
{
    const int slash = cidr.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return {cidr, 32u};
    }
    return {cidr.left(slash), cidr.mid(slash + 1).toUInt()};
}

/// NetworkManager stores an IPv4 address as the host-order 32-bit value, i.e.
/// 10.0.0.5 becomes 0x0a000005.
uint ipv4ToUint(const QString &addr)
{
    return QHostAddress(addr).toIPv4Address();
}
} // namespace

// ---------------------------------------------------------------------------
// Ip4ConfigObject
// ---------------------------------------------------------------------------

Ip4ConfigObject::Ip4ConfigObject(Backend *backend, const QString &path, const QString &ifname, QObject *parent)
    : ObjectBase(path, parent)
    , m_backend(backend)
    , m_ifname(ifname)
{
    registerDbusContainerTypes();

    connect(m_backend, &Backend::deviceChanged, this, &Ip4ConfigObject::onDeviceChanged);

    // Prime the cache without notifying: the object is registered by
    // Backend::registerObjects() after we are constructed, so there is nobody to
    // notify yet.
    refresh();
}

QList<QVariantMap> Ip4ConfigObject::addressData() const
{
    return m_addressData;
}

QList<QList<uint>> Ip4ConfigObject::addresses() const
{
    return m_addresses;
}

QString Ip4ConfigObject::gateway() const
{
    return m_gateway;
}

QList<QList<uint>> Ip4ConfigObject::routes() const
{
    // Gap: InterfaceWatcher only surfaces the default gateway, not the full routing
    // table, so there are no non-default routes to report here.
    return {};
}

QList<QVariantMap> Ip4ConfigObject::routeData() const
{
    return {};
}

QList<QVariantMap> Ip4ConfigObject::nameserverData() const
{
    return m_nameserverData;
}

QList<uint> Ip4ConfigObject::nameservers() const
{
    // Legacy "au" form is left empty; NameserverData is the property clients read.
    return {};
}

QStringList Ip4ConfigObject::domains() const
{
    return {};
}

QStringList Ip4ConfigObject::searches() const
{
    return {};
}

QStringList Ip4ConfigObject::dnsOptions() const
{
    return {};
}

int Ip4ConfigObject::dnsPriority() const
{
    return 0;
}

QStringList Ip4ConfigObject::winsServerData() const
{
    return {};
}

QVariantMap Ip4ConfigObject::refresh()
{
    const InterfaceInfo info = m_backend->device(m_ifname).info;

    QList<QVariantMap> addressData;
    QList<QList<uint>> addresses;
    addressData.reserve(info.addresses.size());
    addresses.reserve(info.addresses.size());
    for (const QString &cidr : info.addresses) {
        const auto [addr, prefix] = splitCidr(cidr);

        QVariantMap entry;
        entry.insert(QStringLiteral("address"), addr);
        entry.insert(QStringLiteral("prefix"), prefix);
        addressData.append(entry);

        // Legacy aau entry: [address, prefix, gateway]. NetworkManager reports the
        // per-address gateway as 0 and keeps the default gateway in Gateway.
        addresses.append(QList<uint>{ipv4ToUint(addr), prefix, 0u});
    }

    QList<QVariantMap> nameserverData;
    nameserverData.reserve(info.dns.size());
    for (const QString &dns : info.dns) {
        QVariantMap entry;
        entry.insert(QStringLiteral("address"), dns);
        nameserverData.append(entry);
    }

    QVariantMap changed;
    if (addressData != m_addressData) {
        m_addressData = addressData;
        changed.insert(QStringLiteral("AddressData"), QVariant::fromValue(m_addressData));
    }
    if (addresses != m_addresses) {
        m_addresses = addresses;
        changed.insert(QStringLiteral("Addresses"), QVariant::fromValue(m_addresses));
    }
    if (info.gateway != m_gateway) {
        m_gateway = info.gateway;
        changed.insert(QStringLiteral("Gateway"), m_gateway);
    }
    if (nameserverData != m_nameserverData) {
        m_nameserverData = nameserverData;
        changed.insert(QStringLiteral("NameserverData"), QVariant::fromValue(m_nameserverData));
    }
    return changed;
}

void Ip4ConfigObject::onDeviceChanged(const QString &path, const QStringList &changedProps)
{
    Q_UNUSED(changedProps);
    if (path != m_backend->devicePathFor(m_ifname)) {
        return;
    }
    notify(QLatin1String(IfaceIp4Config), refresh());
}

// ---------------------------------------------------------------------------
// Dhcp4ConfigObject
// ---------------------------------------------------------------------------

Dhcp4ConfigObject::Dhcp4ConfigObject(Backend *backend, const QString &path, const QString &ifname, QObject *parent)
    : ObjectBase(path, parent)
    , m_backend(backend)
    , m_ifname(ifname)
{
    // Honesty note: netifrc shells out to a DHCP client (dhcpcd/udhcpc/...) and then
    // forgets about it, so unlike NetworkManager there is no lease we can introspect
    // for the DHCP option map. We expose the one thing we do know - which netifrc
    // interface this config belongs to - and nothing else. Options is therefore
    // constant, which is why no Backend signal is connected here.
}

QVariantMap Dhcp4ConfigObject::options() const
{
    return {{QStringLiteral("netifrc.interface"), m_ifname}};
}

} // namespace netifrcd
