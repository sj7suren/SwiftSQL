// ConversationStore.cpp — see header. JSON-serialise a Conversation, encrypt the
// whole blob with core::EncryptSecret, and store it as a base64 token file under
// <UserDataDir>/chats/<id>.dat. Load reverses that. We reuse the AI layer's
// dependency-free ai::Json so string escaping (message text carries newlines,
// arrows, code fences) is handled for us.
//
// Timestamps are stored as decimal strings so they survive any long/double width
// difference (time_t is 64-bit, long is 32-bit on MSVC) with no precision loss.
#include "ui/ConversationStore.h"

#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <algorithm>

#include "ai/Json.h"
#include "core/Secret.h"

namespace ui {

namespace {

// <UserDataDir>/chats, created on demand. Mirrors AiKnowledgeStore's KbDir().
wxString ChatsDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir()
                 + wxFileName::GetPathSeparator() + L"chats";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString FilePath(const wxString& id)
{
    return ChatsDir() + wxFileName::GetPathSeparator() + id + L".dat";
}

// A conversation id is only ever a decimal integer (see NextId). Reject anything
// else so a stray file in the directory can't be treated as an id.
bool IsNumericId(const wxString& s)
{
    if (s.IsEmpty()) return false;
    for (wxUniChar c : s) if (c < '0' || c > '9') return false;
    return true;
}

// Read + decrypt + parse one .dat file into a JsonValue. Returns false on any error.
bool ReadDoc(const wxString& path, ai::JsonValue& root)
{
    if (!wxFileName::FileExists(path)) return false;
    wxString token;
    {
        wxFile f(path);
        if (!f.IsOpened() || !f.ReadAll(&token, wxConvUTF8)) return false;
    }
    token.Trim(true).Trim(false);
    const wxString json = core::DecryptSecret(token);
    if (json.IsEmpty()) return false;
    root = ai::JsonValue::Parse(json);
    return root.isObject();
}

long long ToLL(const wxString& s)
{
    long long v = 0;
    return s.ToLongLong(&v) ? v : 0;
}

} // namespace

std::vector<ConvMeta> ConversationStore::List()
{
    std::vector<ConvMeta> out;
    const wxString dir = ChatsDir();
    wxDir d(dir);
    if (!d.IsOpened()) return out;

    wxString name;
    for (bool more = d.GetFirst(&name, L"*.dat", wxDIR_FILES); more;
         more = d.GetNext(&name)) {
        wxFileName fn(name);
        const wxString id = fn.GetName();
        if (!IsNumericId(id)) continue;

        ai::JsonValue root;
        if (!ReadDoc(dir + wxFileName::GetPathSeparator() + name, root)) continue;

        ConvMeta m;
        m.id        = id;
        m.title     = root[L"title"].asString();
        m.updatedAt = ToLL(root[L"updatedAt"].asString());
        out.push_back(std::move(m));
    }

    std::sort(out.begin(), out.end(),
              [](const ConvMeta& a, const ConvMeta& b) { return a.updatedAt > b.updatedAt; });
    return out;
}

bool ConversationStore::Load(const wxString& id, Conversation& out)
{
    if (!IsNumericId(id)) return false;
    ai::JsonValue root;
    if (!ReadDoc(FilePath(id), root)) return false;

    Conversation c;
    c.id        = id;
    c.title     = root[L"title"].asString();
    c.createdAt = ToLL(root[L"createdAt"].asString());
    c.updatedAt = ToLL(root[L"updatedAt"].asString());

    const ai::JsonValue& msgs = root[L"messages"];
    for (size_t i = 0; i < msgs.size(); ++i) {
        ConvMessage m;
        m.role = msgs[i][L"role"].asString();
        m.text = msgs[i][L"text"].asString();
        if (m.role.IsEmpty()) continue;
        c.messages.push_back(std::move(m));
    }

    out = std::move(c);
    return true;
}

void ConversationStore::Save(const Conversation& c)
{
    if (c.id.IsEmpty() || c.messages.empty()) return;   // nothing durable to write

    ai::JsonWriter w;
    w.BeginObject();
    w.Key(L"v").Int(1);
    w.Key(L"id").Str(c.id);
    w.Key(L"title").Str(c.title);
    w.Key(L"createdAt").Str(wxString::Format(L"%lld", c.createdAt));
    w.Key(L"updatedAt").Str(wxString::Format(L"%lld", c.updatedAt));
    w.Key(L"messages").BeginArray();
    for (const ConvMessage& m : c.messages) {
        w.BeginObject();
        w.Key(L"role").Str(m.role);
        w.Key(L"text").Str(m.text);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();

    const wxString token = core::EncryptSecret(w.str());
    if (token.IsEmpty()) return;   // encryption unavailable

    wxFile f(FilePath(c.id), wxFile::write);
    if (f.IsOpened()) f.Write(token, wxConvUTF8);
}

void ConversationStore::Delete(const wxString& id)
{
    if (!IsNumericId(id)) return;
    const wxString path = FilePath(id);
    if (wxFileName::FileExists(path)) wxRemoveFile(path);
}

wxString ConversationStore::NextId()
{
    long long maxId = 0;
    const wxString dir = ChatsDir();
    wxDir d(dir);
    if (d.IsOpened()) {
        wxString name;
        for (bool more = d.GetFirst(&name, L"*.dat", wxDIR_FILES); more;
             more = d.GetNext(&name)) {
            const wxString id = wxFileName(name).GetName();
            if (!IsNumericId(id)) continue;
            maxId = std::max(maxId, ToLL(id));
        }
    }
    return wxString::Format(L"%lld", maxId + 1);
}

} // namespace ui
