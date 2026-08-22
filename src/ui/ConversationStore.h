// ConversationStore.h — encrypted, on-disk persistence for AI chat conversations.
//
// Each conversation is a small record: a stable id, a title (the first user message
// truncated), created/updated timestamps and an ordered list of {role, text} turns.
// It is serialised to JSON (via ai::JsonWriter), encrypted with core::EncryptSecret
// (the same DPAPI-backed AES-GCM used for API keys / connection passwords / the KB
// cache) and written as one file per conversation under <UserDataDir>/chats/<id>.dat.
//
// Mirrors AiKnowledgeStore's encrypted-blob approach. Ids are DETERMINISTIC (a
// monotonic "max existing numeric id + 1" scan), never time- or rand-based, so a
// build/test environment without a wall clock or RNG still produces unique, legal
// filenames.
#pragma once

#include <wx/string.h>

#include <vector>

namespace ui {

// One turn in a conversation. role is "user" or "ai".
struct ConvMessage {
    wxString role;
    wxString text;
};

// A full conversation record (durable).
struct Conversation {
    wxString                 id;          // stable, unique, legal filename stem
    wxString                 title;       // first user message, truncated (~24 chars)
    long long                createdAt = 0;  // unix seconds (0 = unset)
    long long                updatedAt = 0;  // unix seconds (0 = unset)
    std::vector<ConvMessage> messages;
};

// Lightweight listing entry (drawer rows) — avoids materialising every message.
struct ConvMeta {
    wxString  id;
    wxString  title;
    long long updatedAt = 0;
};

class ConversationStore {
public:
    // All stored conversations, newest first (by updatedAt desc). Unreadable /
    // corrupt files are silently skipped.
    static std::vector<ConvMeta> List();

    // Read + decrypt + deserialise `id` into `out`. Returns false when the file is
    // absent, decryption fails or the payload can't be parsed (out left untouched).
    static bool Load(const wxString& id, Conversation& out);

    // Serialise + encrypt + write. Empty-id or empty-message conversations are not
    // written (nothing to persist). Silent no-op on any failure — never throws.
    static void Save(const Conversation& c);

    // Remove the on-disk file for `id` (no-op if absent).
    static void Delete(const wxString& id);

    // A deterministic, unique id for a NEW conversation: one greater than the largest
    // existing numeric id (or "1" when the store is empty). No clock, no RNG.
    static wxString NextId();
};

} // namespace ui
