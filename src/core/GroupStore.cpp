// GroupStore.cpp — see header.
#include "core/GroupStore.h"

#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <algorithm>
#include <memory>

namespace core {
namespace {

// Set by UseFile(); empty means "the real user-data location".
wxString g_override;

wxString StorePath()
{
    if (!g_override.IsEmpty()) return g_override;
    wxString dir = wxStandardPaths::Get().GetUserDataDir();   // …/SwiftSQL
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFileName::GetPathSeparator() + L"groups.ini";
}

std::unique_ptr<wxFileConfig> Open()
{
    return std::make_unique<wxFileConfig>(
        wxEmptyString, wxEmptyString, StorePath(), wxEmptyString,
        wxCONFIG_USE_LOCAL_FILE);
}

// EVERYTHING A SCOPE HOLDS, in memory. Read-modify-write of a whole scope is
// the only mutation shape here, and deliberately so: a scope is a handful of
// short strings, while the alternative — surgical edits to indexed ini keys —
// is where an "index 3 now means something else" bug would live. Rewriting the
// scope wholesale makes every operation obviously order-preserving.
struct ScopeData {
    std::vector<wxString>                      groups;    // creation order
    std::vector<std::pair<wxString, wxString>> members;   // member → group
};

// Scope names are user-derived (connection and database names), so they cannot
// be ini section names — same reasoning as ConnectionStore's conn0/conn1 scheme.
// Sections are indexed and carry the real scope string as a VALUE.
//
// Returns an empty string when the scope has never been written.
wxString FindScopeSection(wxFileConfig& cfg, const wxString& scope)
{
    wxString sec; long idx = 0;
    cfg.SetPath(L"/");
    bool more = cfg.GetFirstGroup(sec, idx);
    while (more) {
        cfg.SetPath(L"/" + sec);
        const wxString s = cfg.Read(L"scope", wxEmptyString);
        cfg.SetPath(L"/");
        if (s == scope) return sec;
        more = cfg.GetNextGroup(sec, idx);
    }
    return wxString();
}

// An unused section name. Scans rather than counting, so a hand-deleted section
// in the middle cannot make this collide with a live one.
wxString AllocScopeSection(wxFileConfig& cfg)
{
    wxString sec; long idx = 0; long maxN = -1;
    cfg.SetPath(L"/");
    bool more = cfg.GetFirstGroup(sec, idx);
    while (more) {
        long n = 0;
        if (sec.StartsWith(L"s") && sec.Mid(1).ToLong(&n)) maxN = std::max(maxN, n);
        more = cfg.GetNextGroup(sec, idx);
    }
    return wxString::Format(L"s%ld", maxN + 1);
}

ScopeData Read(wxFileConfig& cfg, const wxString& scope)
{
    ScopeData d;
    const wxString sec = FindScopeSection(cfg, scope);
    if (sec.IsEmpty()) return d;

    // Groups and members are read by INDEX, stopping at the first gap — the
    // writer always emits a dense 0..n-1 run, so a gap means the file was
    // hand-edited and stopping is the conservative answer.
    for (long i = 0;; ++i) {
        const wxString path = wxString::Format(L"/%s/g%ld", sec, i);
        if (!cfg.HasGroup(path)) break;
        cfg.SetPath(path);
        const wxString name = cfg.Read(L"name", wxEmptyString);
        cfg.SetPath(L"/");
        if (name.IsEmpty()) break;
        d.groups.push_back(name);
    }
    for (long i = 0;; ++i) {
        const wxString path = wxString::Format(L"/%s/m%ld", sec, i);
        if (!cfg.HasGroup(path)) break;
        cfg.SetPath(path);
        const wxString name  = cfg.Read(L"name",  wxEmptyString);
        const wxString group = cfg.Read(L"group", wxEmptyString);
        cfg.SetPath(L"/");
        if (name.IsEmpty()) break;
        d.members.push_back({ name, group });
    }
    return d;
}

void Write(wxFileConfig& cfg, const wxString& scope, const ScopeData& d)
{
    wxString sec = FindScopeSection(cfg, scope);
    cfg.SetPath(L"/");
    if (!sec.IsEmpty()) cfg.DeleteGroup(sec);   // full rewrite — see ScopeData
    else                sec = AllocScopeSection(cfg);

    // A scope with no groups AND no memberships is not worth a section; leaving
    // it out keeps groups.ini empty for the overwhelmingly common case of a user
    // who never made a group.
    if (d.groups.empty() && d.members.empty()) { cfg.Flush(); return; }

    cfg.SetPath(L"/" + sec);
    cfg.Write(L"scope", scope);
    cfg.SetPath(L"/");
    for (size_t i = 0; i < d.groups.size(); ++i) {
        cfg.SetPath(wxString::Format(L"/%s/g%zu", sec, i));
        cfg.Write(L"name", d.groups[i]);
        cfg.SetPath(L"/");
    }
    size_t w = 0;
    for (const auto& m : d.members) {
        if (m.second.IsEmpty()) continue;       // ungrouped → store nothing
        cfg.SetPath(wxString::Format(L"/%s/m%zu", sec, w++));
        cfg.Write(L"name",  m.first);
        cfg.Write(L"group", m.second);
        cfg.SetPath(L"/");
    }
    cfg.Flush();
}

bool Has(const std::vector<wxString>& v, const wxString& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

wxString Trimmed(const wxString& s)
{
    wxString t = s;
    t.Trim(true).Trim(false);
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
wxString GroupStore::FilePath() { return StorePath(); }
void     GroupStore::UseFile(const wxString& path) { g_override = path; }

wxString GroupStore::ConnScope() { return L"conn"; }

wxString GroupStore::ObjectScope(const wxString& connName, const wxString& db,
                                 const wxString& category)
{
    return L"obj:" + connName + L"\t" + db + L"\t" + category;
}

std::vector<wxString> GroupStore::Groups(const wxString& scope)
{
    auto cfg = Open();
    return Read(*cfg, scope).groups;
}

bool GroupStore::AddGroup(const wxString& scope, const wxString& name)
{
    const wxString n = Trimmed(name);
    if (n.IsEmpty()) return false;
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    if (Has(d.groups, n)) return false;
    d.groups.push_back(n);
    Write(*cfg, scope, d);
    return true;
}

bool GroupStore::RenameGroup(const wxString& scope, const wxString& oldName,
                             const wxString& newName)
{
    const wxString n = Trimmed(newName);
    if (n.IsEmpty() || oldName.IsEmpty()) return false;
    if (n == oldName) return true;                 // nothing to do, not a failure
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    if (!Has(d.groups, oldName) || Has(d.groups, n)) return false;
    for (wxString& g : d.groups) if (g == oldName) g = n;
    // The members move WITH the name. Missing this is how a rename silently
    // empties a group: the folder reappears under the new name and every member
    // is left pointing at a name nothing renders any more.
    for (auto& m : d.members) if (m.second == oldName) m.second = n;
    Write(*cfg, scope, d);
    return true;
}

void GroupStore::RemoveGroup(const wxString& scope, const wxString& name)
{
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    d.groups.erase(std::remove(d.groups.begin(), d.groups.end(), name), d.groups.end());
    // Members are UNGROUPED, never removed (see the header).
    d.members.erase(std::remove_if(d.members.begin(), d.members.end(),
                                   [&](const std::pair<wxString, wxString>& m) {
                                       return m.second == name;
                                   }),
                    d.members.end());
    Write(*cfg, scope, d);
}

void GroupStore::ClearGroup(const wxString& scope, const wxString& name)
{
    if (name.IsEmpty()) return;
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    // The group STAYS in d.groups — that is the whole difference from
    // RemoveGroup, and an empty folder the user can refill is the point.
    const size_t before = d.members.size();
    d.members.erase(std::remove_if(d.members.begin(), d.members.end(),
                                   [&](const std::pair<wxString, wxString>& m) {
                                       return m.second == name;
                                   }),
                    d.members.end());
    if (d.members.size() != before) Write(*cfg, scope, d);
}

std::vector<wxString> GroupStore::MembersOf(const wxString& scope, const wxString& group)
{
    std::vector<wxString> out;
    if (group.IsEmpty()) return out;
    // Through Memberships() rather than Read() directly, so this inherits its
    // "a membership naming a deleted group is not a member" rule for free.
    for (const auto& m : Memberships(scope))
        if (m.second == group) out.push_back(m.first);
    return out;   // Memberships is a std::map → already sorted by member name
}

std::map<wxString, wxString> GroupStore::Memberships(const wxString& scope)
{
    auto cfg = Open();
    const ScopeData d = Read(*cfg, scope);
    // A membership naming a group that no longer exists is reported as
    // UNGROUPED (dropped here) rather than as a phantom folder. Write() prunes
    // those on the next mutation; this makes the read honest in the meantime.
    std::map<wxString, wxString> out;
    for (const auto& m : d.members)
        if (!m.second.IsEmpty() && Has(d.groups, m.second)) out[m.first] = m.second;
    return out;
}

wxString GroupStore::GroupOf(const wxString& scope, const wxString& member)
{
    const std::map<wxString, wxString> m = Memberships(scope);
    auto it = m.find(member);
    return it == m.end() ? wxString() : it->second;
}

void GroupStore::SetMemberGroup(const wxString& scope, const wxString& member,
                                const wxString& group)
{
    if (member.IsEmpty()) return;
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    if (!group.IsEmpty() && !Has(d.groups, group)) return;   // no silent creation
    bool found = false;
    for (auto& m : d.members)
        if (m.first == member) { m.second = group; found = true; break; }
    if (!found && !group.IsEmpty()) d.members.push_back({ member, group });
    Write(*cfg, scope, d);
}

void GroupStore::RenameMember(const wxString& scope, const wxString& from,
                              const wxString& to)
{
    if (from.IsEmpty() || to.IsEmpty() || from == to) return;
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    bool touched = false;
    for (auto& m : d.members)
        if (m.first == from) { m.first = to; touched = true; break; }
    if (touched) Write(*cfg, scope, d);
}

void GroupStore::RemoveMember(const wxString& scope, const wxString& member)
{
    auto cfg = Open();
    ScopeData d = Read(*cfg, scope);
    const size_t before = d.members.size();
    d.members.erase(std::remove_if(d.members.begin(), d.members.end(),
                                   [&](const std::pair<wxString, wxString>& m) {
                                       return m.first == member;
                                   }),
                    d.members.end());
    if (d.members.size() != before) Write(*cfg, scope, d);
}

} // namespace core
