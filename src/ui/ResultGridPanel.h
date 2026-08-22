// ResultGridPanel.h — the shared results/data-browser surface: a grid + messages
// notebook, an optional right-hand table-info panel, a bottom cell viewer, a
// data-edit toolbar (edit → INSERT/UPDATE/DELETE diff), sort/hide, filter, form
// view, CSV import/export, and a pager. Extracted from EditorPage per the
// 1000-line file charter (docs/CHARTER.md) so both the SQL editor tab and the
// dedicated open-table tab render results through the same component.
//
// A `Features` switch turns optional chrome on/off: an SQL editor result view
// uses Full() (everything), an open-table browse view uses Browse() (pager +
// editing + cell viewer + info panel, no filter/sort/form/import/export). Gated
// chrome is never created or laid out when its flag is off.
//
// The grid is editable when the current result is a simple single-table SELECT
// with a primary key (see EditableSpec): edits / added / deleted rows are diffed
// into INSERT/UPDATE/DELETE and handed to a caller-supplied apply callback.
#pragma once

#include <wx/panel.h>
#include <functional>
#include <utility>
#include <vector>
#include "db/DbDriver.h"      // db::QueryResult / db::Dialect / db::TableDetail
#include "ui/DataTransfer.h"  // ExportRequest
#include "ui/IQueryTab.h"     // EditableSpec
#include "ui/ResultGridSql.h" // ui::SortKey + the pure SQL generators (gridsql::)

class wxButton;
class wxChoice;
class wxGrid;
class wxGridEvent;
class wxKeyEvent;
class wxNotebook;
class wxSizer;
class wxSplitterWindow;
class wxStaticText;
class wxTextCtrl;

namespace ui {

class CellViewerPanel;   // bottom cell-content viewer (HEX/JSON/XML/…)
class DataGrid;          // wxGrid with an owner-drawn active-row label
class FilterPanel;       // column condition builder → WHERE
class SortPanel;         // multi-column ORDER BY editor
class TableInfoPanel;    // right-hand table-info fields

class ResultGridPanel : public wxPanel {
public:
    // Metadata switch for the optional chrome. Full() = SQL editor result view
    // (everything); Browse() = open-table view (pager + editing + cell viewer +
    // info panel only). A false flag means the control isn't built, laid out, or
    // wired at all.
    struct Features {
        bool pager      = true;
        bool editable   = true;   // edit toolbar (add/del/save) + txn buttons
        bool filter     = true;
        bool sortHide   = true;   // header sort + hide column + 排序 button
        bool formView   = true;   // 表单 page + grid/form toggle
        bool infoPanel  = true;   // right-hand table-info panel
        bool cellViewer = true;
        bool importCsv  = true;
        bool exportCsv  = true;
        bool exportBottom = false; // 导出 button at the BOTTOM (query window; no top toolbar)
        bool resultTabs = true;   // 结果/消息 notebook; off → bare grid + inline errors

        static Features Full() { return Features{}; }
        static Features Browse()
        {
            Features f;
            f.filter = f.sortHide = f.formView = f.importCsv = f.exportCsv = false;
            return f;
        }
        // "打开表" dedicated data browser: everything Full() has except the form
        // (表单) view, which is deferred to P2. Keeps filter/sort/import/export on.
        // No 结果/消息 notebook either — the browser shows only the data grid, and
        // query errors surface in an inline banner instead of a hidden 消息 page.
        static Features DataBrowser()
        {
            Features f;
            f.formView   = false;
            f.resultTabs = false;
            return f;
        }
    };

    ResultGridPanel(wxWindow* parent, const Features& feat);

    void ShowResult(const db::QueryResult& r, const wxString& target,
                    const EditableSpec& spec = {});
    void ShowError(const wxString& err);
    void Clear();                        // empty the grid (drop stale results/edit state)
    void SetRunning(bool on);

    // Called with the generated DML when the user saves data edits (autonomous:
    // the handler wraps its own BEGIN/COMMIT).
    void SetApplyHandler(std::function<void(const wxString& dml)> h)
    { applyDml_ = std::move(h); }

    // Called to save while a manual transaction is open: the handler runs the DML
    // inside the already-open transaction and does NOT commit (the applied edits sit
    // in the open txn until the user hits 提交事务 / 回滚事务). No nested BEGIN.
    void SetApplyInTxnHandler(std::function<void(const wxString& dml)> h)
    { applyInTxn_ = std::move(h); }

    // Pager row-counter: runs COUNT(*) for `table` (+ optional WHERE) → done(total).
    void SetCountHandler(std::function<void(const wxString& table, const wxString& where,
                                            std::function<void(long long)> done)> h)
    { countRows_ = std::move(h); }

    // Single-table info for the right-hand panel: runs GetTableDetail → done(detail).
    void SetTableDetailHandler(std::function<void(const wxString& table,
                               std::function<void(const db::TableDetail&)> done)> h)
    { tableDetail_ = std::move(h); }

    // The pager/filter stage the SQL to re-run here; the container decides how to
    // surface it (editor buffer for a query tab, footer line for a data tab).
    void SetQuerySink(std::function<void(const wxString& sql)> h)
    { stageQuery_ = std::move(h); }

    // Fired whenever the rows/db/SQL readout changes; the container (MainFrame)
    // mirrors it to the global bottom status bar (shown only for data-browser tabs).
    void SetStatusChanged(std::function<void()> h) { statusChanged_ = std::move(h); }
    void GetStatusInfo(wxString& rows, wxString& db, wxString& sql) const;

    // The container turns a packaged export request / import trigger into a
    // dialog against the live connection (the grid holds no IConnection).
    void SetExportHandler(std::function<void(const ExportRequest&)> h)
    { exportHandler_ = std::move(h); }
    void SetImportHandler(
        std::function<void(const wxString& db, const wxString& table, db::Dialect)> h)
    { importHandler_ = std::move(h); }

    // Scripted verification hook (SWIFTSQL_AUTOEDIT): set row 0's `col` cell to
    // `value` and return the resulting DML — or, if either the bind gate or the
    // PK gate refuses, WHICH one refused and why, with the grid left untouched.
    // Statements are produced only for ScriptedEditStatus::Ok.
    gridsql::ScriptedEditResult ScriptedEdit(const wxString& col, const wxString& value);

    // ---- data browser (打开表) hosting ----
    // Bind this grid to a concrete table so its pager / sort / filter build a
    // db-qualified SELECT (MySQL needs `db`.`table` when no default schema is
    // selected). Marks the panel as "browse mode": page/sort/where state is owned
    // by the pager, not reset by ShowResult.
    void SetSourceTable(const wxString& db, const wxString& table, db::Dialect dialect);
    void LoadFirstPage();               // count + load page 1 (call after handlers wired)

    // Unsaved-edit guard (design decision B). Returns true if the caller may
    // proceed (no pending diff, or user chose 保存/放弃); false to abort (取消).
    bool ConfirmDiscardPendingEdits();
    bool HasUnsavedEdits() const;       // any pending INSERT/UPDATE/DELETE diff

private:
    void OnAddRow(wxCommandEvent&);
    void OnDeleteRow(wxCommandEvent&);
    void OnSaveData(wxCommandEvent&);
    void DiscardPendingReload();                 // 取消 / txn close → reload current page
    void AutoSaveOnLeave();                       // apply pending when focus leaves the grid
    // The full edit diff: statements + dirty flag + the PK gate's verdict.
    // Everything that needs "is there anything pending?" must read .dirty, not
    // .sql — .sql is also empty when the gate REFUSES, and treating a refusal as
    // "clean" is how the refused edits get silently thrown away.
    gridsql::EditDml BuildEditDml() const;
    wxString BuildDml() const;                  // .sql only: "" if clean OR refused
    // Tell the user why the edit path refused. No-op when the gate is Ok.
    void ReportPkGateRefusal(const gridsql::EditDml& d);
    void UpdateEditButtons();

    // ---- multi-column sort model (design: docs/UI/data-browser-sort-spec.md) ----
    // The type itself is ui::SortKey (ResultGridModel.h) so the pure sort/SQL
    // modules and this widget share one definition.
    using SortKey = ui::SortKey;

    // Snapshot helpers: lift the wxGrid's contents into plain values so the pure
    // modules (gridsql:: / gridmodel::) can do the actual work.
    gridsql::DmlContext MakeDmlContext() const;
    gridsql::CellGrid   SnapshotGrid() const;                        // every row
    gridsql::CellGrid   SnapshotRows(const std::vector<int>& rows) const;
    std::vector<int>    SelectedOrCursorRows() const;   // selection block, else cursor row

    // ---- data browser: cell menu / sort / hide-column ----
    void OnGridCellRightClick(wxGridEvent&);
    void OnGridLabelLeftClick(wxGridEvent&);
    void OnGridLabelRightClick(wxGridEvent&);
    void ShowRowLabelMenu(int row);              // whole-row right-click menu (gutter)
    void SetRowWash(int row, bool on);           // amber wash on the editing row
    void SortByColumns(const std::vector<SortKey>& keys);  // client-side multi-key sort
    void ApplySort(int col, bool asc);                     // replace sort with one key
    void ToggleSortKey(int col);                           // Shift-click: append/flip/remove
    void ApplySortKeys(const std::vector<std::pair<int, bool>>& keys);  // from SortPanel
    void SetSortKeysAndApply(std::vector<SortKey> keys);   // guard + assign + re-query
    void ToggleSortPanel();
    void UpdateTxnButtons();                      // stateful 开始 ↔ 回滚+提交 toolbar swap
    void RemoveAllSortAndFilters();               // clear WHERE + ORDER BY → page 1
    void FilterByCellValue(int col, bool exclude);// col = / <> this cell's value
    void SetCellViewerType(int typeIndex);        // 显示▶ render-type switch
    void RefreshCurrent();                         // guarded refresh (re-run current page)
    void RelabelSortHeaders();
    void DeleteGridRow(int row);
    void SetCellText(int row, int col, const wxString& v);
    void RecolourNull(int row, int col);         // grey NULL marker / normal for a value
    void OnGridKeyDown(wxKeyEvent&);              // P0 grid shortcuts (copy/paste/delete/…)
    void OnGridDeleteKey();                       // Delete: full-row → delete rec, cell → NULL
    void DeleteSelectedRecordsWithConfirm();      // confirmed, bottom-up row delete
    std::vector<int> SelectedFullRows() const;   // rows selected whole (row-label click)
    void CopyCells();                            // full-row TSV if rows selected, else block/cell
    void CopyRowsAsSql();
    void CopyRowsAsDml(bool asUpdate);           // 复制为 UPDATE (true) / DELETE (false) 语句
    void PasteIntoCell(int row, int col);        // whole-row fill for TSV, else single cell
    std::vector<SortKey> sortKeys_;               // ORDER BY priority (index 0 = first key)

    // ---- chrome assembly (ResultGridPanelChrome.cpp) ----
    // The ctor stays a readable skeleton: it configures the grid + splitter and
    // calls these to build each toolbar/panel band in order.
    void BuildTopToolbar(wxSizer* v);               // 筛选·排序·文本▾·事务·导入·导出
    void BuildViewTypeButton(wxWindow* bar, wxSizer* h);  // the 文本▾ split button
    void BuildCellViewerChrome(wxSizer* v);         // collapsible header + viewer
    void BuildBottomBar(wxSizer* v);                // edit ops + pager + view toggle
    void BuildBottomExportBar(wxSizer* v);          // 导出 at the very bottom (query window)
    void BuildFilterSortArea(wxSizer* v);           // container + draggable divider
    void PostCommand(int id);                       // bubble a MainFrame command ID up
    void UpdatePanelAreaVisibility();               // show/hide the area + grip
    void OnGridSelectCell(wxGridEvent&);
    void ToggleFilterPanel();
    void ToggleCellViewer();                        // collapse/expand the cell viewer
    void UpdateCellViewerSummary(const wxString& value);
    void ShowViewTypeMenu();                         // split-button ▾ side: type menu
    void ToggleViewTypeBody();                       // split-button body: show/hide viewer
    void ShowCellViewerAs(int typeIndex);            // reveal viewer + set render type
    wxString QualifiedPageTable() const;            // `db`.`table` for browse mode
    void ApplyFilter(const wxString& where);
    ExportRequest BuildExportRequest() const;       // package the current view for export
    void StartExport();                             // 导出 / 将数据另存为… → export handler
    void StartImport();                             // 导入 → import handler

    // ---- grid / form (表单) view toggle + record status ----
    void RefreshFormView(int rowOverride = -1);
    void ShowFormView();
    void UpdateRecordStatus(int row);

    // ---- pagination ----
    void GoToPage(int page);
    void LoadPage();
    void RequestCount();
    void UpdatePager();

    // ---- right-hand info panel ----
    void RefreshInfoPanel();

    Features          feat_;
    wxSplitterWindow* resultsSplit_ = nullptr;        // [ results notebook | info panel ]
    TableInfoPanel*   infoPanel_ = nullptr;
    std::function<void(const wxString&, std::function<void(const db::TableDetail&)>)> tableDetail_;
    wxNotebook*       results_ = nullptr;              // null in no-tab (data browser) mode
    wxWindow*         resultsPane_ = nullptr;          // left pane of resultsSplit_ (notebook or bare grid)
    wxStaticText*     errorBar_ = nullptr;             // inline error banner when there is no 消息 tab
    wxGrid*           grid_ = nullptr;
    DataGrid*         activeGrid_ = nullptr;   // same object as grid_, typed for SetActiveRow
    int               activeRow_  = -1;        // current row (gutter highlight)
    int               editRow_    = -1;        // row being edited (amber wash)
    wxString          lastSql_;                // last staged paged SELECT (status bar)
    std::function<void()> statusChanged_;
    wxTextCtrl*       messages_ = nullptr;
    wxStaticText*     status_ = nullptr;
    wxButton*         addRow_ = nullptr;
    wxButton*         delRow_ = nullptr;
    wxButton*         saveData_ = nullptr;
    wxButton*         cancelBtn_ = nullptr;   // 取消: discard pending edits (safety net)
    wxButton*         filterBtn_ = nullptr;
    wxWindow*         viewTypeBtn_ = nullptr;   // top-toolbar 文本▾ split button (body | ▾)
    bool              viewTypeHover_ = false;   // hover tint for the split button
    wxButton*         sortBtn_   = nullptr;
    wxButton*         importBtn_ = nullptr;
    wxButton*         exportBtn_ = nullptr;
    // Stateful transaction slot on the toolbar: 开始事务 when idle; 回滚+提交 while
    // a manual transaction is open. inTxn_ drives UpdateTxnButtons().
    wxButton*         txBeginBtn_    = nullptr;
    wxButton*         txRollbackBtn_ = nullptr;
    wxButton*         txCommitBtn_   = nullptr;
    bool              inTxn_ = false;
    FilterPanel*      filterPanel_ = nullptr;
    SortPanel*        sortPanel_   = nullptr;
    // Resizable filter/sort area: a container holding both panels + a draggable grip
    // between it and the grid. Shown only while at least one panel is open.
    wxWindow*         panelArea_ = nullptr;
    wxWindow*         panelGrip_ = nullptr;
    int               panelAreaHeight_ = 168;   // drag-controlled height of the area
    int               dragStartY_ = 0, dragStartH_ = 0;
    CellViewerPanel*  cellViewer_ = nullptr;
    // Cell-viewer collapse chrome (unified chevron pattern). Collapsed state
    // persists across page turns (never reset in ShowResult/LoadPage).
    wxWindow*     cvHeader_  = nullptr;
    wxButton*     cvChevron_ = nullptr;
    wxStaticText* cvSummary_ = nullptr;
    bool          cellViewerCollapsed_ = false;
    bool          cellViewerVisible_   = false;   // hidden until 文本▾ is used

    // ---- grid / form view ----
    wxWindow*         formView_    = nullptr;
    int               formPageIdx_ = -1;
    wxButton*         gridViewBtn_ = nullptr;
    wxButton*         formViewBtn_ = nullptr;
    wxStaticText*     recordLabel_ = nullptr;

    // ---- bottom toolbar: left ops + right pager ----
    wxButton*     stopBtn_   = nullptr;
    wxButton*     pageFirst_ = nullptr;
    wxButton*     pagePrev_  = nullptr;
    wxButton*     pageNext_  = nullptr;
    wxButton*     pageLast_  = nullptr;
    wxStaticText* pageLabel_ = nullptr;
    wxChoice*     pageSizeChoice_ = nullptr;

    // ---- pagination state ----
    std::function<void(const wxString&, const wxString&, std::function<void(long long)>)> countRows_;
    std::function<void(const wxString& sql)> stageQuery_;
    wxString    pageTable_;
    wxString    pageDb_;                    // db qualifier for browse mode ("" = none)
    wxString    pageWhere_;
    db::Dialect pageDialect_ = db::Dialect::MySQL;
    bool        browseMode_  = false;       // "打开表": page/sort/where owned by pager
    wxString    infoShownTable_;            // table currently shown in the info panel
    int         pageSize_  = 1000;          // data-browser default (design §6)
    int         curPage_   = 0;
    long long   totalRows_ = -1;
    bool        paginating_ = false;

    // ---- data-edit state ----
    EditableSpec                       spec_;
    std::vector<std::vector<wxString>> origRows_;
    std::vector<int>                   rowOrig_;
    std::vector<std::vector<wxString>> deleted_;
    std::vector<wxString>              columns_;

    std::function<void(const wxString&)> applyDml_;
    std::function<void(const wxString&)> applyInTxn_;   // apply inside open txn (no commit)
    std::function<void(const ExportRequest&)> exportHandler_;
    std::function<void(const wxString&, const wxString&, db::Dialect)> importHandler_;
};

} // namespace ui
