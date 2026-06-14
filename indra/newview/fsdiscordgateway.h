/**
 * @file fsdiscordgateway.h
 * @brief Direct C++ Discord Gateway client — replaces discord_relay.py
 * @author supah.girl
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

#ifndef FS_FSDISCORDGATEWAY_H
#define FS_FSDISCORDGATEWAY_H

#include "llsingleton.h"
#include "lluuid.h"
#include "fsfloaterimcontainer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <boost/json.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

// ── SSE event queued for the internal HTTP server ───────────────────────────

// Note: struct DiscordContact is defined in fsfloaterimcontainer.h

struct SSEDiscordEvent
{
    boost::json::object payload;
};

// ── FSDiscordGateway ────────────────────────────────────────────────────────

class FSDiscordGateway : public LLSingleton<FSDiscordGateway>
{
    LLSINGLETON(FSDiscordGateway);
    LOG_CLASS(FSDiscordGateway);

public:
    enum EState
    {
        STATE_DISCONNECTED = 0,
        STATE_CONNECTING,
        STATE_CONNECTED,
        STATE_RECONNECTING,
        STATE_FAILED
    };

    ~FSDiscordGateway();

    /// Start the gateway: begins WebSocket connection to Discord
    /// and the internal HTTP server on 127.0.0.1:3002
    void start();

    /// Graceful stop
    void stop();

    /// Current connection state
    EState getState() const { return mState.load(); }

    /// Expose SSE event queue for the HTTP server (thread-safe)
    /// Returns nullptr if no event available (non-blocking)
    bool pollSSEEvent(boost::json::object& out);

    // ── Called by the existing C++ IM code (replaces TCP socket calls) ──────

    /// Send a message to a Discord channel/DM (replaces POST /send to relay)
    void sendDiscordMessage(const LLUUID& session_uuid, const std::string& text);

    /// Send typing indicator (replaces POST /typing to relay)
    void sendDiscordTyping(const LLUUID& session_uuid, bool typing);

    /// Get the list of contacts (replaces GET /contacts to relay)
    std::vector<DiscordContact> getContacts();

    /// Mute/unmute a discord user or channel (replaces POST /mute)
    void setMuted(const std::string& discord_id, bool muted);

    /// Check if a discord_id is muted
    bool isMuted(const std::string& discord_id) const;

    /// Activate/deactivate a channel for receiving messages (replaces POST /channel_active)
    void setChannelActive(const std::string& channel_id, bool active);

    /// Set Discord presence status (replaces POST /discord_status)
    void setStatus(const std::string& status);

    /// Register a resolved session UUID → original Discord UUID
    /// (mirrors sDiscordOriginalUUIDs from the existing code)
    void registerSessionUUID(const LLUUID& real_session_id, const LLUUID& original_uuid);
    bool resolveSessionUUID(const LLUUID& real_session_id, LLUUID& out_original) const;

private:
    // ── Internal types ──────────────────────────────────────────────────

    struct GatewaySessionInfo
    {
        std::string session_id;     // from READY
        std::string resume_url;     // from READY
        int         sequence = 0;   // last seq number for resume
        std::string bot_user_id;
        std::string bot_username;
    };

    struct DiscordUserInfo
    {
        std::string discord_id;
        std::string display_name;
        std::string channel_id;     // 0 for users needing DM channel resolution
    };

    // ── Thread entry points ─────────────────────────────────────────────

    void gatewayThreadFunc();
    void httpServerThreadFunc();

    // ── Gateway protocol ────────────────────────────────────────────────

    bool connectGateway();
    void sendPresenceUpdate(const std::string& status);
    bool handleGatewayMessage(const std::string& data);  // returns true if reconnect needed
    void handleDispatch(const boost::json::object& d, const std::string& t, int seq);
    void handleHello(const boost::json::object& d);
    void scheduleReconnect();

    // ── Discord REST API (libcurl) ──────────────────────────────────────

    std::string restGet(const std::string& path);
    std::string restPost(const std::string& path, const std::string& body);

    // ── Contact cache management ────────────────────────────────────────

    void refreshContactCache();
    void cacheFriendPresence(const std::string& discord_id, const std::string& status);
    void cacheChannel(const std::string& channel_id, const std::string& name,
                      const std::string& guild_name);

    // ── Internal HTTP server ────────────────────────────────────────────

    void handleHttpRequest(boost::asio::ip::tcp::socket& client_sock, const std::string& request);
    void serveSSE(boost::asio::ip::tcp::socket client_sock);
    void sendHttpResponse(boost::asio::ip::tcp::socket& client_sock, int status,
                          const std::string& content_type,
                          const std::string& body, bool keep_alive = false);

    // ── Shared helpers ──────────────────────────────────────────────────

    static LLUUID discordUUID(const std::string& discord_id);

    // ── State ───────────────────────────────────────────────────────────

    std::atomic<EState> mState{STATE_DISCONNECTED};
    std::atomic<bool>   mRunning{false};

    // Gateway connection
    std::string mUserToken;
    std::unique_ptr<std::thread> mGatewayThread;
    GatewaySessionInfo mSession;

    // Used to shutdown the blocking ws.read() on viewer quit
    // Stores the native socket handle (SOCKET on Windows, int on POSIX)
    std::atomic<boost::asio::ip::tcp::socket::native_handle_type> mGatewaySocketFd{
        boost::asio::ip::tcp::socket::native_handle_type(-1)};

    // Heartbeat
    std::chrono::milliseconds mHeartbeatInterval{41250};
    std::chrono::steady_clock::time_point mLastHeartbeatAck;
    std::chrono::steady_clock::time_point mLastHeartbeatSent;

    // Presence update queue (gateway thread consumes this)
    std::mutex mPresenceMutex;
    std::string mPendingPresenceStatus; // non-empty = update pending

    // REST API base
    static constexpr const char* API_BASE = "https://discord.com/api/v10";

    // Contact cache
    mutable std::mutex mContactMutex;
    std::map<std::string, DiscordContact> mContactCache; // discord_id → info
    std::map<std::string, std::string>    mChannelIdToName; // channel_id → display_name

    // Session registry (mirrors sDiscordOriginalUUIDs)
    mutable std::mutex mSessionRegistryMutex;
    std::map<LLUUID, LLUUID> mSessionOriginalUUIDs;  // real_session_id → original discord UUID

    // Muted set
    mutable std::mutex mMuteMutex;
    std::set<std::string> mMutedIds;

    // Active channels set
    mutable std::mutex mActiveChannelsMutex;
    std::set<std::string> mActiveChannels;

    // User info cache (session_uuid → {discord_id, display_name, channel_id})
    mutable std::mutex mUserInfoMutex;
    std::map<std::string, DiscordUserInfo> mUserInfo;

    // SSE event queue
    mutable std::mutex mSSEMutex;
    std::queue<SSEDiscordEvent> mSSEQueue;
    std::condition_variable mSSECV;

    // HTTP server
    std::unique_ptr<std::thread>              mHttpServerThread;
    std::unique_ptr<boost::asio::io_context>  mHttpIoc;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> mHttpAcceptor;
};

#endif // FS_FSDISCORDGATEWAY_H
