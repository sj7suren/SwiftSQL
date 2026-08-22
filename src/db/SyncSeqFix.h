// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncSeqFix.h — identity/sequence repair postamble for the table-data sync
// suite (ADR-015 T5).
//
// The bug this exists to prevent: data sync copies identity/auto-increment
// values LITERALLY (it must — foreign keys point at them). But copying the
// values does not advance the target's generator. A target whose sequence still
// sits at 1 while rows 1..10000 now exist will hand out 1 on its very next
// INSERT and fail with a duplicate key — long after the sync "succeeded", in
// production, to someone who has no idea a sync ever happened. So every table
// carrying an identity column needs a generator-advance statement appended after
// its data.
//
// Free functions in their own translation unit, deliberately: MySqlDriver.cpp is
// at exactly 1000/1000 lines (project charter hard limit), so nothing may be
// added there. This is also the honest home for the logic anyway — it is
// cross-dialect and depends on no connection state.
//
// Pure: no IConnection, no vendor header.
#pragma once

#include "db/SchemaModel.h"
#include <wx/string.h>
#include <vector>

namespace db { enum class Dialect; }   // opaque; see SyncValueMap.h

namespace db::sync {

// One generator-advance statement for one identity column.
struct SeqFix {
    wxString column;      // the identity/auto-increment column it repairs
    wxString statement;   // ready to execute, no trailing semicolon
    // True when `statement` is self-contained (computes the max itself on the
    // server). False means it was baked from the caller-supplied maxValue and is
    // only valid if no one else wrote to the table in between.
    bool     selfComputing = false;
};

// Build the postamble for one table.
//
// `qualifiedTable` must already be quoted/qualified for the target dialect (the
// same string the DML uses). `maxValue` is the highest identity value the sync
// actually wrote; pass -1 when unknown. Dialects that can compute the max
// server-side (PostgreSQL) ignore it; dialects that need a literal (MySQL, SQL
// Server) fail with `why` set when it is -1 rather than guessing a value.
//
// Returns false when no fix could be produced; `why` then explains. Note the two
// very different false cases, distinguished by `out`:
//   * out empty + why empty  -> nothing to do (no identity column). Not an error.
//   * out empty + why filled -> a fix IS needed but could not be built. The
//                               caller must surface this, because silently
//                               skipping it recreates exactly the latent
//                               duplicate-key bug this file exists to prevent.
bool BuildSequenceFixes(const TableSchema& schema, Dialect tgtDialect,
                        const wxString& qualifiedTable, long long maxValue,
                        std::vector<SeqFix>& out, wxString& why);

} // namespace db::sync
