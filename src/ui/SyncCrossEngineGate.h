// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCrossEngineGate.h -- the wizard-entry cross-engine policy gate, factored
// out of SyncWizardDialog.cpp (where it used to be an anonymous-namespace
// free function) into its own pure, non-wx translation unit so it can be
// unit-tested directly (tests/SyncCrossEngineGateTests.cpp) without linking
// the rest of the wizard dialog's wx GUI dependency chain (wx/wx.h,
// ConnectionTree, SyncDiffPanel, SyncRunnerDialog, Theme, UiFonts, ...).
//
// QA-scope exception (deliberate, small, documented): this is the ONE
// src/ change made while adding regression tests for the cross-engine sync
// feature. Semantics are unchanged byte-for-byte from the original
// anonymous-namespace definition -- only its location/visibility moved.
// SyncWizardDialog.cpp now calls this instead of defining its own copy.
#pragma once

#include "db/DbDriver.h"

namespace ui {

// Phase 1 wizard-level cross-engine gate. Deliberately narrower than what
// SyncEngine itself will attempt: the engine gates column-level Add/Modify by
// per-column type mappability (SyncTypeMap/DialectProfile), dialect-pair
// agnostic -- it doesn't know or care which pairing it's asked to compare.
// This helper is the UI's own, stricter policy on top of that: MVP only
// exposes the wizard for the one pairing that's actually been exercised
// end-to-end (MySQL <-> Postgres). Every other cross-engine pairing (anything
// touching Sqlite/SqlServer/Oracle while dialects differ) stays blocked here
// until a later phase validates it -- lives here, not in SyncEngine.h/.cpp,
// because it's a wizard-entry policy choice, not an engine invariant.
bool CrossEngineSupported(db::Dialect a, db::Dialect b);

} // namespace ui
