// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Shared D-Bus constants and path builders.
//
// All enum values and interface names come from libnm's nm-dbus-interface.h, which is
// the authoritative, glib-free description of the NetworkManager D-Bus API. Using it
// means our wire protocol can never silently drift from upstream.

#pragma once

#include <QDBusObjectPath>
#include <QString>
#include <QVariantMap>

// Pulls in NM_DBUS_SERVICE, NM_DBUS_INTERFACE*, NM_DBUS_PATH*, NMState, NMDeviceState,
// NMDeviceType, NMConnectivityState, NM80211*, NMActiveConnectionState, ... and the
// NM_CLIENT_PERMISSION_* values.
#include <nm-dbus-interface.h>

namespace netifrcd
{

// ---------------------------------------------------------------------------
// D-Bus interface names (aliases so call sites read well)
// ---------------------------------------------------------------------------

inline constexpr auto Service = NM_DBUS_SERVICE;
inline constexpr auto IfaceManager = NM_DBUS_INTERFACE;
inline constexpr auto IfaceDevice = NM_DBUS_INTERFACE_DEVICE;
inline constexpr auto IfaceDeviceWired = NM_DBUS_INTERFACE_DEVICE_WIRED;
inline constexpr auto IfaceDeviceWireless = NM_DBUS_INTERFACE_DEVICE_WIRELESS;
inline constexpr auto IfaceDeviceStatistics = NM_DBUS_INTERFACE_DEVICE_STATISTICS;
inline constexpr auto IfaceAccessPoint = NM_DBUS_INTERFACE_ACCESS_POINT;
inline constexpr auto IfaceActiveConnection = NM_DBUS_INTERFACE_ACTIVE_CONNECTION;
inline constexpr auto IfaceSettings = NM_DBUS_INTERFACE_SETTINGS;
inline constexpr auto IfaceSettingsConnection = NM_DBUS_INTERFACE_SETTINGS_CONNECTION;
inline constexpr auto IfaceIp4Config = NM_DBUS_INTERFACE_IP4_CONFIG;
inline constexpr auto IfaceDhcp4Config = NM_DBUS_INTERFACE_DHCP4_CONFIG;
inline constexpr auto IfaceAgentManager = NM_DBUS_INTERFACE_AGENT_MANAGER;
inline constexpr auto IfaceSecretAgent = NM_DBUS_INTERFACE_SECRET_AGENT;
inline constexpr auto IfaceProperties = "org.freedesktop.DBus.Properties";
inline constexpr auto IfaceDbusIntrospectable = "org.freedesktop.DBus.Introspectable";

// ---------------------------------------------------------------------------
// Object paths
// ---------------------------------------------------------------------------

inline constexpr auto PathManager = NM_DBUS_PATH; // /org/freedesktop/NetworkManager
inline constexpr auto PathSettings = NM_DBUS_PATH_SETTINGS;
inline constexpr auto PathAgentManager = NM_DBUS_PATH_AGENT_MANAGER;

inline QString devicePath(int id)
{
    return QStringLiteral("%1/Devices/%2").arg(QLatin1String(PathManager)).arg(id);
}

inline QString accessPointPath(int id)
{
    return QStringLiteral("%1/AccessPoint/%2").arg(QLatin1String(PathManager)).arg(id);
}

inline QString activeConnectionPath(int id)
{
    return QStringLiteral("%1/ActiveConnection/%2").arg(QLatin1String(PathManager)).arg(id);
}

inline QString settingsConnectionPath(int id)
{
    return QStringLiteral("%1/%2").arg(QLatin1String(PathSettings)).arg(id);
}

inline QString ip4ConfigPath(int id)
{
    return QStringLiteral("%1/IP4Config/%2").arg(QLatin1String(PathManager)).arg(id);
}

inline QString dhcp4ConfigPath(int id)
{
    return QStringLiteral("%1/DHCP4Config/%2").arg(QLatin1String(PathManager)).arg(id);
}

inline QDBusObjectPath objPath(const QString &p)
{
    return QDBusObjectPath(p);
}

/// NetworkManager uses the root path "/" to mean "no object" in properties such as
/// Device.ActiveConnection or Device.Wireless.ActiveAccessPoint. An empty object path
/// is NOT a valid D-Bus value and makes Qt drop the whole reply with a marshalling
/// error, so every path-typed value must go through here.
inline QDBusObjectPath nonePath()
{
    return QDBusObjectPath(QStringLiteral("/"));
}

inline QDBusObjectPath toPath(const QString &p)
{
    return p.isEmpty() ? nonePath() : QDBusObjectPath(p);
}

// ---------------------------------------------------------------------------
// Settings-map helpers
//
// NetworkManager represents a connection profile as an "a{sa{sv}}" map:
// { "connection": {...}, "802-11-wireless": {...}, "ipv4": {...}, ... }
// The same shape is used on the wire for Settings.Connection.GetSettings and for
// the arguments of ActivateConnection / AddAndActivateConnection. These helpers keep
// the construction sites readable.
// ---------------------------------------------------------------------------

using SettingsMap = QMap<QString, QVariantMap>;

// NetworkManager's own type strings for the "connection"."type" key.
inline constexpr auto TypeWired = "802-3-ethernet";
inline constexpr auto TypeWireless = "802-11-wireless";
inline constexpr auto TypeLoopback = "loopback";

// Maps a netifrc interface name to the NetworkManager connection type string.
// We only manage wired and wireless interfaces; anything else is exposed as generic
// and left unmanaged.
inline QString typeForInterface(const QString &ifname)
{
    if (ifname.startsWith(QLatin1String("wlan")) || ifname.startsWith(QLatin1String("wlp"))
        || ifname.startsWith(QLatin1String("wl"))) {
        return QLatin1String(TypeWireless);
    }
    return QLatin1String(TypeWired);
}

} // namespace netifrcd
