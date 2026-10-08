// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "wifi/WifiScanner.h"

#include "common/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

#include <nm-dbus-interface.h>

namespace netifrcd
{

namespace
{
constexpr auto kSupplicantDir = "/etc/wpa_supplicant";
constexpr auto kCtrlDir = "/run/wpa_supplicant";
constexpr int kScanTimeoutMs = 20000;

QByteArray runTool(const QString &program, const QStringList &args, int timeoutMs = kScanTimeoutMs)
{
    const QString path = QStandardPaths::findExecutable(program);
    if (path.isEmpty()) {
        return {};
    }
    QProcess proc;
    proc.setProgram(path);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(timeoutMs)) {
        qCWarning(NETIFRC) << program << args << "timed out";
        proc.kill();
        proc.waitForFinished(2000);
        return {};
    }
    return proc.readAllStandardOutput();
}

bool supplicantRunning(const QString &ifname)
{
    return QFileInfo::exists(QStringLiteral("%1/%2").arg(QLatin1String(kCtrlDir), ifname));
}

/// NetworkManager reports signal strength as a 0-100 percentage; iw/wpa_cli give dBm.
int dbmToPercent(int dbm)
{
    const int pct = 2 * (dbm + 100);
    return std::clamp(pct, 0, 100);
}

void applyCipher(const QString &cipher, bool group, int *wpaFlags, int *rsnFlags, bool rsn)
{
    int bits = 0;
    if (cipher.contains(QLatin1String("CCMP"))) {
        bits |= group ? NM_802_11_AP_SEC_GROUP_CCMP : NM_802_11_AP_SEC_PAIR_CCMP;
    }
    if (cipher.contains(QLatin1String("TKIP"))) {
        bits |= group ? NM_802_11_AP_SEC_GROUP_TKIP : NM_802_11_AP_SEC_PAIR_TKIP;
    }
    if (cipher.contains(QLatin1String("WEP104"))) {
        bits |= group ? NM_802_11_AP_SEC_GROUP_WEP104 : NM_802_11_AP_SEC_PAIR_WEP104;
    }
    if (cipher.contains(QLatin1String("WEP40"))) {
        bits |= group ? NM_802_11_AP_SEC_GROUP_WEP40 : NM_802_11_AP_SEC_PAIR_WEP40;
    }
    (rsn ? rsnFlags : wpaFlags)[0] |= 0; // no-op, keeps clang-tidy quiet about unused
    if (rsn) {
        *rsnFlags |= bits;
    } else {
        *wpaFlags |= bits;
    }
}

void applyAuth(const QString &auth, int *flags)
{
    if (auth.contains(QLatin1String("802.1X")) || auth.contains(QLatin1String("IEEE 802.1X"))) {
        *flags |= NM_802_11_AP_SEC_KEY_MGMT_802_1X;
    }
    if (auth.contains(QLatin1String("SAE"))) {
        *flags |= NM_802_11_AP_SEC_KEY_MGMT_SAE;
    }
    if (auth.contains(QLatin1String("OWE"))) {
        *flags |= NM_802_11_AP_SEC_KEY_MGMT_OWE;
    }
    if (auth.contains(QLatin1String("PSK"))) {
        *flags |= NM_802_11_AP_SEC_KEY_MGMT_PSK;
    }
}

/// Parses one wpa_cli scan_results line:
///   bssid \t frequency \t signal \t flags \t ssid
AccessPointInfo parseWpaCliLine(const QString &line)
{
    const QStringList parts = line.split(QLatin1Char('\t'));
    if (parts.size() < 4) {
        return {};
    }
    AccessPointInfo ap;
    ap.bssid = parts.at(0).trimmed();
    ap.frequency = parts.at(1).toInt();
    ap.strength = dbmToPercent(parts.at(2).toInt());
    const QString flags = parts.at(3);
    ap.ssid = parts.size() > 4 ? parts.mid(4).join(QLatin1Char('\t')) : QString();
    ap.mode = NM_802_11_MODE_INFRA;

    if (flags.contains(QLatin1String("WPA2")) || flags.contains(QLatin1String("RSN"))) {
        applyCipher(QLatin1String("CCMP"), false, &ap.wpaFlags, &ap.rsnFlags, true);
        applyAuth(flags, &ap.rsnFlags);
        ap.flags |= NM_802_11_AP_FLAGS_PRIVACY;
    }
    if (flags.contains(QLatin1String("WPA-")) || flags.contains(QLatin1String("WPA1"))) {
        applyCipher(QLatin1String("TKIP"), false, &ap.wpaFlags, &ap.rsnFlags, false);
        applyAuth(flags, &ap.wpaFlags);
        ap.flags |= NM_802_11_AP_FLAGS_PRIVACY;
    }
    if (flags.contains(QLatin1String("WEP"))) {
        ap.wpaFlags |= NM_802_11_AP_SEC_PAIR_WEP40 | NM_802_11_AP_SEC_GROUP_WEP40;
        ap.rsnFlags |= NM_802_11_AP_SEC_PAIR_WEP40 | NM_802_11_AP_SEC_GROUP_WEP40;
        ap.flags |= NM_802_11_AP_FLAGS_PRIVACY;
    }
    return ap;
}

/// Parses `iw dev <iface> scan` output.
QList<AccessPointInfo> parseIwScan(const QString &output)
{
    QList<AccessPointInfo> out;
    AccessPointInfo cur;
    bool have = false;
    bool inRsn = false;
    bool inWpa = false;

    const QStringList lines = output.split(QLatin1Char('\n'));
    auto flush = [&]() {
        if (have && !cur.ssid.isEmpty()) {
            out.append(cur);
        }
        cur = AccessPointInfo();
        have = false;
        inRsn = false;
        inWpa = false;
    };

    static const QRegularExpression bssRe(QStringLiteral(R"(^BSS\s+([0-9a-fA-F:]{17}))"));
    static const QRegularExpression freqRe(QStringLiteral(R"(^\s*freq:\s*(\d+))"));
    static const QRegularExpression sigRe(QStringLiteral(R"(^\s*signal:\s*(-?[\d.]+)\s*dBm)"));
    static const QRegularExpression ssidRe(QStringLiteral(R"(^\s*SSID:\s*(.*)$)"));
    static const QRegularExpression capRe(QStringLiteral(R"(^\s*capability:\s*(.*)$)"));
    static const QRegularExpression rateRe(QStringLiteral(R"(^\s*\*\s*([\d.]+)\s*Mbps)"));

    for (const QString &line : lines) {
        const auto bss = bssRe.match(line);
        if (bss.hasMatch()) {
            flush();
            cur.bssid = bss.captured(1);
            cur.mode = NM_802_11_MODE_INFRA;
            have = true;
            continue;
        }
        if (!have) {
            continue;
        }
        if (line.contains(QLatin1String("RSN:"))) {
            inRsn = true;
            inWpa = false;
            continue;
        }
        if (line.contains(QLatin1String("WPA:"))) {
            inWpa = true;
            inRsn = false;
            continue;
        }

        const auto freq = freqRe.match(line);
        if (freq.hasMatch()) {
            cur.frequency = freq.captured(1).toInt();
            continue;
        }
        const auto sig = sigRe.match(line);
        if (sig.hasMatch()) {
            cur.strength = dbmToPercent(static_cast<int>(sig.captured(1).toDouble()));
            continue;
        }
        const auto ssid = ssidRe.match(line);
        if (ssid.hasMatch()) {
            cur.ssid = ssid.captured(1).trimmed();
            continue;
        }
        const auto cap = capRe.match(line);
        if (cap.hasMatch()) {
            const QString c = cap.captured(1);
            if (c.contains(QLatin1String("Privacy"))) {
                cur.flags |= NM_802_11_AP_FLAGS_PRIVACY;
            }
            if (c.contains(QLatin1String("IBSS"))) {
                cur.mode = NM_802_11_MODE_ADHOC;
            }
            continue;
        }
        const auto rate = rateRe.match(line);
        if (rate.hasMatch()) {
            cur.maxBitrate = std::max(cur.maxBitrate, static_cast<int>(rate.captured(1).toDouble() * 1000.0));
            continue;
        }

        if (inRsn || inWpa) {
            const QString t = line.trimmed();
            if (t.startsWith(QLatin1String("* Group cipher:"))) {
                applyCipher(t.mid(15).trimmed(), true, &cur.wpaFlags, &cur.rsnFlags, inRsn);
            } else if (t.startsWith(QLatin1String("* Pairwise ciphers:"))) {
                applyCipher(t.mid(19).trimmed(), false, &cur.wpaFlags, &cur.rsnFlags, inRsn);
            } else if (t.startsWith(QLatin1String("* Authentication suites:"))) {
                applyAuth(t.mid(24).trimmed(), inRsn ? &cur.rsnFlags : &cur.wpaFlags);
            }
        }
    }
    flush();

    // WEP-only access points carry Privacy but no WPA/RSN block.
    for (AccessPointInfo &ap : out) {
        if ((ap.flags & NM_802_11_AP_FLAGS_PRIVACY) && ap.wpaFlags == 0 && ap.rsnFlags == 0) {
            ap.wpaFlags |= NM_802_11_AP_SEC_PAIR_WEP40 | NM_802_11_AP_SEC_GROUP_WEP40;
            ap.rsnFlags |= NM_802_11_AP_SEC_PAIR_WEP40 | NM_802_11_AP_SEC_GROUP_WEP40;
        }
    }
    return out;
}

QString escapeWpaString(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        }
        out.append(c);
    }
    return out;
}
} // namespace

bool WifiScanner::wpaSupplicantAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("wpa_supplicant")).isEmpty();
}

QString WifiScanner::wpaConfigPath(const QString &ifname)
{
    return QStringLiteral("%1/wpa_supplicant-%2.conf").arg(QLatin1String(kSupplicantDir), ifname);
}

QList<AccessPointInfo> WifiScanner::scan(const QString &ifname, QString *error)
{
    if (supplicantRunning(ifname)) {
        // The supplicant owns the radio's scan machinery while it is running, so ask it.
        runTool(QStringLiteral("wpa_cli"), {QStringLiteral("-i") + ifname, QStringLiteral("scan")}, 10000);
        // Results are not ready immediately; the supplicant pushes SCAN-RESULTS shortly after.
        QProcess::execute(QStringLiteral("sleep"), {QStringLiteral("1")});
        const QByteArray raw = runTool(QStringLiteral("wpa_cli"), {QStringLiteral("-i") + ifname, QStringLiteral("scan_results")}, 10000);
        const QString out = QString::fromUtf8(raw);
        QList<AccessPointInfo> aps;
        const QStringList lines = out.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("bssid")) || line.trimmed().isEmpty()) {
                continue;
            }
            const AccessPointInfo ap = parseWpaCliLine(line);
            if (!ap.bssid.isEmpty() && !ap.ssid.isEmpty()) {
                aps.append(ap);
            }
        }
        if (!aps.isEmpty()) {
            return aps;
        }
    }

    const QByteArray raw = runTool(QStringLiteral("iw"), {QStringLiteral("dev"), ifname, QStringLiteral("scan")});
    if (raw.isEmpty()) {
        if (error) {
            *error = QStringLiteral("no scan results for %1 (is the interface up and free?)").arg(ifname);
        }
        return {};
    }
    return parseIwScan(QString::fromUtf8(raw));
}

bool WifiScanner::writeNetwork(const QString &ifname, const QString &ssid, const QString &psk, bool hidden, QString *error)
{
    if (!QDir().mkpath(QLatin1String(kSupplicantDir))) {
        if (error) {
            *error = QStringLiteral("cannot create %1").arg(QLatin1String(kSupplicantDir));
        }
        return false;
    }

    // Keep any other networks the administrator configured, drop a stale entry for the
    // same SSID, and append the new one.
    removeNetwork(ifname, ssid, nullptr);

    const QString path = wpaConfigPath(ifname);
    QString existing;
    {
        QFile in(path);
        if (in.exists() && in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            existing = QString::fromUtf8(in.readAll());
        }
    }

    if (existing.trimmed().isEmpty()) {
        existing = QStringLiteral("ctrl_interface=DIR=%1 GROUP=wheel\nupdate_config=1\n").arg(QLatin1String(kCtrlDir));
    }
    if (!existing.endsWith(QLatin1Char('\n'))) {
        existing.append(QLatin1Char('\n'));
    }

    QString block = QStringLiteral("\nnetwork={\n\tssid=\"%1\"\n").arg(escapeWpaString(ssid));
    if (psk.isEmpty()) {
        block += QStringLiteral("\tkey_mgmt=NONE\n");
    } else if (psk.size() == 64 && QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(psk).hasMatch()) {
        block += QStringLiteral("\tpsk=%1\n").arg(psk);
    } else {
        block += QStringLiteral("\tpsk=\"%1\"\n").arg(escapeWpaString(psk));
    }
    if (hidden) {
        block += QStringLiteral("\tscan_ssid=1\n");
    }
    block += QStringLiteral("}\n");

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
        }
        return false;
    }
    out.write(existing.toUtf8());
    out.write(block.toUtf8());
    if (!out.commit()) {
        if (error) {
            *error = QStringLiteral("cannot commit %1: %2").arg(path, out.errorString());
        }
        return false;
    }

    qCInfo(NETIFRC) << "wrote wpa_supplicant network for" << ssid << "on" << ifname;
    return true;
}

bool WifiScanner::removeNetwork(const QString &ifname, const QString &ssid, QString *error)
{
    const QString path = wpaConfigPath(ifname);
    QFile in(path);
    if (!in.exists()) {
        return true;
    }
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("cannot read %1: %2").arg(path, in.errorString());
        }
        return false;
    }
    const QString content = QString::fromUtf8(in.readAll());
    in.close();

    const QString needle = QStringLiteral("ssid=\"%1\"").arg(escapeWpaString(ssid));
    const QStringList lines = content.split(QLatin1Char('\n'));
    QStringList kept;
    bool skipping = false;
    bool removed = false;

    for (const QString &line : lines) {
        if (!skipping && line.contains(QLatin1String("network={"))) {
            // Look ahead for the ssid line inside this block.
            skipping = false;
        }
        if (line.trimmed().startsWith(QLatin1String("network={"))) {
            kept.append(line);
            skipping = false;
            continue;
        }
        if (line.contains(needle)) {
            // Drop the opening brace already appended plus this block's body.
            while (!kept.isEmpty() && !kept.last().trimmed().startsWith(QLatin1String("network={"))) {
                kept.removeLast();
            }
            if (!kept.isEmpty()) {
                kept.removeLast();
            }
            skipping = true;
            removed = true;
            continue;
        }
        if (skipping) {
            if (line.trimmed() == QLatin1String("}")) {
                skipping = false;
            }
            continue;
        }
        kept.append(line);
    }

    if (!removed) {
        return true;
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
        }
        return false;
    }
    out.write(kept.join(QLatin1Char('\n')).toUtf8());
    if (!out.commit()) {
        if (error) {
            *error = QStringLiteral("cannot commit %1: %2").arg(path, out.errorString());
        }
        return false;
    }
    qCInfo(NETIFRC) << "removed wpa_supplicant network" << ssid << "from" << ifname;
    return true;
}

} // namespace netifrcd
