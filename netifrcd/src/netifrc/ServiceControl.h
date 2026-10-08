// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Starts and stops netifrc's OpenRC services (/etc/init.d/net.<iface>).
//
// Deliberately narrow: netifrcd does NOT ask OpenRC for interface state. State is read
// straight from the kernel by InterfaceWatcher, which is cheap and non-blocking, while
// OpenRC is only invoked for the (slow, forking) start/stop transitions. That keeps the
// daemon responsive to D-Bus while a DHCP lease is being acquired.

#pragma once

#include <QObject>
#include <QSet>
#include <QString>

class QProcess;

namespace netifrcd
{

class ServiceControl : public QObject
{
    Q_OBJECT

public:
    enum class Action { Start, Stop, Restart };

    explicit ServiceControl(QObject *parent = nullptr);

    /// OpenRC service name backing an interface: "net.eth0".
    static QString serviceFor(const QString &ifname);

    /// True if /etc/init.d/net.<iface> exists.
    static bool serviceExists(const QString &ifname);

    /// Creates the /etc/init.d/net.<iface> -> net.lo symlink if it is missing.
    /// This is what Gentoo's own net-setup does; without it OpenRC has no service.
    bool ensureService(const QString &ifname, QString *error = nullptr);

    /// True while an action is in flight for this interface.
    bool isBusy(const QString &ifname) const;

    /// Queues a start/stop/restart. Completion is reported via actionFinished().
    void perform(Action action, const QString &ifname);

Q_SIGNALS:
    /// Emitted once the OpenRC invocation exits.
    void actionFinished(const QString &ifname, netifrcd::ServiceControl::Action action, bool success, const QString &message);

private:
    void run(const QString &ifname, Action action, const QStringList &args);
    static QString actionName(Action action);

    QSet<QString> m_busy;
};

} // namespace netifrcd

Q_DECLARE_METATYPE(netifrcd::ServiceControl::Action)
