// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "model/ProfileStore.h"

#include "common/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

namespace netifrcd
{

namespace
{
constexpr int StoreVersion = 1;

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

QStringList toStringList(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    QStringList out;
    out.reserve(array.size());
    for (const QJsonValue &entry : array) {
        out.append(entry.toString());
    }
    return out;
}

QJsonObject profileToJson(const Profile &p)
{
    QJsonObject o;
    o.insert(QStringLiteral("uuid"), p.uuid);
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("type"), p.type);
    o.insert(QStringLiteral("interfaceName"), p.interfaceName);
    o.insert(QStringLiteral("autoconnect"), p.autoconnect);
    o.insert(QStringLiteral("timestamp"), QJsonValue(static_cast<double>(p.timestamp)));
    o.insert(QStringLiteral("ipv4Method"), methodToString(p.ipv4Method));
    o.insert(QStringLiteral("addresses"), QJsonArray::fromStringList(p.addresses));
    o.insert(QStringLiteral("gateway"), p.gateway);
    o.insert(QStringLiteral("dns"), QJsonArray::fromStringList(p.dns));
    o.insert(QStringLiteral("ssid"), p.ssid);
    o.insert(QStringLiteral("psk"), p.psk);
    o.insert(QStringLiteral("hidden"), p.hidden);
    return o;
}

Profile profileFromJson(const QJsonObject &o)
{
    Profile p;
    // Missing keys fall back to the defaults declared on the struct.
    p.uuid = o.value(QStringLiteral("uuid")).toString();
    p.id = o.value(QStringLiteral("id")).toString();
    p.type = o.value(QStringLiteral("type")).toString();
    p.interfaceName = o.value(QStringLiteral("interfaceName")).toString();
    p.autoconnect = o.value(QStringLiteral("autoconnect")).toBool(true);
    p.timestamp = static_cast<quint64>(o.value(QStringLiteral("timestamp")).toDouble());
    p.ipv4Method = methodFromString(o.value(QStringLiteral("ipv4Method")).toString());
    p.addresses = toStringList(o.value(QStringLiteral("addresses")));
    p.gateway = o.value(QStringLiteral("gateway")).toString();
    p.dns = toStringList(o.value(QStringLiteral("dns")));
    p.ssid = o.value(QStringLiteral("ssid")).toString();
    p.psk = o.value(QStringLiteral("psk")).toString();
    p.hidden = o.value(QStringLiteral("hidden")).toBool(false);
    return p;
}
} // namespace

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent)
    , m_path(defaultPath())
{
}

QString ProfileStore::defaultPath()
{
    // Dev/test escape hatch, mirroring NETIFRCD_ALLOW_NONROOT and
    // NETIFRCD_NETIFRC_CONF: keep the store somewhere writable so the full D-Bus
    // surface (add / update / delete a connection) can be driven without root.
    const QString override = qEnvironmentVariable("NETIFRCD_PROFILE_STORE");
    return override.isEmpty() ? QStringLiteral("/var/lib/netifrcd/profiles.json") : override;
}

bool ProfileStore::load(QString *error)
{
    m_profiles.clear();
    m_dirty = false;

    QFile file(m_path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        const QString message = QStringLiteral("Failed to open profile store %1: %2").arg(m_path, file.errorString());
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        const QString message = QStringLiteral("Failed to parse profile store %1: %2").arg(m_path, parseError.errorString());
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }
    if (!doc.isObject()) {
        const QString message = QStringLiteral("Profile store %1 is not a JSON object").arg(m_path);
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }

    const QJsonArray array = doc.object().value(QStringLiteral("profiles")).toArray();
    m_profiles.reserve(array.size());
    for (const QJsonValue &entry : array) {
        if (entry.isObject()) {
            m_profiles.append(profileFromJson(entry.toObject()));
        }
    }
    return true;
}

bool ProfileStore::save(QString *error)
{
    const QDir dir = QFileInfo(m_path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        const QString message = QStringLiteral("Failed to create profile store directory %1").arg(dir.absolutePath());
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }

    QJsonArray array;
    for (const Profile &p : m_profiles) {
        array.append(profileToJson(p));
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), StoreVersion);
    root.insert(QStringLiteral("profiles"), array);

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        const QString message = QStringLiteral("Failed to open profile store %1 for writing: %2").arg(m_path, file.errorString());
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        const QString message = QStringLiteral("Failed to write profile store %1: %2").arg(m_path, file.errorString());
        qCWarning(NETIFRC) << message;
        file.cancelWriting();
        if (error) {
            *error = message;
        }
        return false;
    }
    if (!file.commit()) {
        const QString message = QStringLiteral("Failed to commit profile store %1: %2").arg(m_path, file.errorString());
        qCWarning(NETIFRC) << message;
        if (error) {
            *error = message;
        }
        return false;
    }

    m_dirty = false;
    return true;
}

QList<Profile> ProfileStore::profiles() const
{
    return m_profiles;
}

Profile ProfileStore::profileByUuid(const QString &uuid) const
{
    const int index = indexForUuid(uuid);
    return index >= 0 ? m_profiles.at(index) : Profile{};
}

bool ProfileStore::contains(const QString &uuid) const
{
    return indexForUuid(uuid) >= 0;
}

void ProfileStore::addOrUpdate(const Profile &p)
{
    const int index = indexForUuid(p.uuid);
    if (index < 0) {
        m_profiles.append(p);
    } else if (profileToJson(m_profiles.at(index)) != profileToJson(p)) {
        m_profiles[index] = p;
    } else {
        return; // identical, nothing changed
    }

    m_dirty = true;
    Q_EMIT changed();
}

void ProfileStore::remove(const QString &uuid)
{
    const int index = indexForUuid(uuid);
    if (index < 0) {
        return;
    }

    m_profiles.removeAt(index);
    m_dirty = true;
    Q_EMIT changed();
}

int ProfileStore::indexForUuid(const QString &uuid) const
{
    if (uuid.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).uuid == uuid) {
            return i;
        }
    }
    return -1;
}

QString ProfileStore::uuidForIndex(int index) const
{
    if (index < 0 || index >= m_profiles.size()) {
        return QString();
    }
    return m_profiles.at(index).uuid;
}

bool ProfileStore::isDirty() const
{
    return m_dirty;
}

} // namespace netifrcd
