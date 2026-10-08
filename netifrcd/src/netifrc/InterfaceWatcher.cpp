// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "netifrc/InterfaceWatcher.h"

#include "common/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <nm-dbus-interface.h>

namespace netifrcd
{

namespace
{
constexpr auto kSysClassNet = "/sys/class/net";
constexpr int kIpTimeoutMs = 5000;

QByteArray runIp(const QStringList &args)
{
    QProcess proc;
    proc.setProgram(QStandardPaths::findExecutable(QStringLiteral("ip")));
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(kIpTimeoutMs)) {
        qCWarning(NETIFRC) << "ip" << args << "timed out";
        return {};
    }
    if (proc.exitCode() != 0) {
        qCDebug(NETIFRC) << "ip" << args << "exited" << proc.exitCode() << proc.readAllStandardError();
    }
    return proc.readAllStandardOutput();
}

QByteArray runTool(const QString &program, const QStringList &args)
{
    const QString path = QStandardPaths::findExecutable(program);
    if (path.isEmpty()) {
        return {};
    }
    QProcess proc;
    proc.setProgram(path);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(kIpTimeoutMs)) {
        return {};
    }
    return proc.readAllStandardOutput();
}
} // namespace

InterfaceWatcher::InterfaceWatcher(QObject *parent)
    : QObject(parent)
{
}

bool InterfaceWatcher::isWireless(const QString &ifname)
{
    const QString base = QStringLiteral("%1/%2").arg(QLatin1String(kSysClassNet), ifname);
    return QFileInfo::exists(base + QLatin1String("/wireless")) || QFileInfo::exists(base + QLatin1String("/phy80211"));
}

bool InterfaceWatcher::isHardwareBacked(const QString &ifname)
{
    // Real NICs have a "device" symlink pointing at their parent bus device. Docker
    // bridges, veth pairs, dummy and tun interfaces do not.
    return QFileInfo::exists(QStringLiteral("%1/%2/device").arg(QLatin1String(kSysClassNet), ifname));
}

QString InterfaceWatcher::driverFor(const QString &ifname)
{
    const QFileInfo fi(QStringLiteral("%1/%2/device/driver").arg(QLatin1String(kSysClassNet), ifname));
    if (!fi.exists()) {
        return {};
    }
    return QFileInfo(fi.symLinkTarget()).fileName();
}

InterfaceInfo InterfaceWatcher::parseLink(const QJsonObject &obj)
{
    InterfaceInfo info;
    info.name = obj.value(QLatin1String("ifname")).toString();
    info.ifindex = obj.value(QLatin1String("ifindex")).toInt();
    info.mtu = obj.value(QLatin1String("mtu")).toInt();
    info.mac = obj.value(QLatin1String("address")).toString();

    const QJsonArray flags = obj.value(QLatin1String("flags")).toArray();
    for (const QJsonValue &v : flags) {
        const QString f = v.toString();
        if (f == QLatin1String("UP")) {
            info.up = true;
        } else if (f == QLatin1String("LOWER_UP") || f == QLatin1String("RUNNING")) {
            info.carrier = true;
        } else if (f == QLatin1String("LOOPBACK")) {
            info.loopback = true;
        }
    }

    const QJsonArray addrInfo = obj.value(QLatin1String("addr_info")).toArray();
    for (const QJsonValue &v : addrInfo) {
        const QJsonObject a = v.toObject();
        if (a.value(QLatin1String("family")).toString() != QLatin1String("inet")) {
            continue;
        }
        const QString local = a.value(QLatin1String("local")).toString();
        const int prefix = a.value(QLatin1String("prefixlen")).toInt();
        if (!local.isEmpty()) {
            info.addresses.append(QStringLiteral("%1/%2").arg(local).arg(prefix));
        }
    }

    if (info.loopback) {
        info.deviceType = NM_DEVICE_TYPE_LOOPBACK;
    } else if (isWireless(info.name)) {
        info.deviceType = NM_DEVICE_TYPE_WIFI;
    } else {
        info.deviceType = NM_DEVICE_TYPE_ETHERNET;
    }

    info.driver = driverFor(info.name);

    if (info.deviceType == NM_DEVICE_TYPE_WIFI && info.up) {
        const QString out = QString::fromUtf8(runTool(QStringLiteral("iw"), {QStringLiteral("dev"), info.name, QStringLiteral("link")}));
        static const QRegularExpression ssidRe(QStringLiteral(R"(^\s*SSID:\s*(.+)$)"), QRegularExpression::MultilineOption);
        const auto m = ssidRe.match(out);
        if (m.hasMatch()) {
            info.ssid = m.captured(1).trimmed();
        }
        static const QRegularExpression rateRe(QStringLiteral(R"(tx bitrate:\s*([0-9.]+)\s*MBit/s)"));
        const auto rm = rateRe.match(out);
        if (rm.hasMatch()) {
            info.bitrate = static_cast<int>(rm.captured(1).toDouble() * 1000.0);
        }
    }

    return info;
}

void InterfaceWatcher::readRoutes(QHash<QString, QString> *gateways)
{
    const QByteArray raw = runIp({QStringLiteral("-j"), QStringLiteral("route"), QStringLiteral("show"), QStringLiteral("default")});
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) {
        return;
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject r = v.toObject();
        const QString dev = r.value(QLatin1String("dev")).toString();
        const QString gw = r.value(QLatin1String("gateway")).toString();
        // Prefer the IPv4 default route; netifrc does not distinguish further.
        if (!dev.isEmpty() && !gw.isEmpty() && !gateways->contains(dev)) {
            gateways->insert(dev, gw);
        }
    }
}

void InterfaceWatcher::readDns(QHash<QString, QStringList> *dns)
{
    // netifrc (via resolvconf or a direct write) manages one global resolv.conf, so we
    // cannot attribute nameservers to a specific interface. We hand the same list to
    // every active interface and let the UI show it.
    QFile f(QStringLiteral("/etc/resolv.conf"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QStringList servers;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1String("nameserver"))) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                servers.append(parts.at(1));
            }
        }
    }
    if (!servers.isEmpty()) {
        dns->insert(QStringLiteral("*"), servers);
    }
}

void InterfaceWatcher::setExtraManaged(const QStringList &ifnames)
{
    if (m_extraManaged == ifnames) {
        return;
    }
    m_extraManaged = ifnames;
    refresh();
}

void InterfaceWatcher::refresh()
{
    const QByteArray raw = runIp({QStringLiteral("-j"), QStringLiteral("addr"), QStringLiteral("show")});
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) {
        qCWarning(NETIFRC) << "could not parse `ip -j addr show` output";
        return;
    }

    QHash<QString, QString> gateways;
    readRoutes(&gateways);
    QHash<QString, QStringList> dns;
    readDns(&dns);

    QList<InterfaceInfo> fresh;
    for (const QJsonValue &v : doc.array()) {
        InterfaceInfo info = parseLink(v.toObject());
        if (info.name.isEmpty()) {
            continue;
        }
        // Skip loopback and container/bridge plumbing unless netifrc is explicitly
        // configured for it.
        const bool explicitlyManaged = m_extraManaged.contains(info.name);
        if (!explicitlyManaged && (info.loopback || !isHardwareBacked(info.name))) {
            continue;
        }
        info.gateway = gateways.value(info.name);
        if (!info.addresses.isEmpty()) {
            info.dns = dns.value(QStringLiteral("*"));
        }
        fresh.append(info);
    }

    auto signature = [](const QList<InterfaceInfo> &list) {
        QStringList parts;
        for (const InterfaceInfo &i : list) {
            parts << QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                         .arg(i.name)
                         .arg(i.up)
                         .arg(i.carrier)
                         .arg(i.addresses.join(QLatin1Char(',')))
                         .arg(i.gateway)
                         .arg(i.dns.join(QLatin1Char(',')))
                         .arg(i.ssid)
                         .arg(i.bitrate)
                         .arg(i.mac);
        }
        return parts.join(QLatin1Char(';'));
    };

    const QString oldSig = signature(m_interfaces);
    m_interfaces = fresh;
    const QString newSig = signature(m_interfaces);

    if (!m_primed || oldSig != newSig) {
        m_primed = true;
        qCInfo(NETIFRC) << "interface state changed:" << newSig;
        Q_EMIT changed();
    }
}

QList<InterfaceInfo> InterfaceWatcher::interfaces() const
{
    return m_interfaces;
}

InterfaceInfo InterfaceWatcher::info(const QString &ifname) const
{
    for (const InterfaceInfo &i : m_interfaces) {
        if (i.name == ifname) {
            return i;
        }
    }
    return {};
}

} // namespace netifrcd
