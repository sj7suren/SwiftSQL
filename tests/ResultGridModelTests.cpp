// ResultGridModelTests.cpp — headless unit tests for ui::gridmodel
// (src/ui/ResultGridModel.{h,cpp}): the pure, widget-free half of the result
// grid — sort-key algebra, the client-side multi-key row reorder, clipboard
// TSV encode/parse, and pager arithmetic.
//
// WHY THIS FILE EXISTS. Two of the four areas below are classic silent-failure
// territory that was previously reachable only by clicking:
//
//   * SortRows carries each row's `orig` index (its position in the ORIGINAL
//     result) alongside its cells. If that mapping does not travel with the
//     cells through the sort, a subsequent 保存 writes the edit to the WRONG
//     RECORD — with no error and no visible symptom until the data is wrong.
//     This project has already been bitten once by a position-keyed selection
//     (see SyncSelectionTests' reorder-invariance case); this is the same
//     hazard in the data browser.
//   * The pager's enable-state and its "3 / 12" indicator used to be computed
//     from two separate copies of the same maxPage expression, in GoToPage and
//     UpdatePager. Off-by-one page math is exactly the kind of thing that is
//     tedious to click through and trivial to assert.
//
// Same dependency-free harness as the rest of tests/. Compiles
// ResultGridModel.cpp directly and links only swiftsql::core — the model
// depends on nothing but wxString. That core-only link is a FITNESS FUNCTION,
// not an optimization (same rule as SyncSelectionTests): if anyone pulls a db
// type or a wx widget into ResultGridModel.{h,cpp}, this target stops linking.
// Keep it on swiftsql::core.
#include "ui/ResultGridModel.h"

#include <cstdio>

using ui::SortKey;
namespace gm = ui::gridmodel;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectEq(const char* what, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n        want: [%s]\n        got : [%s]\n",
                    what, (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", what);
    }
}

static void ExpectTrue(const char* what, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", what); }
    else       { std::printf("  ok   %s\n", what); }
}

// Render a key list as "col:asc,col:desc" so a mismatch prints readably.
static wxString KeysText(const std::vector<SortKey>& keys)
{
    wxString s;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) s += L",";
        s += wxString::Format(L"%d:%s", keys[i].col, keys[i].asc ? L"asc" : L"desc");
    }
    return s;
}

// ---------------------------------------------------------------------------
// Sort-key algebra
// ---------------------------------------------------------------------------
static void TestSortKeyAlgebra()
{
    std::printf("-- sort-key algebra --\n");

    // Plain header click: three-state on the SAME column, replace otherwise.
    ExpectEq("click on unsorted grid -> single asc",
             KeysText(gm::NextKeysOnHeaderClick({}, 2)), L"2:asc");
    ExpectEq("click again on the same column -> desc",
             KeysText(gm::NextKeysOnHeaderClick({ { 2, true } }, 2)), L"2:desc");
    ExpectEq("third click clears the sort entirely",
             KeysText(gm::NextKeysOnHeaderClick({ { 2, false } }, 2)), wxString());
    ExpectEq("click on a DIFFERENT column replaces, does not append",
             KeysText(gm::NextKeysOnHeaderClick({ { 2, false } }, 0)), L"0:asc");
    // The three-state cycle applies only to a lone key: a multi-key sort is
    // replaced outright, even when the clicked column is already in the list.
    ExpectEq("click collapses a multi-key sort to a single asc key",
             KeysText(gm::NextKeysOnHeaderClick({ { 2, true }, { 0, false } }, 2)),
             L"2:asc");

    // Shift-click / 加入多列排序: append, flip, drop — preserving the order of
    // the other keys, which IS the ORDER BY priority.
    ExpectEq("toggle appends a new key as asc",
             KeysText(gm::NextKeysOnToggle({ { 2, true } }, 0)), L"2:asc,0:asc");
    ExpectEq("toggle flips an existing asc key in place",
             KeysText(gm::NextKeysOnToggle({ { 2, true }, { 0, true } }, 2)),
             L"2:desc,0:asc");
    ExpectEq("toggle drops an existing desc key",
             KeysText(gm::NextKeysOnToggle({ { 2, false }, { 0, true } }, 2)), L"0:asc");
    ExpectEq("dropping the middle key preserves the priority of the rest",
             KeysText(gm::NextKeysOnToggle({ { 1, true }, { 2, false }, { 3, true } }, 2)),
             L"1:asc,3:asc");
    ExpectEq("toggle on an empty list appends asc",
             KeysText(gm::NextKeysOnToggle({}, 5)), L"5:asc");

    // Header decoration.
    const std::vector<wxString> cols = { L"id", L"name", L"created_at" };
    std::vector<wxString> l = gm::SortHeaderLabels(cols, {});
    ExpectEq("unsorted headers are bare", l[0] + L"|" + l[1] + L"|" + l[2],
             L"id|name|created_at");
    l = gm::SortHeaderLabels(cols, { { 1, true } });
    ExpectEq("single key gets an arrow and NO priority digit",
             l[1], wxString(L"name  ") + L"▲");
    ExpectEq("unsorted columns stay bare alongside a sorted one", l[0], L"id");
    l = gm::SortHeaderLabels(cols, { { 2, false }, { 1, true } });
    ExpectEq("multi-key: first key shows priority 1", l[2],
             wxString(L"created_at  ") + L"▼1");
    ExpectEq("multi-key: second key shows priority 2", l[1],
             wxString(L"name  ") + L"▲2");
    ExpectTrue("labels are produced one per column",
               gm::SortHeaderLabels(cols, {}).size() == 3);
}

// ---------------------------------------------------------------------------
// Client-side multi-key row reorder
// ---------------------------------------------------------------------------
static wxString RowsText(const std::vector<gm::SortableRow>& rows)
{
    wxString s;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) s += L" ";
        for (size_t c = 0; c < rows[i].cells.size(); ++c) {
            if (c) s += L",";
            s += rows[i].cells[c];
        }
        s += wxString::Format(L"#%d", rows[i].orig);
    }
    return s;
}

static void TestSortRows()
{
    std::printf("-- client-side row reorder --\n");

    auto make = [] {
        return std::vector<gm::SortableRow>{
            { { L"3", L"carol" }, 0 },
            { { L"1", L"alice" }, 1 },
            { { L"2", L"bob"   }, 2 },
        };
    };

    std::vector<gm::SortableRow> r = make();
    gm::SortRows(r, {});
    ExpectEq("empty key list is a no-op", RowsText(r), RowsText(make()));

    // THE headline property: the `orig` index travels with its own cells. If
    // these got decoupled, a later 保存 would UPDATE the wrong record.
    r = make();
    gm::SortRows(r, { { 0, true } });
    ExpectEq("ascending sort carries each row's orig index with its cells",
             RowsText(r), L"1,alice#1 2,bob#2 3,carol#0");

    r = make();
    gm::SortRows(r, { { 0, false } });
    ExpectEq("descending sort likewise", RowsText(r), L"3,carol#0 2,bob#2 1,alice#1");

    // Numeric vs text: "10" must sort AFTER "9" when both parse as numbers —
    // lexicographic ordering here is the classic wrong answer.
    std::vector<gm::SortableRow> n = {
        { { L"10" }, 0 }, { { L"9" }, 1 }, { { L"100" }, 2 }, { { L"2" }, 3 },
    };
    gm::SortRows(n, { { 0, true } });
    ExpectEq("numeric cells sort numerically, not lexicographically",
             RowsText(n), L"2#3 9#1 10#0 100#2");

    // Non-numeric cells fall back to case-insensitive text.
    std::vector<gm::SortableRow> t = {
        { { L"banana" }, 0 }, { { L"Apple" }, 1 }, { { L"cherry" }, 2 },
    };
    gm::SortRows(t, { { 0, true } });
    ExpectEq("text cells sort case-insensitively",
             RowsText(t), L"Apple#1 banana#0 cherry#2");

    // Multi-key: the first non-equal key decides; ties fall through to the next.
    std::vector<gm::SortableRow> m = {
        { { L"b", L"2" }, 0 },
        { { L"a", L"2" }, 1 },
        { { L"b", L"1" }, 2 },
        { { L"a", L"1" }, 3 },
    };
    gm::SortRows(m, { { 0, true }, { 1, false } });
    ExpectEq("multi-key: first key decides, ties broken by the second",
             RowsText(m), L"a,2#1 a,1#3 b,2#0 b,1#2");

    // Stability: equal rows keep their relative order (so a re-sort on an
    // all-equal column does not shuffle the grid under the user).
    std::vector<gm::SortableRow> s = {
        { { L"x" }, 0 }, { { L"x" }, 1 }, { { L"x" }, 2 },
    };
    gm::SortRows(s, { { 0, true } });
    ExpectEq("equal rows preserve input order (stable sort)",
             RowsText(s), L"x#0 x#1 x#2");

    // A stale/out-of-range key must be skipped rather than read out of bounds.
    std::vector<gm::SortableRow> o = { { { L"b" }, 0 }, { { L"a" }, 1 } };
    gm::SortRows(o, { { 7, true }, { 0, true } });
    ExpectEq("out-of-range keys are skipped, later keys still apply",
             RowsText(o), L"a#1 b#0");

    std::vector<gm::SortableRow> one = { { { L"z" }, 5 } };
    gm::SortRows(one, { { 0, true } });
    ExpectEq("single row is a no-op", RowsText(one), L"z#5");

    // New (unsaved) rows carry orig == -1 and must survive the sort as such,
    // otherwise a pending INSERT silently turns into an UPDATE.
    std::vector<gm::SortableRow> mixed = {
        { { L"2" }, 0 }, { { L"1" }, -1 },
    };
    gm::SortRows(mixed, { { 0, true } });
    ExpectEq("a new row's -1 marker survives the sort", RowsText(mixed), L"1#-1 2#0");
}

// ---------------------------------------------------------------------------
// Clipboard TSV
// ---------------------------------------------------------------------------
static void TestClipboardText()
{
    std::printf("-- clipboard TSV --\n");
    ExpectEq("empty block -> empty text", gm::JoinTsv({}), wxString());
    ExpectEq("single cell has no separators", gm::JoinTsv({ { L"a" } }), L"a");
    ExpectEq("row is tab-separated", gm::JoinTsv({ { L"a", L"b", L"c" } }), L"a\tb\tc");
    ExpectEq("rows are newline-separated with NO trailing newline",
             gm::JoinTsv({ { L"a", L"b" }, { L"c", L"d" } }), L"a\tb\nc\td");
    ExpectEq("empty cells are preserved as empty fields",
             gm::JoinTsv({ { L"a", wxString(), L"c" } }), L"a\t\tc");

    // Parse side.
    ExpectTrue("empty clipboard is reported empty",
               gm::ParseClipboardPaste(wxString()).empty);
    ExpectTrue("whitespace-only newlines are reported empty",
               gm::ParseClipboardPaste(L"\n\n").empty);

    gm::ClipboardPaste p = gm::ParseClipboardPaste(L"hello");
    ExpectTrue("a single tab-free value is a scalar paste", !p.empty && p.scalar);
    ExpectEq("scalar value round-trips", p.value, L"hello");

    p = gm::ParseClipboardPaste(L"a\tb\tc");
    ExpectTrue("a tabbed single line is a ROW paste, not a scalar",
               !p.empty && !p.scalar && p.rows.size() == 1 && p.rows[0].size() == 3);
    ExpectEq("row fields split on tab", gm::JoinTsv(p.rows), L"a\tb\tc");

    // CRLF from a Windows source must not leave a stray \r in the last field —
    // that would be pasted into the cell and saved to the database.
    p = gm::ParseClipboardPaste(L"a\tb\r\nc\td\r\n");
    ExpectTrue("CRLF input yields two clean rows", p.rows.size() == 2);
    ExpectEq("CR is stripped, trailing blank line dropped",
             gm::JoinTsv(p.rows), L"a\tb\nc\td");

    // Multiple plain lines with no tabs are still a multi-ROW paste (one column
    // each), not a scalar — this is what makes "copy a column, paste it back"
    // fill consecutive rows instead of one cell.
    p = gm::ParseClipboardPaste(L"one\ntwo\nthree");
    ExpectTrue("multiple tab-free lines are a multi-row paste",
               !p.scalar && p.rows.size() == 3);

    // Ragged rows are allowed through; the caller clamps to the column count.
    p = gm::ParseClipboardPaste(L"a\tb\tc\nd\te");
    ExpectTrue("ragged rows are preserved as-is",
               p.rows.size() == 2 && p.rows[0].size() == 3 && p.rows[1].size() == 2);

    // JoinTsv → ParseClipboardPaste is a round-trip for tabbed data.
    const gm::CellGrid orig = { { L"1", L"alice" }, { L"2", L"bob" } };
    p = gm::ParseClipboardPaste(gm::JoinTsv(orig));
    ExpectEq("copy -> paste round-trips", gm::JoinTsv(p.rows), gm::JoinTsv(orig));
}

// ---------------------------------------------------------------------------
// Pager arithmetic
// ---------------------------------------------------------------------------
static void TestPager()
{
    std::printf("-- pager arithmetic --\n");

    // Boundary: an exact multiple of the page size must NOT produce a trailing
    // empty page (1000 rows at 100/page = pages 0..9, not 0..10).
    ExpectTrue("exact multiple yields no trailing empty page",
               gm::MaxPageIndex(1000, 100) == 9);
    ExpectTrue("one row over the boundary adds a page",
               gm::MaxPageIndex(1001, 100) == 10);
    ExpectTrue("one row under stays on the same page",
               gm::MaxPageIndex(999, 100) == 9);
    ExpectTrue("a single row is one page", gm::MaxPageIndex(1, 100) == 0);
    ExpectTrue("zero rows is page 0", gm::MaxPageIndex(0, 100) == 0);
    ExpectTrue("unknown row count pins at page 0", gm::MaxPageIndex(-1, 100) == 0);

    // Clamping (末页 asks for 1<<29 and relies on this).
    ExpectTrue("clamp caps at the last page", gm::ClampPage(1 << 29, 250, 100) == 2);
    ExpectTrue("clamp floors at 0", gm::ClampPage(-5, 250, 100) == 0);
    ExpectTrue("clamp leaves a valid page alone", gm::ClampPage(1, 250, 100) == 1);
    // With the count still unknown, paging forward blind is allowed — the
    // upper clamp must not silently snap the user back to page 0.
    ExpectTrue("unknown count does not cap forward paging",
               gm::ClampPage(7, -1, 100) == 7);

    // Full pager state.
    gm::PagerState s = gm::ComputePagerState(false, -1, 0, 100);
    ExpectEq("no bound table shows the em-dash placeholder", s.label, L"—");
    ExpectTrue("no bound table disables every pager control",
               !s.first && !s.prev && !s.next && !s.last && !s.sizeChoice);

    s = gm::ComputePagerState(true, -1, 0, 100);
    ExpectEq("count pending shows the page number alone", s.label, L"1");
    ExpectTrue("count pending still allows paging forward", s.next);
    ExpectTrue("count pending disables 末页 (no known last page)", !s.last);
    ExpectTrue("page 0 disables 首页/上一页", !s.first && !s.prev);

    s = gm::ComputePagerState(true, 250, 0, 100);
    ExpectEq("first of three pages", s.label, L"1 / 3");
    ExpectTrue("first page: back disabled, forward enabled",
               !s.first && !s.prev && s.next && s.last);

    s = gm::ComputePagerState(true, 250, 1, 100);
    ExpectEq("middle page", s.label, L"2 / 3");
    ExpectTrue("middle page enables all four", s.first && s.prev && s.next && s.last);

    s = gm::ComputePagerState(true, 250, 2, 100);
    ExpectEq("last page", s.label, L"3 / 3");
    ExpectTrue("last page: forward disabled, back enabled",
               s.first && s.prev && !s.next && !s.last);

    s = gm::ComputePagerState(true, 0, 0, 100);
    ExpectEq("empty table reads as one page", s.label, L"1 / 1");
    ExpectTrue("empty table disables navigation but keeps the size choice",
               !s.first && !s.prev && !s.next && !s.last && s.sizeChoice);
}

// ---------------------------------------------------------------------------
// Cell-viewer summary
// ---------------------------------------------------------------------------
static void TestSummary()
{
    std::printf("-- cell summary --\n");
    ExpectEq("short value passes through", gm::SummarizeCellValue(L"hello"), L"hello");
    ExpectEq("newlines/tabs/CR flatten to spaces",
             gm::SummarizeCellValue(L"a\nb\tc\rd"), L"a b c d");
    ExpectEq("empty stays empty", gm::SummarizeCellValue(wxString()), wxString());

    const wxString exact60(L'x', 60);
    ExpectEq("exactly 60 chars is NOT elided", gm::SummarizeCellValue(exact60), exact60);

    const wxString over(L'x', 61);
    const wxString got = gm::SummarizeCellValue(over);
    ExpectTrue("61 chars is elided to 60 + ellipsis", got.length() == 61 &&
               got.Left(60) == wxString(L'x', 60) && got.Last() == L'…');
}

int main()
{
    TestSortKeyAlgebra();
    TestSortRows();
    TestClipboardText();
    TestPager();
    TestSummary();

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
