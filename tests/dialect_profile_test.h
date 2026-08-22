// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// dialect_profile_test.h — shared harness + render helpers for the golden-DDL
// DialectProfile unit tests (see dialect_profile_test.cpp for the suite rationale).
// This header carries the assert loop (g_checks/g_fails), the U8/Expect* helpers,
// and the Render*/construction convenience helpers, so both translation units of
// the test — dialect_profile_test.cpp (column-level tests) and
// dialect_profile_test_ext.cpp (table-level tests) — share ONE harness instance.
// g_checks/g_fails and every helper are C++17 `inline` so they resolve to a single
// definition across the two TUs.
#pragma once

#include "db/DialectProfile.h"

#include <wx/string.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace db;

inline int g_checks = 0;
inline int g_fails  = 0;

inline std::string U8(const wxString& s)
{
    const wxScopedCharBuffer b = s.utf8_str();
    return std::string(b.data(), b.length());
}

inline void ExpectEq(const char* name, const wxString& actual, const wxString& expected)
{
    ++g_checks;
    if (actual == expected) {
        std::printf("  ok   %s\n", name);
    } else {
        ++g_fails;
        std::printf("  FAIL %s\n    expected: %s\n    actual:   %s\n",
                    name, U8(expected).c_str(), U8(actual).c_str());
    }
}

inline void ExpectCount(const char* name, size_t actual, size_t expected)
{
    ++g_checks;
    if (actual == expected) {
        std::printf("  ok   %s (n=%zu)\n", name, actual);
    } else {
        ++g_fails;
        std::printf("  FAIL %s: expected n=%zu, actual n=%zu\n", name, expected, actual);
    }
}

inline void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (cond) std::printf("  ok   %s\n", name);
    else    { ++g_fails; std::printf("  FAIL %s\n", name); }
}

// Render one ColumnEdit against a dialect and return the emitted statements.
inline std::vector<wxString> RenderOne(DbType type, const wxString& db,
                                       const wxString& table, const ColumnEdit& ce)
{
    TableEdit e;
    e.db = db;
    e.table = table;
    e.columns.push_back(ce);
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderAlter(e, stmts, err);
    return stmts;
}

// Convenience: an Add of a column model.
inline ColumnEdit Add(const ColumnModel& m)
{ ColumnEdit e; e.op = ColumnEdit::Add;    e.model = m; return e; }
inline ColumnEdit Modify(const ColumnModel& m)
{ ColumnEdit e; e.op = ColumnEdit::Modify; e.model = m; return e; }
inline ColumnEdit Drop(const wxString& name)
{ ColumnEdit e; e.op = ColumnEdit::Drop;   e.model.name = name; return e; }

// Render one IndexEdit against a dialect and return the emitted statements.
inline std::vector<wxString> RenderIdx(DbType type, const wxString& db,
                                       const wxString& table, const IndexEdit& ie)
{
    TableEdit e;
    e.db = db;
    e.table = table;
    e.indexes.push_back(ie);
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderAlter(e, stmts, err);
    return stmts;
}

inline IndexEdit IdxAdd(const IndexModel& m)
{ IndexEdit e; e.op = IndexEdit::Add;  e.model = m; return e; }
inline IndexEdit IdxDrop(const wxString& name)
{ IndexEdit e; e.op = IndexEdit::Drop; e.model.name = name; return e; }

// Render one ForeignKeyEdit against a dialect and return the emitted statements.
inline std::vector<wxString> RenderFk(DbType type, const wxString& db,
                                      const wxString& table, const ForeignKeyEdit& fe)
{
    TableEdit e;
    e.db = db;
    e.table = table;
    e.fks.push_back(fe);
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderAlter(e, stmts, err);
    return stmts;
}

inline ForeignKeyEdit FkAdd(const ForeignKeyModel& m)
{ ForeignKeyEdit e; e.op = ForeignKeyEdit::Add;  e.model = m; return e; }
inline ForeignKeyEdit FkDrop(const wxString& name)
{ ForeignKeyEdit e; e.op = ForeignKeyEdit::Drop; e.model.name = name; return e; }

// Render a fully-built TableEdit (for options/comment scenarios).
inline std::vector<wxString> RenderEdit(DbType type, const TableEdit& e)
{
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderAlter(e, stmts, err);
    return stmts;
}

// Render one TriggerEdit against a dialect and return the emitted statements.
inline std::vector<wxString> RenderTrig(DbType type, const wxString& db,
                                        const wxString& table, const TriggerEdit& te)
{
    TableEdit e;
    e.db = db;
    e.table = table;
    e.triggers.push_back(te);
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderAlter(e, stmts, err);
    return stmts;
}

inline TriggerEdit TrigAdd(const TriggerModel& m)
{ TriggerEdit e; e.op = TriggerEdit::Add;  e.model = m; return e; }
inline TriggerEdit TrigDrop(const wxString& name)
{ TriggerEdit e; e.op = TriggerEdit::Drop; e.model.name = name; return e; }

// Render a full CREATE TABLE from a TableModel snapshot.
inline std::vector<wxString> RenderCreateOf(DbType type, const TableModel& m)
{
    std::vector<wxString> stmts;
    wxString err;
    GetDialectProfile(type).RenderCreate(m, stmts, err);
    return stmts;
}
