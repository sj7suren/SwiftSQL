// SyncRoutineTreeTests.cpp — ui::MakeRoutineCategoryNode / BuildDiffTree's
// routine half (src/ui/SyncDiffModel.{h,cpp}, ADR-013): the pure model behind
// the 函数 / 存储过程 categories the compare grid renders.
//
// WHAT THIS FILE IS GUARDING
// The user asked for 「同步时应该弹出两边不同的表，不同的函数，存储过程」 and the
// standing CEO ruling is that routines may be SHOWN and never applied. The data
// layer enforces the second half structurally (db::sync::RoutineDiffSet has no
// DDL field and is not part of SyncPlan). This file pins the UI's half of it:
//
//   * a Routine node never carries an `id` and is never `checkable`, which are
//     the two things ui::SyncSelection needs to be able to hold check state for
//     something. No id, no ui::TableKey, no entry in the ExecutionSpec.
//   * a DEGRADED read (permission denied / unsupported engine) renders the
//     category with its 不可见 label and ZERO children — never a half list,
//     because "the target is missing 40 functions" read off a catalog we could
//     not see is a false claim rather than an incomplete one.
//   * the body row's `differs` flag — which is the detail pane's HIGHLIGHT
//     authorization — is true for a same-engine BodyDiffers and false for a
//     cross-engine NotComparable. Highlighting two bodies written in different
//     procedural languages would assert a difference the data layer explicitly
//     declines to claim.
//   * an unreadable body renders as ui::UnreadableSide(), never as empty text:
//     two hidden bodies must not look identical.
//
// Same dependency-free harness as SyncDiffModelTests.cpp. Compiles
// SyncDiffModel.cpp directly and links swiftsql::db (for CompareRoutines); no
// wxWindow, no GUI.
#include "ui/SyncDiffModel.h"

#include "db/RoutineCompare.h"

#include <cstdio>
#include <vector>
#include <wx/init.h>
#include <wx/string.h>

using namespace ui;
using namespace db::sync;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectEq(const char* name, long long got, long long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%lld got=%lld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// ---- builders ---------------------------------------------------------------

static RoutineDef Fn(const wxString& name, const wxString& body,
                     const wxString& lang = L"SQL",
                     RoutineKind kind = RoutineKind::Function)
{
    RoutineDef r;
    r.schema   = L"app";
    r.name     = name;
    r.kind     = kind;
    r.language = lang;
    r.body     = body;
    r.argTypes.push_back(L"int");
    return r;
}

static const DiffNode* Category(const DiffTree& tree, DiffCategory cat)
{
    for (const auto& root : tree.roots)
        if (root->kind == DiffNodeKind::Category && root->category == cat) return root.get();
    return nullptr;
}

static const DiffNode* Leaf(const DiffNode& routine, const wxString& label)
{
    for (const auto& c : routine.children)
        if (c->kind == DiffNodeKind::ValueDiff && c->label == label) return c.get();
    return nullptr;
}

// ---- tests ------------------------------------------------------------------

// Same engine, one changed body, one identical routine, one procedure only on
// the source. Two categories, differences only, counts stated.
static void TestSameEngineCategories()
{
    std::vector<RoutineDef> src, tgt;
    src.push_back(Fn(L"f_changed", L"BEGIN RETURN 1; END"));
    tgt.push_back(Fn(L"f_changed", L"BEGIN RETURN 2; END"));
    src.push_back(Fn(L"f_same", L"BEGIN RETURN 0; END"));
    tgt.push_back(Fn(L"f_same", L"BEGIN RETURN 0; END"));
    src.push_back(Fn(L"p_only_src", L"BEGIN END", L"SQL", RoutineKind::Procedure));

    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::MySQL, db::Dialect::MySQL);
    const RoutineDiffSet rd =
        CompareRoutines(src, tgt, RoutineReadStatus::Ok, RoutineReadStatus::Ok, opt);

    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);

    const DiffNode* fn = Category(tree, DiffCategory::Functions);
    const DiffNode* pr = Category(tree, DiffCategory::Procedures);
    ExpectTrue("functions category exists", fn != nullptr);
    ExpectTrue("procedures category exists", pr != nullptr);
    if (!fn || !pr) return;

    // Only the DIFFERENCE becomes a child; the identical routine is accounted
    // for in the summary so an empty category is never ambiguous.
    ExpectEq("functions: only the differing one is a child", (long long)fn->children.size(), 1);
    ExpectEq("procedures: the source-only one is a child", (long long)pr->children.size(), 1);
    ExpectTrue("function child is a Routine node",
               fn->children[0]->kind == DiffNodeKind::Routine);
    ExpectTrue("summary states both totals", fn->summary.Contains(L"共 2 项") &&
                                             fn->summary.Contains(L"差异 1 项"));

    // The whole point of the round: the grid can render 状态 for a routine.
    ExpectStr("body diff verdict label", fn->children[0]->status, L"函数体不同");
    ExpectTrue("verdict carried structurally",
               fn->children[0]->verdict == RoutineVerdict::BodyDiffers);

    // Same engine => the body row MAY be highlighted.
    const DiffNode* body = Leaf(*fn->children[0], RoutineBodyRowLabel());
    ExpectTrue("body leaf exists", body != nullptr);
    if (body) {
        ExpectTrue("same-engine body diff is highlighted", body->differs);
        ExpectStr("target body is the left side", body->before, L"BEGIN RETURN 2; END");
        ExpectStr("source body is the right side", body->after, L"BEGIN RETURN 1; END");
    }

    // A source-only procedure: op reads as an addition and the target side is
    // a stated absence rather than an empty string.
    const DiffNode& only = *pr->children[0];
    ExpectTrue("source-only is an Add", only.op == DiffOp::Add);
    ExpectStr("source-only verdict", only.status, L"仅源端存在");
    const DiffNode* sig = Leaf(only, L"签名");
    ExpectTrue("signature leaf exists", sig != nullptr);
    if (sig) ExpectStr("absent target side is stated", sig->before, AbsentSide());
}

// THE CEO BOUNDARY, as far as this layer can pin it: nothing under a routine
// category can acquire check state, because nothing there has an identity for
// ui::SyncSelection to key on.
static void TestRoutinesAreNotSelectable()
{
    std::vector<RoutineDef> src, tgt;
    src.push_back(Fn(L"f_a", L"BEGIN RETURN 1; END"));
    tgt.push_back(Fn(L"f_b", L"BEGIN RETURN 1; END"));

    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::MySQL, db::Dialect::MySQL);
    const RoutineDiffSet rd =
        CompareRoutines(src, tgt, RoutineReadStatus::Ok, RoutineReadStatus::Ok, opt);
    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);

    int nodes = 0;
    for (DiffCategory cat : { DiffCategory::Functions, DiffCategory::Procedures }) {
        const DiffNode* c = Category(tree, cat);
        if (!c) continue;
        ExpectTrue("category is not checkable", !c->checkable);
        ExpectTrue("category has no id", c->id.IsEmpty());
        for (const auto& r : c->children) {
            ++nodes;
            ExpectTrue("routine is not checkable", !r->checkable);
            ExpectTrue("routine has no id", r->id.IsEmpty());
            ExpectTrue("routine has no rowKey", r->rowKey.IsEmpty());
            for (const auto& leaf : r->children) {
                ExpectTrue("routine leaf has no id", leaf->id.IsEmpty());
                ExpectTrue("routine leaf has no rowKey", leaf->rowKey.IsEmpty());
                ExpectTrue("routine leaf is not checkable", !leaf->checkable);
            }
        }
    }
    ExpectTrue("the two one-sided routines were rendered", nodes == 2);
}

// Cross engine: the bodies are shown but NOT judged, so the highlight flag must
// be off even though the two texts obviously differ.
static void TestCrossEngineBodiesAreNotHighlighted()
{
    std::vector<RoutineDef> src, tgt;
    src.push_back(Fn(L"f_calc", L"BEGIN RETURN 1; END", L"SQL"));
    tgt.push_back(Fn(L"f_calc", L"BEGIN RETURN 2; END", L"plpgsql"));

    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::MySQL, db::Dialect::Postgres);
    const RoutineDiffSet rd =
        CompareRoutines(src, tgt, RoutineReadStatus::Ok, RoutineReadStatus::Ok, opt);
    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);

    const DiffNode* fn = Category(tree, DiffCategory::Functions);
    ExpectTrue("cross-engine functions category exists", fn != nullptr);
    if (!fn || fn->children.empty()) { ExpectTrue("cross-engine pair rendered", false); return; }

    const DiffNode& r = *fn->children[0];
    ExpectTrue("cross-engine verdict is NotComparable",
               r.verdict == RoutineVerdict::NotComparable);
    ExpectStr("cross-engine verdict label", r.status, L"无法比较");

    const DiffNode* body = Leaf(r, RoutineBodyRowLabel());
    ExpectTrue("cross-engine body leaf exists", body != nullptr);
    if (body) {
        ExpectTrue("cross-engine bodies are NOT highlighted", !body->differs);
        // Both are still SHOWN — that is the deliverable.
        ExpectStr("target body shown as-is", body->before, L"BEGIN RETURN 2; END");
        ExpectStr("source body shown as-is", body->after, L"BEGIN RETURN 1; END");
    }
    // The two languages are visible, which is what makes 无法比较 legible.
    const DiffNode* lang = Leaf(r, L"语言");
    ExpectTrue("language row exists", lang != nullptr);
    if (lang) ExpectTrue("languages differ", lang->differs);

    ExpectTrue("category says cross-engine", fn->summary.Contains(L"跨引擎"));
}

// A side we could not read: label the category, show NOTHING under it.
static void TestPermissionDeniedRendersNoChildren()
{
    std::vector<RoutineDef> src, tgt;
    tgt.push_back(Fn(L"f_target_side", L"BEGIN END"));

    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::MySQL, db::Dialect::MySQL);
    RoutineDiffSet rd = CompareRoutines(src, tgt, RoutineReadStatus::PermissionDenied,
                                        RoutineReadStatus::Ok, opt);
    rd.sourceStatusDetail = L"SELECT command denied";

    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);
    for (DiffCategory cat : { DiffCategory::Functions, DiffCategory::Procedures }) {
        const DiffNode* c = Category(tree, cat);
        ExpectTrue("degraded category exists", c != nullptr);
        if (!c) continue;
        ExpectEq("degraded category has zero children", (long long)c->children.size(), 0);
        ExpectStr("degraded status names the side and the reason",
                  c->status, L"源端不可见（权限不足）");
        ExpectTrue("server detail is surfaced", c->summary.Contains(L"SELECT command denied"));
    }
}

static void TestUnsupportedEngineRendersNoChildren()
{
    const std::vector<RoutineDef> none;
    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::Sqlite, db::Dialect::Sqlite);
    const RoutineDiffSet rd = CompareRoutines(none, none, RoutineReadStatus::Unsupported,
                                              RoutineReadStatus::Unsupported, opt);

    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);
    const DiffNode* c = Category(tree, DiffCategory::Functions);
    ExpectTrue("unsupported category exists", c != nullptr);
    if (!c) return;
    ExpectEq("unsupported category has zero children", (long long)c->children.size(), 0);
    // BOTH sides are named: an unsupported engine is unsupported on both ends
    // and saying so beats a single ambiguous 不可见.
    ExpectTrue("both sides labelled unreadable",
               c->status.Contains(L"源端不可见（无法读取）") &&
               c->status.Contains(L"目标端不可见（无法读取）"));
    // Never "there are no functions".
    ExpectTrue("status is not empty", !c->status.IsEmpty());
}

// MySQL lists a routine to anyone but hides the body without privileges. That
// must render as a STATED refusal, never as an empty body.
static void TestUnreadableBodyIsStatedNotBlank()
{
    std::vector<RoutineDef> src, tgt;
    RoutineDef a = Fn(L"f_secret", wxString());
    a.bodyReadable = false;
    RoutineDef b = Fn(L"f_secret", wxString());
    b.bodyReadable = false;
    src.push_back(a);
    tgt.push_back(b);

    RoutineCompareOptions opt =
        MakeRoutineCompareOptions(db::Dialect::MySQL, db::Dialect::MySQL);
    const RoutineDiffSet rd =
        CompareRoutines(src, tgt, RoutineReadStatus::Ok, RoutineReadStatus::Ok, opt);
    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{}, rd);

    const DiffNode* fn = Category(tree, DiffCategory::Functions);
    ExpectTrue("category exists", fn != nullptr);
    if (!fn || fn->children.empty()) {
        // A hidden-body pair must NOT be reported identical, so it has to be a
        // difference and therefore a child. Failing here is the bug.
        ExpectTrue("hidden-body pair is listed as a difference", false);
        return;
    }
    const DiffNode* body = Leaf(*fn->children[0], RoutineBodyRowLabel());
    ExpectTrue("body leaf exists", body != nullptr);
    if (!body) return;
    ExpectStr("hidden source body is stated", body->after, UnreadableSide());
    ExpectStr("hidden target body is stated", body->before, UnreadableSide());
    ExpectTrue("placeholder is recognized as one", IsPlaceholderSide(body->before));
}

// Without a RoutineDiffSet there are no routine categories at all — the option
// being off must not leave an empty 函数 header implying "none found".
static void TestNoRoutineSetMeansNoCategories()
{
    const DiffTree tree = BuildDiffTree(db::sync::SyncPlan{});
    ExpectTrue("no functions category", Category(tree, DiffCategory::Functions) == nullptr);
    ExpectTrue("no procedures category", Category(tree, DiffCategory::Procedures) == nullptr);
    ExpectTrue("tables category still present",
               Category(tree, DiffCategory::Tables) != nullptr);
}

int main()
{
    // wxBase only (no window): ui::tr() reaches core::Lang, whose lazy load
    // uses wxStandardPaths. Same reasoning as SyncDiffModelTests.cpp's main().
    wxInitializer wxInit;
    if (!wxInit.IsOk()) {
        std::printf("FATAL: could not initialize wxBase\n");
        return 2;
    }

    TestSameEngineCategories();
    TestRoutinesAreNotSelectable();
    TestCrossEngineBodiesAreNotHighlighted();
    TestPermissionDeniedRendersNoChildren();
    TestUnsupportedEngineRendersNoChildren();
    TestUnreadableBodyIsStatedNotBlank();
    TestNoRoutineSetMeansNoCategories();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
