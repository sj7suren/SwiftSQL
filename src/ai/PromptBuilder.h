// PromptBuilder.h — turns a use-case + assembled context + the user's words into the
// messages[] we send to the model. Pure & unit-testable (no I/O). This is where the
// "fast, good prompt" lives: fixed system/few-shot templates (0-cost to assemble,
// and cacheable by the provider's prefix cache) + only the schema that's needed.
//
// See docs/design/ai-integration.md §5.
#pragma once

#include "ai/AiTypes.h"

#include "db/DbDriver.h"   // db::Dialect

#include <vector>

namespace ai {

// What the user is asking the AI to do. Drives which template + which context slots.
enum class UseCase {
    Nl2Sql,     // 自然语言 → SQL（对话窗口主功能）
    DbScript,   // 生成数据库脚本（建表/建库/DDL 批量）
    SqlPerf,    // SQL 性能分析 / 优化 / 索引建议
    Explain,    // 解释一段 SQL
    Diagnose,   // 报错诊断修复
    Transform,  // 跨方言转换
};

// The context the ui layer assembles (via AiContextBuilder) and hands to the prompt.
// All fields optional; the template pulls only what it declares it needs. schemaText
// is a COMPACT signature (one line per table: cols/types/PK/FK), never full CREATE,
// and is empty when the privacy gate (AiSettings::includeSchema) is off.
struct AiContext {
    db::Dialect dialect = db::Dialect::MySQL;
    wxString    dialectName;    // "MySQL 8.0" / "PostgreSQL" …
    wxString    database;       // current database (context only)
    wxString    schemaText;     // compact table signatures, or "" (gate off / none)
    wxString    selectedSql;    // SQL under cursor/selection (Explain/SqlPerf/Transform)
    wxString    explainText;    // raw EXPLAIN output (SqlPerf)
    wxString    errorText;      // engine error message (Diagnose)
    wxString    targetDialect;  // for Transform: destination dialect name
};

// Build the messages for a use case. `userText` is the natural-language ask (may be
// empty for right-click Explain/SqlPerf where the SQL is the whole input).
std::vector<AiMessage> BuildMessages(UseCase useCase,
                                     const AiContext& ctx,
                                     const wxString& userText);

} // namespace ai
