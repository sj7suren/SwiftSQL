// prompt_builder_test.cpp — unit tests for ai::BuildMessages, the pure template
// assembler. Same dependency-free harness as tableio_test.cpp: a tiny assert loop,
// no doctest/Catch2. BuildMessages is a pure function over an AiContext + strings,
// so we assert on message count, role order (System must lead), presence of the
// load-bearing instruction words, and the two schema paths (gate on / off).
//
// Links swiftsql::ai (PromptBuilder.cpp) + swiftsql::db (db::Dialect lives there).
#include "ai/PromptBuilder.h"

#include <cstdio>
#include <vector>
#include <wx/string.h>

using namespace ai;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

// role sequence starts with System, ends with User — the cache-friendly shape.
static bool FirstIsSystem(const std::vector<AiMessage>& m)
{
    return !m.empty() && m.front().role == Role::System;
}
static bool LastIsUser(const std::vector<AiMessage>& m)
{
    return !m.empty() && m.back().role == Role::User;
}

// Does any message contain the substring?
static bool AnyHas(const std::vector<AiMessage>& m, const wxString& needle)
{
    for (const auto& msg : m)
        if (msg.content.Find(needle) != wxNOT_FOUND) return true;
    return false;
}
static bool SystemHas(const std::vector<AiMessage>& m, const wxString& needle)
{
    return FirstIsSystem(m) && m.front().content.Find(needle) != wxNOT_FOUND;
}
static bool LastHas(const std::vector<AiMessage>& m, const wxString& needle)
{
    return LastIsUser(m) && m.back().content.Find(needle) != wxNOT_FOUND;
}

static AiContext MakeCtx(db::Dialect d, const wxString& name)
{
    AiContext c;
    c.dialect     = d;
    c.dialectName = name;
    return c;
}

int main()
{
    std::printf("== PromptBuilder unit tests ==\n");

    const wxString kSchema =
        L"orders(id PK, user_id ->users.id, amount DECIMAL, created_at DATETIME)\n"
        L"users(id PK, name VARCHAR, city VARCHAR)";

    // ------------------------------------------------------------ Nl2Sql (schema on)
    {
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0");
        c.schemaText = kSchema;
        const wxString ask = L"上个月金额超过1000的订单，带上下单人的名字";
        auto m = BuildMessages(UseCase::Nl2Sql, c, ask);

        // System + 1 few-shot pair (User/Assistant) + real User = 4 messages
        ExpectTrue("nl2sql: 4 messages (system+fewshot+user)", m.size() == 4);
        ExpectTrue("nl2sql: first is System", FirstIsSystem(m));
        ExpectTrue("nl2sql: few-shot user is 2nd",
                   m.size() == 4 && m[1].role == Role::User);
        ExpectTrue("nl2sql: few-shot assistant is 3rd",
                   m.size() == 4 && m[2].role == Role::Assistant);
        ExpectTrue("nl2sql: last is User", LastIsUser(m));
        ExpectTrue("nl2sql: system names dialect", SystemHas(m, L"MySQL 8.0"));
        ExpectTrue("nl2sql: system says SQL 专家", SystemHas(m, L"SQL 专家"));
        ExpectTrue("nl2sql: system forbids fence", SystemHas(m, L"代码围栏"));
        ExpectTrue("nl2sql: system says 不要解释", SystemHas(m, L"不要解释"));
        ExpectTrue("nl2sql: schema injected", AnyHas(m, L"数据库结构:"));
        ExpectTrue("nl2sql: schema text present", LastHas(m, L"orders(id PK"));
        ExpectTrue("nl2sql: userText in last User msg", LastHas(m, ask));
        // MySQL few-shot uses LIMIT idiom
        ExpectTrue("nl2sql: mysql few-shot uses LIMIT", AnyHas(m, L"LIMIT 5"));
    }

    // ------------------------------------------------------------ Nl2Sql (schema off)
    {
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0"); // schemaText empty
        const wxString ask = L"列出所有用户";
        auto m = BuildMessages(UseCase::Nl2Sql, c, ask);

        ExpectTrue("nl2sql(no schema): still 4 messages", m.size() == 4);
        ExpectTrue("nl2sql(no schema): first is System", FirstIsSystem(m));
        ExpectTrue("nl2sql(no schema): no schema block", !AnyHas(m, L"数据库结构:"));
        ExpectTrue("nl2sql(no schema): userText still last", LastHas(m, ask));
        // last User is exactly the ask (no schema prefix)
        ExpectTrue("nl2sql(no schema): last User == ask",
                   LastIsUser(m) && m.back().content == ask);
    }

    // few-shot dialect idioms
    {
        AiContext c = MakeCtx(db::Dialect::SqlServer, L"SQL Server 2022");
        auto m = BuildMessages(UseCase::Nl2Sql, c, L"x");
        ExpectTrue("nl2sql: sqlserver few-shot uses TOP", AnyHas(m, L"SELECT TOP 5"));
    }
    {
        AiContext c = MakeCtx(db::Dialect::Oracle, L"Oracle 19c");
        auto m = BuildMessages(UseCase::Nl2Sql, c, L"x");
        ExpectTrue("nl2sql: oracle few-shot uses FETCH FIRST",
                   AnyHas(m, L"FETCH FIRST 5 ROWS ONLY"));
    }
    // fallback dialect name from the enum when dialectName is empty
    {
        AiContext c; c.dialect = db::Dialect::Postgres; // dialectName left empty
        auto m = BuildMessages(UseCase::Nl2Sql, c, L"x");
        ExpectTrue("nl2sql: enum fallback name PostgreSQL",
                   SystemHas(m, L"PostgreSQL"));
    }

    // ------------------------------------------------------------ DbScript
    {
        AiContext c = MakeCtx(db::Dialect::Postgres, L"PostgreSQL 16");
        c.schemaText = kSchema;
        auto m = BuildMessages(UseCase::DbScript, c, L"建一个博客系统的表");
        ExpectTrue("dbscript: 2 messages", m.size() == 2);
        ExpectTrue("dbscript: first is System", FirstIsSystem(m));
        ExpectTrue("dbscript: last is User", LastIsUser(m));
        ExpectTrue("dbscript: system demands DDL", SystemHas(m, L"DDL"));
        ExpectTrue("dbscript: system demands semicolon rule", SystemHas(m, L"分号结尾"));
        ExpectTrue("dbscript: schema injected", LastHas(m, L"现有数据库结构:"));
        ExpectTrue("dbscript: requirement in last User", LastHas(m, L"博客系统"));
        // PG is not a block dialect → no manual-exec warning
        ExpectTrue("dbscript(pg): no PL/SQL warning", !SystemHas(m, L"手动执行"));
    }
    {
        // Oracle IS a block dialect → System must warn about manual execution
        AiContext c = MakeCtx(db::Dialect::Oracle, L"Oracle 19c");
        auto m = BuildMessages(UseCase::DbScript, c, L"建个带触发器的表");
        ExpectTrue("dbscript(oracle): warns manual exec", SystemHas(m, L"手动执行"));
        ExpectTrue("dbscript(oracle): mentions PL/SQL block", SystemHas(m, L"PL/SQL"));
    }

    // ------------------------------------------------------------ SqlPerf
    {
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0");
        c.schemaText  = kSchema;
        c.selectedSql = L"SELECT * FROM orders WHERE amount > 1000";
        c.explainText = L"-> Table scan on orders (cost=101)";
        auto m = BuildMessages(UseCase::SqlPerf, c, L"");
        ExpectTrue("sqlperf: 2 messages", m.size() == 2);
        ExpectTrue("sqlperf: first is System", FirstIsSystem(m));
        ExpectTrue("sqlperf: reads execution plan", SystemHas(m, L"解读执行计划"));
        ExpectTrue("sqlperf: rewrite instruction", SystemHas(m, L"重写"));
        ExpectTrue("sqlperf: CREATE INDEX advice", SystemHas(m, L"CREATE INDEX"));
        ExpectTrue("sqlperf: selected SQL injected", LastHas(m, L"待分析 SQL:"));
        ExpectTrue("sqlperf: explain injected", LastHas(m, L"执行计划(EXPLAIN):"));
        ExpectTrue("sqlperf: explain text present", LastHas(m, L"Table scan"));
    }
    {
        // explainText empty → the EXPLAIN slot is simply omitted
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0");
        c.selectedSql = L"SELECT 1";
        auto m = BuildMessages(UseCase::SqlPerf, c, L"");
        ExpectTrue("sqlperf(no explain): no EXPLAIN slot",
                   !AnyHas(m, L"执行计划(EXPLAIN):"));
        ExpectTrue("sqlperf(no explain): still has SQL", LastHas(m, L"待分析 SQL:"));
    }

    // ------------------------------------------------------------ Explain
    {
        AiContext c = MakeCtx(db::Dialect::Sqlite, L"SQLite 3");
        c.selectedSql = L"SELECT u.name FROM users u JOIN orders o ON o.user_id=u.id";
        auto m = BuildMessages(UseCase::Explain, c, L"");
        ExpectTrue("explain: 2 messages", m.size() == 2);
        ExpectTrue("explain: first is System", FirstIsSystem(m));
        ExpectTrue("explain: says 逐句解释", SystemHas(m, L"逐句解释"));
        ExpectTrue("explain: SQL injected", LastHas(m, L"待解释 SQL:"));
        ExpectTrue("explain: SQL text present", LastHas(m, L"JOIN orders"));
    }

    // ------------------------------------------------------------ Diagnose
    {
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0");
        c.selectedSql = L"SELECT nam FROM users";
        c.errorText   = L"Unknown column 'nam' in 'field list'";
        auto m = BuildMessages(UseCase::Diagnose, c, L"");
        ExpectTrue("diagnose: 2 messages", m.size() == 2);
        ExpectTrue("diagnose: first is System", FirstIsSystem(m));
        ExpectTrue("diagnose: asks failure cause", SystemHas(m, L"失败原因"));
        ExpectTrue("diagnose: asks corrected SQL", SystemHas(m, L"修正"));
        ExpectTrue("diagnose: failed SQL injected", LastHas(m, L"失败 SQL:"));
        ExpectTrue("diagnose: error text injected", LastHas(m, L"报错信息:"));
        ExpectTrue("diagnose: error message present", LastHas(m, L"Unknown column"));
    }

    // ------------------------------------------------------------ Transform
    {
        AiContext c = MakeCtx(db::Dialect::MySQL, L"MySQL 8.0");
        c.selectedSql    = L"SELECT * FROM t LIMIT 10";
        c.targetDialect  = L"Oracle";
        auto m = BuildMessages(UseCase::Transform, c, L"");
        ExpectTrue("transform: 2 messages", m.size() == 2);
        ExpectTrue("transform: first is System", FirstIsSystem(m));
        ExpectTrue("transform: names source dialect", SystemHas(m, L"MySQL 8.0"));
        ExpectTrue("transform: names target dialect", SystemHas(m, L"Oracle"));
        ExpectTrue("transform: only-output constraint", SystemHas(m, L"只输出"));
        ExpectTrue("transform: source SQL injected", LastHas(m, L"源 SQL"));
        ExpectTrue("transform: SQL text present", LastHas(m, L"LIMIT 10"));
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
