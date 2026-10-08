// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Read/write access to netifrc's configuration file, /etc/conf.d/net.
//
// netifrc is a set of POSIX shell functions sourced by /etc/init.d/net.<iface>. Its
// configuration is a plain shell file of assignments such as:
//
//     config_eth0="dhcp"
//     config_wlan0="
//     10.0.0.5/24
//     "
//     routes_eth0="
//     default via 10.0.0.1
//     "
//
// IMPORTANT: netifrc's own _get_array() helper processes the value line-by-line, so
// list-valued variables are canonically newline separated. We always render that form.
//
// We never rewrite the user's file wholesale. netifrcd owns a single delimited block
// and rewrites only that, leaving everything the administrator wrote untouched.

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace netifrcd
{

class NetifrcConfig
{
public:
    enum class Method {
        None, // interface not mentioned in the config at all
        Dhcp,
        Static,
        Loopback,
        Unknown, // mentioned, but we do not recognise the handler
    };

    struct InterfaceConfig {
        bool configured = false;
        Method method = Method::None;
        QStringList addresses; // CIDR strings, e.g. "10.0.0.5/24"
        QStringList routes;    // netifrc route lines
        QStringList modules;   // modules_<ifvar>
        QString essid;         // legacy wireless
        QString psk;           // legacy wireless
    };

    explicit NetifrcConfig(QString path = defaultPath());

    static QString defaultPath();
    static QString ifvar(const QString &ifname);

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr);

    InterfaceConfig configFor(const QString &ifname) const;
    void setInterface(const QString &ifname, const InterfaceConfig &cfg);
    void removeInterface(const QString &ifname);

    /// Interfaces that appear in the file, whether in our managed block or not.
    QStringList knownInterfaces() const;

    /// Interfaces netifrcd itself is managing (i.e. present in the managed block).
    QStringList managedInterfaces() const;

    bool isDirty() const
    {
        return m_dirty;
    }

    /// Raw value of a variable, from the effective (user + managed) view.
    QString variable(const QString &name) const;

    // Convenience renderers.
    void setDhcp(const QString &ifname);
    void setStatic(const QString &ifname, const QStringList &addresses, const QStringList &routes = {});

private:
    struct Assignment {
        QString name;
        QStringList values;
    };

    static bool parseLine(const QString &line, Assignment *out);
    static QString renderValue(const QStringList &values);
    static QString quoteShell(const QString &raw);

    QString m_path;
    QStringList m_preamble;             // everything outside the managed block
    QMap<QString, QStringList> m_managed; // variables netifrcd owns
    QStringList m_managedOrder;         // insertion order for stable output
    QMap<QString, QStringList> m_effective; // parsed view of the whole file
    bool m_dirty = false;
};

} // namespace netifrcd
