// OpenAiCompatProvider.h — adapter for the OpenAI Chat Completions wire protocol and
// every endpoint that clones it: OpenAI, DeepSeek, 智谱GLM (paas/v4), Kimi, 通义千问,
// OpenRouter, Groq, xAI, Mistral, and local LM Studio / vLLM / Ollama's /v1 shim.
//
// Pure translator (no I/O): BuildRequest → POST {baseUrl}/chat/completions with a
// Bearer key; streaming events are SSE "data:" JSON with the token in
// choices[0].delta.content and the "[DONE]" sentinel closing the stream.
#pragma once

#include "ai/IAiProvider.h"

namespace ai {

class OpenAiCompatProvider final : public IAiProvider {
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
