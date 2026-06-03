/**
 * @file llllmnetworkmanager.h
 * @brief Asynchronous HTTP client for LLM API requests (Ollama, Llama.cpp, OpenRouter)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 */

#ifndef FS_LLMNETWORKMANAGER_H
#define FS_LLMNETWORKMANAGER_H

#include "llsingleton.h"
#include "llsd.h"
#include "httpcommon.h"
#include "httphandler.h"
#include "httpresponse.h"
#include "httprequest.h"
#include "httpheaders.h"
#include "httpoptions.h"

/// Singleton LLSingleton-based async HTTP client for LLM providers.
/// Survives for the lifetime of the viewer — calls mHttpRequest->update()
/// periodically via an idle callback.
class FSLLMNetworkManager : public LLSingleton<FSLLMNetworkManager>
{
    LLSINGLETON(FSLLMNetworkManager);
    LOG_CLASS(FSLLMNetworkManager);

    /// Internal handler — one per request, lives on the heap via shared_ptr
    /// until the HTTP response arrives and onCompleted fires.
    class LLMHandler : public LLCore::HttpHandler
    {
    public:
        LLUUID mSessionID; // AI agent session to inject response into
        std::string mAgentName; // display name for the response
    
        void onCompleted(LLCore::HttpHandle handle,
                         LLCore::HttpResponse* response) override;
    };

public:
    ~FSLLMNetworkManager();

    /// Send a chat completion request to an LLM endpoint.
    void sendRequest(const std::string& endpoint,
                     const std::string& apiKey,
                     bool isOpenRouter,
                     const LLSD& bodyLLSD,
                     const LLUUID& sessionID = LLUUID::null,
                     const std::string& agentName = "");

    /// Send a STREAMING chat completion request — tokens arrive as SSE events
    /// and are injected into the chat character by character.
    void sendStreamRequest(const std::string& endpoint,
                           const std::string& apiKey,
                           bool isOpenRouter,
                           const LLSD& bodyLLSD,
                           const LLUUID& sessionID,
                           const std::string& agentName);

    /// Must be called periodically from the main thread to dispatch
    /// completion callbacks. Registered as an idle callback in the ctor.
    void update();

private:
    /// Serialize the LLSD payload to a JSON string for the request body.
    std::string serializeToJSON(const LLSD& body) const;

    LLCore::HttpRequest::ptr_t mHttpRequest;
    LLCore::HttpOptions::ptr_t mHttpOptions;
};

#endif // FS_LLMNETWORKMANAGER_H
