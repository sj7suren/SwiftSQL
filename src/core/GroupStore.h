// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// GroupStore.h — user-defined GROUPS (folders) for things the sidebar tree
// shows, persisted to groups.ini under the user's data dir.
//
// ===========================================================================
// ONE MECHANISM, MANY SCOPES — and why it is not two separate stores
// ===========================================================================
// The tree needs grouping in two visually different places: connections under
// the root, and tables (or views / functions / …) under a database's category
// folder. They look different but are the SAME problem — an ordered list of
// group names, plus a member→group mapping — so they are one store keyed by a
// SCOPE string rather than two stores that would drift apart.
//
//   ConnScope()                     -> "conn"
//   ObjectScope(conn, db, "tables") -> "obj:conn\tdb\ttables"
//
// A scope is opaque to this class: build it with the helpers, never by hand.
// The separator is a TAB precisely because it cannot appear in a connection
// name, a database name, or an object name typed through this program's
// dialogs, so two different triples can never collide into one scope string.
//
// ===========================================================================
// GROUPS EXIST INDEPENDENTLY OF THEIR MEMBERS
// ===========================================================================
// A group with nothing in it is a real, persisted thing — the user creates
// "生产环境" and then drags connections into it, which cannot work if an empty
// group evaporates on save. So the group LIST is stored separately from the
// membership map, and removing the last member of a group leaves the group.
//
// Group order is CREATION order and is preserved; it is what the tree renders
// top-to-bottom. Membership is a plain member→group map: a member belongs to at
// most one group, and a member with no entry (or an entry naming a group that no
// longer exists) is UNGROUPED — the tree draws it at the top level, which is
// exactly where it was before this feature existed.
//
// ===========================================================================
// WHAT THIS DELIBERATELY DOES NOT DO
// ===========================================================================
// No nesting. A group cannot contain a group. Navicat allows it; this does not,
// because every consumer here (tree placement, the 移动到分组 menu, drag-drop
// validation) would need a path type instead of a name, and nothing in the
// request needs it. If nesting is ever wanted, that is a schema change here plus
// a tree-placement change — not something to fake with "a\b" names.
//
// Renaming a member (a connection renamed in its dialog, a table renamed via F2)
// must CARRY ITS GROUP: see RenameMember, and call it from every rename path, or
// the object silently jumps back to ungrouped.
#pragma once

#include <map>
#include <vector>
#include <wx/string.h>

namespace core {

class GroupStore {
public:
    // ---- scope builders (never spell a scope string by hand) ----
    static wxString ConnScope();
    // `category` is a stable ASCII tag for the tree's Category enum
    // ("tables"/"views"/"functions"/"procedures"/"triggers"), not a display
    // label — a translated label would re-scope every group on a language switch.
    static wxString ObjectScope(const wxString& connName, const wxString& db,
                                const wxString& category);

    // ---- groups ----
    // Ordered by creation. Empty vector = this scope has no groups (the state
    // every scope starts in, and the state that renders exactly as before).
    static std::vector<wxString> Groups(const wxString& scope);
    // False when `name` is empty/whitespace or already exists (case-sensitive,
    // matching how the tree and the ini both compare).
    static bool AddGroup(const wxString& scope, const wxString& name);
    // False when `oldName` is absent, `newName` is empty, or `newName` already
    // exists. Carries every member of the group across the rename.
    static bool RenameGroup(const wxString& scope, const wxString& oldName,
                            const wxString& newName);
    // The group disappears; its members become UNGROUPED. Never deletes the
    // members themselves — this is a view-organisation store, and a group delete
    // that could drop connections would be a data-loss bug wearing a folder icon.
    static void RemoveGroup(const wxString& scope, const wxString& name);
    // Empty the group but KEEP it. The difference from RemoveGroup is the folder
    // itself: 清空分组 leaves an empty folder the user can refill, 删除分组 does
    // not. Both leave the members alive and merely ungrouped.
    static void ClearGroup(const wxString& scope, const wxString& name);

    // ---- membership ----
    // member → group, for every member with an assignment. One read for the
    // whole scope: the tree needs all of it at once when it renders a level, and
    // asking per member would re-parse the ini once per node.
    static std::map<wxString, wxString> Memberships(const wxString& scope);
    // "" = ungrouped (no entry, or an entry naming a group that was deleted).
    static wxString GroupOf(const wxString& scope, const wxString& member);
    // Just this group's members, sorted by name. What the 分组 list views render.
    static std::vector<wxString> MembersOf(const wxString& scope, const wxString& group);
    // Empty `group` removes the assignment (moves the member back to top level).
    // A group that does not exist yet is NOT auto-created — callers create the
    // group first, so a typo cannot silently invent one.
    static void SetMemberGroup(const wxString& scope, const wxString& member,
                               const wxString& group);
    // Carry an assignment across a rename of the MEMBER itself. No-op when the
    // old name had no assignment.
    static void RenameMember(const wxString& scope, const wxString& from,
                             const wxString& to);
    // Forget a member entirely (it was deleted). Its group is left alone.
    static void RemoveMember(const wxString& scope, const wxString& member);

    // ---- file ----
    static wxString FilePath();
    // TEST SEAM. Redirects every read/write to `path` for the rest of the
    // process. Exists so the unit test can exercise real persistence without
    // touching the developer's actual groups.ini; production never calls it.
    // Pass an empty string to restore the default location.
    static void UseFile(const wxString& path);
};

} // namespace core
