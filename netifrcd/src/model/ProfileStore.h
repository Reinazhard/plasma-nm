// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
//
// Persistent store for connection profiles.
//
// The list index is significant: it is the index used to build the
// Settings.Connection D-Bus object path (/org/freedesktop/NetworkManager/Settings/N),
// so it must stay stable for the lifetime of the process.

#pragma once

#include "model/Profile.h"

#include <QList>
#include <QObject>
#include <QString>

namespace netifrcd
{

class ProfileStore : public QObject
{
    Q_OBJECT

public:
    explicit ProfileStore(QObject *parent = nullptr);

    /// /var/lib/netifrcd/profiles.json
    static QString defaultPath();

    /// Loads the store from disk. A missing file is not an error: the store is
    /// simply left empty. Returns false only when an existing file cannot be read
    /// or parsed.
    bool load(QString *error = nullptr);

    /// Writes the store atomically via QSaveFile, creating the parent directory
    /// if needed.
    bool save(QString *error = nullptr);

    QList<Profile> profiles() const;
    Profile profileByUuid(const QString &uuid) const;
    bool contains(const QString &uuid) const;

    /// Inserts a new profile or replaces the existing one with the same uuid.
    /// Marks the store dirty and emits changed() when the list actually changed.
    void addOrUpdate(const Profile &p);

    /// Removes the profile with the given uuid. Emits changed() when one was removed.
    void remove(const QString &uuid);

    int indexForUuid(const QString &uuid) const;
    QString uuidForIndex(int index) const;

    bool isDirty() const;

Q_SIGNALS:
    void changed();

private:
    QString m_path;
    QList<Profile> m_profiles; // the list index IS the Settings.Connection path index
    bool m_dirty = false;
};

} // namespace netifrcd
