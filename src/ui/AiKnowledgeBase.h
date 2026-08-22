// AiKnowledgeBase.h — a per-(connection, database) schema "knowledge base" built once
// when the user selects a database in the AI chat page, then reused as grounding
// context for every chat turn (so the model never re-learns the schema per message).
//
// Built in two passes (this is the "对抗式 / adversarial" part the product asked for):
//   1. Introspect — ListTables + GetColumns + GetForeignKeys → compact table
//      signatures + an explicit FK/relationship map (ground truth from the DB).
//   2. Adversarial self-check — the model is shown the schema and asked to state its
//      understanding of the entities and how tables relate, and to flag anything
//      ambiguous. We cross-check its claims against the real metadata, correct
//      contradictions, and keep the VERIFIED narrative. This catches table/column/
//      relationship hallucinations before they poison SQL generation.
//
// Persisted (encrypted) via AiKnowledgeStore: the FIRST successful build is written
// to <UserDataDir>/kb/<hash>.dat, and later Build() calls reload that cache in a
// blink — no dedicated connection, no worker, no LLM — unless the caller forces a
// rebuild ("更新知识库"). The blocking introspection runs on a
// core::CrashLog::GuardedThread; the adversarial LLM pass goes through ai::AiClient
// (whose callbacks fire on the GUI thread). Progress + completion are marshalled back
// to the GUI thread.
#pragma once

#include <wx/string.h>

#include <functional>
#include <memory>

namespace ai { class AiClient; }
namespace db { class IConnection; }

namespace ui {

// The assembled grounding context for one database.
struct AiKnowledge {
    wxString database;
    wxString schemaText;       // compact per-table signatures (cols/types/PK/FK)
    wxString relationships;    // verified relationship narrative (adversarial pass output)
    bool     ready = false;    // true once both passes succeeded
    wxString error;            // non-empty if the build failed (chat still usable, no grounding)
};

class AiKnowledgeBase {
public:
    AiKnowledgeBase();
    ~AiKnowledgeBase();

    // Build (or rebuild) the KB for `db` over a DEDICATED connection whose ownership
    // this call TAKES. The connection is used ONLY by the internal introspection
    // worker thread and destroyed there when the pass ends — it must never be shared
    // with the UI (DB drivers are not thread-safe), so the caller hands off a fresh
    // connection it built on the GUI thread. `client` drives the adversarial pass
    // (may be nullptr → introspection-only, no self-check). `onProgress` (GUI thread)
    // receives status lines like "读取表结构…"; `onDone` (GUI thread) delivers the
    // finished AiKnowledge. Safe to call again to rebuild; a prior in-flight build is
    // abandoned (its own dedicated connection is torn down with its worker).
    //
    // onProgress carries (status, percent): percent 0-100 drives a determinate bar;
    // -1 means "unknown / keep the current value". Introspection reports real,
    // countable progress (0→70% across the N tables); the single LLM verify pass
    // has no sub-steps and reports 70% on entry, 100% on completion.
    //
    // `key` identifies this (connection, database) for the on-disk cache. When
    // `forceRebuild` is false and a ready cache exists, Build loads it instantly and
    // returns WITHOUT touching `dedicated` (the caller pre-checks the same cache so a
    // hit never even opens a connection — on a hit `dedicated` may legitimately be
    // null). On a miss / forced rebuild the full introspection+LLM flow runs and, on
    // success, the result is written back to the cache.
    void Build(std::unique_ptr<db::IConnection> dedicated, const wxString& db,
               const wxString& key, ai::AiClient* client, bool forceRebuild,
               std::function<void(const wxString& status, int percent)> onProgress,
               std::function<void(const AiKnowledge&)>                   onDone);

    // The current knowledge (ready==false until a Build completes).
    const AiKnowledge& Current() const { return kb_; }
    bool Ready() const { return kb_.ready; }

    // Abandon any in-flight build (called on panel teardown / db switch).
    void Cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AiKnowledge           kb_;
};

} // namespace ui
