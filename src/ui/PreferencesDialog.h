// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// PreferencesDialog.h — 偏好设置 / Preferences.
//   • Language (中文/English)
//   • Oracle client (OCI dynamic library) path — runtime-loaded oci.dll.
//   • AI service — provider list, base URL / api key / model, model discovery and
//     the "send schema to AI" privacy gate. The provider SELECTED in the dropdown
//     when OK is pressed is the one the AI features use; there is no separate
//     "set as default" step.
#pragma once

#include <wx/dialog.h>
#include <wx/string.h>

#include <memory>

#include "ai/AiConfig.h"       // ai::AiSettings — held by value as the in-memory model
#include "ui/CenteredDialog.h"

class wxChoice;
class wxTextCtrl;
class wxStaticText;
class wxComboBox;
class wxCheckBox;
class wxButton;
class wxFlexGridSizer;

namespace ai { class AiClient; }

namespace ui {

class PreferencesDialog : public CenteredDialog {
public:
    explicit PreferencesDialog(wxWindow* parent);
    ~PreferencesDialog() override;   // out-of-line: unique_ptr<ai::AiClient> is incomplete here

private:
    void OnOk(wxCommandEvent&);
    void OnCancel(wxCommandEvent&);
    void OnBrowseOci(wxCommandEvent&);
    void OnClearOci(wxCommandEvent&);

    // Apply `path` to the loader and repaint the status line by probing.
    void RefreshOciStatus(const wxString& path);

    // ---- AI service section -------------------------------------------------
    void BuildAiSection(wxFlexGridSizer* grid);       // build controls + seed from store
    void RebuildProviderChoice(int select);           // repopulate dropdown
    void LoadProviderIntoControls(int index);         // provider → right-hand controls
    void CommitControlsIntoProvider();                // controls → current provider (in-memory)
    wxString MakeUniqueProviderId(const wxString& base) const;

    void OnAiProviderChanged(wxCommandEvent&);
    void OnAiFieldEdited(wxCommandEvent&);
    void OnAiFetchModels(wxCommandEvent&);
    void OnAiAddProvider(wxCommandEvent&);

    wxChoice*     language_   = nullptr;
    wxTextCtrl*   ociPath_    = nullptr;
    wxStaticText* ociStatus_  = nullptr;
    wxString      ociOriginal_;   // restore on cancel (probing mutates loader)

    // AI service — in-memory model + controls
    ai::AiSettings aiSettings_;
    int            aiCurrent_ = -1;      // index into aiSettings_.providers, or -1
    bool           aiLoading_ = false;   // guard: suppress write-back while loading controls

    wxChoice*   aiProvider_      = nullptr;
    wxTextCtrl* aiBaseUrl_       = nullptr;
    wxTextCtrl* aiApiKey_        = nullptr;
    wxComboBox* aiModel_         = nullptr;
    wxButton*   aiFetchModels_   = nullptr;
    wxCheckBox* aiIncludeSchema_ = nullptr;

    std::unique_ptr<ai::AiClient> aiModelsClient_;   // keeps the async ListModels alive
};

} // namespace ui
