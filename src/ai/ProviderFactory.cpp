// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ProviderFactory.cpp — the one place that maps a ProviderKind to a concrete adapter.
// Declared in IAiProvider.h; keeps AiClient free of any #include of a specific provider.
//
// Gemini has no native adapter this phase: it falls back to OpenAiCompat, which works
// against Gemini's OpenAI-compatible endpoint (a native GeminiProvider lands later).
#include "ai/IAiProvider.h"

#include "ai/OpenAiCompatProvider.h"
#include "ai/AnthropicProvider.h"
#include "ai/OllamaProvider.h"

namespace ai {

std::unique_ptr<IAiProvider> CreateProvider(ProviderKind kind)
{
    switch (kind) {
        case ProviderKind::OpenAiCompat:
            return std::make_unique<OpenAiCompatProvider>();
        case ProviderKind::Anthropic:
            return std::make_unique<AnthropicProvider>();
        case ProviderKind::OllamaNative:
            return std::make_unique<OllamaProvider>();
        case ProviderKind::Gemini:
            return std::make_unique<OpenAiCompatProvider>();   // fallback this phase
    }
    return std::make_unique<OpenAiCompatProvider>();
}

} // namespace ai
