// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "netifrc/NetifrcConfig.h"

#include "common/Logging.h"

#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

namespace netifrcd
{

namespace
{
constexpr auto kBeginMarker = "# >>> netifrcd managed block >>>";
constexpr auto kEndMarker = "# <<< netifrcd managed block <<<";
constexpr auto kInterfaceListVar = "netifrcd_interfaces";

// netifrc's shell_var(): everything that is not [A-Za-z0-9] becomes '_'.
// We keep '_' as well so that already-mangled names round-trip.
QString shellVar(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in) {
        out.append((c.isLetterOrNumber() && c.unicode() < 128) ? c : QLatin1Char('_'));
    }
    return out;
}

/// Splits a shell word list, honouring single and double quotes.
QStringList splitShellWords(const QString &in)
{
    QStringList out;
    QString cur;
    bool have = false;
    QChar quote;

    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else {
                cur.append(c);
            }
            have = true;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            have = true;
            continue;
        }
        if (c.isSpace()) {
            if (have) {
                out.append(cur);
                cur.clear();
                have = false;
            }
            continue;
        }
        cur.append(c);
        have = true;
    }
    if (have) {
        out.append(cur);
    }
    return out;
}

bool looksLikeAddress(const QString &token)
{
    if (token.isEmpty()) {
        return false;
    }
    const QChar c = token.at(0);
    return c.isDigit() || c == QLatin1Char(':');
}

int netmaskToPrefix(const QString &mask)
{
    bool ok = false;
    const quint32 m = mask.toUInt(&ok);
    if (!ok) {
        return -1;
    }
    int bits = 0;
    for (int i = 31; i >= 0; --i) {
        if (m & (1u << i)) {
            ++bits;
        } else {
            break;
        }
    }
    // Reject non-contiguous masks.
    const quint32 expected = bits == 0 ? 0u : (~0u << (32 - bits));
    return m == expected ? bits : -1;
}

/// Expands netifrc config entries into individual CIDR address strings.
/// Handles both the newline-separated list form and the legacy
/// "10.0.0.5 netmask 255.255.255.0" single-entry form.
QStringList extractAddresses(const QStringList &entries)
{
    QStringList out;
    for (const QString &entry : entries) {
        const QString trimmed = entry.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QStringList tokens = splitShellWords(trimmed);
        if (tokens.isEmpty() || !looksLikeAddress(tokens.first())) {
            continue;
        }

        QString addr = tokens.first();
        // "10.0.0.5 netmask 255.255.255.0"
        const int nmIdx = tokens.indexOf(QStringLiteral("netmask"));
        if (nmIdx >= 0 && nmIdx + 1 < tokens.size() && !addr.contains(QLatin1Char('/'))) {
            const int prefix = netmaskToPrefix(tokens.at(nmIdx + 1));
            if (prefix >= 0) {
                addr += QLatin1Char('/') + QString::number(prefix);
            }
        } else if (tokens.size() > 1 && looksLikeAddress(tokens.at(1)) && !addr.contains(QLatin1Char('/'))) {
            // Space separated list of bare addresses: treat each as its own entry.
            for (const QString &t : tokens) {
                if (looksLikeAddress(t)) {
                    out.append(t);
                }
            }
            continue;
        }
        out.append(addr);
    }
    return out;
}
} // namespace

NetifrcConfig::NetifrcConfig(QString path)
    : m_path(std::move(path))
{
}

QString NetifrcConfig::defaultPath()
{
    // Dev/test escape hatch, mirroring NETIFRCD_ALLOW_NONROOT: point the daemon at a
    // scratch file so the managed-block writer can be exercised without root and without
    // touching the machine's real /etc/conf.d/net.
    const QString override = qEnvironmentVariable("NETIFRCD_NETIFRC_CONF");
    return override.isEmpty() ? QStringLiteral("/etc/conf.d/net") : override;
}

QString NetifrcConfig::ifvar(const QString &ifname)
{
    return shellVar(ifname);
}

bool NetifrcConfig::parseLine(const QString &line, Assignment *out)
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$)"));
    const auto match = re.match(line);
    if (!match.hasMatch()) {
        return false;
    }

    out->name = match.captured(1);
    QString value = match.captured(2).trimmed();
    // Strip a trailing comment that is not inside quotes. netifrc configs rarely use
    // inline comments, but administrators do add them.
    if (!value.startsWith(QLatin1Char('"')) && !value.startsWith(QLatin1Char('\''))) {
        const int hash = value.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            value = value.left(hash).trimmed();
        }
    }

    if (value.startsWith(QLatin1Char('(')) && value.endsWith(QLatin1Char(')'))) {
        // Bash array form, still accepted by netifrc with a deprecation warning.
        out->values = splitShellWords(value.mid(1, value.size() - 2));
        return true;
    }

    if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\'')))) {
        const QChar q = value.at(0);
        if (value.endsWith(q)) {
            value = value.mid(1, value.size() - 2);
        }
    }

    // netifrc's _get_array() trims each line and drops blanks, so mirror that.
    const QStringList lines = value.split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty()) {
            out->values.append(t);
        }
    }
    if (out->values.isEmpty() && !value.trimmed().isEmpty()) {
        out->values.append(value.trimmed());
    }
    return true;
}

QString NetifrcConfig::quoteShell(const QString &raw)
{
    QString escaped = raw;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    escaped.replace(QLatin1Char('$'), QLatin1String("\\$"));
    escaped.replace(QLatin1Char('`'), QLatin1String("\\`"));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString NetifrcConfig::renderValue(const QStringList &values)
{
    // Canonical netifrc list form: newline separated, with a leading and trailing
    // newline. A single scalar is emitted on one line for readability.
    if (values.size() == 1) {
        return quoteShell(values.first());
    }
    return quoteShell(QStringLiteral("\n") + values.join(QLatin1Char('\n')) + QStringLiteral("\n"));
}

bool NetifrcConfig::load(QString *error)
{
    m_preamble.clear();
    m_managed.clear();
    m_managedOrder.clear();
    m_effective.clear();
    m_dirty = false;

    QFile file(m_path);
    if (!file.exists()) {
        qCInfo(NETIFRC) << "netifrc config" << m_path << "does not exist; starting from an empty configuration";
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("cannot read %1: %2").arg(m_path, file.errorString());
        }
        return false;
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    bool inManaged = false;
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed == QLatin1String(kBeginMarker)) {
            inManaged = true;
            continue;
        }
        if (trimmed == QLatin1String(kEndMarker)) {
            inManaged = false;
            continue;
        }

        Assignment a;
        if (parseLine(line, &a)) {
            m_effective.insert(a.name, a.values);
            if (inManaged) {
                if (!m_managed.contains(a.name)) {
                    m_managedOrder.append(a.name);
                }
                m_managed.insert(a.name, a.values);
            }
        }

        if (!inManaged) {
            m_preamble.append(line);
        }
    }

    // Drop trailing blank lines left behind where the managed block used to be.
    while (!m_preamble.isEmpty() && m_preamble.last().trimmed().isEmpty()) {
        m_preamble.removeLast();
    }

    qCInfo(NETIFRC) << "loaded" << m_path << "with" << m_managedOrder.size() << "managed variable(s)";
    return true;
}

bool NetifrcConfig::save(QString *error)
{
    QStringList out = m_preamble;
    while (!out.isEmpty() && out.last().trimmed().isEmpty()) {
        out.removeLast();
    }

    if (!m_managedOrder.isEmpty()) {
        out.append(QString());
        out.append(QLatin1String(kBeginMarker));
        out.append(QStringLiteral("# Generated by netifrcd from its connection profiles."));
        out.append(QStringLiteral("# Edits inside this block are overwritten. Configure via the Plasma applet"));
        out.append(QStringLiteral("# or remove the block to manage these interfaces by hand again."));
        for (const QString &name : m_managedOrder) {
            if (!m_managed.contains(name)) {
                continue;
            }
            out.append(QStringLiteral("%1=%2").arg(name, renderValue(m_managed.value(name))));
        }
        out.append(QLatin1String(kEndMarker));
    }
    out.append(QString());

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("cannot write %1: %2").arg(m_path, file.errorString());
        }
        return false;
    }
    QTextStream ts(&file);
    ts << out.join(QLatin1Char('\n'));
    ts.flush();
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("cannot commit %1: %2").arg(m_path, file.errorString());
        }
        return false;
    }

    m_dirty = false;
    qCInfo(NETIFRC) << "wrote" << m_path;
    return true;
}

QString NetifrcConfig::variable(const QString &name) const
{
    const QStringList v = m_effective.value(name);
    return v.join(QLatin1Char('\n'));
}

NetifrcConfig::InterfaceConfig NetifrcConfig::configFor(const QString &ifname) const
{
    InterfaceConfig cfg;
    const QString iv = ifvar(ifname);

    const QString configVar = QStringLiteral("config_%1").arg(iv);
    if (!m_effective.contains(configVar)) {
        return cfg; // not configured
    }
    cfg.configured = true;

    const QStringList raw = m_effective.value(configVar);
    const QString first = raw.isEmpty() ? QString() : raw.first().trimmed();

    if (first == QLatin1String("dhcp") || first.startsWith(QLatin1String("dhcp"))) {
        cfg.method = Method::Dhcp;
    } else if (first == QLatin1String("noop") || first == QLatin1String("null")) {
        cfg.method = Method::Unknown;
    } else if (first == QLatin1String("lo") || ifname == QLatin1String("lo")) {
        cfg.method = Method::Loopback;
    } else if (looksLikeAddress(first)) {
        cfg.method = Method::Static;
        cfg.addresses = extractAddresses(raw);
    } else {
        cfg.method = Method::Unknown;
    }

    cfg.routes = m_effective.value(QStringLiteral("routes_%1").arg(iv));
    cfg.modules = m_effective.value(QStringLiteral("modules_%1").arg(iv));
    cfg.essid = variable(QStringLiteral("essid_%1").arg(iv));
    cfg.psk = variable(QStringLiteral("key_%1").arg(iv));
    return cfg;
}

void NetifrcConfig::setInterface(const QString &ifname, const InterfaceConfig &cfg)
{
    const QString iv = ifvar(ifname);

    auto put = [this](const QString &name, const QStringList &values) {
        if (!m_managed.contains(name)) {
            m_managedOrder.append(name);
        }
        m_managed.insert(name, values);
        m_effective.insert(name, values);
    };
    auto drop = [this](const QString &name) {
        m_managed.remove(name);
        m_managedOrder.removeAll(name);
        m_effective.remove(name);
    };

    switch (cfg.method) {
    case Method::Dhcp:
        put(QStringLiteral("config_%1").arg(iv), {QStringLiteral("dhcp")});
        break;
    case Method::Static:
        put(QStringLiteral("config_%1").arg(iv), cfg.addresses);
        break;
    case Method::Loopback:
        put(QStringLiteral("config_%1").arg(iv), {QStringLiteral("127.0.0.1/8")});
        break;
    case Method::None:
    case Method::Unknown:
        break;
    }

    if (cfg.routes.isEmpty()) {
        drop(QStringLiteral("routes_%1").arg(iv));
    } else {
        put(QStringLiteral("routes_%1").arg(iv), cfg.routes);
    }

    if (cfg.modules.isEmpty()) {
        drop(QStringLiteral("modules_%1").arg(iv));
    } else {
        put(QStringLiteral("modules_%1").arg(iv), cfg.modules);
    }

    if (!cfg.essid.isEmpty()) {
        put(QStringLiteral("essid_%1").arg(iv), {cfg.essid});
    }
    if (!cfg.psk.isEmpty()) {
        put(QStringLiteral("key_%1").arg(iv), {cfg.psk});
    }

    // Track the interface list so we can map mangled ifvars back to real names.
    QStringList interfaces = m_managed.value(QLatin1String(kInterfaceListVar));
    if (!interfaces.contains(ifname)) {
        interfaces.append(ifname);
        put(QLatin1String(kInterfaceListVar), interfaces);
    }

    m_dirty = true;
}

void NetifrcConfig::removeInterface(const QString &ifname)
{
    const QString iv = ifvar(ifname);
    static const QStringList suffixes = {
        QStringLiteral("config_%1"),
        QStringLiteral("routes_%1"),
        QStringLiteral("modules_%1"),
        QStringLiteral("essid_%1"),
        QStringLiteral("key_%1"),
        QStringLiteral("wpa_supplicant_%1"),
    };
    for (const QString &suffix : suffixes) {
        const QString name = suffix.arg(iv);
        m_managed.remove(name);
        m_managedOrder.removeAll(name);
        m_effective.remove(name);
    }

    // When the last interface is released the list variable itself has to go, otherwise
    // save() still emits a block holding an empty netifrcd_interfaces="" - the managed
    // block would never actually disappear, so the interface would look handed back
    // while the file still claimed it.
    QStringList interfaces = m_managed.value(QLatin1String(kInterfaceListVar));
    if (interfaces.removeAll(ifname) > 0) {
        if (interfaces.isEmpty()) {
            m_managed.remove(QLatin1String(kInterfaceListVar));
            m_managedOrder.removeAll(QLatin1String(kInterfaceListVar));
            m_effective.remove(QLatin1String(kInterfaceListVar));
        } else {
            m_managed.insert(QLatin1String(kInterfaceListVar), interfaces);
        }
    }
    m_dirty = true;
}

QStringList NetifrcConfig::managedInterfaces() const
{
    return m_managed.value(QLatin1String(kInterfaceListVar));
}

QStringList NetifrcConfig::knownInterfaces() const
{
    static const QRegularExpression re(QStringLiteral(R"(^config_(.+)$)"));
    QStringList out;
    for (auto it = m_effective.constBegin(); it != m_effective.constEnd(); ++it) {
        const auto m = re.match(it.key());
        if (m.hasMatch()) {
            out.append(m.captured(1));
        }
    }
    out.removeDuplicates();
    return out;
}

void NetifrcConfig::setDhcp(const QString &ifname)
{
    InterfaceConfig cfg;
    cfg.configured = true;
    cfg.method = Method::Dhcp;
    setInterface(ifname, cfg);
}

void NetifrcConfig::setStatic(const QString &ifname, const QStringList &addresses, const QStringList &routes)
{
    InterfaceConfig cfg;
    cfg.configured = true;
    cfg.method = Method::Static;
    cfg.addresses = addresses;
    cfg.routes = routes;
    setInterface(ifname, cfg);
}

} // namespace netifrcd
