// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.Settings and its child
// org.freedesktop.NetworkManager.Settings.Connection.
//
// Both classes live in this pair because a Settings.Connection object only ever
// exists underneath a Settings object and they share the same settings-map plumbing.
// Every property is a Q_PROPERTY so Qt answers org.freedesktop.DBus.Properties for
// us; we only emit PropertiesChanged ourselves via ObjectBase::notify().

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

namespace netifrcd
{

class Backend;

/// /org/freedesktop/NetworkManager/Settings
class SettingsObject : public ObjectBase, public QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Settings")

    Q_PROPERTY(QList<QDBusObjectPath> Connections READ connections)
    Q_PROPERTY(QString Hostname READ hostname)
    Q_PROPERTY(bool CanModify READ canModify)
    Q_PROPERTY(uint Metered READ metered)

public:
    explicit SettingsObject(Backend *backend, QObject *parent = nullptr);

    QList<QDBusObjectPath> connections() const;
    QString hostname() const;
    bool canModify() const;
    uint metered() const;

public Q_SLOTS:
    QList<QDBusObjectPath> ListConnections();
    QDBusObjectPath GetConnectionByUuid(const QString &uuid);
    QDBusObjectPath AddConnection(const SettingsMap &settings);
    QDBusObjectPath AddConnectionUnsaved(const SettingsMap &settings);
    QDBusObjectPath AddConnection2(const SettingsMap &settings, uint flags, const QVariantMap &args);
    void SaveHostname(const QString &hostname);
    bool ReloadConnections();

Q_SIGNALS:
    void NewConnection(const QDBusObjectPath &connection);
    void ConnectionRemoved(const QDBusObjectPath &connection);

private Q_SLOTS:
    /// Diffs Backend::settingsChanged() against the cached path set and emits
    /// NewConnection/ConnectionRemoved plus a Connections PropertiesChanged.
    void refreshConnections();

private:
    Backend *m_backend = nullptr;
    QSet<QString> m_knownConnections;
};

/// /org/freedesktop/NetworkManager/Settings/<id>
class SettingsConnectionObject : public ObjectBase, public QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.Settings.Connection")

    Q_PROPERTY(bool Unsaved READ unsaved)
    Q_PROPERTY(uint Flags READ flags)
    Q_PROPERTY(QString Filename READ filename)

public:
    explicit SettingsConnectionObject(Backend *backend, const QString &settingsPath, QObject *parent = nullptr);

    bool unsaved() const;
    uint flags() const;
    QString filename() const;

public Q_SLOTS:
    SettingsMap GetSettings();
    SettingsMap GetSecrets(const QString &settingName);
    void Update(const SettingsMap &settings);
    void UpdateUnsaved(const SettingsMap &settings);
    void Update2(const SettingsMap &settings, uint flags, const QVariantMap &args);
    void Delete();
    void Save();

Q_SIGNALS:
    void Removed();
    void Updated();

private Q_SLOTS:
    void onBackendConnectionChanged(const QString &path);
    void onBackendSettingsChanged();

private:
    Backend *m_backend = nullptr;
};

} // namespace netifrcd
