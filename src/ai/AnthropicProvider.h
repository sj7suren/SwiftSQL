// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AnthropicProvider.h — adapter for Anthropic's Messages API (Claude). Differs from
// the OpenAI shape in three ways this adapter absorbs: System turns are hoisted to a
// top-level `system` field (messages[] holds only user/assistant), `max_tokens` is
// mandatory, and streaming is a typed SSE event flow (content_block_delta carries the
// text, message_start/message_delta carry token usage, message_stop terminates).
#pragma once

#include "ai/IAiProvider.h"

namespace ai {

class AnthropicProvider final : public IAiProvider {
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
