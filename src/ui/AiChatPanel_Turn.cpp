// AiChatPanel_Turn.cpp — the "run a chat turn + paint the transcript" half of
// AiChatPanel, split into its own translation unit to keep AiChatPanel.cpp under the
// 1000-line charter limit (same class, cross-TU member definitions — mirrors the
// MainFrame_*.cpp / AiChatPanel_Conversations.cpp split).
//
// Responsibilities:
//   • SendTurn                — ground the prompt with the KB, stream the reply, route SQL
//   • FlushStreamingReply     — coalesced push of curReply_ into the streaming bubble
//   • RouteGeneratedSql       — classify SELECT (run) vs write (insert, don't run)
//
// The transcript is a virtualized, self-drawn ChatTranscript (no child windows); this TU
// drives it via AddMessage / BeginAiStream / SetStreamText / FinishStream.
//
// The SQL-classification helpers below live in this TU's anonymous namespace because
// SendTurn / RouteGeneratedSql are their only users (AiSqlCompleter.cpp keeps its own,
// independent DialectName — both have internal linkage, so there is no conflict).
#include "ui/AiChatPanel.h"

#include <wx/textctrl.h>

#include <algorithm>
#include <memory>

#include "ai/AiClient.h"
#include "ai/AiConfig.h"
#include "ai/AiConfigStore.h"
#include "ai/AiTypes.h"
#include "ai/PromptBuilder.h"
#include "db/DbDriver.h"
#include "db/SqlScript.h"       // db::BuildExplainSql (EXPLAIN, never ANALYZE)
#include "ui/AiChatWidgets.h"   // complete IconCircleButton (send/stop button Enable)
#include "ui/ChatTranscript.h"
#include "ui/ConversationStore.h"
#include "ui/I18n.h"
#include "ui/MermaidChat.h"     // ScanMermaid / RenderMermaidBlocks for inline diagrams

namespace ui {

namespace {

// Human-readable dialect name fed into the prompt (ai::AiContext::dialectName).
wxString DialectName(db::Dialect d)
{
    switch (d) {
    case db::Dialect::MySQL:     return L"MySQL";
    case db::Dialect::Postgres:  return L"PostgreSQL";
    case db::Dialect::Sqlite:    return L"SQLite";
    case db::Dialect::SqlServer: return L"SQL Server";
    case db::Dialect::Oracle:    return L"Oracle";
    }
    return L"SQL";
}

// The set of leading keywords that mark a statement as a READ (routed to runQuery).
// Everything else that is a recognised statement is treated as a WRITE.
bool IsReadKeyword(const wxString& kw)
{
    return kw == L"SELECT" || kw == L"WITH" || kw == L"SHOW" ||
           kw == L"EXPLAIN" || kw == L"DESC" || kw == L"DESCRIBE";
}

// Any keyword we recognise as the start of a SQL statement. Used to decide whether an
// un-fenced model reply is bare SQL (vs prose we should leave alone).
bool IsSqlKeyword(const wxString& kw)
{
    if (IsReadKeyword(kw)) return true;
    return kw == L"INSERT" || kw == L"UPDATE" || kw == L"DELETE" ||
           kw == L"CREATE" || kw == L"ALTER"  || kw == L"DROP"   ||
           kw == L"TRUNCATE" || kw == L"REPLACE" || kw == L"MERGE" ||
           kw == L"GRANT"  || kw == L"REVOKE" || kw == L"SET"    ||
           kw == L"CALL"   || kw == L"USE"    || kw == L"BEGIN"  ||
           kw == L"COMMIT" || kw == L"PRAGMA" || kw == L"VALUES";
}

// The first word of `sql`, upper-cased, skipping leading whitespace, line comments
// (-- …), block comments (/* … */) and stray punctuation (e.g. a wrapping "(SELECT …)").
wxString FirstKeyword(const wxString& sql)
{
    const size_t n = sql.length();
    size_t i = 0;
    while (i < n) {
        const wxUniChar c = sql[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(') { ++i; continue; }
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {          // line comment
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {          // block comment
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        size_t j = i;
        while (j < n) {
            const wxUniChar d = sql[j];
            const bool word = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                              (d >= '0' && d <= '9') || d == '_';
            if (!word) break;
            ++j;
        }
        if (j == i) { ++i; continue; }   // punctuation — skip and keep scanning
        return sql.Mid(i, j - i).Upper();
    }
    return wxString();
}

// Extract the SQL body from a model reply: prefer a ```sql fenced block (any ``` fence),
// else treat the whole reply as SQL when it starts with a recognised statement keyword.
// Returns "" when the reply carries no SQL (pure prose).
wxString ExtractSql(const wxString& reply)
{
    const size_t fence = reply.find(L"```");
    if (fence != wxString::npos) {
        size_t p = fence + 3;
        const size_t nl = reply.find(L'\n', p);
        if (nl != wxString::npos) p = nl + 1;      // drop the ```lang tag line
        const size_t end = reply.find(L"```", p);
        wxString body = (end != wxString::npos) ? reply.Mid(p, end - p) : reply.Mid(p);
        body.Trim(true).Trim(false);
        return body;
    }
    wxString trimmed = reply;
    trimmed.Trim(true).Trim(false);
    if (IsSqlKeyword(FirstKeyword(trimmed))) return trimmed;
    return wxString();
}

} // namespace

// ---------------------------------------------------------------------------- turns

// ---------------------------------------------------------------------------
// SQL 性能分析 — plan first, then the model.
// ---------------------------------------------------------------------------
wxString AiChatPanel::FetchExplain(const wxString& sql, wxString& why)
{
    db::IConnection* conn = SelectedConnection();
    if (!conn) { why = tr(L"未选择数据库连接，本次分析没有执行计划"); return wxString(); }

    // EXPLAIN only — see db::BuildExplainSql. An empty list means either the
    // statement has no plan (DDL/SET) or this engine has no non-executing plan
    // facility; both are reported rather than silently producing a thin answer.
    const std::vector<wxString> stmts = db::BuildExplainSql(sql, conn->GetDialect());
    if (stmts.empty()) {
        why = tr(L"该语句无法获取执行计划（仅 SELECT/INSERT/UPDATE/DELETE 可以），本次仅按 SQL 与表结构分析");
        return wxString();
    }

    wxBusyCursor busy;   // synchronous: a plan query is small
    wxString out, err;
    for (size_t i = 0; i < stmts.size(); ++i) {
        db::QueryResult r;
        if (!conn->Execute(stmts[i], r, err)) {
            why = tr(L"获取执行计划失败：") + err;
            return wxString();
        }
        // Only the LAST statement returns the plan (Oracle's first one stores it).
        if (i + 1 != stmts.size()) continue;
        for (const auto& c : r.columns) { if (!out.IsEmpty() && !out.EndsWith(L"\n")) out += L"\t"; out += c; }
        out += L"\n";
        for (const auto& row : r.rows) {
            for (size_t k = 0; k < row.size(); ++k) { if (k) out += L"\t"; out += row[k]; }
            out += L"\n";
        }
    }
    if (out.Trim().IsEmpty()) why = tr(L"执行计划为空");
    return out;
}

void AiChatPanel::AnalyzeSql(const wxString& sql)
{
    wxString s = sql;
    s.Trim(true).Trim(false);
    if (s.IsEmpty()) return;

    wxString why;
    const wxString plan = FetchExplain(s, why);
    if (!why.IsEmpty())   // say WHY the analysis is thinner than usual
        transcript_->AddMessage(ChatTranscript::Kind::System, tr(L"提示"), why);

    SendTurn(wxString(), ai::UseCase::SqlPerf, s, plan);
}

void AiChatPanel::SendTurn(const wxString& userText, ai::UseCase useCase,
                           const wxString& selectedSql, const wxString& explainText)
{
    db::IConnection* conn = SelectedConnection();
    if (!conn) return;

    // Pick up any provider change made in Preferences since the last turn BEFORE
    // reading the config below. Without it client_ would still point at the previous
    // endpoint while req carried the NEW provider's model — the exact mismatch that
    // used to require closing and reopening the window. No-op when nothing changed.
    SyncProviderFromSettings();

    const ai::AiSettings settings = ai::AiConfigStore::Load();
    const ai::AiProviderConfig* prov = settings.DefaultProvider();
    if (!prov || !client_) return;

    // The user bubble shows what was ASKED. For a performance analysis nobody
    // typed anything, so echo the statement under analysis instead of an empty
    // bubble — otherwise the transcript reads as if the AI answered nothing.
    transcript_->AddMessage(ChatTranscript::Kind::User, tr(L"你"),
                            useCase == ai::UseCase::SqlPerf
                                ? (tr(L"性能分析：\n") + selectedSql)
                                : userText);

    ai::AiContext ctx;
    ctx.dialect     = conn->GetDialect();
    ctx.dialectName = DialectName(ctx.dialect);
    ctx.database    = SelectedDatabase();
    ctx.selectedSql = selectedSql;    // read by the SqlPerf/Explain templates
    ctx.explainText = explainText;    // read by SqlPerf; empty is tolerated
    if (!kb_.Current().schemaText.IsEmpty()) {
        ctx.schemaText = kb_.Current().schemaText;
        if (!kb_.Current().relationships.IsEmpty())
            ctx.schemaText += L"\n" + kb_.Current().relationships;
    }

    // One live AI bubble; show a "思考中…" placeholder until the first token replaces it.
    transcript_->BeginAiStream(L"AI");
    transcript_->SetStreamText(tr(L"思考中…"));

    ai::AiRequest req;
    req.model       = currentModel_.IsEmpty() ? prov->model : currentModel_;
    req.messages    = ai::BuildMessages(useCase, ctx, userText);
    req.temperature = prov->temperature;
    req.maxTokens   = prov->maxTokens;
    req.stream      = prov->streaming;

    curReply_.clear();

    // --- busy on ---
    busy_ = true;
    ShowBusyButtons(true);
    sendBtn_->Enable(false);
    stopBtn_->Enable(true);
    input_->Enable(false);
    connChoice_->Enable(false);
    dbChoice_->Enable(false);

    ai::AiCallbacks cb;
    cb.onDelta = [this](const wxString& delta) {     // GUI thread (AiHttp guarantees it)
        // Cheap here: just accumulate + mark dirty. The 80ms streamTimer_ does the one
        // expensive relayout-of-the-streaming-bubble per tick, not per token — no stutter.
        // The first non-empty flush replaces the "思考中…" placeholder automatically.
        curReply_ += delta;
        streamDirty_ = true;
        if (!streamTimer_.IsRunning()) streamTimer_.Start(80);
    };
    cb.onDone = [this, userText](int, int) {
        streamTimer_.Stop();
        streamDirty_ = true;
        FlushStreamingReply();               // paint the final, complete reply
        if (curReply_.IsEmpty())             // display-only fallback (route on curReply_)
            transcript_->SetStreamText(L"(No response)");

        // Detect ```mermaid blocks in the finished reply. When present, show the reply
        // WITHOUT the fenced source (only prose + the rendered image) while the RAW reply
        // — with the mermaid source intact — is what gets persisted, so reopening the
        // conversation re-renders. SQL routing runs on the display text so a mermaid
        // block is never mistaken for SQL. Done BEFORE FinishStream (SetStreamText needs
        // the live stream) and BEFORE RouteGeneratedSql (so the diagram lands on this AI
        // bubble, not a later system message).
        wxString routeText = curReply_;
        std::vector<wxBitmap> diagrams;
        if (!curReply_.IsEmpty()) {
            const MermaidScan scan = ScanMermaid(curReply_);
            if (!scan.blocks.empty()) {
                routeText = scan.displayText;
                transcript_->SetStreamText(scan.displayText);   // may be empty → image only
                const int mw = std::max(120,
                    static_cast<int>(transcript_->GetClientSize().x * 0.8));
                diagrams = RenderMermaidBlocks(scan.blocks, mw);
            }
        }
        transcript_->FinishStream();
        for (size_t k = 0; k < diagrams.size(); ++k) {
            if (k == 0) transcript_->SetLastDiagram(diagrams[k]);  // primary → AI bubble
            else        transcript_->AddDiagram(diagrams[k]);      // extras → image rows
        }
        RouteGeneratedSql(routeText);
        // Persist this completed turn (both messages) and refresh the drawer. Only
        // finished turns are stored — an aborted / errored turn leaves current_ intact.
        // curReply_ keeps the mermaid source so a reopen can re-render the diagram.
        current_.messages.push_back(ConvMessage{L"user", userText});
        current_.messages.push_back(ConvMessage{L"ai", curReply_});
        SaveCurrentConversation();
        EnsureDrawerShown();   // first saved turn reveals the (previously hidden) drawer
        busy_ = false;
        ShowBusyButtons(false);
        sendBtn_->Enable(hasProvider_);
        stopBtn_->Enable(false);
        input_->Enable(true);
        connChoice_->Enable(true);
        dbChoice_->Enable(true);
    };
    cb.onError = [this](const ai::AiError& e) {
        streamTimer_.Stop();
        transcript_->SetStreamText(wxEmptyString);   // drop the placeholder / partial
        transcript_->FinishStream();
        transcript_->AddMessage(ChatTranscript::Kind::Error, L"Error", e.message);
        busy_ = false;
        ShowBusyButtons(false);
        sendBtn_->Enable(hasProvider_);
        stopBtn_->Enable(false);
        input_->Enable(true);
        connChoice_->Enable(true);
        dbChoice_->Enable(true);
    };
    client_->Chat(req, cb);
}

// Push curReply_ into the streaming bubble — the single (throttled) relayout step,
// done at most once per streamTimer_ tick instead of once per streamed token. The
// control re-lays-out only the streaming message + the y-cascade after it, then repaints
// just the visible bubbles, so this stays cheap regardless of transcript length.
void AiChatPanel::FlushStreamingReply()
{
    if (!streamDirty_) return;
    streamDirty_ = false;
    transcript_->SetStreamText(curReply_);
}

void AiChatPanel::RouteGeneratedSql(const wxString& sql)
{
    const wxString extracted = ExtractSql(sql);
    if (extracted.IsEmpty()) return;   // pure prose — the streamed text is the answer

    const wxString kw = FirstKeyword(extracted);
    if (IsReadKeyword(kw)) {
        if (host_.runQuery) host_.runQuery(extracted);
        transcript_->AddMessage(ChatTranscript::Kind::System, L"SwiftSQL",
                                tr(L"已在查询页执行该查询"));
    } else {
        if (host_.insertToEditor) host_.insertToEditor(extracted);
        transcript_->AddMessage(ChatTranscript::Kind::System, L"SwiftSQL",
                                tr(L"这是写操作，已插入编辑器，请你自行检查后执行"));
    }
}

} // namespace ui
