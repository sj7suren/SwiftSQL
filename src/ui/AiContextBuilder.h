// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiContextBuilder.h — assembles the COMPACT schema signature the AI dialog hands
// to ai::PromptBuilder (ai::AiContext::schemaText). One line per table:
//
//   表名(列 类型 [PK], 列 类型, 外键列 →目标表.列, ...)
//
// This is a deliberately small footprint (never a full CREATE): it fits inside a
// prompt cheaply and never leaks row data. When a database has more tables than the
// budget allows (>200) it degrades to a bare table-name list plus a truncation note,
// so a huge schema can never blow the context window. Pure logic (blocking DB calls
// only), no wx GUI — the caller runs it off the main thread.
#pragma once

#include <wx/string.h>

namespace db { class IConnection; }

namespace ui {

// Build the compact signature for every base table in `database`. Returns the text
// (empty on failure, with `err` filled). Blocking: issues ListTables + per-table
// GetColumns/GetForeignKeys, so call from a worker thread for large schemas.
wxString BuildSchemaSignature(db::IConnection* conn, const wxString& database,
                              wxString& err);

} // namespace ui
