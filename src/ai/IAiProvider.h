// IAiProvider.h — the provider seam. A provider is a PURE translator for one API
// family: it turns an AiRequest into an HTTP request spec, and turns response bytes
// (a streaming SSE/NDJSON event, or a full non-stream body) into normalised deltas.
//
// Providers do NO networking and NO threading — AiClient owns the wxWebRequest
// transport and the SSE/NDJSON framing, and calls these pure methods. That keeps
// every provider trivially unit-testable (feed a canned event line → assert delta).
#pragma once

#include "ai/AiTypes.h"
#include "ai/AiConfig.h"

#include <memory>
#include <utility>
#include <vector>

namespace ai {

// A fully-specified HTTP call the transport can execute as-is.
struct HttpRequestSpec {
    wxString                                        url;
    wxString                                        method = L"POST";
    std::vector<std::pair<wxString, wxString>>      headers;   // name → value
    wxString                                        body;      // JSON
};

class IAiProvider {
public:
    virtual ~IAiProvider() = default;

    // Build the wire request for `req` under `cfg` (URL incl. any model-in-path,
    // auth headers, and the JSON body in this vendor's shape).
    virtual HttpRequestSpec BuildRequest(const AiRequest& req,
                                         const AiProviderConfig& cfg) const = 0;

    // Parse ONE streaming event payload (for SSE this is the text after "data: ";
    // for Ollama NDJSON it is one JSON line). Append any new text to `outDelta`;
    // set `outDone` when this vendor's terminator is seen (OpenAI "[DONE]",
    // Anthropic message_stop, Gemini/Ollama final chunk). `usage*` are filled with
    // token counts when the event carries them (else left untouched). Never throws.
    //
    // `outError` IS NOT OPTIONAL TO IMPLEMENT. Every vendor here can report a
    // failure INSIDE a 200 stream (bad model id, quota, rate limit, content
    // filter) instead of as a non-2xx status: OpenAI-compat sends a JSON object
    // carrying "error", Anthropic sends {"type":"error",...}, Ollama sends
    // {"error":"..."}. A parser that only looks for content silently yields
    // nothing, the stream ends "successfully" with zero deltas, and the user is
    // told "(No response)" while the real reason is thrown away — which is
    // exactly the bug this parameter exists to end. Fill `outError` when the
    // event is an error report; AiClient turns it into onError.
    virtual void ParseStreamEvent(const wxString& payload,
                                  wxString& outDelta, bool& outDone,
                                  int& promptTokens, int& completionTokens,
                                  AiError& outError) const = 0;

    // Parse a COMPLETE non-streaming response body → the assistant's full text.
    // On an API-level error carried in the body, fill `err` and return "".
    virtual wxString ParseFullResponse(const wxString& body, AiError& err) const = 0;

    // Map an HTTP status + (optional) error body to a normalised AiError. Called by
    // AiClient when the transport reports a non-2xx status.
    virtual AiError MapHttpError(int status, const wxString& body) const = 0;

    // ---- model discovery (for the Preferences "获取模型列表" button) ----
    // Build the GET request that lists this provider's available models
    // (OpenAI-compat: GET {base}/models; Ollama: GET {base}/api/tags; Anthropic:
    // GET {base}/models). Same auth headers as a chat call.
    virtual HttpRequestSpec BuildModelsRequest(const AiProviderConfig& cfg) const = 0;
    // Parse the model-list response body → model ids. Empty on error / unsupported.
    virtual std::vector<wxString> ParseModels(const wxString& body) const = 0;
};

// Factory: build the right provider for a config's kind. Defined in ProviderFactory.cpp.
std::unique_ptr<IAiProvider> CreateProvider(ProviderKind kind);

} // namespace ai
