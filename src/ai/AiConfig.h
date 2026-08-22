// AiConfig.h — AI provider configuration model, persisted to settings.ini and with
// the apiKey encrypted at rest via core::Secret (same DPAPI mechanism as connection
// passwords). Mirrors core::ConnectionProfile's "plain struct + separate Store" style.
//
// Multiple providers coexist; one is the default. A provider whose baseUrl is empty
// falls back to the built-in default for its kind (see ai::DefaultBaseUrl).
#pragma once

#include <wx/string.h>

#include <vector>

namespace ai {

// Which wire protocol a provider speaks. OpenAiCompat is the workhorse: it covers
// OpenAI + every OpenAI-compatible endpoint (DeepSeek, 智谱GLM, Kimi, 通义千问,
// OpenRouter, Groq, xAI, Mistral, and local LM Studio / vLLM / Ollama's /v1 shim).
enum class ProviderKind { OpenAiCompat = 0, Anthropic, Gemini, OllamaNative };

struct AiProviderConfig {
    wxString     id;                                   // stable key = ini section name
    wxString     name;                                 // display label ("我的 DeepSeek")
    ProviderKind kind = ProviderKind::OpenAiCompat;
    wxString     baseUrl;                              // "" → DefaultBaseUrl(kind)
    wxString     apiKey;                               // plaintext in memory; encrypted on disk
    wxString     model;                                // default model id for this provider
    int          timeoutSec  = 60;
    bool         streaming   = true;
    double       temperature = 0.7;
    int          maxTokens   = 2048;
};

// A provider is "usable" (can actually fetch models / answer) when it carries the
// credentials its protocol needs: a non-empty apiKey for the key-based kinds, or
// nothing for local Ollama. Used to skip unconfigured presets when auto-selecting.
inline bool CanFetchModels(const AiProviderConfig& p)
{
    if (p.kind == ProviderKind::OllamaNative) return true;   // local, no key needed
    return !p.apiKey.IsEmpty();
}

// Identity of a provider AS A WIRE CLIENT: every field ai::AiClient bakes in when
// it is constructed. Callers that cache an AiClient compare this key against the
// one they built with to notice that Preferences changed underneath them and the
// cached client now points at the wrong endpoint / carries the wrong key.
inline wxString ProviderKey(const AiProviderConfig& p)
{
    return wxString::Format(L"%d\t%s\t%s\t%s", static_cast<int>(p.kind),
                            p.baseUrl, p.model, p.apiKey);
}

struct AiSettings {
    std::vector<AiProviderConfig> providers;
    // The provider in use. Preferences writes whichever provider is SELECTED when
    // the user presses OK — there is no separate "set as default" step, so being
    // selected IS being in use.
    wxString                      defaultProviderId;
    // Privacy gate: when false, schema (table/column names) is NEVER sent to the
    // model — NL→SQL still works on NL + dialect alone. Default OFF (opt-in).
    bool                          includeSchema = false;

    // Effective provider for AI features.
    //   1. The recorded selection wins OUTRIGHT, configured or not. Silently
    //      falling through to some other provider that happens to carry a key
    //      would send the user's prompts somewhere they did not pick; callers
    //      instead check CanFetchModels() and surface "please configure AI".
    //   2. No selection recorded (settings.ini written before the selection was
    //      persisted) → the first provider that carries credentials.
    //   3. Otherwise the first provider, then nullptr.
    const AiProviderConfig* DefaultProvider() const
    {
        if (!defaultProviderId.IsEmpty())
            for (const auto& p : providers)
                if (p.id == defaultProviderId) return &p;
        for (const auto& p : providers)
            if (CanFetchModels(p)) return &p;
        return providers.empty() ? nullptr : &providers.front();
    }
};

// Built-in base URL / display name / default model for each kind — used to seed the
// "add provider" presets. These are conveniences, NOT a whitelist: the user can
// always point OpenAiCompat at any base URL. Defined in AiConfigStore.cpp.
wxString     DefaultBaseUrl(ProviderKind kind);
const wchar_t* KindDisplayName(ProviderKind kind);

// The five presets the product explicitly requires: Anthropic / OpenAI / DeepSeek /
// 智谱GLM / Ollama. Returns ready-to-edit AiProviderConfig rows (empty apiKey).
std::vector<AiProviderConfig> BuiltinProviderPresets();

} // namespace ai
