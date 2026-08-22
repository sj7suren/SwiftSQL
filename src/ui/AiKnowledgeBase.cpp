// AiKnowledgeBase.cpp — two-pass "adversarial self-check" schema knowledge base.
//
// Pass 1 (introspection, off the GUI thread): ListTables → per-table GetColumns +
// GetForeignKeys → a compact per-table signature (schemaText) plus a ground-truth
// FK/relationship listing (fkList) and the raw FK edges we later verify against.
//
// Pass 2 (adversarial self-check, GUI thread via ai::AiClient, stream=false): the
// model is shown the schema + FK list and asked, in Chinese, to state each table's
// responsibility and how tables relate, and to flag anything ambiguous. We then
// cross-check every relationship the model asserts against the REAL foreign keys:
// claims backed by an actual FK edge are kept verbatim; claims with no supporting
// FK (or that reference a table absent from the database) are annotated
// "（未在外键中确认）". The verified narrative + an authoritative FK appendix become
// AiKnowledge::relationships. Any failure keeps the schemaText so chat can still
// degrade to a schema-only grounding.
//
// Lifetime: a per-build shared_ptr<atomic<bool>> generation guard makes the
// worker-thread CallAfter and the AiClient callbacks no-op once the KB is torn down
// or a newer Build() supersedes them. Re-invoking Build() abandons the prior build
// (and cancels its in-flight LLM request so the new pass isn't a no-op).
#include "ui/AiKnowledgeBase.h"

#include <wx/app.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ai/AiClient.h"
#include "ai/AiTypes.h"
#include "core/CrashLog.h"
#include "db/DbDriver.h"
#include "ui/AiKnowledgeStore.h"
#include "ui/I18n.h"

namespace ui {

namespace {

// Beyond this many tables we emit names only (a full signature for hundreds of
// tables would dominate the prompt and risk a context overflow) — mirrors the
// budget in AiContextBuilder so the two grounding paths agree.
constexpr size_t kMaxTablesFull = 200;

// "varchar" + "80" → "varchar(80)"; "" length → the bare base type. Length may
// already carry precision+scale ("10,2") — kept verbatim inside the parens.
wxString TypeText(const db::ColumnInfo& c)
{
    if (c.length.IsEmpty()) return c.type;
    return c.type + L"(" + c.length + L")";
}

// ---- ground truth carried from pass 1 (worker) to pass 2 (GUI thread) ----
struct Introspected {
    wxString                    schemaText;   // compact per-table signatures
    wxString                    fkList;       // human-readable FK listing
    std::vector<db::ForeignKey> realFks;      // authoritative edges (for verify)
    std::vector<wxString>       tableNames;   // for "table exists?" checks
    wxString                    error;        // non-empty → introspection failed
    bool                        truncated = false;
};

// ---- pass 1: blocking introspection (runs on a GuardedThread) ----
// `onStep(done, total)` is invoked (on the WORKER thread) after each table is
// probed so the caller can report real, countable progress. It must NOT touch any
// GUI object directly — the caller marshals it to the GUI thread.
Introspected Introspect(db::IConnection* conn, const wxString& database,
                        const std::function<void(int done, int total)>& onStep)
{
    Introspected r;
    if (!conn) { r.error = tr(L"数据库连接不可用"); return r; }

    std::vector<db::TableInfo> tables;
    if (!conn->ListTables(database, tables, r.error)) return r;

    for (const db::TableInfo& t : tables) r.tableNames.push_back(t.name);

    // Over budget → bare names, no per-table probing (keeps the prompt bounded).
    if (tables.size() > kMaxTablesFull) {
        r.truncated = true;
        wxString names;
        for (const db::TableInfo& t : tables) names << t.name << L" ";
        names.Trim(true);
        r.schemaText = tr(L"（结构过大已截断，仅列出表名）") + wxString(L"\n") + names;
        r.fkList     = tr(L"（表数量过多，未提取外键关系）");
        return r;
    }

    wxString schema;
    wxString fkText;
    const int total = static_cast<int>(tables.size());
    int done = 0;
    for (const db::TableInfo& t : tables) {
        std::vector<db::ColumnInfo> cols;
        wxString e;
        if (!conn->GetColumns(database, t.name, cols, e)) { if (onStep) onStep(++done, total); continue; }  // best-effort

        std::vector<db::ForeignKey> fks;
        conn->GetForeignKeys(database, t.name, fks, e);              // best-effort

        wxString line = t.name + L"(";
        bool first = true;
        for (const db::ColumnInfo& c : cols) {
            if (!first) line << L", ";
            first = false;
            line << c.name << L" " << TypeText(c);
            if (c.key == L"PK") line << L" PK";
        }
        for (const db::ForeignKey& fk : fks) {
            if (!first) line << L", ";
            first = false;
            line << fk.fromColumn << L" →" << fk.toTable << L"." << fk.toColumn;
        }
        line << L")";
        schema << line << L"\n";

        for (const db::ForeignKey& fk : fks) {
            fkText << fk.fromTable << L"." << fk.fromColumn << L" → "
                   << fk.toTable   << L"." << fk.toColumn   << L"\n";
            r.realFks.push_back(fk);
        }
        if (onStep) onStep(++done, total);
    }
    schema.Trim(true);
    fkText.Trim(true);

    r.schemaText = schema;
    r.fkList     = fkText.IsEmpty() ? tr(L"（该库未声明外键约束）") : fkText;
    return r;
}

// ---- pass 2 helpers: parse the model's prose and verify it vs. real FKs ----

// A qualified reference "table.column" pulled out of a model line.
struct QRef { wxString table; wxString column; };

bool IsIdentChar(wxUniChar c)
{
    const auto v = c.GetValue();
    if (v == '_')               return true;
    if (v >= '0' && v <= '9')   return true;
    if (v >= 'A' && v <= 'Z')   return true;
    if (v >= 'a' && v <= 'z')   return true;
    return v > 127;   // non-ASCII (e.g. a Chinese table name) counts as ident
}

bool IsQuoteChar(wxUniChar c)
{
    const auto v = c.GetValue();
    return v == '`' || v == '"' || v == '\'' || v == '[' || v == ']';
}

// Extract every "ident.ident" reference from a line. Quote/bracket chars are
// skipped without breaking a reference (so `t`.`c` and [t].[c] still parse); any
// other separator (space, arrow, comma…) ends the current run.
std::vector<QRef> ExtractRefs(const wxString& line)
{
    std::vector<QRef> out;
    const size_t n = line.length();
    size_t i = 0;
    wxString last;
    bool haveLast = false;
    bool dot      = false;
    while (i < n) {
        wxUniChar c = line.GetChar(i);
        if (IsIdentChar(c)) {
            wxString tok;
            while (i < n && IsIdentChar(line.GetChar(i))) { tok << line.GetChar(i); ++i; }
            if (dot && haveLast) out.push_back({ last, tok });
            last = tok; haveLast = true; dot = false;
        } else if (c == '.') {
            dot = true; ++i;
        } else if (IsQuoteChar(c)) {
            ++i;                       // ignore quoting, keep the pending state
        } else {
            haveLast = false; dot = false; ++i;
        }
    }
    return out;
}

// Order-independent key for a table pair, so an FK edge matches a claim stated in
// either direction. A self-referential FK collapses to "t\x1t".
std::wstring PairKey(const wxString& a, const wxString& b)
{
    std::wstring al = a.Lower().ToStdWstring();
    std::wstring bl = b.Lower().ToStdWstring();
    return (al <= bl) ? (al + L"\x1" + bl) : (bl + L"\x1" + al);
}

std::vector<wxString> SplitLines(const wxString& s)
{
    std::vector<wxString> out;
    wxString cur;
    for (size_t i = 0; i < s.length(); ++i) {
        wxUniChar c = s.GetChar(i);
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur << c;
    }
    out.push_back(cur);
    return out;
}

// Cross-check the model narrative against ground truth. Relationship-claim lines
// (an arrow with ≥2 qualified refs, or ≥2 distinct referenced tables) whose table
// pair has no supporting FK edge get an inline "（未在外键中确认）" note; a claim
// referencing a table absent from the database names it. Everything else — table
// responsibilities, the 疑点 section, headings — is kept verbatim. An authoritative
// FK appendix is appended so downstream chat always sees the real edges.
wxString VerifyNarrative(const wxString& modelText, const Introspected& g)
{
    // Ground-truth sets.
    std::set<std::wstring> tableSet;     // lowercased table names
    for (const wxString& t : g.tableNames) tableSet.insert(t.Lower().ToStdWstring());
    std::set<std::wstring> fkPairs;      // canonical table-pair keys
    for (const db::ForeignKey& fk : g.realFks) fkPairs.insert(PairKey(fk.fromTable, fk.toTable));

    wxString body;
    for (const wxString& raw : SplitLines(modelText)) {
        std::vector<QRef> refs = ExtractRefs(raw);

        std::set<std::wstring> distinct;
        for (const QRef& r : refs) distinct.insert(r.table.Lower().ToStdWstring());

        const bool hasArrow = raw.Contains(L"→") || raw.Contains(L"->");
        const bool isRel = (distinct.size() >= 2) || (hasArrow && refs.size() >= 2);

        if (!isRel) { body << raw << L"\n"; continue; }

        // Confirmed if any pair of referenced tables is a real FK edge (either
        // direction; self-edge included). Table-level match is intentionally
        // forgiving of small column mistakes — the relationship itself is real.
        bool confirmed = false;
        for (size_t i = 0; i < refs.size() && !confirmed; ++i)
            for (size_t j = i + 1; j < refs.size(); ++j)
                if (fkPairs.count(PairKey(refs[i].table, refs[j].table))) { confirmed = true; break; }

        if (confirmed) { body << raw << L"\n"; continue; }

        // Unconfirmed: collect any referenced tables that don't exist at all.
        std::vector<wxString> missing;
        std::set<std::wstring> seen;
        for (const QRef& r : refs) {
            std::wstring lc = r.table.Lower().ToStdWstring();
            if (tableSet.count(lc)) continue;
            if (seen.insert(lc).second) missing.push_back(r.table);
        }

        wxString note;
        if (missing.empty()) {
            note = tr(L"（未在外键中确认）");
        } else {
            wxString names;
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i) names << L"、";
                names << missing[i];
            }
            note = wxString::Format(tr(L"（未在外键中确认；表 %s 不在该库中）"), names);
        }
        body << raw << L" " << note << L"\n";
    }
    body.Trim(true);

    if (body.IsEmpty()) body = tr(L"（AI 未返回关系分析）");

    wxString out = body;
    out << L"\n\n" << tr(L"——— 数据库真实外键（权威，来自元数据）———") << L"\n" << g.fkList;
    return out;
}

// ---- pass 2 prompt ----
std::vector<ai::AiMessage> BuildAdversarialMessages(const Introspected& g)
{
    // Prompt content is deliberately literal Chinese (not tr): we want the model's
    // answer in Chinese regardless of the app's UI language, and the fixed
    // "源表.列 → 目标表.列" line format is what VerifyNarrative parses.
    ai::AiMessage sys;
    sys.role = ai::Role::System;
    sys.content =
        L"你是资深数据库架构分析师。基于给定的数据库结构，用简体中文说明各表的职责与表间关系，"
        L"并指出任何看起来矛盾、命名歧义或语义上应关联却缺少外键约束之处。"
        L"只依据所给结构推断，不要臆造未出现的表或列。";

    wxString u;
    u << L"数据库表结构（每行一张表：表名(列 类型 [PK], 外键列 →目标表.列)）：\n"
      << g.schemaText << L"\n\n"
      << L"已声明的外键关系：\n"
      << g.fkList << L"\n\n"
      << L"请严格按以下部分输出：\n"
      << L"## 表职责\n"
      << L"逐张表一行，格式「表名：职责说明」。\n"
      << L"## 表间关系\n"
      << L"每条关系单独一行，固定格式：\n"
      << L"- 关系：源表.列 → 目标表.列 —— 说明\n"
      << L"只列出你从结构中推断出的关系。\n"
      << L"## 疑点\n"
      << L"逐条列出任何矛盾、命名歧义或缺少外键但语义上应关联之处；若无则写「无」。";

    ai::AiMessage user;
    user.role = ai::Role::User;
    user.content = u;
    return { std::move(sys), std::move(user) };
}

// Drive the adversarial LLM pass. Callbacks fire on the GUI thread (AiHttp
// guarantees this) and are gated by `alive` so a torn-down / superseded KB is a
// no-op. `kb` and `chatInFlight` are references into the live AiKnowledgeBase; they
// are only touched after the alive check passes.
void RunAdversarialPass(AiKnowledge& kb,
                        std::shared_ptr<std::atomic<bool>> alive,
                        ai::AiClient* client, bool& chatInFlight,
                        const Introspected& g, const wxString& key,
                        const std::function<void(const wxString&, int)>& onProgress,
                        const std::function<void(const AiKnowledge&)>& onDone)
{
    // Enter the LLM verify phase at 70%. This one call has no sub-steps, so the
    // panel smoothly creeps the bar 70→~95 on a GUI-thread timer until we finish.
    if (onProgress) onProgress(tr(L"AI 正在校验表关系…"), 70);

    ai::AiRequest req;
    req.messages    = BuildAdversarialMessages(g);
    req.temperature = 0.2;         // low → stable, verifiable structure
    req.maxTokens   = 3072;
    req.stream      = false;       // one-shot; onDelta still carries the full text

    auto full = std::make_shared<wxString>();

    ai::AiCallbacks cb;
    cb.onDelta = [full](const wxString& delta) { *full << delta; };

    cb.onDone = [&kb, &chatInFlight, alive, g, key, full, onProgress, onDone](int, int) {
        if (!alive->load()) return;
        chatInFlight = false;
        kb.relationships = VerifyNarrative(*full, g);
        kb.ready = true;
        kb.error.clear();
        AiKnowledgeStore::Save(key, kb);   // persist the freshly-built KB (encrypted)
        if (onProgress) onProgress(tr(L"知识库已就绪"), 100);
        if (onDone) onDone(kb);
    };

    cb.onError = [&kb, &chatInFlight, alive, g, key, onProgress, onDone](const ai::AiError& e) {
        if (!alive->load()) return;
        chatInFlight = false;
        // Degrade: keep the schemaText already in kb and fall back to the authoritative
        // FK list. Chat can still be grounded by real metadata even if the AI self-check
        // failed, so this is usable knowledge rather than an incomplete KB.
        kb.relationships = g.fkList;
        kb.error = e.message;
        kb.ready = !kb.schemaText.IsEmpty();
        if (kb.ready) AiKnowledgeStore::Save(key, kb);   // usable (schema-grounded) → cache it
        if (onProgress) onProgress(tr(L"AI 校验失败，已降级为原始外键关系"), 100);
        if (onDone) onDone(kb);
    };

    chatInFlight = true;
    client->Chat(req, cb);
}

} // namespace

// ---- in-flight state ----
struct AiKnowledgeBase::Impl {
    std::shared_ptr<std::atomic<bool>> alive;         // current-generation guard
    ai::AiClient*                      client = nullptr;
    bool                               chatInFlight = false;
};

AiKnowledgeBase::AiKnowledgeBase() : impl_(std::make_unique<Impl>()) {}

AiKnowledgeBase::~AiKnowledgeBase() { Cancel(); }

void AiKnowledgeBase::Cancel()
{
    if (impl_->alive) impl_->alive->store(false);   // abandon in-flight callbacks
    // Abort OUR in-flight adversarial request so a follow-up Build()'s Chat() isn't
    // rejected as a no-op (AiClient serves one request at a time).
    if (impl_->chatInFlight && impl_->client) impl_->client->Cancel();
    impl_->chatInFlight = false;
    impl_->client = nullptr;
}

void AiKnowledgeBase::Build(std::unique_ptr<db::IConnection> dedicated, const wxString& db,
                            const wxString& key, ai::AiClient* client, bool forceRebuild,
                            std::function<void(const wxString& status, int percent)> onProgress,
                            std::function<void(const AiKnowledge&)>                   onDone)
{
    Cancel();   // drop any prior in-flight build

    // Fresh generation: a new guard so late callbacks from the old build no-op.
    impl_->alive        = std::make_shared<std::atomic<bool>>(true);
    impl_->client       = client;
    impl_->chatInFlight = false;
    auto alive = impl_->alive;

    kb_ = AiKnowledge{};
    kb_.database = db;

    // ---- cache fast-path -------------------------------------------------------
    // A previously-built, encrypted KB reloads instantly: NO dedicated connection,
    // NO worker thread, NO LLM round-trip — the cheapest possible path and zero
    // crash surface. The caller pre-checks the same cache and skips building a
    // connection on a hit (so `dedicated` is legitimately null here on a hit); this
    // in-Build check is the authoritative gate that also covers direct callers.
    if (!forceRebuild) {
        AiKnowledge cached;
        if (AiKnowledgeStore::Load(key, cached) && cached.ready) {
            dedicated.reset();          // never needed on a hit
            kb_ = cached;
            kb_.database = db;
            if (onProgress) onProgress(tr(L"知识库已就绪 ✓"), 100);
            if (onDone) onDone(kb_);
            return;
        }
    }

    if (onProgress) onProgress(tr(L"读取表结构…"), 0);

    if (!dedicated) {
        kb_.error = tr(L"数据库连接不可用");
        kb_.ready = false;
        if (onDone) onDone(kb_);
        return;
    }

    // Pass 1 off the GUI thread. The dedicated connection is MOVED into the worker,
    // which is its sole owner and toucher: it is used only by Introspect here and
    // destroyed exactly once when this lambda returns (on the worker) — after
    // introspection has finished with it, and regardless of success/failure/cancel.
    // Because no other thread ever touches this connection, the driver's non-thread-
    // safe buffers can't be raced (no heap corruption); because the UI holds its own
    // separate connection, closing that one can't dangle this one (no UAF). The GUI-
    // thread CallAfter below carries only the copied-out ground truth `g`, never the
    // connection, and is gated by `alive`.
    std::thread t = core::CrashLog::GuardedThread(
        L"AiKnowledgeBuild",
        [this, alive, dedicated = std::move(dedicated), db, key, client,
         onProgress, onDone]() mutable {
            // Per-table progress: introspection owns 0→70%. This lambda runs on the
            // WORKER thread, so it touches NOTHING GUI/shared — it only marshals a
            // computed percent to the GUI thread via CallAfter, guarded by `alive`.
            std::function<void(int, int)> onStep =
                [alive, onProgress](int done, int total) {
                    if (total <= 0 || !onProgress) return;
                    const int pct = static_cast<int>(
                        static_cast<long long>(done) * 70 / total);
                    wxTheApp->CallAfter([alive, onProgress, pct]() {
                        if (!alive->load()) return;   // KB gone / superseded
                        onProgress(tr(L"正在读取表结构…"), pct);
                    });
                };
            Introspected g = Introspect(dedicated.get(), db, onStep);
            wxTheApp->CallAfter(
                [this, alive, key, client, g = std::move(g), onProgress, onDone]() mutable {
                    if (!alive->load()) return;   // KB gone / superseded mid-build

                    kb_.schemaText = g.schemaText;

                    if (!g.error.IsEmpty()) {
                        kb_.error = g.error;
                        kb_.ready = false;
                        if (onProgress) onProgress(tr(L"读取表结构失败"), -1);
                        if (onDone) onDone(kb_);
                        return;
                    }

                    // No client → introspection-only: relationships are the raw
                    // (authoritative) FK list, no adversarial self-check. Nothing
                    // more to do, so the bar completes at 100%.
                    if (!client) {
                        kb_.relationships = g.fkList;
                        kb_.ready = true;
                        AiKnowledgeStore::Save(key, kb_);   // persist (encrypted)
                        if (onProgress) onProgress(tr(L"知识库已就绪"), 100);
                        if (onDone) onDone(kb_);
                        return;
                    }

                    RunAdversarialPass(kb_, alive, client, impl_->chatInFlight,
                                       g, key, onProgress, onDone);
                });
        });
    t.detach();
}

} // namespace ui
