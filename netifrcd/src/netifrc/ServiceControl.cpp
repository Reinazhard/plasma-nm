// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "netifrc/ServiceControl.h"

#include "common/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace netifrcd
{

namespace
{
constexpr auto kInitDir = "/etc/init.d";
constexpr auto kTemplateService = "net.lo";
constexpr int kActionTimeoutMs = 120000; // DHCP on a slow network can take a while.
} // namespace

ServiceControl::ServiceControl(QObject *parent)
    : QObject(parent)
{
}

QString ServiceControl::serviceFor(const QString &ifname)
{
    return QStringLiteral("net.") + ifname;
}

bool ServiceControl::serviceExists(const QString &ifname)
{
    return QFileInfo::exists(QStringLiteral("%1/%2").arg(QLatin1String(kInitDir), serviceFor(ifname)));
}

bool ServiceControl::ensureService(const QString &ifname, QString *error)
{
    if (serviceExists(ifname)) {
        return true;
    }

    const QString templatePath = QStringLiteral("%1/%2").arg(QLatin1String(kInitDir), QLatin1String(kTemplateService));
    if (!QFileInfo::exists(templatePath)) {
        if (error) {
            *error = QStringLiteral("no netifrc init script template at %1").arg(templatePath);
        }
        return false;
    }

    const QString linkPath = QStringLiteral("%1/%2").arg(QLatin1String(kInitDir), serviceFor(ifname));
    if (!QFile::link(templatePath, linkPath)) {
        if (error) {
            *error = QStringLiteral("cannot create %1 (are we running as root?)").arg(linkPath);
        }
        return false;
    }

    qCInfo(NETIFRC) << "created netifrc service" << linkPath << "->" << templatePath;
    return true;
}

bool ServiceControl::isBusy(const QString &ifname) const
{
    return m_busy.contains(ifname);
}

QString ServiceControl::actionName(Action action)
{
    switch (action) {
    case Action::Start:
        return QStringLiteral("start");
    case Action::Stop:
        return QStringLiteral("stop");
    case Action::Restart:
        return QStringLiteral("restart");
    }
    return QStringLiteral("start");
}

void ServiceControl::perform(Action action, const QString &ifname)
{
    if (m_busy.contains(ifname)) {
        qCWarning(NETIFRC) << "ignoring" << actionName(action) << "for" << ifname << "- an action is already in flight";
        return;
    }

    QString error;
    if (action == Action::Start && !ensureService(ifname, &error)) {
        Q_EMIT actionFinished(ifname, action, false, error);
        return;
    }

    m_busy.insert(ifname);
    run(ifname, action, {actionName(action)});
}

void ServiceControl::run(const QString &ifname, Action action, const QStringList &args)
{
    QString program = QStandardPaths::findExecutable(QStringLiteral("rc-service"));
    if (program.isEmpty()) {
        program = QStringLiteral("/sbin/rc-service");
    }

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(QStringList{serviceFor(ifname)} + args);

    connect(proc, &QProcess::errorOccurred, this, [this, proc, ifname, action](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) {
            return;
        }
        m_busy.remove(ifname);
        const QString msg = QStringLiteral("failed to launch %1: %2").arg(proc->program(), proc->errorString());
        qCWarning(NETIFRC) << msg;
        Q_EMIT actionFinished(ifname, action, false, msg);
        proc->deleteLater();
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, proc, ifname, action](int code, QProcess::ExitStatus status) {
        m_busy.remove(ifname);

        const QString output = QString::fromUtf8(proc->readAll()).trimmed();
        bool ok = (status == QProcess::NormalExit && code == 0);

        // rc-service exits non-zero when asked to start an already-started service
        // (and vice versa), or when netifrc backgrounds the service while it waits
        // for the link - e.g. the wpa_supplicant module returns while the supplicant
        // is still associating ("has started, but is inactive"). Neither is a failure
        // from our point of view: the real state is derived from the interface.
        if (!ok && (output.contains(QLatin1String("already started")) || output.contains(QLatin1String("already stopped"))
                    || output.contains(QLatin1String("but is inactive")))) {
            ok = true;
        }

        qCInfo(NETIFRC) << "rc-service" << serviceFor(ifname) << actionName(action) << "exit" << code << (ok ? "ok" : "FAILED");
        if (!output.isEmpty()) {
            qCDebug(NETIFRC) << "output:" << output;
        }
        Q_EMIT actionFinished(ifname, action, ok, output.isEmpty() ? QStringLiteral("rc-service exited with code %1").arg(code) : output);
        proc->deleteLater();
    });

    // Safety net so a wedged script cannot leave the interface permanently "busy".
    QTimer::singleShot(kActionTimeoutMs, proc, [this, proc, ifname, action] {
        if (proc->state() != QProcess::NotRunning) {
            qCWarning(NETIFRC) << "timeout waiting for rc-service" << serviceFor(ifname) << actionName(action);
            proc->kill();
            m_busy.remove(ifname);
            Q_EMIT actionFinished(ifname, action, false, QStringLiteral("timed out"));
        }
    });

    qCInfo(NETIFRC) << "running" << program << proc->arguments();
    proc->start();
}

} // namespace netifrcd
