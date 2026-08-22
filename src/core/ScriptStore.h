// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ScriptStore.h — the persisted "saved SQL script library". Each script is a
// plain .sql file under %APPDATA%/SwiftSQL/scripts/, with owner/exec-user
// metadata kept in a sibling scripts_index.ini (the file itself is the source of
// truth for size/created/modified — read from the filesystem, never duplicated).
//
// Storage layout:
//   <UserDataDir>/scripts/<sanitized-name>.sql     — the script body (UTF-8)
//   <UserDataDir>/scripts/scripts_index.ini         — per-script owner/exec + display name
//
// The index is keyed by the sanitized filename stem. List() walks the *.sql files
// on disk (not the index), so a hand-deleted .sql simply drops out and a
// hand-dropped .sql still shows (with blank owner/exec), keeping the two in sync
// tolerantly.
#pragma once

#include <wx/string.h>
#include <wx/datetime.h>
#include <vector>

namespace core {

struct ScriptMeta {
    wxString   name;                 // display name (as the user typed it)
    wxString   stem;                 // sanitized filename stem (index key)
    wxString   path;                 // absolute .sql path
    long long  sizeBytes = -1;       // from the filesystem (-1 = unknown)
    wxDateTime created;              // filesystem creation time (invalid if unknown)
    wxDateTime modified;             // filesystem modification time
    wxString   owner;                // connection user at save time ("" = none)
    wxString   exec;                 // connection user (execution) ("" = none)
};

class ScriptStore {
public:
    // Absolute scripts directory, created (recursively) if it does not exist.
    static wxString Dir();

    // Write `sql` to <name>.sql (overwriting an existing same-name script) and
    // record owner/exec in the index. Returns false + `err` on I/O failure.
    static bool Save(const wxString& name, const wxString& sql,
                     const wxString& owner, const wxString& exec, wxString& err);

    // Every script currently on disk, sorted by name (case-insensitive). Missing
    // files are skipped; files without an index entry appear with blank owner/exec.
    static std::vector<ScriptMeta> List();

    // Read a script body by display name (or sanitized stem). false + `err` if
    // the file is missing / unreadable.
    static bool Load(const wxString& name, wxString& sql, wxString& err);

    // Delete a script's .sql file and its index entry. Returns true if the file
    // was removed (or was already gone).
    static bool Remove(const wxString& name);

    // Does a script with this (display) name already exist on disk?
    static bool Exists(const wxString& name);

    // Filename-safe form of a display name: illegal path characters → '_',
    // trimmed; empty input yields "script".
    static wxString Sanitize(const wxString& name);
};

} // namespace core
