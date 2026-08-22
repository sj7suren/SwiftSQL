// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDesignView.h — table structure browser & editor, styled per docs/UI's
// "表设计" board: header + tab row (字段/索引/外键/DDL).
//
// The 字段 tab is NOT a wxGrid: it is a form of *always-live* controls — every
// editable cell is a real wxTextCtrl / wxComboBox / wxCheckBox, so clicking a cell
// places the caret in it instantly (no select→edit step, no editor spawn, no
// flash). Rows can be added / dropped / retyped, then the diff against the loaded
// snapshot is rendered to ALTER TABLE (ADR-014, via db::DialectProfile::RenderAlter)
// and executed after an explicit preview-confirm.
//
// ADR-014: the editor talks ONLY to a db::DialectProfile (resolved from the
// connection's DbType) — no `if (dialect == …)` here. The type dropdown, the
// length/precision editability, the right-hand attribute panel, and the save path
// are all data-driven off the profile's Types() / ApplicableAttrs() / RenderAlter().
#pragma once

#include <wx/arrstr.h>
#include <wx/panel.h>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "db/DbDriver.h"
#include "db/DialectProfile.h"

class wxButton;
class wxCheckBox;
class wxComboBox;
class wxControl;
class wxFlexGridSizer;
class wxGrid;
class wxScrolledWindow;
class wxSimplebook;
class wxSizer;
class wxStaticText;
class wxStyledTextCtrl;
class wxTextCtrl;

namespace ui {

class FlatButton;   // flat icon+label toolbar button (ui/FlatControls.h)

class TableDesignView : public wxPanel {
public:
    explicit TableDesignView(wxWindow* parent);

    // `type` (from ConnectionProfile.type) selects the DialectProfile that drives
    // the type dropdown, attribute panel and DDL rendering (ADR-014).
    void Load(db::IConnection* conn, const wxString& database,
              const wxString& table, db::DbType type);

    // Scripted verification hook: append a column with the given definition,
    // generate the ALTER DDL, execute it (no confirm), reload. Returns the DDL
    // that was run, or "ERR: <msg>". Exercises the real edit→DDL→execute path.
    wxString ScriptedAddColumn(const wxString& name, const wxString& type,
                               const wxString& length);

    // Tab close guard (this view is NOT an IQueryTab, so MainFrame calls these
    // directly for design tabs). HasUnsavedChanges compares the edit form against
    // the loaded original snapshot; ConfirmClose asks before discarding if dirty.
    bool HasUnsavedChanges() const;
    bool ConfirmClose();   // true = ok to close (no changes, or user confirmed discard)

    // The 关闭 toolbar button asks the host (MainFrame) to remove this tab. MainFrame
    // wires a callback that programmatically deletes the page (bypassing the tab-X
    // close flow so ConfirmClose isn't prompted twice) after this view has confirmed.
    void SetOnRequestClose(std::function<void()> cb) { onCloseRequest_ = std::move(cb); }

protected:   // protected (not private) so NewTableView can reuse the machinery
    // One always-live editable row of the field form. Grid-visible values live in
    // the controls (the source of truth for name/type/length/…); panel-only edits
    // (default value + dialect attribute bag) and the load-time diff baseline live
    // in the struct fields alongside them.
    struct FieldRow {
        // num/key are read-only, borderless, centred wxTextCtrls (not wxStaticText):
        // a single-line text control vertically centres its text natively, so they
        // fill the cell for the grid-line look AND sit on the row's vertical midline.
        wxWindow*     num = nullptr;      // 序号: painted NumCell (select row + right-click)
        wxTextCtrl*   name = nullptr;
        wxTextCtrl*   type = nullptr;     // 类型: TypeCell (borderless text + custom filter popup)
        wxTextCtrl*   length = nullptr;
        wxTextCtrl*   scale = nullptr;
        wxWindow*     key = nullptr;      // BadgeCell: painted, non-focusable PKn/FK/UQ badge
        wxCheckBox*   notNull = nullptr;
        wxCheckBox*   virt = nullptr;     // 虚拟: checked = VIRTUAL, unchecked = STORED
        wxTextCtrl*   comment = nullptr;

        wxString      origName;           // name before edit ("" = freshly added row)
        wxString      baseKey;            // non-PK introspected badge ("FK"/"UQ"/""); PK
                                          // membership + order is tracked in pkOrder_
        wxString      defaultVal;         // universal default (edited in the panel)
        std::vector<std::pair<wxString, wxString>> attrVals;  // dialect attrs (panel)
        db::ColumnModel baseline;         // load-time snapshot for the real diff
        bool          hasBaseline = false;
    };

    void SelectTab(int index);
    void ShowEmpty(const wxString& msg);
    void PopulateFields();
    void UpdateEditability();
    // Whether the design is editable (drives toolbar buttons + the attribute panel).
    // The edit flow needs a loaded table; NewTableView overrides to drop the table_
    // requirement (a new table has no name yet, so everything would wrongly disable).
    virtual bool IsEditable() const
    { return conn_ && conn_->IsConnected() && !table_.IsEmpty(); }
    // Ctrl+S handler. Base = the original design-table behaviour (save field changes),
    // so the edit flow is byte-for-byte unchanged; NewTableView overrides to CREATE.
    virtual void SaveViaShortcut()
    { if (conn_ && conn_->IsConnected() && !table_.IsEmpty()) SaveChanges(); }

    // ---- field form ----
    void BuildHeaderRow();
    void ResizeColumn(int col, int newWidth);   // header-drag: set a column's width
    FieldRow* AddRowControls();               // create one row's controls, append them
    void FillRowFromColumn(FieldRow* fr, const db::ColumnInfo& c);
    void RenumberRows();
    void SetCurrentRow(int row, bool focusName = false);
    void RepaintRow(int row);                 // apply current/selected look to one row
    void RepaintSelection();                  // …to every row
    int  RowIndexOf(const FieldRow* fr) const;
    void OnRowFocus(FieldRow* fr);            // a control in this row gained focus

    // ---- 序号 row selection (single / Ctrl-toggle / Shift-range) + clipboard ----
    void SelectRow(int row, bool ctrl, bool shift);   // 序号 left-click selection
    void ShowFieldMenu(int row);              // 序号 right-click context menu
    void CopySelectedRows();                  // Ctrl+C: snapshot selected rows
    void PasteRows();                         // Ctrl+V: fill selected blank rows, else append
    void DuplicateCurrentField();             // context menu: clone the current row
    void InferTypeFromName(FieldRow* fr);     // name → default type (id/…→int, date→datetime)
    void OnViewKey(wxKeyEvent& e);            // Ctrl+S / Ctrl+C / Ctrl+V at the view level
    // One field row captured for copy/paste (lossless: raw control + panel state).
    struct RowClip {
        wxString name, type, length, scale, comment, defaultVal;
        bool notNull = false, virt = false;
        std::vector<std::pair<wxString, wxString>> attrVals;
    };
    RowClip CaptureRow(const FieldRow* fr) const;
    void    ApplyRowClip(FieldRow* fr, const RowClip& c);
    void OnRowTypeChanged(FieldRow* fr);      // type edited → length/scale + panel refresh
    void ApplyRowTypeEditability(int row);
    void OnKeyClick(FieldRow* fr);            // toggle this row's composite-PK membership
    void UpdateKeyBadges();                   // repaint every key cell (PKn / FK / UQ)
    // The 9 windows this row occupies in formSizer_, in column order. NOTE the 非空/虚拟
    // checkboxes live inside a filler panel, so the SIZER item is that panel, not the
    // checkbox — detach/move must target these, not the raw fr->notNull/fr->virt.
    std::vector<wxWindow*> RowCells(FieldRow* fr) const;

    // ---- toolbar (field operations) ----
    void OnAddField();          // append a new field row at the end
    void OnInsertField();       // insert a new field row above the current one
    void InsertFieldAt(int at); // shared: seed a fresh row and place it (at<0 = append)
    void OnDeleteField();       // drop the current field row
    void OnToggleKeyOfCurrent();// toggle the current row's composite-PK membership
    void OnMoveField(int delta);// -1 = 上移 / +1 = 下移 (reorder adjacent rows)
    void OnCloseTab();          // confirm (save/discard) then ask the host to close
    void OnSaveChanges();       // preview + execute the ALTER (wrapper over SaveChanges)
    bool SaveChanges();         // do the save; true = applied, false = cancelled/failed
    void SwapAdjacentRows(int a);// swap rows_[a] and rows_[a+1] in the sizer + vector

    // ---- context toolbar (rebuilt per active tab; M2.2) ----
    void RebuildToolbar(int tab);   // clear toolbar_ + add the buttons for `tab`
    virtual void OnSaveActiveTab(); // 保存 dispatcher: routes by tab (NewTableView overrides)
    void OnCopyActiveCode();        // 复制: SQL 预览 / TABLE DDL pane → clipboard
    // Shared save primitive: render an already-built TableEdit → preview → execute →
    // reload. Every tab's save funnels through this. Returns true when applied.
    bool ExecuteEdit(db::TableEdit& edit);
    void SaveComment();             // 注释 tab → ALTER … COMMENT (via ExecuteEdit)

    // ---- generated tabs (⑥SQL 预览 / ⑦TABLE DDL; regenerated on tab switch) ----
    virtual void RefreshGeneratedTabs();  // recompute panes (NewTableView → CREATE only)
    void BuildTableModel(db::TableModel& model) const;  // full snapshot → RenderCreate
    void ApplyCodeKeywords();       // feed db::Keywords(dialect) to the STC panes (colour)
    void SetCodePane(wxStyledTextCtrl* pane, const wxString& text);  // read-only-safe set
    // Assemble EVERY pending edit (columns/index/FK/trigger/comment/options) into one
    // TableEdit — the single source for ⑥SQL 预览 and the close-time unsaved check.
    void BuildFullEdit(db::TableEdit& edit) const;

    // ---- 索引 tab (always-live grid, mirrors the field form; M3b) ----
    // One always-live index row. The popup cells live in the .cpp anonymous namespace,
    // so they are held as wxWindow* here and static_cast where used (like FieldRow.num).
    struct IndexRow {
        wxWindow*   num = nullptr;      // NumCell: select row + right-click
        wxTextCtrl* name = nullptr;     // 索引名
        wxWindow*   cols = nullptr;     // MultiPickCell*: 字段 (multi-select)
        wxWindow*   type = nullptr;     // PopupListCell*: 类型 (IndexTypes())
        wxWindow*   method = nullptr;   // PopupListCell*: 方式 (IndexMethods())
        wxTextCtrl* comment = nullptr;  // 注释
        wxString    origName;           // "" = freshly added index
        db::IndexModel baseline;        // load-time snapshot for the diff
        bool        hasBaseline = false;
    };
    void BuildIndexGrid();          // one-time 索引 tab construction (from the ctor)
    void BuildIndexHeaderRow();
    IndexRow* AddIndexRowControls();
    void PopulateIndexes(const std::vector<db::IndexInfo>& idx);
    void SetCurrentIndexRow(int row);
    void RenumberIndexRows();
    int  IndexRowIndexOf(const IndexRow* ir) const;
    wxArrayString CurrentColumnNames() const;   // live field names → index-column candidates
    db::IndexModel IndexRowToModel(int row) const;
    bool BuildIndexEdits(db::TableEdit& edit, wxString& err) const;
    void SaveIndexChanges();

    // ---- 外键 tab (always-live grid; M3c) ----
    // Introspection (db::ForeignKey) only carries from/to columns — no constraint name
    // or ON DELETE/UPDATE — so loaded FKs are view-only (can't DROP without the name);
    // the tab's live value is ADDING new FKs. refColumns candidates are fetched live
    // from the typed referenced table.
    struct FkRow {
        wxWindow*   num = nullptr;
        wxTextCtrl* name = nullptr;
        wxWindow*   cols = nullptr;      // MultiPickCell*: 本表字段
        wxTextCtrl* refDb = nullptr;     // referenced schema (text; "" = same)
        wxTextCtrl* refTable = nullptr;  // referenced table (text)
        wxWindow*   refCols = nullptr;   // MultiPickCell*: 引用字段 (live provider)
        wxWindow*   onDelete = nullptr;  // PopupListCell*: FkActions()
        wxWindow*   onUpdate = nullptr;  // PopupListCell*: FkActions()
        wxString    origName;
        db::ForeignKeyModel baseline;
        bool        hasBaseline = false; // true = loaded (view-only)
    };
    void BuildFkGrid();             // one-time 外键 tab construction (from the ctor)
    void BuildFkHeaderRow();
    FkRow* AddFkRowControls();
    void PopulateForeignKeys(const std::vector<db::ForeignKey>& fks);
    void SetCurrentFkRow(int row);
    void RenumberFkRows();
    int  FkRowIndexOf(const FkRow* fr) const;
    db::ForeignKeyModel FkRowToModel(int row) const;
    bool BuildFkEdits(db::TableEdit& edit, wxString& err) const;
    void SaveFkChanges();

    // ---- 触发器 tab (master/detail; add-only — no introspection) --------------
    // db::IConnection exposes no trigger introspection, so existing triggers are NOT
    // loaded — the tab only ADDs. The top is the same always-live grid (序号/名称/
    // 时机/事件); the bottom multiline box edits the current row's FOR EACH ROW body.
    struct TriggerRow {
        wxWindow*   num = nullptr;      // NumCell: select row + right-click
        wxTextCtrl* name = nullptr;     // 名称
        wxWindow*   timing = nullptr;   // PopupListCell*: 时机 (TriggerTimings())
        wxWindow*   event = nullptr;    // PopupListCell*: 事件 (TriggerEvents())
        wxString    body;               // FOR EACH ROW body (live in triggerBody_ when current)
        wxString    origName;           // always "" (add-only)
    };
    void BuildTriggerGrid();        // one-time 触发器 tab construction (from the ctor)
    void BuildTriggerHeaderRow();
    TriggerRow* AddTriggerRowControls();
    void SetCurrentTriggerRow(int row);
    void RenumberTriggerRows();
    int  TriggerRowIndexOf(const TriggerRow* tr) const;
    db::TriggerModel TriggerRowToModel(int row) const;
    bool BuildTriggerEdits(db::TableEdit& edit, wxString& err) const;
    void SaveTriggerChanges();
    void ResetTriggerGrid();        // clear rows + body (table switch / empty)
    void AppendTriggerModels(db::TableModel& model) const;  // BuildTableModel helper

    // ---- 选项 tab (whole-table form; TableOptionSpecs-driven) -----------------
    struct OptCtrl { wxString id; bool isChoice = false; wxWindow* ctrl = nullptr; };
    void BuildOptionsForm();        // build empty page+sizer (ctor) / rebuild+fill (Load)
    void SaveOptions();
    void AppendOptionsModel(db::TableModel& model) const;   // BuildTableModel helper
    bool CurrentOptions(db::TableOptions& out) const;       // read the options form
    bool OptionsChanged(const db::TableOptions& cur) const; // differs from loaded?

    // Per-tab operations (index/fk/trigger editors land in M3; stubs for now).
    void OnAddIndex();      void OnDeleteIndex();
    void OnAddForeignKey(); void OnDeleteForeignKey();
    void OnAddTrigger();    void OnDeleteTrigger();  void OnMoveTrigger(int delta);

    // ---- profile-driven helpers (ADR-014) ----
    const db::TypeDescriptor* FindType(const wxString& name) const;
    db::ColumnModel RowToModel(int row) const;   // form + panel state → ColumnModel
    bool BuildTableEdit(db::TableEdit& edit, wxString& err) const;
    // Columns whose position changed vs the loaded order (only when the dialect can
    // reorder). reorder[origName] = current name of the new predecessor ("" = FIRST).
    void ComputeColumnReorder(std::map<wxString, wxString>& reorder) const;

    // ---- right-hand attribute panel ----
    void BuildAttrPanel(int row);           // full teardown + recreate for the row's type
    void ReloadAttrPanel(int row);          // fast path: same control set, just reload values
    wxString AttrSignature(int row) const;  // ordered applicable-attr ids → panel structure key
    void FlushAttrPanel();                  // write current controls → rows_[attrRow_]
    const db::DbCreateOption* FindCreateOption(const wxString& id) const;
    void RefillCollationCombo(const wxString& charset);

    // ---- state ----
    db::IConnection*             conn_ = nullptr;
    db::DbType                   dbType_ = db::DbType::MySQL;
    const db::DialectProfile*    profile_ = nullptr;
    wxString                     db_;
    wxString                     table_;
    std::vector<db::ColumnInfo>  origCols_;
    std::vector<int>             colW_;          // per-column width (drag-resizable header)
    std::vector<wxWindow*>       headerCells_;   // header cells, by column (for resize)
    std::vector<std::unique_ptr<FieldRow>> rows_;
    std::vector<FieldRow*>       pkOrder_;        // composite primary key, in click order
    std::set<int>                selRows_;        // 序号-selected rows (multi-select)
    int                          selAnchor_ = -1; // Shift-range anchor
    std::vector<RowClip>         clip_;           // copy/paste buffer
    wxArrayString                typeChoices_;   // profile_->Types() names (AutoComplete)
    db::DbCreateCaps             createCaps_;     // charset/collation candidates

    // One live control in the attribute panel, kept so FlushAttrPanel can read it.
    struct PanelCtrl {
        wxString                     id;
        db::AttrDescriptor::Editor   editor = db::AttrDescriptor::Text;
        wxControl*                   ctrl = nullptr;
    };
    std::vector<PanelCtrl> panelCtrls_;
    int      attrRow_ = -1;    // current row (form highlight + panel binding); -1 = none
    wxString panelSig_;        // AttrSignature of the built panel; == ⇒ reload not rebuild
    bool     populating_ = false;  // suppress focus/type reactions during Populate

    // ---- widgets ----
    wxStaticText* title_ = nullptr;
    wxStaticText* subtitle_ = nullptr;
    wxPanel*      toolbar_ = nullptr;        // context toolbar host (rebuilt per tab)
    wxSizer*      toolbarSizer_ = nullptr;   // horizontal button strip inside toolbar_
    std::function<void()> onCloseRequest_;   // host-provided: remove this tab

    // The 8 design tabs, in wxSimplebook page order. 触发器 is hidden per-dialect.
    enum TabId {
        Tab_Fields = 0, Tab_Indexes, Tab_ForeignKeys, Tab_Triggers,
        Tab_Options, Tab_Comment, Tab_SqlPreview, Tab_TableDdl, Tab_Count
    };
    wxWindow*     tabs_[Tab_Count] = {};   // painted TabCell per tab (see .cpp)
    wxSimplebook* book_ = nullptr;

    wxScrolledWindow* formScroll_ = nullptr;   // the always-live field form host
    wxFlexGridSizer*  formSizer_ = nullptr;
    wxScrolledWindow* attrScroll_ = nullptr;   // right-hand attribute panel host
    wxSizer*          attrSizer_ = nullptr;
    wxStaticText*     attrHint_ = nullptr;
    wxStaticText*     fieldLbl_ = nullptr;      // panel's "name · type" header (reused)
    wxTextCtrl*       defaultCtrl_ = nullptr;   // panel's 默认值 field (current row)
    wxComboBox*       charsetCombo_ = nullptr;  // panel's 字符集 combo (current row)
    wxComboBox*       collationCombo_ = nullptr;// panel's 排序规则 combo (current row)
    wxScrolledWindow* indexScroll_ = nullptr;   // 索引 tab: always-live grid host
    wxFlexGridSizer*  indexSizer_ = nullptr;
    std::vector<int>       colWIdx_;             // per-column widths (index grid)
    std::vector<wxWindow*> idxHeaderCells_;      // index grid header cells (for resize)
    std::vector<std::unique_ptr<IndexRow>> idxRows_;
    std::vector<wxString> origIndexNames_;        // index names at load (to detect drops)
    int               curIdxRow_ = -1;            // 索引 current row (highlight + delete target)
    wxScrolledWindow* fkScroll_ = nullptr;        // 外键 tab: always-live grid host
    wxFlexGridSizer*  fkSizer_ = nullptr;
    std::vector<int>       colWFk_;
    std::vector<wxWindow*> fkHeaderCells_;
    std::vector<std::unique_ptr<FkRow>> fkRows_;
    int               curFkRow_ = -1;
    wxPanel*          triggersPage_ = nullptr;   // 触发器 tab host (master/detail)
    wxScrolledWindow* triggerScroll_ = nullptr;  // 触发器 list host (always-live grid)
    wxFlexGridSizer*  triggerSizer_ = nullptr;
    std::vector<int>       colWTrig_;
    std::vector<wxWindow*> trigHeaderCells_;
    std::vector<std::unique_ptr<TriggerRow>> triggerRows_;
    int               curTrigRow_ = -1;          // 触发器 current row (body binding)
    wxTextCtrl*       triggerBody_ = nullptr;    // 触发器 detail: current row's body
    wxPanel*          optionsPage_ = nullptr;    // 选项 tab host (whole-table form)
    wxSizer*          optionsSizer_ = nullptr;
    std::vector<OptCtrl> optionCtrls_;           // one control per TableOptionSpecs() item
    wxPanel*          commentPage_ = nullptr;    // 注释 tab host
    wxTextCtrl*       commentEdit_ = nullptr;    // 注释 tab: table-comment textarea
    wxString          origComment_;              // loaded table comment (dirty baseline)
    db::TableOptions  origOptions_;              // loaded engine/charset/collation (baseline)
    wxStyledTextCtrl* sqlPreview_ = nullptr;     // ⑥SQL 预览: incremental ALTER (coloured)
    wxStyledTextCtrl* ddl_ = nullptr;            // ⑦TABLE DDL: full CREATE (coloured)
};

} // namespace ui
