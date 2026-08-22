#include "ui/PreferencesDialog.h"

#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

#include <memory>

#include "ai/AiClient.h"
#include "ai/AiConfigStore.h"
#include "core/Lang.h"
#include "core/Settings.h"
#include "db/OciConfig.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace ui {

PreferencesDialog::~PreferencesDialog() = default;

PreferencesDialog::PreferencesDialog(wxWindow* parent)
    : CenteredDialog(parent, wxID_ANY, tr(L"偏好设置"),
                     wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(theme::kWhite);
    ociOriginal_ = db::OciLibraryPath();

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* grid = new wxFlexGridSizer(2, 10, 12);
    grid->AddGrowableCol(1, 1);

    // ---- language ----------------------------------------------------------
    auto* langLabel = new wxStaticText(this, wxID_ANY, tr(L"语言"));
    langLabel->SetForegroundColour(theme::kTextBody);

    // Populate from the registered language packs → new packs auto-appear.
    language_ = new wxChoice(this, wxID_ANY);
    const wxString cur = core::Lang::Current();
    int sel = 0;
    for (const core::LangInfo& li : core::Lang::Available()) {
        const int idx = language_->Append(li.name, new wxStringClientData(li.code));
        if (li.code == cur) sel = idx;
    }
    language_->SetSelection(sel);

    grid->Add(langLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(language_, 1, wxEXPAND);

    // ---- Oracle client (OCI dynamic library) -------------------------------
    auto* ociLabel = new wxStaticText(this, wxID_ANY, tr(L"Oracle 客户端 (OCI 动态库)"));
    ociLabel->SetForegroundColour(theme::kTextBody);

    ociPath_ = new wxTextCtrl(this, wxID_ANY, db::OciLibraryPath(),
                              wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    ociPath_->SetHint(tr(L"自动搜索 (程序目录 / PATH)"));

    auto* browse = new wxButton(this, wxID_ANY, tr(L"浏览…"),
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    browse->Bind(wxEVT_BUTTON, &PreferencesDialog::OnBrowseOci, this);
    auto* clear = new wxButton(this, wxID_ANY, tr(L"清除"),
                               wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    clear->Bind(wxEVT_BUTTON, &PreferencesDialog::OnClearOci, this);

    auto* ociRow = new wxBoxSizer(wxHORIZONTAL);
    ociRow->Add(ociPath_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    ociRow->Add(browse, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    ociRow->Add(clear, 0, wxALIGN_CENTER_VERTICAL);

    grid->Add(ociLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(ociRow, 1, wxEXPAND);

    // status line sits under the OCI row (spans the value column)
    ociStatus_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    grid->AddSpacer(0);
    grid->Add(ociStatus_, 1, wxEXPAND);

    // ---- AI service (provider list + credentials + model discovery) --------
    BuildAiSection(grid);

    root->Add(grid, 1, wxEXPAND | wxALL, 18);

    auto* hint = new wxStaticText(this, wxID_ANY, tr(L"切换语言后将自动重启应用。"));
    hint->SetForegroundColour(theme::kTextFaint);
    root->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 18);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer();
    auto* cancel = new wxButton(this, wxID_CANCEL, tr(L"取消"));
    cancel->Bind(wxEVT_BUTTON, &PreferencesDialog::OnCancel, this);
    btns->Add(cancel, 0, wxRIGHT, 8);
    auto* ok = new wxButton(this, wxID_OK, tr(L"确定"));
    ok->SetBackgroundColour(theme::kPrimary);
    ok->SetForegroundColour(theme::kWhite);
    ok->Bind(wxEVT_BUTTON, &PreferencesDialog::OnOk, this);
    btns->Add(ok, 0);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 18);

    SetSizerAndFit(root);
    SetMinSize(wxSize(520, -1));
    Fit();

    // Reflect the current (persisted) path's availability without forcing a
    // probe on every open: only probe when a path is actually configured.
    if (db::OciLibraryPath().IsEmpty()) {
        ociStatus_->SetLabel(tr(L"未配置 — 连接 Oracle 时将自动搜索"));
        ociStatus_->SetForegroundColour(theme::kTextFaint);
    } else {
        RefreshOciStatus(db::OciLibraryPath());
    }
    CentreOnParent();
}

void PreferencesDialog::RefreshOciStatus(const wxString& path)
{
    // OciAvailable probes via the *currently configured* path, so apply first.
    db::SetOciLibraryPath(path);
    ociPath_->SetValue(path);

    wxString resolved;
    if (db::OciAvailable(resolved)) {
        ociStatus_->SetLabel(tr(L"✓ 已就绪: ") + resolved);
        ociStatus_->SetForegroundColour(theme::kGreen);
    } else {
        ociStatus_->SetLabel(tr(L"未找到 (连接 Oracle 时需要)"));
        ociStatus_->SetForegroundColour(theme::kDotRed);
    }
    Layout();
}

void PreferencesDialog::OnBrowseOci(wxCommandEvent&)
{
    wxFileDialog dlg(this, tr(L"选择 oci.dll"), wxEmptyString, L"oci.dll",
                     L"oci.dll|oci.dll|DLL (*.dll)|*.dll",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    RefreshOciStatus(dlg.GetPath());
}

void PreferencesDialog::OnClearOci(wxCommandEvent&)
{
    db::SetOciLibraryPath(wxEmptyString);
    ociPath_->SetValue(wxEmptyString);
    ociStatus_->SetLabel(tr(L"未配置 — 连接 Oracle 时将自动搜索"));
    ociStatus_->SetForegroundColour(theme::kTextFaint);
    Layout();
}

void PreferencesDialog::OnCancel(wxCommandEvent& ev)
{
    // Browsing/clearing mutated the live loader for the status probe; undo that
    // if the user backs out so nothing is applied without an explicit OK.
    if (db::OciLibraryPath() != ociOriginal_)
        db::SetOciLibraryPath(ociOriginal_);
    ev.Skip();   // let wxID_CANCEL close the dialog
}

void PreferencesDialog::OnOk(wxCommandEvent& ev)
{
    // Language: persist. Most labels are set once at construction, so a change only
    // fully applies on a fresh launch → auto-restart (below).
    bool langChanged = false;
    const int i = language_->GetSelection();
    if (i != wxNOT_FOUND) {
        auto* data = static_cast<wxStringClientData*>(language_->GetClientObject(i));
        const wxString code = data ? data->GetData() : wxString(L"zh");
        if (code != core::Lang::Current()) {
            core::Lang::Set(code);
            langChanged = true;
        }
    }

    // OCI path: apply to the loader now (takes effect for the next connection)
    // and persist to the shared settings.ini for the next launch.
    const wxString ociPath = ociPath_->GetValue();
    db::SetOciLibraryPath(ociPath);
    core::Settings::WriteString(core::keys::kOciLibraryPath, ociPath);

    // AI service: fold the live controls back into the selected provider, then record
    // THAT provider as the one in use — selecting it in the dropdown is the whole
    // gesture, there is no separate "set as default" step to forget.
    // AiConfigStore encrypts every apiKey (DPAPI) on write.
    CommitControlsIntoProvider();
    aiSettings_.includeSchema = aiIncludeSchema_->GetValue();
    if (aiCurrent_ >= 0 && aiCurrent_ < static_cast<int>(aiSettings_.providers.size()))
        aiSettings_.defaultProviderId = aiSettings_.providers[aiCurrent_].id;
    else if (aiSettings_.defaultProviderId.IsEmpty() && !aiSettings_.providers.empty())
        aiSettings_.defaultProviderId = aiSettings_.providers.front().id;
    ai::AiConfigStore::Save(aiSettings_);

    if (langChanged) {
        // Auto-restart so the whole UI rebuilds in the new language. Deferred to after
        // this dialog closes: relaunch a fresh instance, then close the current top
        // window — its close guards still run, so unsaved work is never lost silently.
        const wxString exe = wxStandardPaths::Get().GetExecutablePath();
        wxTheApp->CallAfter([exe] {
            wxExecute(exe, wxEXEC_ASYNC);
            if (wxWindow* top = wxTheApp->GetTopWindow()) top->Close();
        });
    }

    ev.Skip();   // let wxID_OK close the dialog
}

// ── AI service section ───────────────────────────────────────────────────────────────

void PreferencesDialog::BuildAiSection(wxFlexGridSizer* grid)
{
    // Load persisted settings; seed with the five built-in presets on first run so the
    // dropdown is never empty (Anthropic / OpenAI / DeepSeek / 智谱GLM / Ollama).
    aiSettings_ = ai::AiConfigStore::Load();
    if (aiSettings_.providers.empty())
        aiSettings_.providers = ai::BuiltinProviderPresets();

    // ---- section heading (own row; value column left empty) ----
    grid->AddSpacer(6);
    grid->AddSpacer(6);
    auto* heading = new wxStaticText(this, wxID_ANY, tr(L"AI 服务"));
    heading->SetForegroundColour(theme::kTextStrong);
    wxFont hf = heading->GetFont();
    hf.MakeBold();
    heading->SetFont(hf);
    grid->Add(heading, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);

    // ---- provider dropdown + [+ 新增] ----
    // There is no "set as default" affordance: whatever is selected here when OK is
    // pressed IS the provider the AI features use (see OnOk).
    auto* provLabel = new wxStaticText(this, wxID_ANY, tr(L"供应商"));
    provLabel->SetForegroundColour(theme::kTextBody);

    aiProvider_ = new wxChoice(this, wxID_ANY);
    aiProvider_->SetToolTip(tr(L"当前选中的供应商即为 AI 功能使用的服务"));
    aiProvider_->Bind(wxEVT_CHOICE, &PreferencesDialog::OnAiProviderChanged, this);

    auto* addBtn = new wxButton(this, wxID_ANY, tr(L"+ 新增"),
                                wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    addBtn->Bind(wxEVT_BUTTON, &PreferencesDialog::OnAiAddProvider, this);

    auto* provRow = new wxBoxSizer(wxHORIZONTAL);
    provRow->Add(aiProvider_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    provRow->Add(addBtn, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(provLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(provRow, 1, wxEXPAND);

    // ---- interface base URL ----
    auto* urlLabel = new wxStaticText(this, wxID_ANY, tr(L"接口地址"));
    urlLabel->SetForegroundColour(theme::kTextBody);
    aiBaseUrl_ = new wxTextCtrl(this, wxID_ANY);
    aiBaseUrl_->SetHint(tr(L"留空则用该类型的默认地址"));
    aiBaseUrl_->Bind(wxEVT_TEXT, &PreferencesDialog::OnAiFieldEdited, this);
    grid->Add(urlLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(aiBaseUrl_, 1, wxEXPAND);

    // ---- api key (masked) ----
    auto* keyLabel = new wxStaticText(this, wxID_ANY, tr(L"密钥"));
    keyLabel->SetForegroundColour(theme::kTextBody);
    aiApiKey_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    aiApiKey_->SetHint(tr(L"保存时加密存储"));
    aiApiKey_->Bind(wxEVT_TEXT, &PreferencesDialog::OnAiFieldEdited, this);
    grid->Add(keyLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(aiApiKey_, 1, wxEXPAND);

    // ---- model combo (editable) + [获取模型列表] ----
    auto* modelLabel = new wxStaticText(this, wxID_ANY, tr(L"模型"));
    modelLabel->SetForegroundColour(theme::kTextBody);
    aiModel_ = new wxComboBox(this, wxID_ANY);
    aiModel_->Bind(wxEVT_TEXT, &PreferencesDialog::OnAiFieldEdited, this);
    aiModel_->Bind(wxEVT_COMBOBOX, &PreferencesDialog::OnAiFieldEdited, this);

    aiFetchModels_ = new wxButton(this, wxID_ANY, tr(L"获取模型列表"),
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    aiFetchModels_->Bind(wxEVT_BUTTON, &PreferencesDialog::OnAiFetchModels, this);

    auto* modelRow = new wxBoxSizer(wxHORIZONTAL);
    modelRow->Add(aiModel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    modelRow->Add(aiFetchModels_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(modelLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(modelRow, 1, wxEXPAND);

    // ---- privacy gate ----
    aiIncludeSchema_ = new wxCheckBox(this, wxID_ANY,
                                      tr(L"允许发送数据库结构给 AI（表名 / 列名）"));
    aiIncludeSchema_->SetForegroundColour(theme::kTextBody);
    aiIncludeSchema_->SetValue(aiSettings_.includeSchema);
    grid->AddSpacer(0);
    grid->Add(aiIncludeSchema_, 1, wxEXPAND);

    // Select the default provider (or the first) and mirror it into the controls.
    int sel = 0;
    for (size_t i = 0; i < aiSettings_.providers.size(); ++i)
        if (aiSettings_.providers[i].id == aiSettings_.defaultProviderId) {
            sel = static_cast<int>(i);
            break;
        }
    RebuildProviderChoice(sel);
    LoadProviderIntoControls(sel);
}

void PreferencesDialog::RebuildProviderChoice(int select)
{
    aiProvider_->Clear();
    for (const auto& p : aiSettings_.providers)
        aiProvider_->Append(p.name.IsEmpty() ? p.id : p.name);
    if (select >= 0 && select < static_cast<int>(aiSettings_.providers.size()))
        aiProvider_->SetSelection(select);
}

void PreferencesDialog::LoadProviderIntoControls(int index)
{
    aiCurrent_ = index;
    if (index < 0 || index >= static_cast<int>(aiSettings_.providers.size()))
        return;
    const ai::AiProviderConfig& p = aiSettings_.providers[index];

    // ChangeValue()/Clear() under the guard so the resulting EVT_TEXT don't write back.
    aiLoading_ = true;
    aiBaseUrl_->ChangeValue(p.baseUrl);
    aiApiKey_->ChangeValue(p.apiKey);
    aiModel_->Clear();               // drop any previously fetched dropdown list
    aiModel_->ChangeValue(p.model);  // editable text = this provider's model
    aiLoading_ = false;
}

void PreferencesDialog::CommitControlsIntoProvider()
{
    if (aiCurrent_ < 0 || aiCurrent_ >= static_cast<int>(aiSettings_.providers.size()))
        return;
    ai::AiProviderConfig& p = aiSettings_.providers[aiCurrent_];
    p.baseUrl = aiBaseUrl_->GetValue();
    p.apiKey  = aiApiKey_->GetValue();
    p.model   = aiModel_->GetValue();
}

wxString PreferencesDialog::MakeUniqueProviderId(const wxString& base) const
{
    for (int n = 1; ; ++n) {
        const wxString candidate = base + wxString::Format(L"-%d", n);
        bool taken = false;
        for (const auto& p : aiSettings_.providers)
            if (p.id == candidate) { taken = true; break; }
        if (!taken)
            return candidate;
    }
}

void PreferencesDialog::OnAiProviderChanged(wxCommandEvent&)
{
    // Persist edits to the outgoing provider (aiCurrent_ still points at it), then load
    // the newly selected one into the controls.
    CommitControlsIntoProvider();
    LoadProviderIntoControls(aiProvider_->GetSelection());
}

void PreferencesDialog::OnAiFieldEdited(wxCommandEvent& ev)
{
    if (!aiLoading_)
        CommitControlsIntoProvider();
    ev.Skip();
}

void PreferencesDialog::OnAiAddProvider(wxCommandEvent&)
{
    const wxString name = GetTextCentered(this, tr(L"新供应商名称"),
                                          tr(L"新增 AI 供应商"), tr(L"自定义"));
    if (name.IsEmpty())
        return;
    CommitControlsIntoProvider();

    // Simple version: new providers are OpenAI-compatible (the workhorse kind); the user
    // then fills in base URL + key. id is auto-generated and stable (= ini section name).
    ai::AiProviderConfig p;
    p.id      = MakeUniqueProviderId(L"custom");
    p.name    = name;
    p.kind    = ai::ProviderKind::OpenAiCompat;
    p.baseUrl = ai::DefaultBaseUrl(ai::ProviderKind::OpenAiCompat);
    aiSettings_.providers.push_back(std::move(p));

    const int idx = static_cast<int>(aiSettings_.providers.size()) - 1;
    RebuildProviderChoice(idx);
    LoadProviderIntoControls(idx);
}

void PreferencesDialog::OnAiFetchModels(wxCommandEvent&)
{
    if (aiCurrent_ < 0 || aiCurrent_ >= static_cast<int>(aiSettings_.providers.size()))
        return;
    CommitControlsIntoProvider();

    // Build a throwaway config from the live controls; substitute the kind's default base
    // URL when the field was left blank so the GET has a valid host to hit.
    ai::AiProviderConfig cfg = aiSettings_.providers[aiCurrent_];
    if (cfg.baseUrl.IsEmpty())
        cfg.baseUrl = ai::DefaultBaseUrl(cfg.kind);

    aiFetchModels_->Enable(false);
    aiFetchModels_->SetLabel(tr(L"获取中…"));

    // The AiClient must outlive this handler — a temporary would cancel the in-flight GET
    // the instant we return. The sink fires on the GUI thread (AiHttp guarantees it); if
    // the dialog is destroyed mid-flight the client is torn down and the request cancelled
    // silently, so `this` is never touched after death.
    aiModelsClient_ = std::make_unique<ai::AiClient>(cfg);
    aiModelsClient_->ListModels(
        [this](std::vector<wxString> models, ai::AiError err) {
            aiFetchModels_->SetLabel(tr(L"获取模型列表"));
            aiFetchModels_->Enable(true);

            if (!err.ok()) {
                wxMessageBox(err.message.IsEmpty()
                                 ? tr(L"获取模型列表失败")
                                 : tr(L"获取模型列表失败：") + err.message,
                             tr(L"AI 服务"), wxOK | wxICON_WARNING, this);
                return;
            }
            if (models.empty()) {
                wxMessageBox(tr(L"该供应商未返回任何模型"),
                             tr(L"AI 服务"), wxOK | wxICON_INFORMATION, this);
                return;
            }

            const wxString keep = aiModel_->GetValue();   // preserve the user's text
            aiLoading_ = true;
            aiModel_->Clear();
            for (const wxString& m : models)
                aiModel_->Append(m);
            aiModel_->ChangeValue(keep);
            aiLoading_ = false;
        });
}

} // namespace ui
