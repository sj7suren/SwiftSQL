// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "core/FavoriteStore.h"

#include <wx/base64.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

namespace core {
namespace {

wxString FilePath()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFileName::GetPathSeparator() + L"favorites.ini";
}

// SECURITY BOUNDARY: favorites are NON-SECRET storage.
//
// The base64 wrapping below is only a transport/format convenience (it keeps
// multi-line SQL and special characters intact in the .ini file). It is NOT
// encryption and provides NO confidentiality — favorites.ini contents are
// plaintext-equivalent and anyone with file access can trivially recover them.
//
// Do NOT rely on this to protect credentials. Users must not save SQL that
// embeds plaintext passwords (e.g. `... IDENTIFIED BY 'secret'`) as a favorite.
// If a favorite genuinely needs to carry a secret, that secret belongs in a
// real vault via core::Secret (DPAPI-backed), not here. Encoding != protection.
wxString Enc(const wxString& s)
{
    const wxScopedCharBuffer utf8 = s.utf8_str();
    return wxBase64Encode(utf8.data(), utf8.length());
}

// Inverse of Enc(). See the SECURITY BOUNDARY note above: this is decoding,
// not decryption — the stored value was never confidential.
wxString Dec(const wxString& b64)
{
    wxMemoryBuffer buf = wxBase64Decode(b64);
    return wxString::FromUTF8(static_cast<const char*>(buf.GetData()), buf.GetDataLen());
}

} // namespace

std::vector<Favorite> FavoriteStore::LoadAll()
{
    std::vector<Favorite> out;
    wxFileConfig cfg(wxEmptyString, wxEmptyString, FilePath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    long idx = 0; wxString group;
    for (bool more = cfg.GetFirstGroup(group, idx); more;
         more = cfg.GetNextGroup(group, idx)) {
        cfg.SetPath(L"/" + group);
        Favorite f;
        f.name = cfg.Read(L"name", group);
        f.sql = Dec(cfg.Read(L"sql", wxEmptyString));
        cfg.SetPath(L"/");
        if (!f.name.IsEmpty()) out.push_back(f);
    }
    return out;
}

void FavoriteStore::Save(const Favorite& f)
{
    wxFileConfig cfg(wxEmptyString, wxEmptyString, FilePath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    // find existing group with this name, else a new index
    wxString target;
    long idx = 0; wxString group;
    for (bool more = cfg.GetFirstGroup(group, idx); more;
         more = cfg.GetNextGroup(group, idx)) {
        if (cfg.Read(L"/" + group + L"/name", wxEmptyString) == f.name) {
            target = group; break;
        }
    }
    if (target.IsEmpty()) {
        int n = 0;
        while (cfg.HasGroup(wxString::Format(L"fav%d", n))) ++n;
        target = wxString::Format(L"fav%d", n);
    }
    cfg.SetPath(L"/" + target);
    cfg.Write(L"name", f.name);
    cfg.Write(L"sql", Enc(f.sql));
    cfg.Flush();
}

void FavoriteStore::Remove(const wxString& name)
{
    wxFileConfig cfg(wxEmptyString, wxEmptyString, FilePath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    long idx = 0; wxString group;
    for (bool more = cfg.GetFirstGroup(group, idx); more;
         more = cfg.GetNextGroup(group, idx)) {
        if (cfg.Read(L"/" + group + L"/name", wxEmptyString) == name) {
            cfg.DeleteGroup(L"/" + group);
            cfg.Flush();
            return;
        }
    }
}

} // namespace core
