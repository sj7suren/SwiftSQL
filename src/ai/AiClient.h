// AiClient.h — the AI layer's gateway facade. The ui layer talks ONLY to this class:
// give it an AiProviderConfig at construction and a stream of AiRequest/AiCallbacks at
// runtime, and it orchestrates the three collaborators behind the seam —
//
//   IAiProvider  (pure vendor translator)  · SseParser (stream framing)  · AiHttp (wx::net)
//
// so callers never see wxWebRequest, SSE/NDJSON framing, or vendor JSON. One request is
// in flight at a time (Busy()); everything is normalised to AiTypes.h vocabulary before
// it reaches the callbacks, which fire on the GUI thread (AiHttp guarantees that).
#pragma once

#include "ai/AiConfig.h"
#include "ai/AiHttp.h"
#include "ai/AiTypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace ai {

class IAiProvider;
class SseParser;

class AiClient {
public:
    explicit AiClient(AiProviderConfig cfg);
    ~AiClient();
    AiClient(const AiClient&)            = delete;
    AiClient& operator=(const AiClient&) = delete;

    // Send one chat turn. `req.model` empty → falls back to the config's default model.
    // Deltas/done/error are reported via `cb`. If a request is already in flight this is
    // a no-op — callers gate on Busy() (a single AiClient serves one conversation).
    void Chat(const AiRequest& req, const AiCallbacks& cb);

    // Model discovery for the Preferences "获取模型列表" button. Fires a non-streaming
    // GET (built by the config's provider), then reports either the parsed model ids or
    // a normalised AiError via `sink` on the GUI thread. Single call in flight — a new
    // ListModels / Chat cancels any previous transport (they share one AiHttp).
    using ModelsSink = std::function<void(std::vector<wxString> models, AiError err)>;
    void ListModels(const ModelsSink& sink);

    // Abort the in-flight request (if any). No callback fires for the cancelled call.
    void Cancel();

    // True from Chat() until the terminal onDone / onError (or a Cancel()).
    bool Busy() const { return busy_; }

private:
    // AiHttp transport callbacks (all on the GUI thread).
    void HandleData(const wxString& chunk);
    void HandleDone(int status, const wxString& body);
    void HandleErr(AiHttp::Fail reason, const wxString& detail);

    // ListModels transport callbacks (all on the GUI thread).
    void HandleModelsDone(int status, const wxString& body);
    void HandleModelsErr(AiHttp::Fail reason, const wxString& detail);

    void EmitPayload(const wxString& payload);   // one framed event → provider → onDelta
    void Finish() { busy_ = false; }

    AiProviderConfig cfg_;
    AiHttp           http_;
    AiCallbacks      cb_;

    std::unique_ptr<IAiProvider> provider_;   // rebuilt per Chat() for the config's kind
    std::unique_ptr<SseParser>   parser_;     // Sse, or Ndjson for OllamaNative

    std::unique_ptr<IAiProvider> modelsProvider_;  // rebuilt per ListModels()
    ModelsSink                   modelsSink_;       // held for the in-flight ListModels

    bool busy_    = false;
    bool stream_  = false;
    bool gotData_ = false;   // any bytes seen → a later transport failure is StreamInterrupted
    // Did the stream ever yield actual text? A 2xx stream that produced no delta
    // was not really a stream (see HandleDone) — the body has to be re-read as a
    // whole response before the turn may end.
    bool gotDelta_ = false;
    // An error the provider found INSIDE a 200 stream. Held rather than reported
    // on the spot: the transport still drives the stream to its terminal onDone,
    // and firing onError early would leave the caller's UI restored twice. The
    // FIRST such error wins and decides the turn's outcome in HandleDone().
    AiError streamError_;
    int  promptTokens_     = -1;
    int  completionTokens_ = -1;
};

} // namespace ai
