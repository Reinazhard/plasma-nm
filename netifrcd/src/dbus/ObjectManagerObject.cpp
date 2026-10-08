// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/ObjectManagerObject.h"

#include "common/Logging.h"
#include "model/Backend.h"

#include <QDBusMetaType>

namespace netifrcd
{

ObjectManagerObject::ObjectManagerObject(Backend *backend, QObject *parent)
    : ObjectBase(QStringLiteral("/org/freedesktop"), parent)
    , m_backend(backend)
{
    // Qt has no built-in marshaller for these nested containers, and silently drops a
    // method whose signature it cannot build. Registering them up front is what makes
    // GetManagedObjects() marshal as a{oa{sa{sv}}}.
    qDBusRegisterMetaType<InterfacePropertiesMap>();
    qDBusRegisterMetaType<ManagedObjectsMap>();
}

ManagedObjectsMap ObjectManagerObject::GetManagedObjects() const
{
    return m_backend->managedObjects();
}

} // namespace netifrcd
