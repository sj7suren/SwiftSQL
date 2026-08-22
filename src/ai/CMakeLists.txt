# swiftsql_ai — the AI layer: unified LLM provider abstraction (Anthropic / OpenAI /
# DeepSeek / 智谱GLM / Ollama), prompt templates, streaming HTTP over wxWebRequest,
# and a self-contained JSON reader/writer. Leaf module: depends only on core (for
# Settings/Secret) + db (for db::Dialect in prompts) + wxWidgets base/net. No ui dep.
add_library(swiftsql_ai STATIC
    Json.cpp
    SseParser.cpp
    PromptBuilder.cpp
    AiConfigStore.cpp
    OpenAiCompatProvider.cpp
    AnthropicProvider.cpp
    OllamaProvider.cpp
    ProviderFactory.cpp
    AiHttp.cpp
    AiClient.cpp
)
add_library(swiftsql::ai ALIAS swiftsql_ai)

target_include_directories(swiftsql_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)

# wxWebRequest lives in the wxWidgets "net" component (non-GUI). db is PUBLIC only for
# the db::Dialect enum used in prompt templates.
target_link_libraries(swiftsql_ai
    PUBLIC  swiftsql::core swiftsql::db wx::core wx::base wx::net
)
