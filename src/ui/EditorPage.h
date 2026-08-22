// EditorPage.h — one SQL editor tab: a syntax-highlighted editor (with
// autocompletion + SQL formatting) atop a shared ResultGridPanel that renders
// the query result (grid/messages, editing, sort/filter/form, pager, info
// panel, CSV). The run / format / explain actions live in MainFrame's top
// toolbar and act on the active tab. Self-contained so multiple tabs never
// clobber each other. Extracted from MainFrame per the 1000-line file charter
// (docs/CHARTER.md); the results surface itself lives in ResultGridPanel.
//
// ---------------------------------------------------------------------------
// WHERE THE IMPLEMENTATION LIVES. EditorPage is ONE class across four TUs, split
// again under the same charter ceiling. The seam was chosen for testability, not
// by halving the widget:
//
//   EditorPage.cpp        construction, the IQueryTab surface, autocompletion
//                         wiring, key routing.
//   EditorPageObject.cpp  object-editor mode — the 连接/数据库 selector bar,
//                         CommitRunTarget, the bottom save-status strip, F10.
//   EditorPageFind.cpp    the Ctrl+F find/replace bar.
//   EditorSqlFormat.{h,cpp}   PURE, unit-tested: the SQL beautifier.
//   EditorCompletion.{h,cpp}  PURE, unit-tested: which words complete and which
//                             schema source answers a dot-qualified prefix.
//
// The two PURE modules are the payoff. Everything they hold used to be inline
// among wxSTC calls and was reachable only by typing into a running app attached
// to a live server. The two .cpp-only siblings are widget-level seams (the
// ResultGridPanelChrome.cpp precedent) and are NOT testable headlessly — which
// is exactly why they are isolated, keeping untestable construction out of the
// TUs that hold behaviour.
#pragma once

#include <wx/panel.h>
#include <functional>
#include <memory>
#include <vector>
#include "db/DbDriver.h"       // db::Dialect
#include "ui/IQueryTab.h"      // IQueryTab / EditableSpec
#include "ui/ResultGridSql.h"  // gridsql::ScriptedEditResult (the AUTOEDIT hook)
#include "ui/ObjectTemplates.h" // ObjectKind
#include "ui/AiChatPanel.h"     // AiChatHost (held by value; the panel is lazy)
#include "ui/FlatControls.h"  // ui::BoxDropdown (the 连接/数据库 selectors)

class wxBitmapButton;
class wxCheckBox;
class wxKeyEvent;
class wxSplitterWindow;
class wxStaticBitmap;
class wxStaticText;
class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;

namespace ui {

class AiSqlCompleter;
class GhostTextOverlay;
class ResultGridPanel;

class EditorPage : public wxPanel, public IQueryTab {
public:
    explicit EditorPage(wxWindow* parent, const wxString& sql);
    ~EditorPage() override;

    // The embedded results surface (so MainFrame can wire its grid handlers).
    ResultGridPanel* Grid() const { return grid_; }

    void InsertQuery(const wxString& sql);

    // ---- AI (right-side panel + performance analysis) ----------------------
    // MainFrame supplies the connection/database/knowledge-base half of the
    // host; this page overrides the two SQL-routing callbacks so generated SQL
    // lands in THIS editor rather than "whichever tab is active".
    void SetAiHost(AiChatHost host);
    // 性能分析: take the statement under the caret, ask the database for its
    // EXECUTION PLAN (EXPLAIN — never ANALYZE, so nothing is executed), then
    // hand plan + schema + SQL to the model and stream the answer into the
    // right-side panel, opening it if needed. No-op with a status hint when
    // there is no connection or the statement has no plan (DDL, SET, …).
    void AnalyzePerformance();
    void FormatActive();                         // beautify selection / whole buffer

    // Thin forwards to the embedded wxStyledTextCtrl so the frame 编辑 menu
    // (撤销/重做/剪切/复制/粘贴) can act on the active editor tab without reaching
    // into stc_ directly. No-ops are handled inside wxSTC (e.g. Undo with an empty
    // undo stack). FullText() returns the whole buffer (QueryText() would return the
    // selection when one exists) for 文件 ▸ 保存.
    void EditUndo();
    void EditRedo();
    void EditCut();
    void EditCopy();
    void EditPaste();
    wxString FullText() const;

    // Object-editor tabs (新建/修改 视图·函数·存储过程·包) run a CREATE statement
    // and then rename their tab to the object name; MainFrame checks this flag in
    // the run-success path. A plain query tab leaves it false.
    void MarkObjectEditor(bool on, ObjectKind kind = ObjectKind::View)
        { objectEditor_ = on; objKind_ = kind; }
    bool IsObjectEditor() const { return objectEditor_; }
    ObjectKind ObjectEditorKind() const { return objKind_; }   // valid when IsObjectEditor()

    // F10 in a function / stored-procedure editor → run the object currently in the
    // buffer with a parameter dialog. EditorPage stays decoupled from the connection
    // tree: it hands the (opaque connection handle, db, kind, object name) chosen in
    // the top selector to this hook, which shows RoutineExecDialog and executes the
    // CALL/SELECT (MainFrame routes it to ConnectionTree::ExecuteRoutine). The handle
    // is the same opaque value SetObjectTarget deals in — never dereferenced here.
    using RunRoutineHook = std::function<void(void* conn, const wxString& db,
                                              ObjectKind kind, const wxString& name)>;
    void SetRunRoutineHook(RunRoutineHook h) { runRoutine_ = std::move(h); }

    // Bottom result strip for an object editor: green "保存成功" on a successful
    // CREATE, or a red error line on failure. MainFrame's run-result path calls
    // this instead of the results grid for object-editor tabs (they run DDL that
    // returns no rows, so the editor stays full-window). No-op if not an object
    // editor.
    void ShowObjectSaveStatus(bool ok, const wxString& msg);

    // Object-editor top selector bar (连接 + 数据库). Only object editors show it;
    // a plain query tab never builds it. All data comes from MainFrame via these
    // hooks so EditorPage stays decoupled from the connection tree. Connection
    // handles are opaque (void*): EditorPage never dereferences them, it only
    // hands them back to `databases` / `setTarget`, which validate liveness.
    struct ObjectTarget {
        // (display label, opaque connection handle) for every *connected* connection.
        std::function<std::vector<std::pair<wxString, void*>>()>          connections;
        // databases of the given connection handle (empty if none / disconnected).
        std::function<std::vector<wxString>(void*)>                       databases;
        // Make (handle, db) the run target right before a run; on a dead/stale
        // handle it fills `err` and returns false so the caller aborts the run.
        std::function<bool(void*, const wxString& db, wxString& err)>     setTarget;
    };
    // Build + populate the selector bar and remember the default (connection, db).
    void SetObjectTarget(ObjectTarget src, void* defConn, const wxString& defDb);

    // Ctrl+S in a PLAIN SQL editor (not an object editor) saves the whole buffer
    // as a named script file. EditorPage stays decoupled from ScriptStore and the
    // connection tree: it hands the full buffer + its current saved name to this
    // hook (set by MainFrame), which prompts for a name on a first save, writes the
    // .sql via core::ScriptStore with the active connection user as owner/exec,
    // refreshes the script-library tab, and returns the saved name ("" = cancelled
    // / failed, so the tab stays "unnamed" and the next Ctrl+S re-prompts).
    using SaveScriptHook = std::function<wxString(const wxString& fullSql,
                                                  const wxString& curName)>;
    void SetSaveScriptHook(SaveScriptHook h) { saveScript_ = std::move(h); }
    // Opening a stored script into this tab primes its saved name so the first
    // Ctrl+S overwrites in place instead of re-prompting.
    void PrimeSavedScriptName(const wxString& name) { savedScriptName_ = name; }
    // Push the bar's selected connection+db to the run target just before a run.
    // Returns true (no-op) for a plain tab or an object editor with no selector;
    // false + `err` if the chosen connection is no longer live (caller aborts).
    bool CommitRunTarget(wxString& err);

    // ---- IQueryTab ----
    wxString QueryText() const override;         // selection, else whole buffer
    void SetRunning(bool on) override;
    void ShowResult(const db::QueryResult& r, const wxString& target,
                    const EditableSpec& spec = {}) override;
    void ShowError(const wxString& err) override;
    bool ConfirmClose() override;                // unsaved-edit guard

    // Autocompletion source: dialect (for keyword/function lists) + live schema
    // (table names, and lazily a table's column names). Set by MainFrame from
    // the active connection.
    struct CompletionSource {
        std::function<db::Dialect()>                              dialect;
        std::function<std::vector<wxString>()>                    databases;
        std::function<std::vector<wxString>()>                    tables;
        std::function<std::vector<wxString>(const wxString&)>     columns;
        // db-qualified sources so completion works even with no active DB
        // selected: tablesOf(db) → tables in db; columnsOf(db, table) → columns.
        std::function<std::vector<wxString>(const wxString&)>                    tablesOf;
        std::function<std::vector<wxString>(const wxString&, const wxString&)>   columnsOf;
    };
    void SetCompletionSource(CompletionSource s);

    // Scripted verification hooks. ScriptedEdit reports an OUTCOME, not just
    // text: an empty statement list has several distinct causes (see
    // gridsql::ScriptedEditStatus) and the hook must not conflate them.
    gridsql::ScriptedEditResult ScriptedEdit(const wxString& col, const wxString& value);
    wxString ScriptedFormat(const wxString& sql) const { return FormatSql(sql); }
    wxString ScriptedComplete(const wxString& context);

private:
    // A Scintilla indicator (0-31) for find/replace match highlighting. High
    // number to stay clear of the SQL lexer's own indicator usage. Shared: the
    // constructor styles it (EditorPage.cpp), the find bar fills and clears it
    // (EditorPageFind.cpp).
    static constexpr int kFindIndicator = 20;

    // Dialect-aware pretty-print. Resolves the active dialect to a keyword set
    // and delegates to ui::editorfmt::FormatSql (EditorSqlFormat.h).
    wxString FormatSql(const wxString& src) const;

    void ApplyKeywordLists(db::Dialect d);      // lexer keyword(0)+function(1) lists
    void OnCharAdded(wxStyledTextEvent&);
    void OnEditorKey(wxKeyEvent&);

    // ---- 灰字建议 / ghost text (GhostTextOverlay + GhostSuggest) -----------
    // Recompute and (re)place the grey suggestion after the caret. Two tiers
    // feed it: ui::ghost::LocalSkeleton answers instantly and offline for the
    // obvious cases (`select ` -> ` * from `), and AiSqlCompleter overrides it
    // with a model suggestion once the statement is long enough to deserve one.
    //
    // MUTUALLY EXCLUSIVE WITH THE COMPLETION POPUP, and the popup wins: while
    // Scintilla's list is up, Tab means "take the highlighted entry", and a
    // second thing on screen claiming the same key is how a user ends up unable
    // to predict what Tab does.
    void UpdateGhost();
    void HideGhost();
    // Insert what the ghost is showing, at the caret, as real text. Returns
    // false when there is nothing to accept — the caller then lets Tab do its
    // ordinary job rather than swallowing the key.
    bool AcceptGhost();

    GhostTextOverlay* ghost_ = nullptr;   // owned by wx (child of this page)

    // ---- 右侧 AI 面板 -------------------------------------------------------
    // Per-tab (each SQL window keeps its own conversation) and LAZY (the panel,
    // its AiClient and its schema knowledge base are built on the first expand,
    // so unopened tabs cost nothing). Collapsing keeps the panel alive so the
    // conversation survives.
    void ToggleAiPanel();

    wxSplitterWindow* aiSplit_     = nullptr;   // [ editor+results | AI panel ]
    AiChatPanel*      aiPanel_     = nullptr;   // created on first expand
    wxPanel*          aiRail_      = nullptr;   // 30px right edge strip
    wxBitmapButton*   aiToggleBtn_ = nullptr;
    AiChatHost        aiHost_;                  // supplied by MainFrame (SetAiHost)
    // Both RETURN THE WORDS THEY PUT IN THE POPUP (empty = nothing shown). That
    // return value is not decoration: ScriptedComplete, the SWIFTSQL_AUTOEDIT
    // verification hook, reports it instead of computing a parallel answer, which
    // is what it used to do and why it could not detect a completion bug.
    std::vector<wxString> ShowWordCompletions(bool force);   // keywords + functions + tables
    std::vector<wxString> ShowColumnCompletions();           // columns of "ident." before caret
    void ShowCallTip();                          // function signature after '('
    // (word, completion category) — the category drives the per-item icon/colour.
    std::vector<std::pair<wxString, int>> WordCandidates() const;
    void RegisterCompletionIcons();             // category icons for the autocomplete list
    wxString ResolveTableForIdent(const wxString& ident);   // alias → real table
    bool IsDatabaseName(const wxString& ident) const;       // ident matches a live db (ci)

    // ---- object-editor top selector bar (连接 + 数据库) — EditorPageObject.cpp ----
    void BuildObjToolbar();                      // lazy: the selector row at the very top
    void PopulateConnChoice();                     // fill 连接 dropdown, select the default
    void PopulateDbChoice(const wxString& prefer); // fill 数据库 for the selected conn

    // (BuildObjStatusBar was grouped under the find bar below; it belongs here.)
    void BuildObjStatusBar();                     // lazy: the object-editor bottom result strip

    // ---- find / replace bar (Ctrl+F) — EditorPageFind.cpp ----
    void BuildFindBar();
    void ShowFindBar();                          // reveal + focus, prefill from selection
    void HideFindBar();                          // hide + clear match highlights
    void DoFind(bool forward);                   // select next/prev match (wraps)
    void DoReplace();                            // replace current match, advance
    void DoReplaceAll();                         // replace every match (one undo step)
    void MarkAllMatches();                       // indicator-highlight every match
    int  SearchFlags() const;                    // case flag from the checkbox
    void RequestRun();                           // object editor: Ctrl+S → shared RUN action
    void SaveScript();                           // plain editor: Ctrl+S → save buffer to a script file
    // F10 → run the current 函数/存储过程 with a parameter dialog. Returns true if
    // the key was consumed (object editor, incl. view/package no-op), false for a
    // plain query tab so the caller lets F10 propagate.
    bool RunCurrentRoutine();

    wxSplitterWindow* resultSplit_ = nullptr;   // editor (top) / results (bottom), draggable
    wxStyledTextCtrl* stc_ = nullptr;
    ResultGridPanel*  grid_ = nullptr;
    std::unique_ptr<AiSqlCompleter> aiCompleter_;
    CompletionSource  comp_;

    // find/replace bar (built lazily on first Ctrl+F)
    wxWindow*     findBar_    = nullptr;
    wxTextCtrl*   findCtrl_   = nullptr;
    wxTextCtrl*   replaceCtrl_ = nullptr;
    wxCheckBox*   matchCase_  = nullptr;
    wxStaticText* matchLabel_ = nullptr;

    // object-editor bottom result strip (built lazily on first run result)
    wxWindow*         objStatusBar_  = nullptr;
    wxStaticBitmap*   objStatusIcon_ = nullptr;
    wxStaticText*     objStatusText_ = nullptr;

    // object-editor top selector bar (built lazily in SetObjectTarget)
    wxWindow*         objToolbar_  = nullptr;
    ui::BoxDropdown*  connChoice_  = nullptr;
    ui::BoxDropdown*  dbChoice_    = nullptr;
    ObjectTarget objTarget_;
    void*        objDefConn_  = nullptr;   // default connection handle
    wxString     objDefDb_;                // default database

    bool objectEditor_ = false;
    ObjectKind      objKind_ = ObjectKind::View;   // set at creation (MarkObjectEditor)
    RunRoutineHook  runRoutine_;                    // F10 executor (set by MainFrame)
    SaveScriptHook  saveScript_;                    // Ctrl+S script save (set by MainFrame)
    wxString        savedScriptName_;               // "" until first successful save; covers overwrite
};

} // namespace ui
