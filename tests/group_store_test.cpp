// group_store_test.cpp — unit tests for core::GroupStore, the persistence
// behind the sidebar's 分组管理 (connection groups + object groups).
//
// WHY THIS IS TESTED AND THE OTHER STORES ARE NOT. ConnectionStore/ScriptStore
// are field-for-field round-trips: write struct, read struct. GroupStore is not
// — it holds an ORDERED list plus a member→group map, and the interesting
// behaviour is what happens to the map when the list changes underneath it
// (rename carries members; delete ungroups them but must never drop them). That
// is real logic, and every one of those rules is a way a user's connections
// could silently vanish from the tree.
//
// It exercises REAL PERSISTENCE, not a mock: every assertion below reads back
// through a fresh wxFileConfig, because "did it survive the ini round-trip" is
// precisely the question — an in-memory fake would pass while a mis-indexed key
// scheme lost data. GroupStore::UseFile redirects that file into the temp dir so
// the developer's own groups.ini is never touched.
//
// Harness: the same dependency-free assert loop as sqlscript_test.cpp.
#include "core/GroupStore.h"

#include <cstdio>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/string.h>

using core::GroupStore;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// Render the group list as one string so ORDER is asserted, not just membership.
// A set-comparison would pass on a store that shuffled the user's folders.
static wxString GroupsText(const wxString& scope)
{
    wxString s;
    for (const wxString& g : GroupStore::Groups(scope)) {
        if (!s.IsEmpty()) s += L"|";
        s += g;
    }
    return s;
}

int main()
{
    // GroupStore reaches wxFileConfig and wxStandardPaths, which need the wx
    // runtime up. wxInitializer brings up wxBase only: no window is created and
    // this stays headless. Same reasoning as SyncRowSelectionTests.cpp.
    //
    // WITHOUT IT THIS TEST STILL "PASSED" ON A CONSOLE and aborted with exit
    // code 3 under ctest — the printf output was line-flushed to a terminal but
    // block-buffered (and so lost) when redirected, hiding the abort behind a
    // clean-looking run. That is why the check below is a hard failure rather
    // than a warning.
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    std::printf("== group_store_test ==\n");

    // Redirect persistence into the temp dir and start from a clean slate, so a
    // rerun cannot pass on state left by the previous run.
    const wxString path = wxFileName::GetTempDir() +
                          wxFileName::GetPathSeparator() + L"swiftsql_grouptest.ini";
    if (wxFileExists(path)) wxRemoveFile(path);
    GroupStore::UseFile(path);

    const wxString conn = GroupStore::ConnScope();
    const wxString objs = GroupStore::ObjectScope(L"cloudpacs-pg", L"cloudpacs", L"tables");

    // ---- a virgin scope is empty, and that is the pre-feature rendering -----
    ExpectEq("virgin scope has no groups", (long long)GroupStore::Groups(conn).size(), 0);
    ExpectEq("virgin scope has no memberships",
             (long long)GroupStore::Memberships(conn).size(), 0);

    // ---- groups are created, ORDERED by creation, and deduped ---------------
    ExpectTrue("AddGroup accepts a new name", GroupStore::AddGroup(conn, L"生产环境"));
    ExpectTrue("AddGroup accepts a second name", GroupStore::AddGroup(conn, L"测试环境"));
    ExpectTrue("AddGroup refuses a duplicate", !GroupStore::AddGroup(conn, L"生产环境"));
    ExpectTrue("AddGroup refuses an empty name", !GroupStore::AddGroup(conn, L""));
    ExpectTrue("AddGroup refuses a whitespace-only name", !GroupStore::AddGroup(conn, L"   "));
    ExpectStr("groups survive the ini round-trip IN CREATION ORDER",
              GroupsText(conn), L"生产环境|测试环境");

    // ---- AN EMPTY GROUP IS A REAL, PERSISTED THING -------------------------
    // The header's load-bearing claim: the user makes a folder first and drags
    // things in second, which is impossible if an empty group evaporates.
    ExpectEq("an empty group still persists (nothing has been assigned yet)",
             (long long)GroupStore::Groups(conn).size(), 2);

    // ---- membership round-trips, and cannot invent a group ------------------
    GroupStore::SetMemberGroup(conn, L"cloudpacs-mysql", L"生产环境");
    GroupStore::SetMemberGroup(conn, L"cloudpacs-pg",    L"生产环境");
    GroupStore::SetMemberGroup(conn, L"dev-sqlite",      L"测试环境");
    ExpectStr("member reads back its group", GroupStore::GroupOf(conn, L"cloudpacs-mysql"),
              L"生产环境");
    ExpectEq("three memberships stored", (long long)GroupStore::Memberships(conn).size(), 3);
    GroupStore::SetMemberGroup(conn, L"orphan", L"不存在的组");
    ExpectStr("assigning to a NON-EXISTENT group is refused, not auto-created",
              GroupStore::GroupOf(conn, L"orphan"), wxString());
    ExpectStr("...and no phantom group appeared in the list",
              GroupsText(conn), L"生产环境|测试环境");

    // ---- an unassigned member is ungrouped, which is the default rendering --
    ExpectStr("a member never assigned is ungrouped",
              GroupStore::GroupOf(conn, L"never-touched"), wxString());

    // ---- moving a member between groups, and back to top level --------------
    GroupStore::SetMemberGroup(conn, L"dev-sqlite", L"生产环境");
    ExpectStr("a member moves between groups", GroupStore::GroupOf(conn, L"dev-sqlite"),
              L"生产环境");
    GroupStore::SetMemberGroup(conn, L"dev-sqlite", wxString());
    ExpectStr("an empty group name moves the member back to top level",
              GroupStore::GroupOf(conn, L"dev-sqlite"), wxString());
    ExpectEq("...and it is no longer counted as a membership",
             (long long)GroupStore::Memberships(conn).size(), 2);

    // ---- RENAME CARRIES ITS MEMBERS ----------------------------------------
    // The rule this exists for: a rename that forgot the map would redraw the
    // folder under its new name and leave every member pointing at a name
    // nothing renders — the group would look emptied by a rename.
    ExpectTrue("RenameGroup accepts a fresh name",
               GroupStore::RenameGroup(conn, L"生产环境", L"PROD"));
    ExpectStr("the rename is in the list, in place", GroupsText(conn), L"PROD|测试环境");
    ExpectStr("member 1 came WITH the rename", GroupStore::GroupOf(conn, L"cloudpacs-mysql"),
              L"PROD");
    ExpectStr("member 2 came WITH the rename", GroupStore::GroupOf(conn, L"cloudpacs-pg"),
              L"PROD");
    ExpectTrue("RenameGroup refuses a name that already exists",
               !GroupStore::RenameGroup(conn, L"PROD", L"测试环境"));
    ExpectTrue("RenameGroup refuses an unknown source group",
               !GroupStore::RenameGroup(conn, L"没有这个组", L"随便"));

    // ---- MembersOf: just this group, sorted --------------------------------
    GroupStore::AddGroup(conn, L"ARCHIVE");
    GroupStore::SetMemberGroup(conn, L"old-1", L"ARCHIVE");
    GroupStore::SetMemberGroup(conn, L"old-2", L"ARCHIVE");
    {
        wxString s;
        for (const wxString& m : GroupStore::MembersOf(conn, L"ARCHIVE")) {
            if (!s.IsEmpty()) s += L"|";
            s += m;
        }
        ExpectStr("MembersOf returns only that group's members, sorted", s, L"old-1|old-2");
    }
    ExpectEq("MembersOf of a non-existent group is empty",
             (long long)GroupStore::MembersOf(conn, L"没有这个组").size(), 0);

    // ---- CLEAR EMPTIES THE GROUP BUT KEEPS THE FOLDER ----------------------
    // The distinction from RemoveGroup, and the reason both exist: 清空分组
    // leaves something to refill, 删除分组 does not.
    GroupStore::ClearGroup(conn, L"ARCHIVE");
    ExpectEq("ClearGroup emptied it", (long long)GroupStore::MembersOf(conn, L"ARCHIVE").size(), 0);
    ExpectTrue("...but the GROUP itself survives, unlike RemoveGroup",
               GroupsText(conn).Contains(L"ARCHIVE"));
    ExpectStr("...and its members are ungrouped, not deleted — still assignable",
              GroupStore::GroupOf(conn, L"old-1"), wxString());
    GroupStore::SetMemberGroup(conn, L"old-1", L"ARCHIVE");
    ExpectStr("...proved by refilling the emptied group",
              GroupStore::GroupOf(conn, L"old-1"), L"ARCHIVE");
    // Clearing must not touch a DIFFERENT group's members.
    ExpectStr("ClearGroup left the other group's member alone",
              GroupStore::GroupOf(conn, L"cloudpacs-pg"), L"PROD");
    GroupStore::RemoveGroup(conn, L"ARCHIVE");   // back to the pre-block state
    GroupStore::RemoveMember(conn, L"old-1");
    GroupStore::RemoveMember(conn, L"old-2");

    // ---- DELETE UNGROUPS, IT DOES NOT DELETE MEMBERS -----------------------
    GroupStore::RemoveGroup(conn, L"PROD");
    ExpectStr("the deleted group is gone from the list", GroupsText(conn), L"测试环境");
    ExpectStr("its member is UNGROUPED, not deleted — it is still assignable",
              GroupStore::GroupOf(conn, L"cloudpacs-mysql"), wxString());
    GroupStore::SetMemberGroup(conn, L"cloudpacs-mysql", L"测试环境");
    ExpectStr("...proved by re-assigning that same member to the surviving group",
              GroupStore::GroupOf(conn, L"cloudpacs-mysql"), L"测试环境");

    // ---- a member renamed keeps its group -----------------------------------
    GroupStore::RenameMember(conn, L"cloudpacs-mysql", L"cloudpacs-mysql-2");
    ExpectStr("a renamed member keeps its group",
              GroupStore::GroupOf(conn, L"cloudpacs-mysql-2"), L"测试环境");
    ExpectStr("...and the old name no longer resolves",
              GroupStore::GroupOf(conn, L"cloudpacs-mysql"), wxString());
    GroupStore::RemoveMember(conn, L"cloudpacs-mysql-2");
    ExpectStr("a removed member's assignment is forgotten",
              GroupStore::GroupOf(conn, L"cloudpacs-mysql-2"), wxString());

    // ---- SCOPES ARE INDEPENDENT --------------------------------------------
    // The whole justification for one store instead of two: connection groups
    // and table groups share the mechanism but must never share state.
    ExpectTrue("object scope: AddGroup", GroupStore::AddGroup(objs, L"业务表"));
    GroupStore::SetMemberGroup(objs, L"OpenIddictApplications", L"业务表");
    ExpectStr("the object scope has its OWN group list", GroupsText(objs), L"业务表");
    ExpectStr("the connection scope is untouched by it", GroupsText(conn), L"测试环境");
    ExpectStr("a member name that exists in both scopes resolves per scope",
              GroupStore::GroupOf(objs, L"OpenIddictApplications"), L"业务表");
    ExpectStr("...and is ungrouped in the other scope",
              GroupStore::GroupOf(conn, L"OpenIddictApplications"), wxString());

    // Scope identity must include the database and the category, or a table
    // group would leak across databases that happen to share table names.
    const wxString otherDb  = GroupStore::ObjectScope(L"cloudpacs-pg", L"other", L"tables");
    const wxString otherCat = GroupStore::ObjectScope(L"cloudpacs-pg", L"cloudpacs", L"views");
    ExpectEq("a different DATABASE is a different scope",
             (long long)GroupStore::Groups(otherDb).size(), 0);
    ExpectEq("a different CATEGORY is a different scope",
             (long long)GroupStore::Groups(otherCat).size(), 0);

    // ---- everything above survives a cold read of the real file ------------
    // Nothing so far proves the data is on DISK rather than in a wxFileConfig
    // instance that keeps being handed back. Re-point UseFile at the same path
    // to force a genuinely new read.
    GroupStore::UseFile(wxString());
    GroupStore::UseFile(path);
    ExpectStr("groups survive a cold reopen of the file", GroupsText(objs), L"业务表");
    ExpectStr("memberships survive a cold reopen of the file",
              GroupStore::GroupOf(objs, L"OpenIddictApplications"), L"业务表");

    if (wxFileExists(path)) wxRemoveFile(path);
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
