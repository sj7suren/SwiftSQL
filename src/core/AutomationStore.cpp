#include "core/AutomationStore.h"

#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <algorithm>

namespace core {
namespace {

// <UserDataDir>/automation (created if missing).
wxString AutoDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    dir += wxFileName::GetPathSeparator();
    dir += L"automation";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString IniPath()
{
    return AutoDir() + wxFileName::GetPathSeparator() + L"automation.ini";
}

// time_t round-trip through the INI (0 = invalid). wxDateTime::GetTicks is the
// Unix epoch seconds; FromTimeT rebuilds the value in local time.
long long ToTicks(const wxDateTime& t)
{
    return t.IsValid() ? static_cast<long long>(t.GetTicks()) : 0;
}
wxDateTime FromTicks(long long v)
{
    if (v <= 0) return wxDateTime();
    return wxDateTime(static_cast<time_t>(v));
}

// Read one group (cfg path already set to "/<stem>") into a job.
AutomationJob ReadJob(wxFileConfig& cfg, const wxString& stem)
{
    AutomationJob j;
    j.name    = cfg.Read(L"name", stem);
    j.type    = static_cast<JobType>(cfg.ReadLong(L"type", static_cast<long>(JobType::SyncBoth)));
    j.srcConn = cfg.Read(L"srcConn", wxEmptyString);
    j.srcDb   = cfg.Read(L"srcDb",   wxEmptyString);
    j.tgtConn = cfg.Read(L"tgtConn", wxEmptyString);
    j.tgtDb   = cfg.Read(L"tgtDb",   wxEmptyString);
    j.dropMissing    = cfg.ReadBool(L"dropMissing", false);
    j.useTransaction = cfg.ReadBool(L"useTransaction", true);
    j.withData       = cfg.ReadBool(L"withData", true);
    j.schedule        = static_cast<ScheduleKind>(cfg.ReadLong(L"schedule", 0));
    j.intervalMinutes = static_cast<int>(cfg.ReadLong(L"intervalMinutes", 60));
    j.dailyHour       = static_cast<int>(cfg.ReadLong(L"dailyHour", 3));
    j.dailyMinute     = static_cast<int>(cfg.ReadLong(L"dailyMinute", 0));
    j.lastRun     = FromTicks(cfg.ReadLong(L"lastRun", 0));
    j.lastResult  = static_cast<RunResult>(cfg.ReadLong(L"lastResult", 0));
    j.lastMessage = cfg.Read(L"lastMessage", wxEmptyString);
    return j;
}

} // namespace

wxDateTime AutomationJob::NextRunAfter(const wxDateTime& from) const
{
    if (schedule == ScheduleKind::EveryMinutes) {
        const int m = intervalMinutes > 0 ? intervalMinutes : 1;
        if (!lastRun.IsValid()) return from;   // never run → due now
        wxDateTime next = lastRun + wxTimeSpan::Minutes(m);
        return next;
    }
    if (schedule == ScheduleKind::DailyAt) {
        const int hh = (dailyHour   >= 0 && dailyHour   < 24) ? dailyHour   : 0;
        const int mm = (dailyMinute >= 0 && dailyMinute < 60) ? dailyMinute : 0;
        auto slotOn = [hh, mm](const wxDateTime& day) {
            wxDateTime s = day;
            s.ResetTime();
            s.SetHour(hh); s.SetMinute(mm); s.SetSecond(0);
            return s;
        };
        if (!lastRun.IsValid()) {
            // Never run: today's slot. If it already passed (relative to now) the
            // tick fires once to "catch up", then lastRun advances the schedule.
            return slotOn(from);
        }
        // Otherwise the next daily slot STRICTLY after the last run (so a job that
        // just ran at its slot waits a full day, and the crossing minute is caught).
        wxDateTime cand = slotOn(lastRun);
        if (cand <= lastRun) cand += wxTimeSpan::Days(1);
        return cand;
    }
    return wxDateTime();   // ScheduleKind::None
}

wxString AutomationStore::Dir() { return AutoDir(); }

wxString AutomationStore::ExportsDir()
{
    wxString dir = AutoDir() + wxFileName::GetPathSeparator() + L"exports";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString AutomationStore::Sanitize(const wxString& name)
{
    wxString out;
    out.reserve(name.length());
    for (wxUniChar ch : name) {
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"'  || ch == '<' || ch == '>' || ch == '|' ||
            static_cast<unsigned>(ch.GetValue()) < 0x20) {
            out += '_';
        } else {
            out += ch;
        }
    }
    out.Trim(true).Trim(false);
    while (!out.IsEmpty() && (out.Last() == '.' || out.Last() == ' '))
        out.RemoveLast();
    if (out.IsEmpty()) out = L"job";
    return out;
}

std::vector<AutomationJob> AutomationStore::List()
{
    std::vector<AutomationJob> out;
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IniPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);

    wxString stem;
    long idx = 0;
    for (bool more = cfg.GetFirstGroup(stem, idx); more;
         more = cfg.GetNextGroup(stem, idx)) {
        cfg.SetPath(L"/" + stem);
        out.push_back(ReadJob(cfg, stem));
        cfg.SetPath(L"/");
    }

    std::sort(out.begin(), out.end(), [](const AutomationJob& a, const AutomationJob& b) {
        return a.name.CmpNoCase(b.name) < 0;
    });
    return out;
}

bool AutomationStore::Get(const wxString& name, AutomationJob& out)
{
    const wxString stem = Sanitize(name);
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IniPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    if (!cfg.HasGroup(stem)) return false;
    cfg.SetPath(L"/" + stem);
    out = ReadJob(cfg, stem);
    return true;
}

bool AutomationStore::Save(const AutomationJob& job, wxString& err)
{
    const wxString stem = Sanitize(job.name);
    if (stem.IsEmpty()) { err = L"作业名称无效"; return false; }

    wxFileConfig cfg(wxEmptyString, wxEmptyString, IniPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(L"/" + stem);
    cfg.Write(L"name", job.name);
    cfg.Write(L"type", static_cast<long>(job.type));
    cfg.Write(L"srcConn", job.srcConn);
    cfg.Write(L"srcDb",   job.srcDb);
    cfg.Write(L"tgtConn", job.tgtConn);
    cfg.Write(L"tgtDb",   job.tgtDb);
    cfg.Write(L"dropMissing", job.dropMissing);
    cfg.Write(L"useTransaction", job.useTransaction);
    cfg.Write(L"withData", job.withData);
    cfg.Write(L"schedule", static_cast<long>(job.schedule));
    cfg.Write(L"intervalMinutes", static_cast<long>(job.intervalMinutes));
    cfg.Write(L"dailyHour", static_cast<long>(job.dailyHour));
    cfg.Write(L"dailyMinute", static_cast<long>(job.dailyMinute));
    cfg.Write(L"lastRun", static_cast<long>(ToTicks(job.lastRun)));
    cfg.Write(L"lastResult", static_cast<long>(job.lastResult));
    cfg.Write(L"lastMessage", job.lastMessage);
    if (!cfg.Flush()) { err = L"无法写入作业文件"; return false; }
    return true;
}

bool AutomationStore::Remove(const wxString& name)
{
    const wxString stem = Sanitize(name);
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IniPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    if (cfg.HasGroup(stem)) { cfg.DeleteGroup(L"/" + stem); cfg.Flush(); }
    return true;
}

bool AutomationStore::Exists(const wxString& name)
{
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IniPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    return cfg.HasGroup(Sanitize(name));
}

} // namespace core
