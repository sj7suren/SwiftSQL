// ai_provider_selection_test.cpp — which provider the AI features actually talk to,
// and when a cached client has to be thrown away.
//
// THE BEHAVIOUR THIS PINS DOWN. Preferences used to carry a separate 「设为默认」
// button, so "the provider I am looking at" and "the provider that answers" were two
// different things and drifted apart constantly. They are now one: Preferences records
// whichever provider is SELECTED when OK is pressed, and DefaultProvider() hands that
// selection back UNCONDITIONALLY.
//
// The unconditional part is the whole point and the easiest thing to regress. The old
// implementation fell through to "the first provider that happens to carry an API key"
// whenever the selected one looked unconfigured. That is a silent redirect: a user who
// picks DeepSeek but has not pasted the key yet would have had their prompts — schema
// included — sent to OpenAI instead. Choosing wrong is the user's to make and to see;
// callers check CanFetchModels() and surface "请到 偏好设置 ▸ AI 配置服务" instead.
//
// The second half covers ProviderKey(), the identity every cached ai::AiClient is
// tagged with. AiClient bakes kind/baseUrl/apiKey in at construction, so a key that
// fails to change after an edit means the chat panel keeps talking to the OLD endpoint
// with the OLD credentials — exactly the bug that used to force a window reopen.
#include "ai/AiConfig.h"

#include <cstdio>
#include <wx/string.h>

using namespace ai;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

// Build a provider row. An empty key models "preset added, not configured yet".
static AiProviderConfig Prov(const wchar_t* id, ProviderKind kind, const wchar_t* key)
{
    AiProviderConfig p;
    p.id      = id;
    p.name    = id;
    p.kind    = kind;
    p.baseUrl = wxString(L"https://") + id + L".example/v1";
    p.apiKey  = key;
    p.model   = wxString(id) + L"-model";
    return p;
}

int main()
{
    // ------------------------------------------------- the selection is the answer
    {
        AiSettings s;
        s.providers = { Prov(L"openai",   ProviderKind::OpenAiCompat, L"sk-configured"),
                        Prov(L"deepseek", ProviderKind::OpenAiCompat, L"") };
        s.defaultProviderId = L"deepseek";

        const AiProviderConfig* got = s.DefaultProvider();
        // The regression that matters: an UNCONFIGURED selection must still win over
        // a configured sibling. Silently answering with openai here would ship the
        // user's prompts to a vendor they did not choose.
        ExpectTrue("unconfigured selection still wins", got && got->id == L"deepseek");
        ExpectTrue("unconfigured selection is reported as unusable",
                   got && !CanFetchModels(*got));

        s.providers[1].apiKey = L"sk-deepseek";
        got = s.DefaultProvider();
        ExpectTrue("configured selection wins",          got && got->id == L"deepseek");
        ExpectTrue("configured selection is usable",     got && CanFetchModels(*got));
    }

    // ------------------------------------------------------------ legacy fallbacks
    {
        // settings.ini written before the selection was persisted: no id recorded.
        AiSettings s;
        s.providers = { Prov(L"anthropic", ProviderKind::Anthropic,    L""),
                        Prov(L"deepseek",  ProviderKind::OpenAiCompat, L"sk-deepseek") };
        const AiProviderConfig* got = s.DefaultProvider();
        ExpectTrue("no selection → first provider carrying credentials",
                   got && got->id == L"deepseek");
    }
    {
        // No selection AND nothing configured: still hand back a row so callers can
        // name a provider in the "please configure" hint rather than saying nothing.
        AiSettings s;
        s.providers = { Prov(L"anthropic", ProviderKind::Anthropic,    L""),
                        Prov(L"deepseek",  ProviderKind::OpenAiCompat, L"") };
        const AiProviderConfig* got = s.DefaultProvider();
        ExpectTrue("nothing configured → first provider", got && got->id == L"anthropic");
    }
    {
        // A selection pointing at a deleted provider must not resurrect it or crash.
        AiSettings s;
        s.providers = { Prov(L"deepseek", ProviderKind::OpenAiCompat, L"sk-deepseek") };
        s.defaultProviderId = L"removed-provider";
        const AiProviderConfig* got = s.DefaultProvider();
        ExpectTrue("stale selection falls back to a real row", got && got->id == L"deepseek");
    }
    {
        AiSettings s;
        ExpectTrue("no providers at all → nullptr", s.DefaultProvider() == nullptr);
    }

    // ------------------------------------------------------------- usability gate
    {
        // Local Ollama needs no credentials, so it is usable with an empty key while
        // every key-based kind is not.
        ExpectTrue("ollama is usable without a key",
                   CanFetchModels(Prov(L"ollama", ProviderKind::OllamaNative, L"")));
        ExpectTrue("openai-compat is not usable without a key",
                   !CanFetchModels(Prov(L"openai", ProviderKind::OpenAiCompat, L"")));
        ExpectTrue("anthropic is not usable without a key",
                   !CanFetchModels(Prov(L"anthropic", ProviderKind::Anthropic, L"")));
    }

    // ------------------------------------- cached-client identity (ai::ProviderKey)
    {
        const AiProviderConfig base = Prov(L"deepseek", ProviderKind::OpenAiCompat, L"sk-1");
        ExpectTrue("identical configs share a key", ProviderKey(base) == ProviderKey(base));

        // Each of these invalidates a cached AiClient. Missing any one of them leaves
        // an open chat panel pointed at the previous provider.
        AiProviderConfig k = base; k.kind    = ProviderKind::Anthropic;
        ExpectTrue("kind change invalidates the key",    ProviderKey(k) != ProviderKey(base));
        AiProviderConfig u = base; u.baseUrl = L"https://other.example/v1";
        ExpectTrue("baseUrl change invalidates the key", ProviderKey(u) != ProviderKey(base));
        AiProviderConfig a = base; a.apiKey  = L"sk-2";
        ExpectTrue("apiKey change invalidates the key",  ProviderKey(a) != ProviderKey(base));
        AiProviderConfig m = base; m.model   = L"deepseek-r2";
        ExpectTrue("model change invalidates the key",   ProviderKey(m) != ProviderKey(base));

        // Renaming a provider is cosmetic: it must NOT force a rebuild, otherwise a
        // stray edit would drop the model pill selection for no reason.
        AiProviderConfig n = base; n.name = L"我的 DeepSeek"; n.id = L"other-id";
        ExpectTrue("name/id are not part of the wire identity",
                   ProviderKey(n) == ProviderKey(base));

        // Fields are separated, not concatenated: two configs that would collide under
        // naive string joining must still differ.
        AiProviderConfig x = base; x.baseUrl = L"https://a"; x.model = L"b-model";
        AiProviderConfig y = base; y.baseUrl = L"https://ab"; y.model = L"-model";
        ExpectTrue("key fields cannot smear into each other", ProviderKey(x) != ProviderKey(y));
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails;
}
