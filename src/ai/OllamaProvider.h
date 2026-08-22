// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// OllamaProvider.h — adapter for a local Ollama server's NATIVE chat API (kind
// OllamaNative), as opposed to its OpenAI-compatible /v1 shim (which OpenAiCompat
// handles). Two shape differences this adapter absorbs: the stream is newline-
// delimited JSON (one object per line, NOT SSE "data:" frames), and there is no auth
// by default — a Bearer header is added only when the config carries an apiKey.
#pragma once

#include "ai/IAiProvider.h"

namespace ai {

class OllamaProvider final : public IAiProvider {
public:
    HttpRequestSpec BuildRequest(const AiRequest& req,
                                 const AiProviderConfig& cfg) const override;

    void ParseStreamEvent(const wxString& payload,
                          wxString& outDelta, bool& outDone,
                          int& promptTokens, int& completionTokens,
                          AiError& outError) const override;

    wxString ParseFullResponse(const wxString& body, AiError& err) const override;

    AiError MapHttpError(int status, const wxString& body) const override;

    HttpRequestSpec BuildModelsRequest(const AiProviderConfig& cfg) const override;

    std::vector<wxString> ParseModels(const wxString& body) const override;
};

} // namespace ai
