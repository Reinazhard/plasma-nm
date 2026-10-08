// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/ObjectBase.h"

#include "common/Constants.h"
#include "common/Logging.h"

#include <QDBusMessage>

namespace netifrcd
{

ObjectBase::ObjectBase(QString path, QObject *parent)
    : QObject(parent)
    , m_path(std::move(path))
{
}

void ObjectBase::sendPropertiesChanged(const QString &path, const QString &interface, const QVariantMap &changed)
{
    QDBusMessage msg = QDBusMessage::createSignal(path, QLatin1String(IfaceProperties), QStringLiteral("PropertiesChanged"));
    msg << interface << changed << QStringList();
    if (!QDBusConnection::systemBus().send(msg)) {
        qCWarning(NETIFRC) << "failed to emit PropertiesChanged for" << path << interface;
    }
}

void ObjectBase::notify(const QString &interface, const QVariantMap &changed)
{
    if (changed.isEmpty()) {
        return;
    }
    sendPropertiesChanged(m_path, interface, changed);
}

void ObjectBase::invalidate(const QString &interface)
{
    QDBusMessage msg = QDBusMessage::createSignal(m_path, QLatin1String(IfaceProperties), QStringLiteral("PropertiesChanged"));
    msg << interface << QVariantMap() << QStringList();
    QDBusConnection::systemBus().send(msg);
}

} // namespace netifrcd
