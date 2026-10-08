// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dbus/AgentManagerObject.h"

#include "common/Logging.h"
#include "model/Backend.h"

namespace netifrcd
{

AgentManagerObject::AgentManagerObject(Backend *backend, QObject *parent)
    : ObjectBase(QString::fromLatin1(PathAgentManager), parent)
    , m_backend(backend)
{
}

void AgentManagerObject::Register(const QString &identifier)
{
    // netifrcd never requests secrets, so this registration is purely ceremonial - but
    // the kded secrets agent treats a failed Register() as a hard error and spams the
    // journal, so we accept it and record who asked.
    rememberAgent(identifier);
    qCInfo(NETIFRC) << "secret agent registered:" << identifier;
}

void AgentManagerObject::RegisterWithCapabilities(const QString &identifier, uint capabilities)
{
    rememberAgent(identifier);
    qCInfo(NETIFRC) << "secret agent registered with capabilities:" << identifier << capabilities;
}

void AgentManagerObject::Unregister()
{
    // NetworkManager ties an agent to the caller's bus name; netifrcd does not track
    // that mapping, so we simply forget the most recently registered identifier.
    if (!m_agents.isEmpty()) {
        qCInfo(NETIFRC) << "secret agent unregistered:" << m_agents.constLast();
        m_agents.removeLast();
    }
}

void AgentManagerObject::rememberAgent(const QString &identifier)
{
    if (!m_agents.contains(identifier)) {
        m_agents.append(identifier);
    }
}

} // namespace netifrcd
