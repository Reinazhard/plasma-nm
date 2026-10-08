// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// org.freedesktop.NetworkManager.AgentManager.
//
// netifrcd stores no secrets and never asks a secret agent for any, but the KDE
// kded secrets agent registers here on startup and logs an error if the call fails.
// We accept the registration, remember who called, and stay quiet - no signals are
// emitted because NetworkManager itself emits none from this interface.

#pragma once

#include "common/Constants.h"
#include "dbus/ObjectBase.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace netifrcd
{

class Backend;

/// /org/freedesktop/NetworkManager/AgentManager
class AgentManagerObject : public ObjectBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.AgentManager")

public:
    explicit AgentManagerObject(Backend *backend, QObject *parent = nullptr);

public Q_SLOTS:
    void Register(const QString &identifier);
    void RegisterWithCapabilities(const QString &identifier, uint capabilities);
    void Unregister();

private:
    void rememberAgent(const QString &identifier);

    Backend *m_backend = nullptr;
    QStringList m_agents;
};

} // namespace netifrcd
