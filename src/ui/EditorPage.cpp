// EditorPage.cpp — the SQL editor tab itself: construction, the IQueryTab
// surface MainFrame drives, autocompletion wiring, and key routing.
//
// This TU is one of four. It was split under the charter's 1000-line ceiling
// (docs/CHARTER.md), and the seam was chosen for TESTABILITY rather than by
// cutting the widget in half — the failure mode ResultGridPanelOps.cpp
// demonstrates, where a mechanical split left the logic entangled and both
// halves grew back over the limit. What came out:
//
//   ui/EditorSqlFormat.{h,cpp}  PURE — the SQL beautifier. Unit-tested.
//   ui/EditorCompletion.{h,cpp} PURE — which words complete, and which schema
//                               source answers a dot-qualified prefix. Unit-tested.
//   ui/EditorPageObject.cpp     WIDGET — object-editor selector bar, run target,
//                               bottom status strip, F10 routine execution.
//   ui/EditorPageFind.cpp       WIDGET — the Ctrl+F find/replace bar.
//
// What stays here is the editor widget's own story: build it, style it, answer
// IQueryTab, route keystrokes, and drive the completion popup from the pure
// decision layer.
#include "ui/EditorPage.h"

#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>
#include <algorithm>
#include <set>

#include "db/SqlKeywords.h"
#include "ui/AiSqlCompleter.h"
#include "ui/EditorCompletion.h"
#include "ui/GhostSuggest.h"        // local skeleton tier of the grey suggestion
#include "ui/I18n.h"                // tr() for the AI panel's tooltips / hints
#include "ui/GhostTextOverlay.h"    // where the grey suggestion is painted
#include "ui/EditorSqlFormat.h"
#include "ui/IconFactory.h"
#include "ui/IQueryTab.h"      // ID_RUN_QUERY
#include "ui/ResultGridPanel.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

// ===========================================================================
// SqlTextCtrl — wxStyledTextCtrl, minus one wxSTC behaviour that breaks IMEs.
// ===========================================================================
// THE BUG: with a Chinese IME, pressing Shift (the 中/英 toggle) made the NEXT
// letter typed disappear. Only in this editor — every plain wxTextCtrl in the
// app was fine.
//
// WHY ONLY HERE, in wx 3.2.9's own code:
//
//   ScintillaWX.cpp  DoKeyDown()          stc.cpp  OnKeyDown()
//     case WXK_SHIFT:  key = 0; break;      int processed = DoKeyDown(evt, &consumed);
//     ...                                   if (!processed && !consumed)
//     if (key) return rv;                       evt.Skip();
//     else     return 1;   // <-- HERE
//
// A bare Shift maps to key 0, so DoKeyDown returns 1 — "processed" — and
// OnKeyDown therefore never calls evt.Skip(). wxMSW then treats
// WM_KEYDOWN(VK_SHIFT) as fully handled and does NOT pass it to DefWindowProc,
// so the IME never sees half of the press/release pair its mode toggle runs on.
// Its state machine ends up inconsistent and eats the next character. A native
// EDIT control has no such path, which is exactly why only this editor showed it.
//
// THE FIX: let a BARE modifier key-down/up go straight to the default window
// procedure, before wx turns it into an event at all. Returning false from
// MSWHandleMessage is wx's documented "I did not handle this" answer, and wx
// then calls MSWDefWindowProc for us.
//
// WHY THIS COSTS SCINTILLA NOTHING: DoKeyDown maps every bare modifier to key 0
// and hands it to KeyDownWithModifiers, which finds no binding for key 0 and
// does nothing at all. Scintilla does not track modifiers from these messages —
// it reads them off the flags of each real key/mouse event (evt.ShiftDown()),
// so shift-click, shift-arrow selection and Alt+drag rectangular selection are
// unaffected. Nothing that was working stops working.
//
// SCOPE: bare VK_SHIFT / VK_CONTROL / VK_MENU only. A modifier COMBINATION
// (Ctrl+F, Shift+Home, …) arrives as a WM_KEYDOWN whose wParam is the LETTER or
// the arrow, not the modifier, so none of those messages match this test and
// every existing shortcut still routes exactly as before.
class SqlTextCtrl : public wxStyledTextCtrl {
public:
    SqlTextCtrl(wxWindow* parent, wxWindowID id) : wxStyledTextCtrl(parent, id) {}

#ifdef __WXMSW__
protected:
    bool MSWHandleMessage(WXLRESULT* result, WXUINT msg,
                          WXWPARAM wParam, WXLPARAM lParam) override
    {
        if ((msg == WM_KEYDOWN || msg == WM_KEYUP) &&
            (wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU))
            return false;   // unhandled → wx calls DefWindowProc → the IME sees it
        return wxStyledTextCtrl::MSWHandleMessage(result, msg, wParam, lParam);
    }
#endif
};

} // namespace

// ---------------------------------------------------------------------------
EditorPage::EditorPage(wxWindow* parent, const wxString& sql)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // The run / stop / beautify / explain actions live in the top toolbar
    // (MainFrame), acting on the active tab — no per-editor action strip.

    // ---- editor / results split (draggable up-down; results hidden until first run) ----
    // ---- AI 面板的容器 ------------------------------------------------------
    // A vertical splitter wrapping everything else, whose right pane is the AI
    // chat. It starts UNSPLIT (Initialize below), so a page nobody opens the
    // panel on pays nothing at all — see ToggleAiPanel for why that matters
    // when every tab owns its own panel.
    aiSplit_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    aiSplit_->SetMinimumPaneSize(FromDIP(260));
    aiSplit_->SetSashGravity(1.0);           // the editor keeps the extra width on resize

    resultSplit_ = new wxSplitterWindow(aiSplit_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxBORDER_NONE);
    resultSplit_->SetMinimumPaneSize(60);
    resultSplit_->SetSashGravity(0.6);       // editor keeps the extra space on resize

    // ---- SQL editor ----
    // SqlTextCtrl, not a plain wxStyledTextCtrl — see the class note above: it
    // exists solely to stop wxSTC swallowing bare modifier keys from the IME.
    stc_ = new SqlTextCtrl(resultSplit_, wxID_ANY);
    stc_->SetLexer(wxSTC_LEX_SQL);
    // default to MySQL keyword/function lists; SetCompletionSource refreshes
    // them per the connected dialect.
    ApplyKeywordLists(db::Dialect::MySQL);
    stc_->SetText(sql);

    const wxFont code = Mono(10.5);
    for (int st = 0; st < wxSTC_STYLE_MAX; ++st) {
        stc_->StyleSetFont(st, code);
        stc_->StyleSetBackground(st, theme::kEditorBg);
        stc_->StyleSetForeground(st, theme::kSynDefault);
    }
    // ---- syntax colour system ----
    stc_->StyleSetForeground(wxSTC_SQL_COMMENT, theme::kSynComment);
    stc_->StyleSetItalic(wxSTC_SQL_COMMENT, true);
    stc_->StyleSetForeground(wxSTC_SQL_COMMENTLINE, theme::kSynComment);
    stc_->StyleSetItalic(wxSTC_SQL_COMMENTLINE, true);
    stc_->StyleSetForeground(wxSTC_SQL_COMMENTDOC, theme::kSynComment);
    stc_->StyleSetItalic(wxSTC_SQL_COMMENTDOC, true);
    stc_->StyleSetForeground(wxSTC_SQL_NUMBER, theme::kSynNumber);
    stc_->StyleSetForeground(wxSTC_SQL_WORD, theme::kSynKeyword);   // keywords
    stc_->StyleSetBold(wxSTC_SQL_WORD, true);
    stc_->StyleSetForeground(wxSTC_SQL_WORD2, theme::kSynFunction); // functions
    stc_->StyleSetForeground(wxSTC_SQL_STRING, theme::kSynString);  // 'str'
    stc_->StyleSetForeground(wxSTC_SQL_CHARACTER, theme::kSynString);
    stc_->StyleSetForeground(wxSTC_SQL_QUOTEDIDENTIFIER, theme::kSynTable); // `col` / "col"
    stc_->StyleSetForeground(wxSTC_SQL_IDENTIFIER, theme::kSynDefault);
    stc_->StyleSetForeground(wxSTC_SQL_OPERATOR, theme::kSynOperator);      // , ( ) = < >
    stc_->StyleSetBold(wxSTC_SQL_OPERATOR, true);
    stc_->SetMarginType(0, wxSTC_MARGIN_NUMBER);
    stc_->SetMarginWidth(0, 44);
    stc_->StyleSetForeground(wxSTC_STYLE_LINENUMBER, theme::kLineNumber);
    stc_->StyleSetBackground(wxSTC_STYLE_LINENUMBER, theme::kEditorBg);
    stc_->SetMarginWidth(1, 0);
    stc_->SetCaretForeground(theme::kPrimary);
    stc_->SetSelBackground(true, wxColour(0xCC, 0xE0, 0xFA));
    // find/replace match highlight — a soft amber round-box behind every match.
    // The indicator is driven from EditorPageFind.cpp; only its styling is here.
    stc_->IndicatorSetStyle(kFindIndicator, wxSTC_INDIC_ROUNDBOX);
    stc_->IndicatorSetForeground(kFindIndicator, wxColour(0xFF, 0xB2, 0x24));
    stc_->IndicatorSetAlpha(kFindIndicator, 90);
    stc_->IndicatorSetOutlineAlpha(kFindIndicator, 160);
    // don't show a permanent horizontal scrollbar — only when a line overflows
    stc_->SetScrollWidth(1);
    stc_->SetScrollWidthTracking(true);
    stc_->SetWrapMode(wxSTC_WRAP_NONE);

    // ---- autocompletion ----
    stc_->AutoCompSetIgnoreCase(true);
    stc_->AutoCompSetAutoHide(true);
    stc_->AutoCompSetMaxHeight(12);
    stc_->AutoCompSetSeparator('\n');
    // Let Scintilla sort the list itself: our candidates mix upper-case keywords with
    // lower-case table/column names, and the default PRESORTED mode's case-sensitive
    // binary search then failed to match once a connection added tables (SELECT would
    // stop popping up). PERFORMSORT sorts case-insensitively so matching is robust.
    stc_->AutoCompSetOrder(wxSTC_ORDER_PERFORMSORT);
    RegisterCompletionIcons();               // per-category coloured icons in the popup
    stc_->Bind(wxEVT_STC_CHARADDED, &EditorPage::OnCharAdded, this);
    stc_->Bind(wxEVT_KEY_DOWN, &EditorPage::OnEditorKey, this);
    aiCompleter_ = std::make_unique<AiSqlCompleter>(stc_);
    aiCompleter_->SetContextProvider([this] {
        ai::AiContext ctx;
        ctx.dialect = comp_.dialect ? comp_.dialect() : db::Dialect::MySQL;
        return ctx;
    });
    // The AI tier no longer opens a popup of its own; it hands its answer to the
    // same grey overlay the local tier draws into, so there is one place a
    // suggestion can appear and one key that accepts it.
    aiCompleter_->SetPresenter([this] { UpdateGhost(); });

    // ---- 灰字建议 overlay ----
    // Parented to the page, not to stc_: it is a wxPopupWindow (see its header
    // for why a child window cannot work over Scintilla) and wx destroys it with
    // this page.
    ghost_ = new GhostTextOverlay(this);
    // Everything that moves the caret or the view invalidates the drawn
    // position. UPDATEUI covers caret moves and selection changes; the scroll
    // and focus cases are not UPDATEUI events, so they are bound separately.
    // Missing any one of these leaves grey text floating over the wrong line.
    stc_->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& e) { UpdateGhost(); e.Skip(); });
    stc_->Bind(wxEVT_STC_PAINTED,  [this](wxStyledTextEvent& e) { e.Skip(); });
    stc_->Bind(wxEVT_KILL_FOCUS,   [this](wxFocusEvent& e) { HideGhost(); e.Skip(); });
    stc_->Bind(wxEVT_MOUSEWHEEL,   [this](wxMouseEvent& e) { HideGhost(); e.Skip(); });
    stc_->Bind(wxEVT_LEFT_DOWN,    [this](wxMouseEvent& e) { HideGhost(); e.Skip(); });

    // ---- results: the shared data-browser surface ----
    // The SQL query window hides the right-hand table-info panel (that's for the 打开表
    // browser) and the 筛选/排序 toolbar — in an ad-hoc query you write WHERE/ORDER BY
    // in the SQL itself.
    auto feats = ResultGridPanel::Features::Full();
    feats.infoPanel    = false;
    feats.filter       = false;
    feats.sortHide     = false;
    feats.importCsv    = false;   // no top toolbar in the query window…
    feats.exportCsv    = false;
    feats.exportBottom = true;    // …export lives as a single button at the bottom
    grid_ = new ResultGridPanel(resultSplit_, feats);
    // The pager/filter build a fresh paged SELECT; feed it back into the editor
    // so the same run path (QueryText → RunSql) picks it up, matching the prior
    // behaviour where paging rewrote the editor buffer.
    grid_->SetQuerySink([this](const wxString& s) { stc_->SetText(s); });

    resultSplit_->Initialize(stc_);          // start with only the editor; reveal on run
    aiSplit_->Initialize(resultSplit_);      // AI pane closed until asked for

    // ---- right edge rail: the icon that opens the AI panel -----------------
    // A 28px strip pinned to the right of the page, so the affordance is where
    // the panel will appear rather than buried in the top toolbar.
    aiRail_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(30, -1)));
    aiRail_->SetBackgroundColour(theme::kSidebarBg);
    {
        auto* rv = new wxBoxSizer(wxVERTICAL);
        aiToggleBtn_ = new wxBitmapButton(
            aiRail_, wxID_ANY, icons::Stroke(icons::Glyph::StarLine, 18, theme::kPrimary),
            wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        aiToggleBtn_->SetBackgroundColour(theme::kSidebarBg);
        aiToggleBtn_->SetToolTip(tr(L"AI 助手（对话 / 性能分析）"));
        aiToggleBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleAiPanel(); });
        rv->Add(aiToggleBtn_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));
        aiRail_->SetSizer(rv);
    }

    auto* h = new wxBoxSizer(wxHORIZONTAL);
    h->Add(aiSplit_, 1, wxEXPAND);
    h->Add(aiRail_, 0, wxEXPAND);
    v->Add(h, 1, wxEXPAND);
    SetSizer(v);
}

EditorPage::~EditorPage() = default;

// ---------------------------------------------------------------------------
wxString EditorPage::QueryText() const
{
    wxString sel = stc_->GetSelectedText();
    return sel.IsEmpty() ? stc_->GetText() : sel;
}

void EditorPage::InsertQuery(const wxString& sql)
{
    stc_->SetText(sql);
}

// Thin forwards to wxSTC for the frame 编辑 menu. wxSTC guards its own no-ops
// (Undo/Redo/Paste do nothing when there is nothing to undo/redo/paste).
void EditorPage::EditUndo()  { stc_->Undo();  }
void EditorPage::EditRedo()  { stc_->Redo();  }
void EditorPage::EditCut()   { stc_->Cut();   }
void EditorPage::EditCopy()  { stc_->Copy();  }
void EditorPage::EditPaste() { stc_->Paste(); }
wxString EditorPage::FullText() const { return stc_->GetText(); }

void EditorPage::SetRunning(bool on)
{
    if (objectEditor_) {
        // An object editor runs a CREATE (no rows) → never reveal the results grid;
        // the editor fills the whole tab. Hide any prior save-status strip while the
        // new run is in flight.
        if (on && objStatusBar_ && objStatusBar_->IsShown()) {
            objStatusBar_->Hide();
            if (auto* s = GetSizer()) s->Layout();
        }
        return;
    }
    if (on) {
        // Reveal the results pane on the first run; drop the previous result so it
        // isn't shown while the new query executes.
        if (resultSplit_ && !resultSplit_->IsSplit())
            resultSplit_->SplitHorizontally(stc_, grid_,
                                            std::max(120, GetClientSize().y * 55 / 100));
        grid_->Clear();
    }
    grid_->SetRunning(on);
}

void EditorPage::ShowError(const wxString& e)
{
    // Ensure the error is visible even when a run is aborted before the results
    // pane was revealed (e.g. a dead 连接/数据库 selection in CommitRunTarget on a
    // plain editor): reveal the split first. Object editors show their own bottom
    // strip via ShowObjectSaveStatus instead and never take this path.
    if (!objectEditor_ && resultSplit_ && !resultSplit_->IsSplit())
        resultSplit_->SplitHorizontally(stc_, grid_,
                                        std::max(120, GetClientSize().y * 55 / 100));
    grid_->ShowError(e);
}
bool EditorPage::ConfirmClose()               { return grid_->ConfirmDiscardPendingEdits(); }

void EditorPage::ShowResult(const db::QueryResult& r, const wxString& target,
                            const EditableSpec& spec)
{
    grid_->ShowResult(r, target, spec);
}

gridsql::ScriptedEditResult EditorPage::ScriptedEdit(const wxString& col,
                                                     const wxString& value)
{
    return grid_->ScriptedEdit(col, value);
}

// ---------------------------------------------------------------------------
// SQL formatting — the algorithm lives in ui/EditorSqlFormat.{h,cpp}; this end
// only resolves the active dialect to a keyword set and moves text in and out of
// the widget.
// ---------------------------------------------------------------------------
void EditorPage::FormatActive()
{
    const wxString sel = stc_->GetSelectedText();
    if (!sel.IsEmpty()) stc_->ReplaceSelection(FormatSql(sel));
    else stc_->SetText(FormatSql(stc_->GetText()));
    // Scroll-width tracking only ever GROWS to the widest line seen; it never
    // shrinks. After beautifying wraps long lines, reset to 1 so Scintilla
    // re-measures the (now shorter) content and drops the stale h-scrollbar.
    stc_->SetScrollWidth(1);
}

wxString EditorPage::FormatSql(const wxString& src) const
{
    std::set<wxString> kw;
    if (comp_.dialect)
        for (const wxString& k : db::Keywords(comp_.dialect())) kw.insert(k.Upper());
    return editorfmt::FormatSql(src, kw);
}

// ---------------------------------------------------------------------------
// Autocompletion
// ---------------------------------------------------------------------------
void EditorPage::ApplyKeywordLists(db::Dialect d)
{
    // Scintilla's SQL lexer matches keyword lists case-insensitively but expects
    // them lower-cased. List 0 → SCE_SQL_WORD (keywords), list 1 → SCE_SQL_WORD2
    // (functions); each gets its own colour.
    wxString kw, fn;
    for (const wxString& k : db::Keywords(d))  kw += k.Lower() + L" ";
    for (const wxString& f : db::Functions(d)) fn += f.Lower() + L" ";
    stc_->SetKeyWords(0, kw);
    stc_->SetKeyWords(1, fn);
}

void EditorPage::SetCompletionSource(CompletionSource s)
{
    comp_ = std::move(s);
    if (comp_.dialect) ApplyKeywordLists(comp_.dialect());   // dialect-aware colours
}

// Bind this page's CompletionSource callbacks to the pure decision layer's
// SchemaLookup. Every callable is left UNSET when its source is unavailable, so
// editorcomp::ResolveQualifier sees the same "no such source" condition the
// inline code used to test for with `if (comp_.columns)` and friends.
//
// LIFETIME: the returned lookup captures `c` and `bufferText` BY REFERENCE. It
// is a call-site temporary only — every caller builds it, uses it, and drops it
// within one expression, with both referents outliving that. Do not store one.
static editorcomp::SchemaLookup MakeLookup(const EditorPage::CompletionSource& c,
                                           const wxString& bufferText)
{
    editorcomp::SchemaLookup look;
    if (c.databases)
        look.isDatabase = [&c](const wxString& id) {
            if (id.IsEmpty()) return false;
            for (const wxString& d : c.databases())
                if (d.IsSameAs(id, false)) return true;   // case-insensitive
            return false;
        };
    if (c.tablesOf)  look.tablesOf  = c.tablesOf;
    if (c.columnsOf) look.columnsOf = c.columnsOf;
    if (c.columns) {
        look.columns      = c.columns;
        look.resolveTable = [&c, &bufferText](const wxString& id) {
            return editorcomp::ResolveTableForIdent(
                bufferText, id,
                [&c](const wxString& t) { return !c.columns(t).empty(); });
        };
    }
    return look;
}

std::vector<std::pair<wxString, int>> EditorPage::WordCandidates() const
{
    // Keywords/functions fall back to MySQL when no connection has set a dialect yet —
    // otherwise a fresh editor (no active conn) completed nothing (e.g. SELECT wouldn't
    // pop up). Databases/tables override so a real object name wins its icon.
    static const std::vector<wxString> kNone;
    const db::Dialect d = comp_.dialect ? comp_.dialect() : db::Dialect::MySQL;
    return editorcomp::MergeWordCandidates(
        db::Keywords(d), db::Functions(d),
        comp_.databases ? comp_.databases() : kNone,
        comp_.tables    ? comp_.tables()    : kNone);
}

// Register a small coloured icon per completion category so the popup distinguishes
// keyword / function / database / table / column at a glance (colour = the category's
// theme colour; the list text itself can't be per-item coloured by Scintilla).
void EditorPage::RegisterCompletionIcons()
{
    stc_->AutoCompSetTypeSeparator('?');
    auto reg = [&](int type, icons::Glyph g, const wxColour& c) {
        stc_->RegisterImage(type, icons::Stroke(g, 14, c, 1.8));
    };
    reg(editorcomp::kKeyword,  icons::Glyph::Code,     theme::kSynKeyword);
    reg(editorcomp::kFunction, icons::Glyph::Function, theme::kSynFunction);
    reg(editorcomp::kDatabase, icons::Glyph::Cylinder, theme::kDotPurple);
    reg(editorcomp::kTable,    icons::Glyph::Table,    theme::kDotAmber);
    reg(editorcomp::kColumn,   icons::Glyph::Key,      theme::kAccent);
}

// Returns the words actually put in the popup — same contract as
// ShowColumnCompletions, for the same reason: the scripted hook reports this
// rather than rebuilding the list. Already word-sorted (MergeWordCandidates
// builds them in a std::map keyed by word), which is what Scintilla needs and
// what the hook used to re-establish with a redundant std::sort.
std::vector<wxString> EditorPage::ShowWordCompletions(bool force)
{
    const int pos = stc_->GetCurrentPos();
    const int ws  = stc_->WordStartPosition(pos, true);
    const int len = pos - ws;
    if (!force && len < 1) return {};   // pop from the first character (smoother)

    const auto cands = WordCandidates();
    if (cands.empty()) return {};
    stc_->AutoCompShow(len, editorcomp::BuildAutoCompList(cands));

    std::vector<wxString> words;
    words.reserve(cands.size());
    for (const auto& c : cands) words.push_back(c.first);
    return words;
}

wxString EditorPage::ResolveTableForIdent(const wxString& ident)
{
    if (!comp_.columns) return ident;
    return editorcomp::ResolveTableForIdent(
        stc_->GetText(), ident,
        [this](const wxString& t) { return !comp_.columns(t).empty(); });
}

bool EditorPage::IsDatabaseName(const wxString& ident) const
{
    if (ident.IsEmpty() || !comp_.databases) return false;
    for (const wxString& d : comp_.databases())
        if (d.IsSameAs(ident, false)) return true;   // case-insensitive
    return false;
}

// Returns the words actually put in the popup, so the SWIFTSQL_AUTOEDIT hook can
// REPORT production's answer instead of computing its own. Empty = nothing shown.
std::vector<wxString> EditorPage::ShowColumnCompletions()
{
    // This used to walk backwards from the caret itself, over SCINTILLA POSITIONS
    // — UTF-8 BYTE offsets — asking wxIsalnum of one byte at a time. On ASCII that
    // is the same walk as editorcomp::ParseQualifierChain; on anything else it is
    // not, because the bytes of a multi-byte character are not that character. For
    // `用户表.` it inspected 0xA8, the last byte of U+8868, decided it was not an
    // identifier character, and produced an empty token — so a CJK table name
    // completed to nothing at all. `表a.` was worse: it found the ASCII tail and
    // silently completed the columns of a table called `a`.
    //
    // The scan is gone. Scintilla hands us the text as a wxString and does its own
    // UTF-8 decoding, and the chain is parsed in whole CHARACTERS by the same
    // function the scripted hook reaches. The one byte computation left is `pos-1`
    // to step over the triggering '.', which is safe by construction: '.' is U+002E
    // and encodes as exactly one byte in UTF-8, so pos-1 is always a character
    // boundary and never lands inside a multi-byte sequence.
    const int pos = stc_->GetCurrentPos();      // caret just after the '.'
    if (pos < 1) return {};
    const wxString head = stc_->GetTextRange(0, pos - 1);   // everything before the '.'
    const wxString buf  = stc_->GetText();

    const auto r = editorcomp::CompleteQualified(head, MakeLookup(comp_, buf));
    if (r.candidates.empty()) return {};
    stc_->AutoCompShow(0, editorcomp::BuildAutoCompList(r.candidates, r.category));
    return r.candidates;
}

void EditorPage::ShowCallTip()
{
    if (!comp_.dialect) return;
    const int pos = stc_->GetCurrentPos();      // caret just after '('
    int i = pos - 2;
    while (i >= 0) {
        const wxChar c = static_cast<wxChar>(stc_->GetCharAt(i));
        if (wxIsalnum(c) || c == '_') --i; else break;
    }
    const wxString name = stc_->GetTextRange(i + 1, pos - 1);
    if (name.IsEmpty()) return;
    const wxString sig = db::FunctionSignature(comp_.dialect(), name);
    if (!sig.IsEmpty()) stc_->CallTipShow(i + 1, sig);
}

// ---------------------------------------------------------------------------
// Key routing
// ---------------------------------------------------------------------------
void EditorPage::OnCharAdded(wxStyledTextEvent& ev)
{
    const int ch = ev.GetKey();
    if (ch == '.') { ShowColumnCompletions(); UpdateGhost(); return; }
    if (ch == '(') { ShowCallTip(); UpdateGhost(); return; }
    if (wxIsalnum(static_cast<wxChar>(ch)) || ch == '_')
        ShowWordCompletions(false);
    if (aiCompleter_) aiCompleter_->Schedule();
    // After the popup decision, never before: UpdateGhost checks whether the
    // list ended up on screen and stays quiet if it did.
    UpdateGhost();
}

// ---------------------------------------------------------------------------
// 右侧 AI 面板
// ---------------------------------------------------------------------------

// LAZY ON PURPOSE. Every SQL tab owns its own panel (the user's choice, so each
// window keeps its own conversation), and an AiChatPanel is not cheap: it holds
// an AiClient, a knowledge base built by introspecting the whole database, and a
// conversation store. Creating one per tab up front would mean ten open tabs =
// ten schema crawls nobody asked for. Creating it on the first expand makes the
// per-tab choice cost exactly what it is used for.
void EditorPage::ToggleAiPanel()
{
    if (!aiSplit_) return;

    if (aiSplit_->IsSplit()) {          // open → close (the panel is KEPT alive,
        aiSplit_->Unsplit(aiPanel_);    // so the conversation survives a collapse)
        return;
    }
    if (!aiPanel_) {
        if (!aiHost_.connections) {     // MainFrame never handed us a host
            wxMessageBox(tr(L"AI 助手尚未就绪。"), tr(L"AI 助手"),
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        aiPanel_ = new AiChatPanel(aiSplit_, aiHost_);
    }
    aiSplit_->SplitVertically(resultSplit_, aiPanel_, -FromDIP(380));
}

void EditorPage::AnalyzePerformance()
{
    // Same text the 解释 button uses: the selection, or the whole buffer.
    wxString sql = QueryText();
    sql.Trim(true).Trim(false);
    if (sql.IsEmpty()) return;

    // Open the panel first — the analysis streams INTO it, and an answer
    // arriving in a collapsed panel would look like nothing happened.
    if (!aiSplit_ || !aiSplit_->IsSplit()) ToggleAiPanel();
    if (aiPanel_) aiPanel_->AnalyzeSql(sql);
}

void EditorPage::SetAiHost(AiChatHost host)
{
    // ROUTED TO THIS EDITOR, not to "whatever tab is active". The panel lives
    // inside this page, so "生成到 SQL 窗口" can only sensibly mean this one —
    // that is the whole point of docking it here rather than leaving it a tab.
    // The connection/database/knowledge-base halves are left exactly as
    // MainFrame built them.
    host.runQuery = [this](const wxString& sql) {
        const wxString q = ScriptedFormat(sql);
        InsertQuery(q);
        RequestRun();
    };
    host.insertToEditor = [this](const wxString& sql) {
        InsertQuery(ScriptedFormat(sql));
    };
    aiHost_ = std::move(host);
}

// ---------------------------------------------------------------------------
// 灰字建议 / ghost text
// ---------------------------------------------------------------------------
void EditorPage::UpdateGhost()
{
    if (!ghost_ || !stc_) return;

    // THE POPUP WINS. While Scintilla's completion list is up it owns Tab, and
    // showing a second suggestion that also claims Tab is how the key stops
    // being predictable.
    if (stc_->AutoCompActive() || stc_->CallTipActive()) { HideGhost(); return; }

    const int pos  = stc_->GetCurrentPos();
    const int line = stc_->LineFromPosition(pos);

    // Local tier first — instant and free. The AI tier arrives later, out of a
    // network callback, and simply calls back in here with better text.
    const wxString before = stc_->GetTextRange(0, pos);
    const wxString after  = stc_->GetTextRange(pos, stc_->GetLineEndPosition(line));
    wxString text = ghost::LocalSkeleton(before, after,
                                         comp_.dialect ? comp_.dialect() : db::Dialect::MySQL);
    // An AI suggestion supersedes the skeleton when one has arrived for this
    // caret position: it saw the whole statement, the skeleton saw one keyword.
    if (aiCompleter_ && !aiCompleter_->Suggestion().IsEmpty())
        text = aiCompleter_->Suggestion();

    if (text.IsEmpty()) { HideGhost(); return; }

    // Geometry: the caret in screen coordinates, the line's own height, and
    // whatever width is left before the editor's right edge. Clipping to that
    // width is what stops a long AI line from painting a floating strip across
    // the desktop.
    const wxPoint caret = stc_->PointFromPosition(pos);
    const int lineH = stc_->TextHeight(line);
    const int maxW  = stc_->GetClientSize().GetWidth() - caret.x - 4;
    if (maxW <= 8) { HideGhost(); return; }        // caret at the right margin
    ghost_->ShowGhost(stc_->ClientToScreen(caret),
                      stc_->StyleGetFont(wxSTC_STYLE_DEFAULT), text, lineH, maxW);
}

void EditorPage::HideGhost()
{
    if (ghost_) ghost_->HideGhost();
}

bool EditorPage::AcceptGhost()
{
    if (!ghost_ || !ghost_->Active() || !stc_) return false;
    const wxString text = ghost_->Suggestion();
    if (text.IsEmpty()) return false;

    HideGhost();
    if (aiCompleter_) aiCompleter_->ClearSuggestion();   // it has been consumed

    // ONE ordinary edit, so it undoes as one Ctrl+Z and the dirty flag moves
    // exactly as it would had the user typed it — which, as far as the document
    // is concerned, is the first moment this text has ever existed.
    const int pos = stc_->GetCurrentPos();
    stc_->InsertText(pos, text);
    stc_->GotoPos(pos + static_cast<int>(text.length()));
    return true;
}

void EditorPage::OnEditorKey(wxKeyEvent& ev)
{
    // TAB ACCEPTS THE GHOST — but only when the ghost is what is on screen.
    // AcceptGhost() returns false if there is nothing to take, and Tab then
    // falls through to its ordinary job (indent / the completion list), so this
    // never swallows the key on a guess.
    if (ev.GetKeyCode() == WXK_TAB && !ev.ControlDown() && !ev.ShiftDown() &&
        !ev.AltDown() && AcceptGhost())
        return;
    // Any other key invalidates what is drawn: the caret is about to move or
    // the text is about to change, and a suggestion computed for the previous
    // position would be pointing at the wrong place. OnCharAdded recomputes it
    // immediately afterwards for the printable ones.
    if (ev.GetKeyCode() != WXK_TAB) HideGhost();

    if (aiCompleter_ && aiCompleter_->HandleKey(ev))
        return;
    // Ctrl+Space → force the completion popup
    if (ev.ControlDown() && ev.GetKeyCode() == WXK_SPACE) {
        ShowWordCompletions(true);
        return;
    }
    // Ctrl+F → open the find/replace bar (prefilled from the current selection).
    if (ev.ControlDown() && !ev.ShiftDown() && !ev.AltDown() && ev.GetKeyCode() == 'F') {
        ShowFindBar();
        return;
    }
    // Ctrl+S → an object editor has no on-disk file, so save == run (execute the
    // CREATE against the database, unchanged). A plain SQL editor instead saves the
    // whole buffer to a named script in the script library.
    if (ev.ControlDown() && !ev.ShiftDown() && !ev.AltDown() && ev.GetKeyCode() == 'S') {
        if (objectEditor_) RequestRun();
        else               SaveScript();
        return;
    }
    // F8 → execute (a second run shortcut alongside F5 / Ctrl+S).
    if (ev.GetKeyCode() == WXK_F8) {
        RequestRun();
        return;
    }
    // F10 → run the current 函数/存储过程 with a parameter dialog (object editors
    // only; RunCurrentRoutine lives in EditorPageObject.cpp). A plain query tab
    // lets F10 fall through (Skip); an object editor consumes it (view/package
    // are a no-op).
    if (ev.GetKeyCode() == WXK_F10) {
        if (RunCurrentRoutine()) return;
        ev.Skip();
        return;
    }
    // Esc closes the find bar when it's open.
    if (ev.GetKeyCode() == WXK_ESCAPE && findBar_ && findBar_->IsShown()) {
        HideFindBar();
        return;
    }
    ev.Skip();
}

void EditorPage::RequestRun()
{
    // Reuse MainFrame's F5/RUN handler (bound to ID_RUN_QUERY on the frame), which
    // runs the *active* tab — this one, since it has focus.
    wxCommandEvent e(wxEVT_MENU, ID_RUN_QUERY);
    e.SetEventObject(this);
    if (wxWindow* top = wxGetTopLevelParent(this))
        top->GetEventHandler()->ProcessEvent(e);
}

// Ctrl+S in a plain SQL editor. Hand the whole buffer + our current saved name to
// MainFrame's hook, which prompts (first save), writes the .sql via ScriptStore,
// and returns the name it saved under (empty = cancelled/failed → stay unnamed).
void EditorPage::SaveScript()
{
    if (!saveScript_ || !stc_) return;
    const wxString name = saveScript_(stc_->GetText(), savedScriptName_);
    if (!name.IsEmpty()) savedScriptName_ = name;
}

// ---------------------------------------------------------------------------
// Scripted verification hook (SWIFTSQL_AUTOEDIT).
// ---------------------------------------------------------------------------
// THIS FUNCTION MUST CONTAIN NO COMPLETION LOGIC. It used to: it ran its own
// qualifier-chain parse over the wxString `context`, sorted the result itself,
// and only then fired the real popup — whose answer it never looked at. So the
// mechanism that exists to VERIFY completion was reporting a second, independently
// written implementation, and the two silently disagreed. They agreed on ASCII,
// which is all anyone ever tested it with, and disagreed on exactly the inputs a
// verifier is for: a CJK identifier (the popup offered nothing, the hook offered
// the columns) and a malformed chain like `a..b.` (likewise).
//
// A verifier that does not share the subject's offset semantics verifies nothing.
// So the hook now DRIVES production and REPORTS what production showed. If
// ShowColumnCompletions regresses, this returns the regressed list; there is no
// longer a second opinion for it to be quietly right against.
wxString EditorPage::ScriptedComplete(const wxString& context)
{
    stc_->SetText(context);
    stc_->GotoPos(stc_->GetTextLength());
    stc_->SetFocus();   // fire the real popup so it can also be seen on screen

    const bool qualified = !context.IsEmpty() && context.Last() == '.';
    const std::vector<wxString> shown = qualified ? ShowColumnCompletions()
                                                  : ShowWordCompletions(true);
    return editorcomp::JoinWords(shown);
}

} // namespace ui
