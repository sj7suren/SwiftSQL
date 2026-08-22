// OpenAiCompatProvider.cpp — see header. Wire shape (OpenAI Chat Completions):
//   POST {baseUrl}/chat/completions   Authorization: Bearer <key>
//   body  {model, messages:[{role,content}], stream, max_tokens[, temperature]}
//   SSE   data: {choices:[{delta:{content}}], usage?}   ... data: [DONE]
//   full  {choices:[{message:{content}}]}  |  {error:{message}}
#include "ai/OpenAiCompatProvider.h"

#include "ai/Json.h"

namespace ai {

namespace {

// baseUrl may or may not carry a trailing '/'; normalise before joining a path.
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

// Pull the OpenAI-shaped error text: {"error":{"message":"..."}} (also tolerate a
// bare top-level {"message":"..."}). Empty string when no error text is present.
wxString ExtractErrorMessage(const JsonValue& root)
{
    const JsonValue& e = root[L"error"];
    if (e.isObject() && e.has(L"message")) return e[L"message"].asString();
    if (root.has(L"message")) return root[L"message"].asString();
    return wxString();
}

} // namespace

HttpRequestSpec OpenAiCompatProvider::BuildRequest(const AiRequest& req,
                                                   const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/chat/completions");
    spec.method = L"POST";
    spec.headers.push_back({L"Authorization", L"Bearer " + cfg.apiKey});
    spec.headers.push_back({L"Content-Type", L"application/json"});

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
    w.Key(L"max_tokens").Int(req.maxTokens);
    if (req.temperature >= 0)
        w.Key(L"temperature").Num(req.temperature);
    w.EndObject();
    spec.body = w.str();

    return spec;
}

void OpenAiCompatProvider::ParseStreamEvent(const wxString& payload,
                                            wxString& outDelta, bool& outDone,
                                            int& promptTokens, int& completionTokens,
                                            AiError& outError) const
{
    wxString p = payload;
    p.Trim(true).Trim(false);
    if (p.IsEmpty()) return;
    if (p == L"[DONE]") { outDone = true; return; }

    const JsonValue root = JsonValue::Parse(p);

    // A failure can arrive INSIDE a 200 stream (unknown model id, quota, rate
    // limit). Deliberately stricter than ExtractErrorMessage(): that helper also
    // accepts a bare top-level "message", which is fine for a whole response body
    // but would misread an ordinary chunk here. Only a real {"error":{...}} object
    // counts as an error event.
    const JsonValue& errObj = root[L"error"];
    if (errObj.isObject()) {
        const wxString msg = errObj.has(L"message") ? errObj[L"message"].asString()
                                                    : wxString();
        outError.kind        = AiErrorKind::Unknown;
        outError.message     = msg.IsEmpty() ? wxString(L"provider reported an error")
                                             : msg;
        outError.providerRaw = p;
        return;
    }

    const wxString content = root.path(L"choices.0.delta.content").asString();
    if (!content.IsEmpty()) outDelta += content;

    const JsonValue& usage = root[L"usage"];
    if (usage.isObject()) {
        if (usage.has(L"prompt_tokens"))
            promptTokens = static_cast<int>(usage[L"prompt_tokens"].asInt());
        if (usage.has(L"completion_tokens"))
            completionTokens = static_cast<int>(usage[L"completion_tokens"].asInt());
    }
}

wxString OpenAiCompatProvider::ParseFullResponse(const wxString& body, AiError& err) const
{
    const JsonValue root = JsonValue::Parse(body);

    const wxString errMsg = ExtractErrorMessage(root);
    if (!errMsg.IsEmpty()) {
        err.kind        = AiErrorKind::Unknown;
        err.message     = errMsg;
        err.providerRaw = body;
        return wxString();
    }
    return root.path(L"choices.0.message.content").asString();
}

HttpRequestSpec OpenAiCompatProvider::BuildModelsRequest(const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/models");
    spec.method = L"GET";
    spec.headers.push_back({L"Authorization", L"Bearer " + cfg.apiKey});
    return spec;
}

std::vector<wxString> OpenAiCompatProvider::ParseModels(const wxString& body) const
{
    // {"data":[{"id":"gpt-4o", ...}, ...]}
    std::vector<wxString> ids;
    const JsonValue root = JsonValue::Parse(body);
    const JsonValue& data = root[L"data"];
    if (!data.isArray()) return ids;
    ids.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        const wxString id = data[i][L"id"].asString();
        if (!id.IsEmpty()) ids.push_back(id);
    }
    return ids;
}

AiError OpenAiCompatProvider::MapHttpError(int status, const wxString& body) const
{
    AiError err;
    err.httpStatus  = status;
    err.providerRaw = body;

    const JsonValue root = JsonValue::Parse(body);
    const wxString  msg  = ExtractErrorMessage(root);

    if (status == 401 || status == 403) {
        err.kind = AiErrorKind::Auth;
    } else if (status == 429) {
        err.kind = AiErrorKind::RateLimit;
    } else if (status == 400) {
        const wxString hint = (msg + L" " + body).Lower();
        err.kind = (hint.Contains(L"context") || hint.Contains(L"token"))
                       ? AiErrorKind::ContextOverflow
                       : AiErrorKind::BadRequest;
    } else if (status >= 500 && status < 600) {
        err.kind = AiErrorKind::ServerError;
    } else {
        err.kind = AiErrorKind::Unknown;
    }

    err.message = msg;
    return err;
}

} // namespace ai
