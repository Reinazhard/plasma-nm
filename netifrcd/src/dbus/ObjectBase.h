// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Base class for every exported NetworkManager object.
//
// Qt exports Q_PROPERTYs of a registered QObject (and of any QDBusAbstractAdaptor
// attached to it) through org.freedesktop.DBus.Properties automatically, so we only
// have to emit PropertiesChanged ourselves. That is what notify() does.

#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>

namespace netifrcd
{

class ObjectBase : public QObject
{
    Q_OBJECT

public:
    explicit ObjectBase(QString path, QObject *parent = nullptr);

    QString path() const
    {
        return m_path;
    }

    QDBusObjectPath dbusPath() const
    {
        return QDBusObjectPath(m_path);
    }

    /// Emits org.freedesktop.DBus.Properties.PropertiesChanged for this object.
    void notify(const QString &interface, const QVariantMap &changed);

    /// Emits PropertiesChanged with an empty changed map (all properties invalidated).
    void invalidate(const QString &interface);

    static void sendPropertiesChanged(const QString &path, const QString &interface, const QVariantMap &changed);

protected:
    QString m_path;
};

} // namespace netifrcd
