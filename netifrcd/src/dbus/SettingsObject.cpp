// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/SettingsObject.h"

#include "common/Logging.h"
#include "model/Backend.h"
#include "model/Profile.h"

#include <QDBusError>
#include <QSysInfo>

namespace netifrcd
{

namespace
{
/// A setting whose sub-map may carry secrets. GetSecrets() only ever hands back
/// these sections, never the whole profile.
bool isSecretSetting(const QString &name)
{
    return name.endsWith(QLatin1String("-security")) || name == QLatin1String("802-1x");
}
} // namespace

// ---------------------------------------------------------------------------
// SettingsObject
// ---------------------------------------------------------------------------

SettingsObject::SettingsObject(Backend *backend, QObject *parent)
    : ObjectBase(QString::fromLatin1(PathSettings), parent)
    , m_backend(backend)
{
    for (const Profile &profile : m_backend->profiles()) {
        const QString path = m_backend->settingsPathFor(profile.uuid);
        if (!path.isEmpty()) {
            m_knownConnections.insert(path);
        }
    }

    connect(m_backend, &Backend::settingsChanged, this, &SettingsObject::refreshConnections);
}

QList<QDBusObjectPath> SettingsObject::connections() const
{
    QList<QDBusObjectPath> paths;
    const QList<Profile> profiles = m_backend->profiles();
    paths.reserve(profiles.size());
    for (const Profile &profile : profiles) {
        const QString path = m_backend->settingsPathFor(profile.uuid);
        if (!path.isEmpty()) {
            paths.append(toPath(path));
        }
    }
    return paths;
}

QString SettingsObject::hostname() const
{
    return QSysInfo::machineHostName();
}

bool SettingsObject::canModify() const
{
    return true;
}

uint SettingsObject::metered() const
{
    // netifrc has no notion of metered links. Report "unknown" rather than guessing,
    // which is what NetworkManager does when it cannot tell either.
    return NM_METERED_UNKNOWN;
}

QList<QDBusObjectPath> SettingsObject::ListConnections()
{
    return connections();
}

QDBusObjectPath SettingsObject::GetConnectionByUuid(const QString &uuid)
{
    const Profile profile = m_backend->profileByUuid(uuid);
    if (!profile.isValid()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("No connection with UUID '%1'").arg(uuid));
        return nonePath();
    }

    const QString path = m_backend->settingsPathFor(profile.uuid);
    if (path.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Connection '%1' has no settings path").arg(uuid));
        return nonePath();
    }
    return toPath(path);
}

QDBusObjectPath SettingsObject::AddConnection(const SettingsMap &settings)
{
    QString error;
    const QString path = m_backend->addConnection(settings, &error);
    if (path.isEmpty()) {
        sendErrorReply(QDBusError::Failed,
                       error.isEmpty() ? QStringLiteral("Failed to add connection") : error);
        return nonePath();
    }
    return toPath(path);
}

QDBusObjectPath SettingsObject::AddConnectionUnsaved(const SettingsMap &settings)
{
    // Gap: netifrcd has no in-memory-only profile store - every profile is written
    // through to netifrc. Backend::addConnection() is the closest existing operation,
    // so an "unsaved" connection is persisted like a saved one and Unsaved stays false.
    return AddConnection(settings);
}

QDBusObjectPath SettingsObject::AddConnection2(const SettingsMap &settings, uint flags, const QVariantMap &args)
{
    // Gap: the flags (TO_DISK / IN_MEMORY / BLOCK_AUTOCONNECT) and args map are not
    // modelled by Backend, which only has addConnection(). Treat every call as the
    // default "persist to disk" behaviour.
    Q_UNUSED(flags);
    Q_UNUSED(args);
    return AddConnection(settings);
}

void SettingsObject::SaveHostname(const QString &hostname)
{
    // Gap: Backend has no hostname setter and netifrc does not manage /etc/hostname.
    // Failing is honest; silently accepting would promise persistence we cannot deliver.
    Q_UNUSED(hostname);
    sendErrorReply(QDBusError::Failed, QStringLiteral("netifrcd does not manage the persistent hostname"));
}

bool SettingsObject::ReloadConnections()
{
    // Gap: Backend has no explicit reload accessor - profiles() is read live from the
    // profile store on every call, so there is nothing to re-read. Re-announce the
    // current list so a client that called us out of paranoia still gets fresh data.
    refreshConnections();
    return true;
}

void SettingsObject::refreshConnections()
{
    QSet<QString> current;
    const QList<Profile> profiles = m_backend->profiles();
    for (const Profile &profile : profiles) {
        const QString path = m_backend->settingsPathFor(profile.uuid);
        if (!path.isEmpty()) {
            current.insert(path);
        }
    }

    for (const QString &path : current) {
        if (!m_knownConnections.contains(path)) {
            Q_EMIT NewConnection(toPath(path));
        }
    }
    for (const QString &path : m_knownConnections) {
        if (!current.contains(path)) {
            Q_EMIT ConnectionRemoved(toPath(path));
        }
    }
    m_knownConnections = current;

    notify(QLatin1String(IfaceSettings),
           {{QStringLiteral("Connections"), QVariant::fromValue(connections())}});
}

// ---------------------------------------------------------------------------
// SettingsConnectionObject
// ---------------------------------------------------------------------------

SettingsConnectionObject::SettingsConnectionObject(Backend *backend, const QString &settingsPath, QObject *parent)
    : ObjectBase(settingsPath, parent)
    , m_backend(backend)
{
    connect(m_backend, &Backend::settingsConnectionChanged, this, &SettingsConnectionObject::onBackendConnectionChanged);
    connect(m_backend, &Backend::settingsChanged, this, &SettingsConnectionObject::onBackendSettingsChanged);
}

bool SettingsConnectionObject::unsaved() const
{
    // See AddConnectionUnsaved(): netifrcd persists every write, so nothing is ever dirty.
    return false;
}

uint SettingsConnectionObject::flags() const
{
    return NM_SETTINGS_CONNECTION_FLAG_NONE;
}

QString SettingsConnectionObject::filename() const
{
    // netifrc keeps profiles in /etc/conf.d/net, not in one file per connection, so
    // there is no NetworkManager-style backing filename to report.
    return QString();
}

SettingsMap SettingsConnectionObject::GetSettings()
{
    return m_backend->settingsForPath(path());
}

SettingsMap SettingsConnectionObject::GetSecrets(const QString &settingName)
{
    const Profile profile = m_backend->profileBySettingsPath(path());
    if (!profile.isValid()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Unknown connection"));
        return {};
    }

    const SettingsMap full = profile.toSettingsMap();

    if (settingName.isEmpty()) {
        SettingsMap secrets;
        for (auto it = full.constBegin(); it != full.constEnd(); ++it) {
            if (isSecretSetting(it.key())) {
                secrets.insert(it.key(), it.value());
            }
        }
        return secrets;
    }

    const QVariantMap section = full.value(settingName);
    if (section.isEmpty()) {
        // No secrets of the requested kind: NetworkManager replies with an empty map,
        // not an error, so the agent does not treat it as a failure.
        return {};
    }
    return {{settingName, section}};
}

void SettingsConnectionObject::Update(const SettingsMap &settings)
{
    QString error;
    if (!m_backend->updateConnection(path(), settings, &error)) {
        sendErrorReply(QDBusError::Failed,
                       error.isEmpty() ? QStringLiteral("Failed to update connection") : error);
        return;
    }
    // Backend::settingsConnectionChanged() drives Updated() and PropertiesChanged.
}

void SettingsConnectionObject::UpdateUnsaved(const SettingsMap &settings)
{
    // Gap: there is no in-memory-only copy of a profile, so an "unsaved" update is
    // persisted immediately. Update() is the closest existing operation.
    Update(settings);
}

void SettingsConnectionObject::Update2(const SettingsMap &settings, uint flags, const QVariantMap &args)
{
    // Gap: Backend does not model the Update2 flags or the args map. Without a flag
    // the NetworkManager default is to persist, which is what Update() does.
    Q_UNUSED(flags);
    Q_UNUSED(args);
    Update(settings);
}

void SettingsConnectionObject::Delete()
{
    QString error;
    if (!m_backend->removeConnection(path(), &error)) {
        sendErrorReply(QDBusError::Failed,
                       error.isEmpty() ? QStringLiteral("Failed to delete connection") : error);
        return;
    }
    // Backend::settingsChanged() drives Removed().
}

void SettingsConnectionObject::Save()
{
    // Nothing to save: every netifrcd write is already persisted (see UpdateUnsaved()).
}

void SettingsConnectionObject::onBackendConnectionChanged(const QString &path)
{
    if (path != this->path()) {
        return;
    }
    if (m_backend->settingsForPath(this->path()).isEmpty()) {
        Q_EMIT Removed();
        return;
    }
    Q_EMIT Updated();
}

void SettingsConnectionObject::onBackendSettingsChanged()
{
    if (m_backend->settingsForPath(path()).isEmpty()) {
        Q_EMIT Removed();
    }
}

} // namespace netifrcd
