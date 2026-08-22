// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiKnowledgeStore.h — encrypted, on-disk persistence for a built AiKnowledge.
//
// Building a knowledge base is expensive (per-table introspection + an LLM
// "verify relationships" pass that costs latency and tokens). Rebuilding it every
// time the AI panel opens or the database is switched is wasteful, so once a KB is
// successfully built we serialise it, encrypt the blob with core::EncryptSecret
// (the same DPAPI-backed AES-GCM used for API keys / connection passwords) and
// write it under <UserDataDir>/kb/<hash>.dat. On the next open we decrypt + reload
// it in a blink — no connection, no worker, no network — and only re-run the full
// build when the user explicitly asks to "更新" (forceRebuild).
//
// The <key> is an opaque per-(connection, database) identity handed in by the
// caller (e.g. "连接label\x1f库名"); we hash it into a filesystem-safe name so the
// caller never has to worry about illegal path characters.
#pragma once

#include <wx/string.h>

#include "ui/AiKnowledgeBase.h"   // AiKnowledge

namespace ui {

class AiKnowledgeStore {
public:
    // Serialise + encrypt + write. Returns false on any failure (encryption
    // unavailable, disk error). A non-ready KB is still storable, but callers
    // should only persist a ready one.
    static bool Save(const wxString& key, const AiKnowledge& kb);

    // Read + decrypt + deserialise into `out` (with ready restored). Returns false
    // when the file is absent, decryption fails, or the payload can't be parsed —
    // in which case `out` is left untouched and the caller should rebuild.
    static bool Load(const wxString& key, AiKnowledge& out);
};

} // namespace ui
