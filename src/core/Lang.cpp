#include "core/Lang.h"

#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <map>

namespace core {

// The built-in English pack (pure data): a flat {source, translation, …, nullptr}
// array. Split across two translation units (LangPackEn.cpp + LangPackEn2.cpp) to
// keep each file under the 1000-line limit; both are merged into the "en" catalog.
extern const wchar_t* const kLangPackEn[];
extern const wchar_t* const kLangPackEn2[];

namespace {

using Pack = std::map<wxString, wxString>;

struct Registry {
    std::map<wxString, Pack>     packs;   // code → catalog
    std::map<wxString, wxString> names;   // code → display name
    wxString current = L"zh";
    bool     loaded = false;
};

Registry& reg()
{
    static Registry r;
    return r;
}

wxString SettingsPath()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFileName::GetPathSeparator() + L"settings.ini";
}

void AddArray(Pack& p, const wchar_t* const* a)
{
    for (int i = 0; a[i] && a[i + 1]; i += 2)
        p[a[i]] = a[i + 1];
}
Pack PackFromArray(const wchar_t* const* a)
{
    Pack p;
    AddArray(p, a);
    return p;
}

// Load external packs from <exeDir>/lang/<code>.ini — each an INI whose
// [strings] section maps source=translation, plus name=<display name>.
void LoadExternalPacks()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    wxString langDir = exe.GetPath() + wxFileName::GetPathSeparator() + L"lang";
    if (!wxDir::Exists(langDir)) return;

    wxDir dir(langDir);
    wxString file;
    for (bool more = dir.GetFirst(&file, L"*.ini", wxDIR_FILES); more;
         more = dir.GetNext(&file)) {
        const wxString code = wxFileName(file).GetName();
        wxFileConfig cfg(wxEmptyString, wxEmptyString,
                         langDir + wxFileName::GetPathSeparator() + file,
                         wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
        cfg.SetPath(L"/strings");
        Pack p;
        wxString key; long idx = 0;
        for (bool e = cfg.GetFirstEntry(key, idx); e; e = cfg.GetNextEntry(key, idx)) {
            if (key == L"name") reg().names[code] = cfg.Read(key, code);
            else                p[key] = cfg.Read(key, key);
        }
        if (!p.empty()) {
            reg().packs[code] = std::move(p);
            if (!reg().names.count(code)) reg().names[code] = code;
        }
    }
}

void EnsureLoaded()
{
    Registry& r = reg();
    if (r.loaded) return;
    r.loaded = true;

    // built-in: Chinese is identity (no pack); English from the data tables
    // (kLangPackEn + kLangPackEn2, merged — part 2 wins on duplicate keys).
    r.names[L"zh"] = L"中文";
    Pack en = PackFromArray(kLangPackEn);
    AddArray(en, kLangPackEn2);
    r.packs[L"en"] = std::move(en);
    r.names[L"en"] = L"English";

    LoadExternalPacks();

    wxFileConfig cfg(wxEmptyString, wxEmptyString, SettingsPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    r.current = cfg.Read(L"/general/language", L"zh");
}

} // namespace

wxString Lang::Current()
{
    EnsureLoaded();
    return reg().current;
}

void Lang::Set(const wxString& code)
{
    EnsureLoaded();
    reg().current = code;
    wxFileConfig cfg(wxEmptyString, wxEmptyString, SettingsPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.Write(L"/general/language", code);
    cfg.Flush();
}

wxString Lang::tr(const wxString& zh)
{
    EnsureLoaded();
    const wxString& code = reg().current;
    if (code == L"zh") return zh;                 // Chinese is the source
    auto it = reg().packs.find(code);
    if (it == reg().packs.end()) return zh;
    auto e = it->second.find(zh);
    return e == it->second.end() ? zh : e->second;   // fallback to source
}

std::vector<LangInfo> Lang::Available()
{
    EnsureLoaded();
    std::vector<LangInfo> out;
    out.push_back({ L"zh", L"中文" });               // always first
    for (const auto& kv : reg().packs) {
        if (kv.first == L"zh") continue;
        auto n = reg().names.find(kv.first);
        out.push_back({ kv.first, n != reg().names.end() ? n->second : kv.first });
    }
    return out;
}

} // namespace core
