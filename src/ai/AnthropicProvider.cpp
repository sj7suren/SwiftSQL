// AnthropicProvider.cpp — see header. Wire shape (Anthropic Messages API):
//   POST {baseUrl}/messages   x-api-key: <key>   anthropic-version: 2023-06-01
//   body  {model, max_tokens, system?, messages:[{role,content}], stream[, temperature]}
//   SSE   event/type: message_start{message.usage.input_tokens},
//         content_block_delta{delta.text}, message_delta{usage.output_tokens},
//         message_stop (terminator); ping/content_block_{start,stop} ignored.
//   full  {content:[{type,text}]}  |  {error:{message}}
#include "ai/AnthropicProvider.h"

#include "ai/Json.h"

namespace ai {

namespace {

wxString JoinUrl(const wxString& base, const wxString& path)
{
    wxString b = base;
    while (!b.IsEmpty() && b.Last() == '/') b.RemoveLast();
    return b + path;
}

// Anthropic errors: {"error":{"type":..,"message":".."}}. Empty when absent.
wxString ExtractErrorMessage(const JsonValue& root)
{
    const JsonValue& e = root[L"error"];
    if (e.isObject() && e.has(L"message")) return e[L"message"].asString();
    return wxString();
}

} // namespace

HttpRequestSpec AnthropicProvider::BuildRequest(const AiRequest& req,
                                                const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/messages");
    spec.method = L"POST";
    spec.headers.push_back({L"x-api-key", cfg.apiKey});
    spec.headers.push_back({L"anthropic-version", L"2023-06-01"});
    spec.headers.push_back({L"Content-Type", L"application/json"});

    // Hoist every System turn to the top-level `system` field; join multiples with \n.
    wxString systemText;
    for (const AiMessage& m : req.messages) {
        if (m.role != Role::System) continue;
        if (!systemText.IsEmpty()) systemText += L"\n";
        systemText += m.content;
    }

    JsonWriter w;
    w.BeginObject();
    w.Key(L"model").Str(req.model);
    w.Key(L"max_tokens").Int(req.maxTokens);        // mandatory for Anthropic
    if (!systemText.IsEmpty())
        w.Key(L"system").Str(systemText);
    w.Key(L"messages").BeginArray();
    for (const AiMessage& m : req.messages) {
        if (m.role == Role::System) continue;       // system lives at top level
        w.BeginObject();
        w.Key(L"role").Str(m.role == Role::Assistant ? L"assistant" : L"user");
        w.Key(L"content").Str(m.content);
        w.EndObject();
    }
    w.EndArray();
    w.Key(L"stream").Bool(req.stream);
    if (req.temperature >= 0)
        w.Key(L"temperature").Num(req.temperature);
    w.EndObject();
    spec.body = w.str();

    return spec;
}

void AnthropicProvider::ParseStreamEvent(const wxString& payload,
                                         wxString& outDelta, bool& outDone,
                                         int& promptTokens, int& completionTokens,
                                         AiError& outError) const
{
    wxString p = payload;
    p.Trim(true).Trim(false);
    if (p.IsEmpty()) return;

    const JsonValue root = JsonValue::Parse(p);
    const wxString  type = root[L"type"].asString();

    // Anthropic reports mid-stream failures (overloaded_error, invalid_request_error,
    // rate limits) as {"type":"error","error":{...}} on a 200 stream. Without this
    // the event fell through to "nothing to emit" and the turn ended with no text.
    if (type == L"error") {
        const wxString msg = ExtractErrorMessage(root);
        outError.kind        = AiErrorKind::Unknown;
        outError.message     = msg.IsEmpty() ? wxString(L"provider reported an error")
                                             : msg;
        outError.providerRaw = p;
        return;
    }

    if (type == L"content_block_delta") {
        const wxString text = root.path(L"delta.text").asString();
        if (!text.IsEmpty()) outDelta += text;
    } else if (type == L"message_delta") {
        const JsonValue& usage = root[L"usage"];
        if (usage.isObject() && usage.has(L"output_tokens"))
            completionTokens = static_cast<int>(usage[L"output_tokens"].asInt());
    } else if (type == L"message_start") {
        const JsonValue& usage = root.path(L"message.usage");
        if (usage.isObject() && usage.has(L"input_tokens"))
            promptTokens = static_cast<int>(usage[L"input_tokens"].asInt());
    } else if (type == L"message_stop") {
        outDone = true;
    }
    // ping / content_block_start / content_block_stop: nothing to emit.
}

wxString AnthropicProvider::ParseFullResponse(const wxString& body, AiError& err) const
{
    const JsonValue root = JsonValue::Parse(body);

    const wxString errMsg = ExtractErrorMessage(root);
    if (!errMsg.IsEmpty()) {
        err.kind        = AiErrorKind::Unknown;
        err.message     = errMsg;
        err.providerRaw = body;
        return wxString();
    }
    return root.path(L"content.0.text").asString();
}

HttpRequestSpec AnthropicProvider::BuildModelsRequest(const AiProviderConfig& cfg) const
{
    HttpRequestSpec spec;
    spec.url    = JoinUrl(cfg.baseUrl, L"/models");
    spec.method = L"GET";
    spec.headers.push_back({L"x-api-key", cfg.apiKey});
    spec.headers.push_back({L"anthropic-version", L"2023-06-01"});
    return spec;
}

std::vector<wxString> AnthropicProvider::ParseModels(const wxString& body) const
{
    // {"data":[{"id":"claude-3-5-sonnet-20241022", ...}, ...]}
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

AiError AnthropicProvider::MapHttpError(int status, const wxString& body) const
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
