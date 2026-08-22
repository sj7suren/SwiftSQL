// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "core/Settings.h"

#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

namespace core {
namespace {

// Same location & filename as core::Lang uses for the language preference, so
// the whole app shares one settings.ini rather than scattering little stores.
wxString SettingsFile()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();  // …/SwiftSQL
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFileName::GetPathSeparator() + L"settings.ini";
}

} // namespace

wxString Settings::FilePath() { return SettingsFile(); }

wxString Settings::ReadString(const wxString& key, const wxString& def)
{
    wxFileConfig cfg(wxEmptyString, wxEmptyString, SettingsFile(),
                     wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    return cfg.Read(key, def);
}

void Settings::WriteString(const wxString& key, const wxString& value)
{
    wxFileConfig cfg(wxEmptyString, wxEmptyString, SettingsFile(),
                     wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
    cfg.Write(key, value);
    cfg.Flush();
}

} // namespace core
