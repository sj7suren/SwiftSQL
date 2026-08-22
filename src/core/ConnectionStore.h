// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionStore.h — persists saved connection profiles to an ini file under
// the user's data dir. Passwords are encrypted via core::Secret before being
// written. Profiles are keyed by their (unique) name.
#pragma once

#include <vector>
#include "core/ConnectionProfile.h"

namespace core {

class ConnectionStore {
public:
    // Load all saved profiles (passwords decrypted). Returns empty on first run.
    static std::vector<ConnectionProfile> LoadAll();

    // Insert or update a profile by name, then persist. Empty name → no-op.
    static void Save(const ConnectionProfile& p);

    // Remove a profile by name, then persist.
    static void Remove(const wxString& name);

    // Absolute path of the backing file (for diagnostics / verification).
    static wxString FilePath();
};

} // namespace core
