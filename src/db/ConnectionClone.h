// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionClone.h — the single chokepoint for duplicating a live IConnection.
//
// db::IConnection is NOT thread-safe (see docs/devlog history on the
// AiKnowledgeBase heap-corruption incident: two threads sharing one driver
// connection concurrently corrupt its internal buffers). Any worker thread that
// needs to run its own queries alongside a connection the UI thread still owns
// must get its OWN independent IConnection — never the UI's.
//
// CloneConnection() is that: given a live connection, it opens a brand-new one
// of the same engine, dialed at the SAME actual endpoint (via EffectiveProfile()
// — see db/DbDriver.h — so an SSH-tunneled source clones correctly instead of
// silently trying to reconnect to a raw host that was never reachable directly).
//
// Deliberately NOT a connection pool — no caching, no reuse, no lifecycle beyond
// "one call in, one connection out, caller owns it." A pool is out of scope for
// this phase; if a future caller needs many short-lived clones, that's a
// decision for whoever adds pooling on top of this chokepoint, not baked in here.
#pragma once

#include <memory>
#include <wx/string.h>
#include "db/DbDriver.h"

namespace db {

// Create a new, independent IConnection of the same engine as `src`, connected
// via src.EffectiveProfile() (never src's raw saved profile — see EffectiveProfile()
// doc in DbDriver.h). Returns nullptr and fills `err` on failure (unsupported
// connection type, or the connect itself failing). The caller owns the returned
// connection exclusively.
std::unique_ptr<IConnection> CloneConnection(const IConnection& src, wxString& err);

} // namespace db
