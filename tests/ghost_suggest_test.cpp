// ghost_suggest_test.cpp — unit tests for ui::ghost::LocalSkeleton, the LOCAL
// tier of the editor's grey inline suggestion.
//
// WHY THIS IS WORTH TESTING. The suggestion is drawn in grey and accepted with
// Tab, so a wrong answer is not merely unhelpful — it is one keystroke away
// from becoming real SQL in the user's buffer. The three rules that keep it out
// of trouble (nothing mid-line, nothing inside a literal or comment, nothing
// while a word is still being typed) are each a way that could happen, and each
// is pinned below with a case that FAILS if the rule is dropped.
//
// Harness: the same dependency-free assert loop as sqlscript_test.cpp; the
// single TU under test is compiled straight into this binary.
#include "ui/GhostSuggest.h"

#include <cstdio>
#include <wx/string.h>

using ui::ghost::LocalSkeleton;
using db::Dialect;

static int g_checks = 0;
static int g_fails  = 0;

static void Expect(const char* name, const wxString& before, const wxString& after,
                   const wxString& want)
{
    ++g_checks;
    const wxString got = LocalSkeleton(before, after, Dialect::MySQL);
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    before: [%s]\n    want: [%s]\n    got : [%s]\n",
                    name, (const char*)before.utf8_str(),
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

int main()
{
    std::printf("== ghost_suggest_test ==\n");

    // ---- the case this feature was asked for ------------------------------
    Expect("SELECT then space suggests ' * from '", L"select ", L"", L"* from ");
    Expect("upper-case SELECT too", L"SELECT ", L"", L"* from ");
    Expect("mixed case", L"SeLeCt ", L"", L"* from ");
    Expect("after a newline and indentation", L"-- note\nselect ", L"", L"* from ");

    // ---- the rest of the skeleton table ------------------------------------
    Expect("INSERT -> into", L"insert ", L"", L"into ");
    Expect("DELETE -> from", L"delete ", L"", L"from ");
    Expect("ORDER -> by",   L"select * from t order ", L"", L"by ");
    Expect("GROUP -> by",   L"select * from t group ", L"", L"by ");
    Expect("LEFT -> join",  L"select * from a left ", L"", L"join ");
    Expect("CREATE -> table", L"create ", L"", L"table ");
    Expect("DROP -> table",   L"drop ", L"", L"table ");
    Expect("IS -> null",      L"select * from t where c is ", L"", L"null");

    // ---- RULE: nothing while a word is still being typed -------------------
    // The keyword popup owns that moment. Drop this rule and both hints appear
    // at once, fighting over the same Tab key.
    Expect("no space yet -> the popup owns it, stay quiet", L"select", L"", L"");
    Expect("half-typed word -> quiet", L"sele", L"", L"");

    // ---- RULE: nothing mid-line -------------------------------------------
    // Suggesting here would propose inserting into the middle of a finished
    // statement, which is never what the user meant.
    Expect("text after the caret -> quiet", L"select ", L"* from orders", L"");
    Expect("even a single token after -> quiet", L"select ", L"x", L"");
    Expect("trailing whitespace after the caret is still end-of-line",
           L"select ", L"   ", L"* from ");

    // ---- RULE: nothing inside a literal or a comment -----------------------
    // Accepting ghost SQL inside 'a literal' would corrupt the literal.
    Expect("inside a single-quoted literal -> quiet",
           L"select 'order ", L"", L"");
    Expect("inside a double-quoted literal -> quiet",
           L"select \"order ", L"", L"");
    Expect("inside a backquoted identifier -> quiet",
           L"select `order ", L"", L"");
    Expect("after a -- line comment -> quiet",
           L"select * from t -- order ", L"", L"");
    Expect("after a # line comment -> quiet",
           L"select * from t # order ", L"", L"");
    Expect("inside a /* block comment -> quiet",
           L"/* order ", L"", L"");
    // ...and the negative controls: a CLOSED literal/comment must NOT silence it,
    // or the feature would go dead for the rest of any statement containing a
    // string — which is most of them.
    Expect("a CLOSED literal does not silence the rest of the line",
           L"select 'x' , c from t where a='b' order ", L"", L"by ");
    Expect("a CLOSED block comment does not silence it",
           L"/* header */ select ", L"", L"* from ");
    Expect("a line comment ENDED by a newline does not silence it",
           L"-- header\nselect ", L"", L"* from ");
    Expect("'' escaped quote inside a literal keeps it closed",
           L"select 'it''s' from t order ", L"", L"by ");

    // ---- pairs that already consumed their continuation ---------------------
    Expect("ORDER BY does not then suggest for BY", L"... order by ", L"", L"");
    Expect("GROUP BY likewise", L"... group by ", L"", L"");
    Expect("INSERT INTO -> the table name is the user's", L"insert into ", L"", L"");
    Expect("DELETE FROM likewise", L"delete from ", L"", L"");
    Expect("LEFT JOIN -> the table name is the user's", L"select * from a left join ",
           L"", L"");
    Expect("CREATE TABLE -> the name is the user's", L"create table ", L"", L"");

    // ---- nothing to say ----------------------------------------------------
    Expect("empty buffer", L"", L"", L"");
    Expect("whitespace only", L"   ", L"", L"");
    Expect("an ordinary identifier is not a keyword", L"select * from orders ", L"", L"");
    Expect("ORDERS is not ORDER", L"select * from orders ", L"", L"");
    Expect("punctuation tail", L"select * from t where (", L"", L"");

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
