#include "core/ScriptStore.h"

#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <algorithm>

namespace core {
namespace {

// <UserDataDir>/scripts (created if missing).
wxString ScriptsDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    dir += wxFileName::GetPathSeparator();
    dir += L"scripts";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString IndexPath()
{
    return ScriptsDir() + wxFileName::GetPathSeparator() + L"scripts_index.ini";
}

wxString SqlPathForStem(const wxString& stem)
{
    return ScriptsDir() + wxFileName::GetPathSeparator() + stem + L".sql";
}

} // namespace

wxString ScriptStore::Dir() { return ScriptsDir(); }

wxString ScriptStore::Sanitize(const wxString& name)
{
    wxString out;
    out.reserve(name.length());
    for (wxUniChar ch : name) {
        // Reject the Windows-illegal set (also unsafe on POSIX for '/') plus any
        // control character; collapse each to '_'.
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"'  || ch == '<' || ch == '>' || ch == '|' ||
            static_cast<unsigned>(ch.GetValue()) < 0x20) {
            out += '_';
        } else {
            out += ch;
        }
    }
    out.Trim(true).Trim(false);
    // Strip trailing dots/spaces (Windows silently drops them from filenames).
    while (!out.IsEmpty() && (out.Last() == '.' || out.Last() == ' '))
        out.RemoveLast();
    if (out.IsEmpty()) out = L"script";
    return out;
}

bool ScriptStore::Save(const wxString& name, const wxString& sql,
                       const wxString& owner, const wxString& exec, wxString& err)
{
    const wxString stem = Sanitize(name);
    const wxString path = SqlPathForStem(stem);

    wxFile f;
    if (!f.Create(path, /*overwrite*/ true, wxS_DEFAULT) && !f.Open(path, wxFile::write)) {
        err = wxString::Format(L"无法写入脚本文件: %s", path);
        return false;
    }
    const wxScopedCharBuffer utf8 = sql.utf8_str();
    if (!f.Write(utf8.data(), utf8.length())) {
        err = wxString::Format(L"写入脚本内容失败: %s", path);
        return false;
    }
    f.Close();

    // Record owner/exec + the original display name under the stem group.
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IndexPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(L"/" + stem);
    cfg.Write(L"name", name);
    cfg.Write(L"owner", owner);
    cfg.Write(L"exec", exec);
    cfg.Flush();
    return true;
}

std::vector<ScriptMeta> ScriptStore::List()
{
    std::vector<ScriptMeta> out;
    const wxString dir = ScriptsDir();

    // Index lookup: stem → (display name, owner, exec). Read once up front.
    wxFileConfig cfg(wxEmptyString, wxEmptyString, IndexPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);

    // Walk the .sql files on disk — the filesystem is the source of truth. An
    // index entry whose file is gone is naturally skipped; a file with no index
    // entry still lists (blank owner/exec).
    wxDir d(dir);
    if (!d.IsOpened()) return out;
    wxString fname;
    for (bool more = d.GetFirst(&fname, L"*.sql", wxDIR_FILES); more;
         more = d.GetNext(&fname)) {
        wxFileName fn(dir, fname);
        const wxString stem = fn.GetName();

        ScriptMeta m;
        m.stem = stem;
        m.path = fn.GetFullPath();

        // Metadata from the index (fallback: display name = stem, blank users).
        if (cfg.HasGroup(stem)) {
            cfg.SetPath(L"/" + stem);
            m.name  = cfg.Read(L"name",  stem);
            m.owner = cfg.Read(L"owner", wxEmptyString);
            m.exec  = cfg.Read(L"exec",  wxEmptyString);
            cfg.SetPath(L"/");
        } else {
            m.name = stem;
        }

        // Size + timestamps from the filesystem.
        if (fn.FileExists()) {
            wxULongLong sz = fn.GetSize();
            if (sz != wxInvalidSize)
                m.sizeBytes = static_cast<long long>(sz.GetValue());
            wxDateTime acc, mod, crt;
            if (fn.GetTimes(&acc, &mod, &crt)) { m.modified = mod; m.created = crt; }
        }
        out.push_back(std::move(m));
    }

    std::sort(out.begin(), out.end(), [](const ScriptMeta& a, const ScriptMeta& b) {
        return a.name.CmpNoCase(b.name) < 0;
    });
    return out;
}

bool ScriptStore::Load(const wxString& name, wxString& sql, wxString& err)
{
    const wxString stem = Sanitize(name);
    const wxString path = SqlPathForStem(stem);
    if (!wxFileName::FileExists(path)) {
        err = wxString::Format(L"脚本文件不存在: %s", path);
        return false;
    }
    wxFile f(path, wxFile::read);
    if (!f.IsOpened()) {
        err = wxString::Format(L"无法打开脚本文件: %s", path);
        return false;
    }
    wxString content;
    if (!f.ReadAll(&content, wxConvUTF8)) {
        err = wxString::Format(L"读取脚本内容失败: %s", path);
        return false;
    }
    sql = content;
    return true;
}

bool ScriptStore::Remove(const wxString& name)
{
    const wxString stem = Sanitize(name);
    const wxString path = SqlPathForStem(stem);

    bool ok = true;
    if (wxFileName::FileExists(path))
        ok = wxRemoveFile(path);

    wxFileConfig cfg(wxEmptyString, wxEmptyString, IndexPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    if (cfg.HasGroup(stem)) { cfg.DeleteGroup(L"/" + stem); cfg.Flush(); }
    return ok;
}

bool ScriptStore::Exists(const wxString& name)
{
    return wxFileName::FileExists(SqlPathForStem(Sanitize(name)));
}

} // namespace core
