// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionTreeInternal.h — shared internals for the ConnectionTree split.
// ConnectionTree's method definitions are spread across several
// ConnectionTree_*.cpp files (per the 1000-line file charter, docs/CHARTER.md);
// the helpers those files share — the object-category enum, the per-engine
// capability table, the tree node tag (NodeData), and the IMAGE-LIST LAYOUT —
// live here so every translation unit sees ONE definition. Single-consumer
// helpers (SidebarTree, PadIcon, ColorSwatch, kConnColors, NewTableTemplate)
// stay file-local in the .cpp that uses them.
//
// The image-list layout moved here when ConnectionTree_Groups.cpp became a
// SECOND consumer: it re-creates a connection node when the user moves it into
// a group (which needs ConnImg) and draws group folders (IMG_GROUP). Two copies
// of an index-into-an-image-list is exactly the seam where a node silently
// renders the wrong icon, so there is one.
#pragma once

#include <wx/treectrl.h>
#include <wx/string.h>
#include <utility>
#include "db/DbDriver.h"
#include "ui/ConnectionTree.h"

namespace ui {

// Object categories shown under a database node (gated per engine by DbCaps).
enum class Category { Tables, Views, Functions, Procedures, Triggers };

// Per-engine capabilities for the database context menu. Five engines collapse
// to two SQL dialects, but database-level operations differ further, so the menu
// is gated on these flags rather than lit up unconditionally. One place to edit
// when a new engine or capability lands.
struct DbCaps {
    bool editDatabase;   // ALTER DATABASE … CHARACTER SET — MySQL family only
    bool multiDbBrowse;  // one connection can browse *other* databases' tables
    bool hasViews;       // show a 视图 category
    bool hasRoutines;    // show a 函数 category (functions, +procedures if split)
    bool hasTriggers;    // show a 触发器 category
    bool splitProcFunc;  // separate 存储过程 from 函数 (MySQL/SQL Server); PG merges
};

inline DbCaps CapsFor(db::Dialect d)
{
    switch (d) {
    case db::Dialect::MySQL:     return { true,  true,  true, true,  true, true  };
    case db::Dialect::Postgres:  return { false, false, true, true,  true, false };
    case db::Dialect::SqlServer: return { false, false, true, true,  true, true  };
    case db::Dialect::Sqlite:    return { false, true,  true, false, true, false };
    // Oracle (and DM in Oracle mode): schemas/users are browsable across the
    // connection (multiDbBrowse), views/routines/triggers all supported, and
    // PROCEDURE/FUNCTION are distinct object types (splitProcFunc). No post-create
    // charset change (editDatabase=false; charset is a database-level setting).
    case db::Dialect::Oracle:    return { false, true,  true, true,  true, true  };
    }
    return { false, false, true, true, true, true };
}

// Image-list layout: for each DbType two connection icons (green connected /
// grey disconnected), then database-open/closed, then one icon per object type
// (table/view/function/procedure/trigger) shared by a category node and its
// members, then the group folder. THE ENUM ORDER IS THE ORDER THE BITMAPS ARE
// ADDED in ConnectionTree's constructor — append only, at both ends together.
constexpr int kConnImgCount = 2 * static_cast<int>(db::DbType::_Count);
enum { IMG_DB = kConnImgCount, IMG_DB_GREY,
       IMG_TABLE, IMG_VIEW, IMG_FUNC, IMG_PROC, IMG_TRIGGER, IMG_GROUP };

inline int ConnImg(db::DbType t, bool connected)
{
    return 2 * static_cast<int>(t) + (connected ? 0 : 1);
}

// Stable ASCII tag for a Category, used to build a core::GroupStore scope.
// NOT the translated tree label: scoping groups by display text would move every
// group to a brand-new scope the moment the user switches language.
inline const wchar_t* CategoryTag(Category c)
{
    switch (c) {
    case Category::Views:      return L"views";
    case Category::Functions:  return L"functions";
    case Category::Procedures: return L"procedures";
    case Category::Triggers:   return L"triggers";
    case Category::Tables:     break;
    }
    return L"tables";
}

// Tags a tree item with what it represents and which connection it belongs to.
//
// GROUP NODES (Kind::Group) are the user's own folders, and they appear at TWO
// levels with deliberately different tagging:
//   * a CONNECTION group sits under the root: entry == nullptr, db/cat unused,
//     `group` is its name. It has no connection because it CONTAINS them.
//   * an OBJECT group sits under a category folder: entry/db/cat identify the
//     scope it groups within (that connection's tables, that database's views…),
//     and `group` is its name.
// `entry == nullptr` is therefore the discriminator, and every consumer must
// check `kind == Group` before dereferencing `entry` — which is why the group
// cases in ShowNodeContextMenu come BEFORE the code that assumes a connection.
class NodeData : public wxTreeItemData {
public:
    enum Kind { Connection, Database, CategoryNode, Table, Group };
    NodeData(Kind k, ConnEntry* e, wxString db = {}, wxString tbl = {})
        : kind(k), entry(e), db(std::move(db)), table(std::move(tbl)) {}
    Kind       kind;
    ConnEntry* entry;
    wxString   db;
    wxString   table;
    wxString   group;                    // valid when kind == Group
    Category   cat = Category::Tables;   // valid when kind == CategoryNode or Group
    bool       loaded = false;
};

} // namespace ui
