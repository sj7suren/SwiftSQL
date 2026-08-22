// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiKnowledgeStore.cpp — see header. JSON-serialise the four durable AiKnowledge
// fields, encrypt the whole blob with core::EncryptSecret, and store it as a base64
// token file under <UserDataDir>/kb/<hash>.dat. Load reverses that. We reuse the
// AI layer's dependency-free ai::Json so string escaping (schema text carries
// newlines, arrows, parens) is handled for us.
#include "ui/AiKnowledgeStore.h"

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <cstdint>

#include "ai/Json.h"
#include "core/Secret.h"

namespace ui {

namespace {

// <UserDataDir>/kb, created on demand. Mirrors core::Secret's UserDataDir helper.
wxString KbDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir()
                 + wxFileName::GetPathSeparator() + L"kb";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

// FNV-1a 64-bit over the key's UTF-8 bytes → a 16-hex-digit filename. Hashing (vs.
// sanitising) guarantees a legal, fixed-length name for ANY key — connection labels
// and database names may contain path-illegal characters, spaces or non-ASCII.
wxString SafeName(const wxString& key)
{
    const wxScopedCharBuffer utf8 = key.utf8_str();
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < utf8.length(); ++i) {
        h ^= static_cast<unsigned char>(utf8.data()[i]);
        h *= 1099511628211ULL;
    }
    return wxString::Format(L"%016llx.dat", static_cast<unsigned long long>(h));
}

wxString FilePath(const wxString& key)
{
    return KbDir() + wxFileName::GetPathSeparator() + SafeName(key);
}

} // namespace

bool AiKnowledgeStore::Save(const wxString& key, const AiKnowledge& kb)
{
    ai::JsonWriter w;
    w.BeginObject();
    w.Key(L"v").Int(1);
    w.Key(L"database").Str(kb.database);
    w.Key(L"schemaText").Str(kb.schemaText);
    w.Key(L"relationships").Str(kb.relationships);
    w.Key(L"ready").Bool(kb.ready);
    w.EndObject();

    const wxString token = core::EncryptSecret(w.str());
    if (token.IsEmpty()) return false;   // encryption unavailable / empty blob

    wxFile f(FilePath(key), wxFile::write);
    return f.IsOpened() && f.Write(token, wxConvUTF8);
}

bool AiKnowledgeStore::Load(const wxString& key, AiKnowledge& out)
{
    const wxString path = FilePath(key);
    if (!wxFileName::FileExists(path)) return false;

    wxString token;
    {
        wxFile f(path);
        if (!f.IsOpened() || !f.ReadAll(&token, wxConvUTF8)) return false;
    }
    token.Trim(true).Trim(false);

    const wxString json = core::DecryptSecret(token);
    if (json.IsEmpty()) return false;    // wrong user / tampered / corrupt

    wxString err;
    ai::JsonValue root = ai::JsonValue::Parse(json, &err);
    if (!root.isObject()) return false;

    out.database      = root[L"database"].asString();
    out.schemaText    = root[L"schemaText"].asString();
    out.relationships = root[L"relationships"].asString();
    out.ready         = root[L"ready"].asBool();
    out.error.clear();
    return true;
}

} // namespace ui
