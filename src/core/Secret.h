// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// Secret.h - encrypt/decrypt sensitive strings for at-rest storage.
// On Windows new secrets are AES-GCM encrypted; the AES master key is protected
// by DPAPI for the current user. Legacy DPAPI-only tokens still decrypt.
// Other platforms fall back to base64 until a platform keychain is wired up.
#pragma once

#include <wx/string.h>

namespace core {

// Returns an opaque, storable token (base64 text). Empty input → empty output.
wxString EncryptSecret(const wxString& plain);

// Inverse of EncryptSecret. Returns empty string on any failure.
wxString DecryptSecret(const wxString& token);

// True when the platform provides real OS-backed encryption (Windows DPAPI).
bool SecretIsSecure();

} // namespace core
