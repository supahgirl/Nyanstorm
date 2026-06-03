/**
 * @file llllmnetworkmanager.cpp
 * @brief Asynchronous HTTP client for LLM API requests
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 */

#include "llviewerprecompiledheaders.h"

#include "llllmnetworkmanager.h"

#include "bufferarray.h"
#include <boost/json.hpp>
#include "llerror.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llimview.h"
#include "fsfloaterim.h"

// ═════════════════════════════════════════════════════════════════════════════
//  LLSD → boost::json converters
// ═════════════════════════════════════════════════════════════════════════════

static boost::json::value llsdToJson(const LLSD& val)
{
    switch (val.type())
    {
    case LLSD::TypeString:
        return boost::json::value(val.asString());
    case LLSD::TypeInteger:
        return boost::json::value(val.asInteger());
    case LLSD::TypeReal:
        return boost::json::value(val.asReal());
    case LLSD::TypeBoolean:
        return boost::json::value(val.asBoolean());
    case LLSD::TypeArray:
    {
        boost::json::array arr;
        for (LLSD::array_const_iterator it = val.beginArray(); it != val.endArray(); ++it)
            arr.push_back(llsdToJson(*it));
        return boost::json::value(std::move(arr));
    }
    case LLSD::TypeMap:
    {
        boost::json::object obj;
        for (LLSD::map_const_iterator it = val.beginMap(); it != val.endMap(); ++it)
            obj[it->first] = llsdToJson(it->second);
        return boost::json::value(std::move(obj));
    }
    default:
        return boost::json::value();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  LLMHandler — one per request, freed after onCompleted
// ═════════════════════════════════════════════════════════════════════════════

void FSLLMNetworkManager::LLMHandler::onCompleted(LLCore::HttpHandle handle,
                                                   LLCore::HttpResponse* response)
{
    if (!response) return;
    LLCore::BufferArray* body = response->getBody();
    LLCore::HttpStatus status = response->getStatus();

    if (!status)
    {
        std::string errText;
        if (body)
        {
            errText.resize(body->size());
            body->read(0, &errText[0], body->size());
        }
        LL_WARNS("LLM") << "Request failed: " << status.toString()
                        << " body=" << errText << LL_ENDL;
        return;
    }
    if (!body) return;

    std::string responseText;
    responseText.resize(body->size());
    body->read(0, &responseText[0], body->size());

    // Parse and inject response into the chat session
    std::string content;
    try
    {
        boost::json::value jv = boost::json::parse(responseText);
        if (jv.is_object())
        {
            auto& root = jv.as_object();
            if (root.contains("choices") && root["choices"].is_array())
            {
                auto& choices = root["choices"].as_array();
                if (!choices.empty() && choices[0].is_object())
                {
                    auto& first = choices[0].as_object();
                    if (first.contains("message") && first["message"].is_object())
                    {
                        auto& msg = first["message"].as_object();
                        if (msg.contains("content") && msg["content"].is_string())
                            content = msg["content"].as_string().c_str();
                    }
                }
            }
            else if (root.contains("message") && root["message"].is_object())
            {
                auto& msg = root["message"].as_object();
                if (msg.contains("content") && msg["content"].is_string())
                    content = msg["content"].as_string().c_str();
            }
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("LLM") << "Failed to parse response: " << e.what() << LL_ENDL;
    }

    // Inject into the IM chat session
    if (!content.empty() && mSessionID.notNull())
    {
        std::string from = mAgentName.empty() ? "AI Agent" : mAgentName;
        LLIMModel::instance().addMessage(mSessionID, from, mSessionID, content);

        // Force the chat panel to refresh immediately
        FSFloaterIM* floater = FSFloaterIM::findInstance(mSessionID);
        if (floater)
            floater->updateMessages();

        LL_INFOS("LLM") << "Injected response into session " << mSessionID << LL_ENDL;

        // Also log to stderr for terminal visibility
        fprintf(stderr, "[LLM] %s: %s\n", from.c_str(), content.c_str());
    }
    else
    {
        LL_INFOS("LLM") << "Response (no session): " << content << LL_ENDL;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Singleton ctor / dtor
// ═════════════════════════════════════════════════════════════════════════════

FSLLMNetworkManager::FSLLMNetworkManager()
    : mHttpRequest(new LLCore::HttpRequest())
    , mHttpOptions(new LLCore::HttpOptions())
{
    mHttpOptions->setTransferTimeout(120L);
    gIdleCallbacks.addFunction([](void*) {
        FSLLMNetworkManager::getInstance()->update();
    }, nullptr);
}

FSLLMNetworkManager::~FSLLMNetworkManager()
{
}

// ═════════════════════════════════════════════════════════════════════════════
//  sendRequest
// ═════════════════════════════════════════════════════════════════════════════

void FSLLMNetworkManager::sendRequest(const std::string& endpoint,
                                       const std::string& apiKey,
                                       bool isOpenRouter,
                                       const LLSD& bodyLLSD,
                                       const LLUUID& sessionID,
                                       const std::string& agentName)
{
    std::string jsonBody = serializeToJSON(bodyLLSD);
    LL_INFOS("LLM") << "Sending request to " << endpoint
                    << " (" << jsonBody.size() << " bytes)"
                    << " body=" << jsonBody.substr(0, 500) << LL_ENDL;

    LLCore::BufferArray* ba = new LLCore::BufferArray();
    ba->append(jsonBody.data(), jsonBody.size());

    LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders());
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");

    if (!apiKey.empty())
        headers->append("Authorization", "Bearer " + apiKey);

    if (isOpenRouter)
    {
        headers->append("HTTP-Referer", "https://firestormviewer.org");
        headers->append("X-Title", "Firestorm Viewer");
    }

    LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions());
    options->setTransferTimeout(120L);

    LLMHandler* h = new LLMHandler();
    h->mSessionID = sessionID;
    h->mAgentName = agentName;
    LLCore::HttpHandler::ptr_t handler(h);

    LLCore::HttpHandle handle = mHttpRequest->requestPost(
        LLCore::HttpRequest::DEFAULT_POLICY_ID,
        endpoint,
        ba,
        options,
        headers,
        handler);

    if (handle == LLCORE_HTTP_HANDLE_INVALID)
    {
        LLCore::HttpStatus status = mHttpRequest->getStatus();
        LL_WARNS("LLM") << "Failed to POST request. Status: " << status.toString() << LL_ENDL;
    }

    ba->release();
}

// ═════════════════════════════════════════════════════════════════════════════
//  update() — called every frame from idle callback
// ═════════════════════════════════════════════════════════════════════════════

void FSLLMNetworkManager::update()
{
    mHttpRequest->update(0L);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Private
// ═════════════════════════════════════════════════════════════════════════════

std::string FSLLMNetworkManager::serializeToJSON(const LLSD& body) const
{
    return boost::json::serialize(llsdToJson(body));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Streaming (curl + SSE, dispatched via queueMCPEvent)
// ═════════════════════════════════════════════════════════════════════════════

#include <curl/curl.h>

// Forward declare queueMCPEvent from fsfloaterim.cpp
extern void queueMCPEvent(const LLUUID& session_id, const std::string& token, bool done);

// Context for a single streaming request
struct LLMStreamContext
{
    LLUUID     session_id;
    std::string agent_name;
    std::string buffer;   // partial SSE line accumulator
};

// Curl write callback — invoked each time data arrives from the server
static size_t llmStreamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    if (total == 0) return 0;

    LLMStreamContext* ctx = static_cast<LLMStreamContext*>(userp);
    ctx->buffer.append(static_cast<const char*>(contents), total);

    // Process complete lines in the buffer
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos)
    {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        // Trim \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Look for SSE data: lines
        const char* prefix = "data: ";
        if (line.rfind(prefix, 0) != 0) continue;
        std::string payload = line.substr(6);

        // Check for stream end
        if (payload == "[DONE]")
        {
            queueMCPEvent(ctx->session_id, "", true);
            continue;
        }

        // Parse JSON delta
        try
        {
            boost::json::value jv = boost::json::parse(payload);
            if (!jv.is_object()) continue;
            auto& root = jv.as_object();

            if (root.contains("choices") && root["choices"].is_array())
            {
                auto& choices = root["choices"].as_array();
                if (!choices.empty() && choices[0].is_object())
                {
                    auto& first = choices[0].as_object();
                    if (first.contains("delta") && first["delta"].is_object())
                    {
                        auto& delta = first["delta"].as_object();
                        if (delta.contains("content") && delta["content"].is_string())
                        {
                            std::string token = delta["content"].as_string().c_str();
                            if (!token.empty())
                                queueMCPEvent(ctx->session_id, token, false);
                        }
                    }
                }
            }
        }
        catch (...) { /* skip malformed chunks */ }
    }
    return total;
}

void FSLLMNetworkManager::sendStreamRequest(const std::string& endpoint,
                                             const std::string& apiKey,
                                             bool isOpenRouter,
                                             const LLSD& bodyLLSD,
                                             const LLUUID& sessionID,
                                             const std::string& agentName)
{
    std::string jsonBody = serializeToJSON(bodyLLSD);

    // Add "stream": true to the body
    boost::json::value jv = boost::json::parse(jsonBody);
    if (jv.is_object())
    {
        jv.as_object()["stream"] = true;
        jsonBody = boost::json::serialize(jv);
    }

    LL_INFOS("LLM") << "Streaming request to " << endpoint << " (" << jsonBody.size() << " bytes)" << LL_ENDL;

    LLMStreamContext* ctx = new LLMStreamContext();
    ctx->session_id = sessionID;
    ctx->agent_name = agentName;

    std::thread([endpoint, apiKey, isOpenRouter, jsonBody, ctx]()
    {
        CURL* curl = curl_easy_init();
        if (!curl) { delete ctx; return; }

        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)jsonBody.size());

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!apiKey.empty())
            headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
        if (isOpenRouter)
        {
            headers = curl_slist_append(headers, "HTTP-Referer: https://firestormviewer.org");
            headers = curl_slist_append(headers, "X-Title: Firestorm Viewer");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, llmStreamWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            LL_WARNS("LLM") << "Stream request failed: " << curl_easy_strerror(res) << LL_ENDL;
            queueMCPEvent(ctx->session_id, "[ERROR] " + std::string(curl_easy_strerror(res)), false);
            queueMCPEvent(ctx->session_id, "", true);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        delete ctx;
    }).detach();
}
