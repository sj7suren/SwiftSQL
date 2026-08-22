// OllamaProvider.cpp — see header. Wire shape (Ollama native chat API):
//   POST {baseUrl}/api/chat   (no auth unless apiKey set → Bearer)
//   body   {model, messages:[{role,content}], stream}
//   NDJSON {message:{content}, done, prompt_eval_count?, eval_count?}  (one per line)
//   full   {message:{content}}  |  {error:"..."}
#include "ai/OllamaProvider.h"

#include "ai/Json.h"

namespace ai {

namespace {

wxString JoinUrl(const wxString& base, const wxString& path)
{
    wxString b = base;
    while (!b.IsEmpty() && b.Last() == '/') b.RemoveLast();
    return b + path;
}

const wchar_t* RoleName(Role r)
{
    switch (r) {
        case Role::System:    return L"system";
        case Role::User:      return L"user";
        case Role::Assistant: return L"assistant";
    }
    return L"user";
}

// Ollama reports errors as a bare top-level string: {"error":"model not found"}.
wxString ExtractErrorMessage(const JsonValue& root)
{
    if (root.has(L"error")) return root[L"error"].asString();
    return wxString();
}

} // namespace

HttpRequestSpec OllamaProvider::BuildRequest(const AiRequest& req,
                                             const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/api/chat");
    spec.method = L"POST";
    spec.headers.push_back({L"Content-Type", L"application/json"});
    if (!cfg.apiKey.IsEmpty())                       // local server usually needs none
        spec.headers.push_back({L"Authorization", L"Bearer " + cfg.apiKey});

    JsonWriter w;
    w.BeginObject();
    w.Key(L"model").Str(req.model);
    w.Key(L"messages").BeginArray();
    for (const AiMessage& m : req.messages) {
        w.BeginObject();
        w.Key(L"role").Str(RoleName(m.role));
        w.Key(L"content").Str(m.content);
        w.EndObject();
    }
    w.EndArray();
    w.Key(L"stream").Bool(req.stream);
    w.EndObject();
    spec.body = w.str();

    return spec;
}

void OllamaProvider::ParseStreamEvent(const wxString& payload,
                                      wxString& outDelta, bool& outDone,
                                      int& promptTokens, int& completionTokens,
                                      AiError& outError) const
{
    wxString p = payload;
    p.Trim(true).Trim(false);
    if (p.IsEmpty()) return;

    const JsonValue root = JsonValue::Parse(p);

    // Ollama reports a pull-needed / unknown model as {"error":"..."} on an
    // otherwise-200 NDJSON stream — the single most common local failure, and
    // previously invisible: the line yielded no content and the turn ended blank.
    const wxString errMsg = ExtractErrorMessage(root);
    if (!errMsg.IsEmpty()) {
        outError.kind        = AiErrorKind::ServerError;
        outError.message     = errMsg;
        outError.providerRaw = p;
        return;
    }

    const wxString content = root.path(L"message.content").asString();
    if (!content.IsEmpty()) outDelta += content;

    if (root[L"done"].asBool()) outDone = true;

    if (root.has(L"prompt_eval_count"))
        promptTokens = static_cast<int>(root[L"prompt_eval_count"].asInt());
    if (root.has(L"eval_count"))
        completionTokens = static_cast<int>(root[L"eval_count"].asInt());
}

wxString OllamaProvider::ParseFullResponse(const wxString& body, AiError& err) const
{
    const JsonValue root = JsonValue::Parse(body);

    const wxString errMsg = ExtractErrorMessage(root);
    if (!errMsg.IsEmpty()) {
        err.kind        = AiErrorKind::ServerError;
        err.message     = errMsg;
        err.providerRaw = body;
        return wxString();
    }
    return root.path(L"message.content").asString();
}

HttpRequestSpec OllamaProvider::BuildModelsRequest(const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/api/tags");
    spec.method = L"GET";
    if (!cfg.apiKey.IsEmpty())                       // local server usually needs none
        spec.headers.push_back({L"Authorization", L"Bearer " + cfg.apiKey});
    return spec;
}

std::vector<wxString> OllamaProvider::ParseModels(const wxString& body) const
{
    // {"models":[{"name":"llama3:latest", ...}, ...]}
    std::vector<wxString> names;
    const JsonValue root = JsonValue::Parse(body);
    const JsonValue& models = root[L"models"];
    if (!models.isArray()) return names;
    names.reserve(models.size());
    for (size_t i = 0; i < models.size(); ++i) {
        const wxString name = models[i][L"name"].asString();
        if (!name.IsEmpty()) names.push_back(name);
    }
    return names;
}

AiError OllamaProvider::MapHttpError(int status, const wxString& body) const
{
    AiError err;
    err.httpStatus  = status;
    err.providerRaw = body;

    const JsonValue root = JsonValue::Parse(body);
    const wxString  msg  = ExtractErrorMessage(root);

    // A dead/unreachable local daemon surfaces as no HTTP status (connect failure).
    if (status <= 0) {
        err.kind    = AiErrorKind::Network;
        err.message = msg.IsEmpty()
                          ? wxString(L"无法连接本地 Ollama 服务，请确认 ollama serve 已启动")
                          : msg;
        return err;
    }

    if (status == 401 || status == 403) {
        err.kind = AiErrorKind::Auth;
    } else if (status == 429) {
        err.kind = AiErrorKind::RateLimit;
    } else if (status == 400) {
        const wxString hint = (msg + L" " + body).Lower();
        err.kind = (hint.Contains(L"context") || hint.Contains(L"token"))
                       ? AiErrorKind::ContextOverflow
                       : AiErrorKind::BadRequest;
    } else if (status == 404) {
        err.kind = AiErrorKind::BadRequest;   // unknown model / route
    } else if (status >= 500 && status < 600) {
        err.kind = AiErrorKind::ServerError;
    } else {
        err.kind = AiErrorKind::Unknown;
    }

    err.message = msg;
    return err;
}

} // namespace ai
