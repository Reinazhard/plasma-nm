// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// netifrcd - a NetworkManager-compatible D-Bus daemon backed by netifrc.
//
// It claims org.freedesktop.NetworkManager on the system bus and services it from
// netifrc (/etc/conf.d/net + /etc/init.d/net.*) instead of NetworkManager itself.
// Because NetworkManagerQt is a pure D-Bus client, the KDE Plasma applet, its KCM and
// the connection editor all keep working unmodified.

#include "common/Logging.h"
#include "model/Backend.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QSocketNotifier>
#include <QTimer>

#include <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
int gSignalFd[2] = {-1, -1};

void handleUnixSignal(int)
{
    const char byte = 1;
    // Async-signal-safe: just poke the notifier.
    [[maybe_unused]] const ssize_t written = ::write(gSignalFd[0], &byte, sizeof(byte));
}

void installSignalHandlers(QCoreApplication *app)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, gSignalFd) != 0) {
        qCWarning(NETIFRC) << "could not create signal socketpair; SIGTERM handling disabled";
        return;
    }

    auto *notifier = new QSocketNotifier(gSignalFd[1], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [app, notifier] {
        notifier->setEnabled(false);
        char byte = 0;
        [[maybe_unused]] const ssize_t readBytes = ::read(gSignalFd[1], &byte, sizeof(byte));
        qCInfo(NETIFRC) << "shutting down";
        app->quit();
    });

    struct sigaction sa = {};
    sa.sa_handler = handleUnixSignal;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("netifrcd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NETIFRCD_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("netifrcd"));

    const QStringList args = QCoreApplication::arguments();
    const bool busActivated = args.contains(QStringLiteral("--bus-activated"));

    // netifrcd rewrites /etc/conf.d/net and drives rc-service, so it needs root. The
    // escape hatch exists purely so the D-Bus surface can be exercised against a private
    // bus during development, without taking over the real NetworkManager bus name.
    const bool allowNonRoot = qEnvironmentVariableIsSet("NETIFRCD_ALLOW_NONROOT");
    if (::geteuid() != 0 && !allowNonRoot) {
        qCCritical(NETIFRC) << "netifrcd must run as root: it rewrites /etc/conf.d/net and drives rc-service";
        return 1;
    }
    if (allowNonRoot && ::geteuid() != 0) {
        qCWarning(NETIFRC) << "running as a non-root user because NETIFRCD_ALLOW_NONROOT is set; "
                              "configuration changes will fail";
    }

    installSignalHandlers(&app);

    netifrcd::Backend backend;

    QString error;
    if (!backend.start(&error)) {
        qCCritical(NETIFRC) << "startup failed:" << error;
        return 1;
    }

    if (!backend.registerObjects(&error)) {
        qCCritical(NETIFRC) << "could not take over the NetworkManager bus name:" << error;
        if (!busActivated) {
            qCCritical(NETIFRC) << "stop NetworkManager first: rc-service NetworkManager stop";
        }
        return 1;
    }

    qCInfo(NETIFRC) << "netifrcd" << NETIFRCD_VERSION << "ready";
    return app.exec();
}
