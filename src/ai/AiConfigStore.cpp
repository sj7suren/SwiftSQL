// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ai/AiConfigStore.h"

#include <wx/fileconf.h>
#include <memory>

#include "core/Secret.h"
#include "core/Settings.h"

namespace ai {
namespace {

// ── Wire encoding for ProviderKind ──────────────────────────────────────────
// Persist the enum as a stable string, never the numeric value: that keeps the
// ini human-readable and immune to future reordering of the enum.
const wchar_t* KindToStr(ProviderKind k)
{
    switch (k) {
    case ProviderKind::Anthropic:    return L"anthropic";
    case ProviderKind::Gemini:       return L"gemini";
    case ProviderKind::OllamaNative: return L"ollama";
    case ProviderKind::OpenAiCompat:
    default:                         return L"openai_compat";
    }
}

ProviderKind StrToKind(const wxString& s)
{
    if (s == L"anthropic") return ProviderKind::Anthropic;
    if (s == L"gemini")    return ProviderKind::Gemini;
    if (s == L"ollama")    return ProviderKind::OllamaNative;
    return ProviderKind::OpenAiCompat;
}

// The whole AI block lives under this ini subtree, alongside language/general
// settings in the very same settings.ini (core::Settings::FilePath()).
constexpr const wchar_t* kProvidersRoot = L"/ai/providers";
constexpr const wchar_t* kDefaultIdKey  = L"/ai/defaultProviderId";
constexpr const wchar_t* kIncludeKey    = L"/ai/includeSchema";

// Open the shared settings.ini exactly the way core::Settings does, so both use
// one backing file rather than scattering little stores (matches ConnectionStore
// pattern, but pointed at Settings::FilePath() per the AI layer's contract).
std::unique_ptr<wxFileConfig> Open()
{
    return std::make_unique<wxFileConfig>(
        wxEmptyString, wxEmptyString, core::Settings::FilePath(), wxEmptyString,
        wxCONFIG_USE_LOCAL_FILE);
}

} // namespace

// ── Free functions declared in AiConfig.h ───────────────────────────────────

wxString DefaultBaseUrl(ProviderKind kind)
{
    switch (kind) {
    case ProviderKind::Anthropic:    return L"https://api.anthropic.com/v1";
    case ProviderKind::Gemini:       return L"https://generativelanguage.googleapis.com/v1beta";
    case ProviderKind::OllamaNative: return L"http://localhost:11434";
    case ProviderKind::OpenAiCompat:
    default:                         return L"https://api.openai.com/v1";
    }
}

const wchar_t* KindDisplayName(ProviderKind kind)
{
    switch (kind) {
    case ProviderKind::Anthropic:    return L"Anthropic";
    case ProviderKind::Gemini:       return L"Gemini";
    case ProviderKind::OllamaNative: return L"Ollama";
    case ProviderKind::OpenAiCompat:
    default:                         return L"OpenAI 兼容";
    }
}

std::vector<AiProviderConfig> BuiltinProviderPresets()
{
    std::vector<AiProviderConfig> out;

    auto add = [&out](const wchar_t* id, const wchar_t* name, ProviderKind kind,
                      const wxString& baseUrl, const wchar_t* model) {
        AiProviderConfig p;
        p.id      = id;
        p.name    = name;
        p.kind    = kind;
        p.baseUrl = baseUrl;              // may equal DefaultBaseUrl(kind); explicit is fine
        p.model   = model;
        // apiKey intentionally left empty — the user fills it in per provider.
        out.push_back(std::move(p));
    };

    add(L"anthropic", L"Anthropic", ProviderKind::Anthropic,
        DefaultBaseUrl(ProviderKind::Anthropic),   L"claude-sonnet-5");
    add(L"openai",    L"OpenAI",    ProviderKind::OpenAiCompat,
        DefaultBaseUrl(ProviderKind::OpenAiCompat), L"gpt-4o");
    add(L"deepseek",  L"DeepSeek",  ProviderKind::OpenAiCompat,
        L"https://api.deepseek.com/v1",             L"deepseek-chat");
    add(L"glm",       L"智谱GLM",   ProviderKind::OpenAiCompat,
        L"https://open.bigmodel.cn/api/paas/v4",    L"glm-4-plus");
    add(L"ollama",    L"Ollama",    ProviderKind::OllamaNative,
        DefaultBaseUrl(ProviderKind::OllamaNative), L"llama3.1");

    return out;
}

// ── AiConfigStore ───────────────────────────────────────────────────────────

AiSettings AiConfigStore::Load()
{
    AiSettings s;
    auto cfg = Open();

    // Enumerate each provider section under /ai/providers; the group name is the id.
    cfg->SetPath(kProvidersRoot);
    wxString group;
    long     idx = 0;
    bool     more = cfg->GetFirstGroup(group, idx);
    while (more) {
        cfg->SetPath(wxString(kProvidersRoot) + L"/" + group);

        AiProviderConfig p;
        p.id      = group;
        p.name    = cfg->Read(L"name", group);
        p.kind    = StrToKind(cfg->Read(L"kind", L"openai_compat"));
        p.baseUrl = cfg->Read(L"baseUrl", wxEmptyString);
        p.apiKey  = core::DecryptSecret(cfg->Read(L"apiKey", wxEmptyString));
        p.model   = cfg->Read(L"model", wxEmptyString);

        long timeout = 60; cfg->Read(L"timeout", &timeout, 60);
        p.timeoutSec = static_cast<int>(timeout);
        p.streaming  = cfg->ReadBool(L"streaming", true);
        double temp = 0.7; cfg->Read(L"temperature", &temp, 0.7);
        p.temperature = temp;
        long maxTok = 2048; cfg->Read(L"maxTokens", &maxTok, 2048);
        p.maxTokens = static_cast<int>(maxTok);

        s.providers.push_back(std::move(p));
        cfg->SetPath(kProvidersRoot);
        more = cfg->GetNextGroup(group, idx);
    }

    cfg->SetPath(L"/");
    s.defaultProviderId = cfg->Read(kDefaultIdKey, wxEmptyString);
    s.includeSchema     = cfg->ReadBool(kIncludeKey, false);
    return s;
}

void AiConfigStore::Save(const AiSettings& settings)
{
    auto cfg = Open();

    // Replace the whole provider subtree so deleted providers don't linger as
    // stale sections. Then rewrite every provider from scratch.
    cfg->DeleteGroup(kProvidersRoot);

    for (const auto& p : settings.providers) {
        if (p.id.IsEmpty()) continue;  // id is the section name — skip malformed rows
        cfg->SetPath(wxString(kProvidersRoot) + L"/" + p.id);
        cfg->Write(L"name",        p.name);
        cfg->Write(L"kind",        wxString(KindToStr(p.kind)));
        cfg->Write(L"baseUrl",     p.baseUrl);
        // apiKey NEVER stored in plaintext — always through DPAPI-backed EncryptSecret.
        cfg->Write(L"apiKey",      core::EncryptSecret(p.apiKey));
        cfg->Write(L"model",       p.model);
        cfg->Write(L"timeout",     static_cast<long>(p.timeoutSec));
        cfg->Write(L"streaming",   p.streaming);
        cfg->Write(L"temperature", p.temperature);
        cfg->Write(L"maxTokens",   static_cast<long>(p.maxTokens));
    }

    cfg->SetPath(L"/");
    cfg->Write(kDefaultIdKey, settings.defaultProviderId);
    cfg->Write(kIncludeKey,   settings.includeSchema);
    cfg->Flush();
}

} // namespace ai
