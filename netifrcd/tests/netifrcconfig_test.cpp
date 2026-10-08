// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Unit tests for NetifrcConfig's managed-block writer.
//
// NetifrcConfig is the one part of netifrcd that is a pure function of a file - it takes
// its path in the constructor - so its invariants are pinned here rather than only
// through the end-to-end shell scripts in this directory, which need root and a real
// radio. Everything runs against a QTemporaryDir, so nothing touches /etc/conf.d/net.
//
// The regression these exist for (found 2026-10-08): removeInterface() removed the
// interface from the netifrcd_interfaces list but re-inserted an *empty* list instead of
// dropping the variable, so save() still emitted a managed block holding
// `netifrcd_interfaces=""`. The block therefore never disappeared: an interface looked
// handed back to the administrator while /etc/conf.d/net still claimed it.

#include "netifrc/NetifrcConfig.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest>

using namespace netifrcd;

namespace {

// These are the on-disk format, not implementation details: the block is delimited by
// these exact comment lines, so the test asserting on them is the point.
constexpr auto kBeginMarker = "# >>> netifrcd managed block >>>";
constexpr auto kEndMarker = "# <<< netifrcd managed block <<<";
constexpr auto kInterfaceListVar = "netifrcd_interfaces";

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool writeAll(const QString &path, const QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

NetifrcConfig::InterfaceConfig dhcpConfig()
{
    NetifrcConfig::InterfaceConfig cfg;
    cfg.configured = true;
    cfg.method = NetifrcConfig::Method::Dhcp;
    return cfg;
}

} // namespace

class NetifrcConfigTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void releaseLastInterfaceRemovesManagedBlock();
    void releasingOneInterfaceKeepsTheOther();
    void administratorLinesAreNeverRewritten();
    void managedBlockIsEmittedAfterTheAdministratorLines();
    void releasingAnUnmanagedInterfaceIsHarmless();
};

/// The regression: after the last interface is released the block must be gone entirely,
/// not left behind holding an empty interface list.
void NetifrcConfigTest::releaseLastInterfaceRemovesManagedBlock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("net"));

    const QString original = QStringLiteral("# Administrator's own configuration.\n"
                                           "config_eth0=\"10.0.0.5/24\"\n");
    QVERIFY(writeAll(path, original));

    NetifrcConfig cfg(path);
    QVERIFY(cfg.load());
    cfg.setInterface(QStringLiteral("eth0"), dhcpConfig());
    cfg.setInterface(QStringLiteral("wlan0"), dhcpConfig());
    QVERIFY(cfg.save());

    QString content = readAll(path);
    QVERIFY(content.contains(QLatin1String(kBeginMarker)));
    QVERIFY(content.contains(QStringLiteral("config_wlan0=\"dhcp\"")));

    // Releasing one of two must leave the block standing for the other.
    cfg.removeInterface(QStringLiteral("eth0"));
    QVERIFY(cfg.save());
    content = readAll(path);
    QVERIFY(content.contains(QLatin1String(kBeginMarker)));
    QVERIFY(content.contains(QStringLiteral("config_wlan0=\"dhcp\"")));
    QVERIFY(!content.contains(QStringLiteral("config_eth0=\"dhcp\"")));

    // Releasing the last one must remove the block entirely.
    cfg.removeInterface(QStringLiteral("wlan0"));
    QVERIFY(cfg.save());
    content = readAll(path);
    QVERIFY2(!content.contains(QLatin1String(kBeginMarker)),
             "the managed block survived the release of its last interface");
    QVERIFY(!content.contains(QLatin1String(kEndMarker)));
    QVERIFY2(!content.contains(QLatin1String(kInterfaceListVar)),
             "an empty netifrcd_interfaces variable was left behind");

    // And the administrator gets their file back exactly as they wrote it.
    QCOMPARE(content, original);
}

/// A release must only drop the named interface, never the whole block.
void NetifrcConfigTest::releasingOneInterfaceKeepsTheOther()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("net"));
    QVERIFY(writeAll(path, QString()));

    NetifrcConfig cfg(path);
    QVERIFY(cfg.load());
    cfg.setInterface(QStringLiteral("eth0"), dhcpConfig());
    cfg.setInterface(QStringLiteral("wlan0"), dhcpConfig());
    QVERIFY(cfg.save());

    cfg.removeInterface(QStringLiteral("wlan0"));
    QVERIFY(cfg.save());

    const QString content = readAll(path);
    QVERIFY(content.contains(QStringLiteral("config_eth0=\"dhcp\"")));
    QVERIFY(!content.contains(QStringLiteral("config_wlan0")));
    // The list variable must still name the survivor.
    QVERIFY(content.contains(QStringLiteral("eth0")));
    QVERIFY(!content.contains(QStringLiteral("wlan0")));
}

/// netifrcd may shadow the administrator's assignments (its block is emitted last), but
/// it must never rewrite or delete their lines. The whole ownership decision leans on
/// this: taking an interface over is reversible precisely because nothing is destroyed.
void NetifrcConfigTest::administratorLinesAreNeverRewritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("net"));

    const QString original = QStringLiteral("# my network\n"
                                           "config_eth0=\"10.0.0.5/24\"\n"
                                           "routes_eth0=\"default via 10.0.0.1\"\n"
                                           "config_wlan0=\"dhcp\"\n");
    QVERIFY(writeAll(path, original));

    NetifrcConfig cfg(path);
    QVERIFY(cfg.load());
    cfg.setInterface(QStringLiteral("eth0"), dhcpConfig()); // takes over eth0
    QVERIFY(cfg.save());

    const QString content = readAll(path);
    const QStringList lines = original.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.isEmpty()) {
            continue;
        }
        QVERIFY2(content.contains(line), qPrintable(QStringLiteral("lost administrator line: ") + line));
    }
}

/// netifrcd's assignments win purely because the block is written *after* the
/// administrator's lines (netifrc sources the file top to bottom). If that ordering ever
/// flipped, claiming an interface would silently stop taking effect.
void NetifrcConfigTest::managedBlockIsEmittedAfterTheAdministratorLines()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("net"));
    QVERIFY(writeAll(path, QStringLiteral("config_eth0=\"10.0.0.5/24\"\n")));

    NetifrcConfig cfg(path);
    QVERIFY(cfg.load());
    cfg.setInterface(QStringLiteral("eth0"), dhcpConfig());
    QVERIFY(cfg.save());

    const QString content = readAll(path);
    const int adminLine = content.indexOf(QStringLiteral("config_eth0=\"10.0.0.5/24\""));
    const int blockStart = content.indexOf(QLatin1String(kBeginMarker));
    const int claimedLine = content.indexOf(QStringLiteral("config_eth0=\"dhcp\""));
    QVERIFY(adminLine >= 0);
    QVERIFY(blockStart >= 0);
    QVERIFY(claimedLine >= 0);
    QVERIFY2(adminLine < blockStart, "the administrator's lines must come before the managed block");
    QVERIFY2(blockStart < claimedLine, "the claimed value must live inside the managed block");
}

/// removeInterface() is also reached for profiles that never claimed anything, and a
/// double release must not corrupt the file or take a different interface down with it.
void NetifrcConfigTest::releasingAnUnmanagedInterfaceIsHarmless()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("net"));
    QVERIFY(writeAll(path, QStringLiteral("config_wlan0=\"dhcp\"\n")));

    NetifrcConfig cfg(path);
    QVERIFY(cfg.load());
    cfg.setInterface(QStringLiteral("wlan0"), dhcpConfig());
    QVERIFY(cfg.save());

    cfg.removeInterface(QStringLiteral("eth0")); // never managed
    QVERIFY(cfg.save());
    QVERIFY(readAll(path).contains(QLatin1String(kBeginMarker)));

    cfg.removeInterface(QStringLiteral("wlan0"));
    cfg.removeInterface(QStringLiteral("wlan0")); // releasing twice must be a no-op
    QVERIFY(cfg.save());

    const QString content = readAll(path);
    QVERIFY(!content.contains(QLatin1String(kBeginMarker)));
    QVERIFY(content.contains(QStringLiteral("config_wlan0=\"dhcp\""))); // the administrator's line
}

QTEST_MAIN(NetifrcConfigTest)
#include "netifrcconfig_test.moc"
