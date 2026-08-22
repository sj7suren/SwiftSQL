// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// PromptBuilder.cpp — the pure, unit-testable assembly of a use-case + AiContext
// + the user's words into messages[]. No I/O, no wx GUI, no provider knowledge.
//
// Design intent (docs/design/ai-integration.md §5): every template is
//   [ System (fixed role+rules+dialect+output-constraint), (optional few-shot), User ]
// The *fixed* text (System, few-shot) comes first so it can hit the provider's
// prefix cache (Anthropic prompt caching / OpenAI prefix caching); the *variable*
// parts (schema signature, selected SQL, user's words) come last. Assembly is
// nothing but string concatenation — ~0ms — which is the whole point.
//
// schemaText is a COMPACT signature and is empty when the privacy gate is off; in
// that case every template still produces a working prompt, just without that slot.
#include "ai/PromptBuilder.h"

#include <wx/string.h>

namespace ai {

namespace {

// Human-facing dialect name for the System line. Prefer the ui-supplied pretty
// name ("MySQL 8.0" / "PostgreSQL 16"); fall back to a stable name from the enum
// so the builder is still correct when called with a bare ctx (e.g. in tests).
wxString DialectName(const AiContext& ctx)
{
    if (!ctx.dialectName.empty()) return ctx.dialectName;
    switch (ctx.dialect) {
        case db::Dialect::MySQL:     return L"MySQL";
        case db::Dialect::Postgres:  return L"PostgreSQL";
        case db::Dialect::Sqlite:    return L"SQLite";
        case db::Dialect::SqlServer: return L"SQL Server";
        case db::Dialect::Oracle:    return L"Oracle";
    }
    return L"SQL";
}

// Oracle / SQL Server carry PL/SQL / T-SQL blocks that this repo's SplitSqlScript
// cannot auto-execute (design §6.1 D4) — DbScript must warn the user accordingly.
bool IsBlockDialect(const AiContext& ctx)
{
    return ctx.dialect == db::Dialect::Oracle
        || ctx.dialect == db::Dialect::SqlServer;
}

void Add(std::vector<AiMessage>& out, Role role, const wxString& content)
{
    out.push_back(AiMessage{ role, content });
}

// Append a labelled section to a User body, blank-line separated, skipping empties
// so a closed privacy gate (schemaText == "") simply omits that slot.
void AppendSection(wxString& body, const wxString& label, const wxString& value)
{
    if (value.empty()) return;
    if (!body.empty()) body += L"\n\n";
    body += label;
    body += L"\n";
    body += value;
}

// One dialect-aware few-shot for Nl2Sql: pins the "only SQL, no fence" output shape
// AND the dialect's row-limit idiom (LIMIT vs TOP vs FETCH FIRST), which is the most
// common thing a model gets wrong when told only the dialect name.
void AddNl2SqlFewShot(std::vector<AiMessage>& out, const AiContext& ctx)
{
    wxString answer;
    switch (ctx.dialect) {
        case db::Dialect::SqlServer:
            answer = L"SELECT TOP 5 name FROM users ORDER BY created_at DESC;";
            break;
        case db::Dialect::Oracle:
            answer = L"SELECT name FROM users ORDER BY created_at DESC "
                     L"FETCH FIRST 5 ROWS ONLY;";
            break;
        default: // MySQL / Postgres / SQLite all use LIMIT
            answer = L"SELECT name FROM users ORDER BY created_at DESC LIMIT 5;";
            break;
    }
    // Non-query message → chat, NOT SQL (so "你好" doesn't get answered with SQL).
    Add(out, Role::User, L"你好");
    Add(out, Role::Assistant,
        L"你好！我是 SwiftSQL 的数据库管理员（DBA），可以帮你编写和优化 SQL、"
        L"设计表结构、管理这个数据库。你想做什么？");
    // Query request → bare SQL (also pins the dialect's row-limit idiom).
    Add(out, Role::User, L"最近注册的 5 个用户的名字");
    Add(out, Role::Assistant, answer);
}

// ---- per-use-case builders -------------------------------------------------

std::vector<AiMessage> BuildNl2Sql(const AiContext& ctx, const wxString& userText)
{
    const wxString dn = DialectName(ctx);
    std::vector<AiMessage> out;

    Add(out, Role::System,
        L"你是 SwiftSQL 里一位资深的 " + dn +
        L" 数据库管理员（DBA），精通 SQL 编写与优化、表结构设计和数据库管理。\n"
        L"· 当用户要求编写或修改 SQL 查询时：只输出一条可执行的 " + dn +
        L" SQL，不要解释、不要 markdown 代码围栏，只用给定的表和列，含糊时选最可能的理解。\n"
        L"· 当用户只是问候、闲聊或提出与具体查询无关的一般性问题时：以专业 DBA 的身份、"
        L"用用户的语言正常对话回复，不要输出 SQL。\n"
        L"· 当用问题用流程、状态或实体关系图能更清晰地解释时（如讲解表关系、查询流程、"
        L"状态流转），可用 ```mermaid 代码块输出一个 flowchart（如 flowchart TD / "
        L"flowchart LR，节点用 [ ] { } ( )，边用 --> 或 -->|标签|），我方会自动渲染成图。"
        L"仅在图确实更有助于理解时使用，不要滥用；日常问答和写 SQL 仍按前述规则。");

    AddNl2SqlFewShot(out, ctx);

    wxString body;
    AppendSection(body, L"数据库结构:", ctx.schemaText);
    if (!body.empty()) body += L"\n\n";
    body += userText;   // the natural-language ask — always last (variable tail)
    Add(out, Role::User, body);
    return out;
}

std::vector<AiMessage> BuildDbScript(const AiContext& ctx, const wxString& userText)
{
    const wxString dn = DialectName(ctx);
    std::vector<AiMessage> out;

    wxString sys =
        L"你是 " + dn + L" 的数据库工程师。请输出完整、可执行的 " + dn +
        L" DDL 脚本：每条语句以分号结尾，不要解释、不要 markdown 代码围栏。";
    if (IsBlockDialect(ctx))
        sys += L" 注意：本工具不能自动执行 PL/SQL / T-SQL 语句块，"
               L"这类脚本仅生成脚本文本，请用户手动执行。";
    Add(out, Role::System, sys);

    wxString body;
    AppendSection(body, L"现有数据库结构:", ctx.schemaText);
    AppendSection(body, L"需求:", userText);
    Add(out, Role::User, body);
    return out;
}

std::vector<AiMessage> BuildSqlPerf(const AiContext& ctx, const wxString& userText)
{
    const wxString dn = DialectName(ctx);
    std::vector<AiMessage> out;

    Add(out, Role::System,
        L"你是 " + dn + L" 的 SQL 性能优化专家。请分点输出：\n"
        L"1. 解读执行计划（指出瓶颈：全表扫描 / 排序 / 临时表等）；\n"
        L"2. 给出重写后的 SQL；\n"
        L"3. 给出 CREATE INDEX 索引建议。\n"
        L"用中文回答，SQL 用代码块呈现。");

    wxString body;
    AppendSection(body, L"数据库结构:", ctx.schemaText);
    AppendSection(body, L"待分析 SQL:", ctx.selectedSql);
    AppendSection(body, L"执行计划(EXPLAIN):", ctx.explainText);
    AppendSection(body, L"补充说明:", userText);
    Add(out, Role::User, body);
    return out;
}

std::vector<AiMessage> BuildExplain(const AiContext& ctx, const wxString& userText)
{
    const wxString dn = DialectName(ctx);
    std::vector<AiMessage> out;

    Add(out, Role::System,
        L"你是 " + dn + L" 的 SQL 专家。请用中文逐句解释下面这段 SQL 的含义与作用，"
        L"按语句 / 子句拆解说明，不要改写 SQL。");

    wxString body;
    AppendSection(body, L"数据库结构:", ctx.schemaText);
    AppendSection(body, L"待解释 SQL:", ctx.selectedSql);
    AppendSection(body, L"补充说明:", userText);
    Add(out, Role::User, body);
    return out;
}

std::vector<AiMessage> BuildDiagnose(const AiContext& ctx, const wxString& userText)
{
    const wxString dn = DialectName(ctx);
    std::vector<AiMessage> out;

    Add(out, Role::System,
        L"你是 " + dn + L" 的 SQL 诊断专家。下面的 SQL 执行失败了。请：\n"
        L"1. 指出失败原因；\n"
        L"2. 给出修正后的可执行 SQL。\n"
        L"用中文回答。");

    wxString body;
    AppendSection(body, L"数据库结构:", ctx.schemaText);
    AppendSection(body, L"失败 SQL:", ctx.selectedSql);
    AppendSection(body, L"报错信息:", ctx.errorText);
    AppendSection(body, L"补充说明:", userText);
    Add(out, Role::User, body);
    return out;
}

std::vector<AiMessage> BuildTransform(const AiContext& ctx, const wxString& userText)
{
    const wxString dn  = DialectName(ctx);
    const wxString tgt = ctx.targetDialect.empty() ? L"目标方言" : ctx.targetDialect;
    std::vector<AiMessage> out;

    Add(out, Role::System,
        L"你是 SQL 方言转换专家。请把下面的 " + dn + L" SQL 等价转换为 " + tgt +
        L" SQL。只输出转换后的 " + tgt +
        L" SQL，不要解释、不要 markdown 代码围栏。");

    wxString body;
    AppendSection(body, L"源 SQL (" + dn + L"):", ctx.selectedSql);
    AppendSection(body, L"补充说明:", userText);
    Add(out, Role::User, body);
    return out;
}

} // namespace

std::vector<AiMessage> BuildMessages(UseCase useCase,
                                     const AiContext& ctx,
                                     const wxString& userText)
{
    switch (useCase) {
        case UseCase::Nl2Sql:    return BuildNl2Sql(ctx, userText);
        case UseCase::DbScript:  return BuildDbScript(ctx, userText);
        case UseCase::SqlPerf:   return BuildSqlPerf(ctx, userText);
        case UseCase::Explain:   return BuildExplain(ctx, userText);
        case UseCase::Diagnose:  return BuildDiagnose(ctx, userText);
        case UseCase::Transform: return BuildTransform(ctx, userText);
    }
    return {}; // unreachable for a valid enum; keeps the compiler quiet
}

} // namespace ai
