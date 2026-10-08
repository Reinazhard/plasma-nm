// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "model/Profile.h"

#include "common/Logging.h"

#include <QHostAddress>
#include <QUuid>

namespace netifrcd
{

namespace
{
QString methodToString(Profile::IpMethod m)
{
    switch (m) {
    case Profile::IpMethod::Manual:
        return QStringLiteral("manual");
    case Profile::IpMethod::Disabled:
        return QStringLiteral("disabled");
    case Profile::IpMethod::Auto:
        break;
    }
    return QStringLiteral("auto");
}

Profile::IpMethod methodFromString(const QString &s)
{
    if (s == QLatin1String("manual")) {
        return Profile::IpMethod::Manual;
    }
    if (s == QLatin1String("disabled")) {
        return Profile::IpMethod::Disabled;
    }
    return Profile::IpMethod::Auto;
}

/// NetworkManager stores IPv4 addresses in "dns" as the address value in host byte
/// order, i.e. 1.2.3.4 becomes 0x01020304.
quint32 ipv4ToUint(const QString &addr)
{
    QHostAddress a(addr);
    return a.toIPv4Address();
}

QString uintToIpv4(quint32 v)
{
    return QHostAddress(v).toString();
}

QPair<QString, uint> splitCidr(const QString &cidr)
{
    const int slash = cidr.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return {cidr, 32};
    }
    return {cidr.left(slash), cidr.mid(slash + 1).toUInt()};
}
} // namespace

QString Profile::newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SettingsMap Profile::toSettingsMap() const
{
    SettingsMap map;

    QVariantMap connection;
    connection.insert(QStringLiteral("id"), id);
    connection.insert(QStringLiteral("uuid"), uuid);
    connection.insert(QStringLiteral("type"), type);
    connection.insert(QStringLiteral("autoconnect"), autoconnect);
    connection.insert(QStringLiteral("timestamp"), static_cast<uint>(timestamp));
    if (!interfaceName.isEmpty()) {
        connection.insert(QStringLiteral("interface-name"), interfaceName);
    }
    map.insert(QStringLiteral("connection"), connection);

    QVariantMap ipv4;
    ipv4.insert(QStringLiteral("method"), methodToString(ipv4Method));
    if (ipv4Method == IpMethod::Manual) {
        QList<QVariantMap> addressData;
        for (const QString &cidr : addresses) {
            const auto [addr, prefix] = splitCidr(cidr);
            QVariantMap entry;
            entry.insert(QStringLiteral("address"), addr);
            entry.insert(QStringLiteral("prefix"), prefix);
            addressData.append(entry);
        }
        ipv4.insert(QStringLiteral("address-data"), QVariant::fromValue(addressData));
        if (!gateway.isEmpty()) {
            ipv4.insert(QStringLiteral("gateway"), gateway);
        }
    }
    if (!dns.isEmpty()) {
        QList<uint> servers;
        for (const QString &d : dns) {
            servers.append(ipv4ToUint(d));
        }
        ipv4.insert(QStringLiteral("dns"), QVariant::fromValue(servers));
    }
    map.insert(QStringLiteral("ipv4"), ipv4);

    if (isWireless()) {
        QVariantMap wifi;
        wifi.insert(QStringLiteral("ssid"), QByteArray(ssid.toUtf8()));
        wifi.insert(QStringLiteral("mode"), QStringLiteral("infrastructure"));
        wifi.insert(QStringLiteral("hidden"), hidden);
        map.insert(QStringLiteral("802-11-wireless"), wifi);

        if (!psk.isEmpty()) {
            QVariantMap sec;
            sec.insert(QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk"));
            sec.insert(QStringLiteral("psk"), psk);
            map.insert(QStringLiteral("802-11-wireless-security"), sec);
        }
    }

    return map;
}

Profile Profile::fromSettingsMap(const SettingsMap &map)
{
    Profile p;

    const QVariantMap connection = map.value(QStringLiteral("connection"));
    p.id = connection.value(QStringLiteral("id")).toString();
    p.uuid = connection.value(QStringLiteral("uuid")).toString();
    p.type = connection.value(QStringLiteral("type")).toString();
    p.autoconnect = connection.value(QStringLiteral("autoconnect"), true).toBool();
    p.timestamp = connection.value(QStringLiteral("timestamp")).toUInt();
    p.interfaceName = connection.value(QStringLiteral("interface-name")).toString();
    if (p.uuid.isEmpty()) {
        p.uuid = newUuid();
    }

    const QVariantMap ipv4 = map.value(QStringLiteral("ipv4"));
    p.ipv4Method = methodFromString(ipv4.value(QStringLiteral("method"), QStringLiteral("auto")).toString());
    p.gateway = ipv4.value(QStringLiteral("gateway")).toString();

    const QVariant addressDataVariant = ipv4.value(QStringLiteral("address-data"));
    if (addressDataVariant.canConvert<QList<QVariantMap>>()) {
        const auto addressData = addressDataVariant.value<QList<QVariantMap>>();
        for (const QVariantMap &entry : addressData) {
            const QString addr = entry.value(QStringLiteral("address")).toString();
            const uint prefix = entry.value(QStringLiteral("prefix"), 32u).toUInt();
            if (!addr.isEmpty()) {
                p.addresses.append(QStringLiteral("%1/%2").arg(addr).arg(prefix));
            }
        }
    } else {
        // Legacy "addresses" (aau): [addr, prefix, gateway] per entry.
        const QVariant legacy = ipv4.value(QStringLiteral("addresses"));
        if (legacy.canConvert<QList<QVariantList>>()) {
            const auto list = legacy.value<QList<QVariantList>>();
            for (const QVariantList &entry : list) {
                if (entry.size() >= 2) {
                    p.addresses.append(QStringLiteral("%1/%2").arg(uintToIpv4(entry.at(0).toUInt())).arg(entry.at(1).toUInt()));
                }
            }
        }
    }

    const QVariant dnsVariant = ipv4.value(QStringLiteral("dns"));
    if (dnsVariant.canConvert<QList<uint>>()) {
        const auto list = dnsVariant.value<QList<uint>>();
        for (uint v : list) {
            p.dns.append(uintToIpv4(v));
        }
    }

    const QVariantMap wifi = map.value(QStringLiteral("802-11-wireless"));
    if (!wifi.isEmpty()) {
        if (p.type.isEmpty()) {
            p.type = QLatin1String(TypeWireless);
        }
        p.ssid = QString::fromUtf8(wifi.value(QStringLiteral("ssid")).toByteArray());
        p.hidden = wifi.value(QStringLiteral("hidden"), false).toBool();
    }
    const QVariantMap sec = map.value(QStringLiteral("802-11-wireless-security"));
    if (!sec.isEmpty()) {
        p.psk = sec.value(QStringLiteral("psk")).toString();
    }

    if (p.type.isEmpty()) {
        p.type = QLatin1String(TypeWired);
    }
    if (p.id.isEmpty()) {
        p.id = p.isWireless() && !p.ssid.isEmpty() ? p.ssid : p.interfaceName;
    }
    return p;
}

Profile Profile::fromNetifrc(const QString &ifname, const NetifrcConfig::InterfaceConfig &cfg)
{
    Profile p;
    p.uuid = newUuid();
    p.id = ifname;
    p.type = typeForInterface(ifname);
    p.interfaceName = ifname;
    p.autoconnect = true;

    switch (cfg.method) {
    case NetifrcConfig::Method::Dhcp:
        p.ipv4Method = IpMethod::Auto;
        break;
    case NetifrcConfig::Method::Static:
        p.ipv4Method = IpMethod::Manual;
        p.addresses = cfg.addresses;
        break;
    case NetifrcConfig::Method::Loopback:
    case NetifrcConfig::Method::None:
    case NetifrcConfig::Method::Unknown:
        p.ipv4Method = IpMethod::Auto;
        break;
    }

    // netifrc expresses the default route as a route line rather than a gateway.
    for (const QString &route : cfg.routes) {
        if (route.startsWith(QLatin1String("default via "))) {
            p.gateway = route.mid(12).section(QLatin1Char(' '), 0, 0);
            break;
        }
    }

    if (p.isWireless()) {
        p.ssid = cfg.essid;
        p.psk = cfg.psk;
    }
    return p;
}

NetifrcConfig::InterfaceConfig Profile::toNetifrc() const
{
    NetifrcConfig::InterfaceConfig cfg;
    cfg.configured = true;

    switch (ipv4Method) {
    case IpMethod::Manual:
        cfg.method = NetifrcConfig::Method::Static;
        cfg.addresses = addresses;
        break;
    case IpMethod::Disabled:
        cfg.method = NetifrcConfig::Method::Unknown;
        break;
    case IpMethod::Auto:
        cfg.method = NetifrcConfig::Method::Dhcp;
        break;
    }

    if (!gateway.isEmpty()) {
        cfg.routes.append(QStringLiteral("default via %1").arg(gateway));
    }

    if (isWireless()) {
        cfg.essid = ssid;
        cfg.psk = psk;
        // netifrc only associates when a module providing "wireless" is loaded. An
        // existing modules_<iface>="!wireless" - common when another manager (or a
        // previous hand-written config) owned the radio - suppresses it entirely.
        // Naming wpa_supplicant here lifts that exclusion and, when iwd is also
        // installed, makes wpa_supplicant the preferred provider. That is the module
        // the wpa_supplicant-<iface>.conf we generate is written for.
        cfg.modules = {QStringLiteral("wpa_supplicant")};
    }
    return cfg;
}

} // namespace netifrcd
