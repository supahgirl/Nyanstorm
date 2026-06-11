/**
 * @file fsdiscordgateway.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsdiscordgateway.h"

#include "llappviewer.h"
#include "llviewercontrol.h"
#include "llmd5.h"

// Debug: always write to stderr for Console.app visibility
#define GWLOG(fmt, ...) fprintf(stderr, "[DiscordGateway] " fmt "\n", ##__VA_ARGS__)

#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <curl/curl.h>
#include <boost/algorithm/string.hpp>

// ═════════════════════════════════════════════════════════════════════════════
//  Constants
// ═════════════════════════════════════════════════════════════════════════════

static constexpr const char* GATEWAY_URL     = "gateway.discord.gg";
static constexpr const char* GATEWAY_PATH    = "/?v=10&encoding=json";
static constexpr int         GATEWAY_PORT    = 443;

static constexpr int         HTTP_PORT       = 3002;
static constexpr const char* HTTP_HOST       = "127.0.0.1";

static constexpr int         HEARTBEAT_GUARD = 3;  // reconnect after N missed acks

// Discord Gateway opcodes
enum GatewayOp : int
{
    OP_DISPATCH        = 0,
    OP_HEARTBEAT       = 1,
    OP_IDENTIFY        = 2,
    OP_PRESENCE_UPDATE = 3,
    OP_VOICE_STATE     = 4,
    OP_RESUME          = 6,
    OP_RECONNECT       = 7,
    OP_REQUEST_MEMBERS = 8,
    OP_INVALID_SESSION = 9,
    OP_HELLO           = 10,
    OP_HEARTBEAT_ACK   = 11
};

// Discord intents
enum GatewayIntents : int
{
    INTENT_GUILDS              = 1 << 0,
    INTENT_GUILD_MEMBERS       = 1 << 1,
    INTENT_GUILD_MESSAGES      = 1 << 9,
    INTENT_GUILD_MESSAGE_TYPING= 1 << 10,
    INTENT_DIRECT_MESSAGES     = 1 << 12,
    INTENT_DIRECT_MESSAGE_TYPING = 1 << 13,
    INTENT_MESSAGE_CONTENT     = 1 << 15
};
static constexpr int REQUIRED_INTENTS =
    INTENT_GUILDS |
    INTENT_GUILD_MEMBERS |
    INTENT_GUILD_MESSAGES |
    INTENT_GUILD_MESSAGE_TYPING |
    INTENT_DIRECT_MESSAGES |
    INTENT_DIRECT_MESSAGE_TYPING |
    INTENT_MESSAGE_CONTENT;

// ═════════════════════════════════════════════════════════════════════════════
//  libcurl write callback
// ═════════════════════════════════════════════════════════════════════════════

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Discord UUID derivation — MUST match the old Python discord_relay.py
// ═════════════════════════════════════════════════════════════════════════════

/*static*/ LLUUID FSDiscordGateway::discordUUID(const std::string& discord_id)
{
    std::string key = "discord:" + discord_id;
    LLMD5 md5;
    md5.update((const U8*)key.c_str(), (U32)key.size());
    md5.finalize();
    LLUUID result;
    md5.raw_digest(result.mData);
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════

FSDiscordGateway::FSDiscordGateway()
{
}

FSDiscordGateway::~FSDiscordGateway()
{
    stop();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Public interface
// ═════════════════════════════════════════════════════════════════════════════

void FSDiscordGateway::start()
{
    if (mRunning.load()) return;
    mRunning.store(true);

    // Re-read token from settings each time start() is called
    mUserToken = gSavedSettings.getString("FSDiscordBotToken");
    if (mUserToken.empty())
    {
        LL_WARNS("DiscordGateway") << "No Discord token set (FSDiscordBotToken). "
                                   << "Use Ctrl+Shift+D and set FSDiscordBotToken, then restart." << LL_ENDL;
    }

    // Start the HTTP server thread first (so the existing C++ code can connect immediately)
    mHttpServerThread = std::make_unique<std::thread>(&FSDiscordGateway::httpServerThreadFunc, this);

    // Start the Gateway connection thread
    mGatewayThread = std::make_unique<std::thread>(&FSDiscordGateway::gatewayThreadFunc, this);

    GWLOG("FSDiscordGateway started");
    LL_INFOS("DiscordGateway") << "FSDiscordGateway started" << LL_ENDL;
}

void FSDiscordGateway::stop()
{
    mRunning.store(false);

    // Shutdown HTTP server socket to wake poll() safely, then close
    if (mHttpServerFd >= 0)
    {
        ::shutdown(mHttpServerFd, SHUT_RDWR);
        ::close(mHttpServerFd);
        mHttpServerFd = -1;
    }

    // Close the Gateway socket to interrupt blocking ws.read()
    if (mGatewaySocketFd.load() >= 0)
    {
        int fd = mGatewaySocketFd.exchange(-1);
        if (fd >= 0)
        {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
    }

    // Notify any SSE waiters
    mSSECV.notify_all();

    if (mGatewayThread && mGatewayThread->joinable())
        mGatewayThread->join();
    if (mHttpServerThread && mHttpServerThread->joinable())
        mHttpServerThread->join();

    mState.store(STATE_DISCONNECTED);
    LL_INFOS("DiscordGateway") << "FSDiscordGateway stopped" << LL_ENDL;
}

bool FSDiscordGateway::pollSSEEvent(boost::json::object& out)
{
    std::lock_guard<std::mutex> lk(mSSEMutex);
    if (mSSEQueue.empty()) return false;
    out = std::move(mSSEQueue.front().payload);
    mSSEQueue.pop();
    return true;
}

void FSDiscordGateway::sendDiscordMessage(const LLUUID& session_uuid, const std::string& text)
{
    // The session_uuid is the original Discord UUID (as an LLUUID).
    // Look up the channel_id directly from the UUID string, bypassing
    // any LLUUID formatting issues.
    std::string uuid_key = session_uuid.asString();

    std::string channel_id;
    {
        std::lock_guard<std::mutex> lk(mUserInfoMutex);
        auto it = mUserInfo.find(uuid_key);
        if (it != mUserInfo.end())
            channel_id = it->second.channel_id;
    }

    if (channel_id.empty())
    {
        GWLOG("sendDiscordMessage: no cached channel_id for [%s], creating DM channel...", uuid_key.c_str());
        // Try to create a DM channel via REST API
        // The uuid_key is discordUUID(discord_user_id), but we need the actual Discord user ID
        // Look it up from mContactCache
        std::string discord_user_id;
        {
            std::lock_guard<std::mutex> lk(mContactMutex);
            for (const auto& kv : mContactCache)
            {
                if (kv.second.uuid.asString() == uuid_key)
                {
                    discord_user_id = kv.first; // key is the discord_id
                    break;
                }
            }
        }
        if (discord_user_id.empty())
        {
            GWLOG("sendDiscordMessage: could not find Discord user ID for [%s]", uuid_key.c_str());
            return;
        }

        // Create DM channel: POST /users/@me/channels {recipient_id: "xxx"}
        boost::json::object dm_body;
        dm_body["recipient_id"] = discord_user_id;
        std::string dm_resp = restPost("/users/@me/channels", boost::json::serialize(dm_body));

        try
        {
            boost::json::value jv = boost::json::parse(dm_resp);
            if (jv.is_object() && jv.as_object().contains("id"))
            {
                channel_id = jv.as_object().at("id").as_string().c_str();
                // Cache it for future use
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                mUserInfo[uuid_key] = {discord_user_id, "", channel_id};
                GWLOG("sendDiscordMessage: created DM channel %s for user %s",
                      channel_id.c_str(), discord_user_id.c_str());
            }
        }
        catch (...) {}

        if (channel_id.empty())
        {
            GWLOG("sendDiscordMessage: failed to create DM channel");
            return;
        }
    }

    boost::json::object body;
    body["content"] = text;
    std::string json = boost::json::serialize(body);

    restPost("/channels/" + channel_id + "/messages", json);
    GWLOG("sendDiscordMessage: sent to channel %s", channel_id.c_str());
}

void FSDiscordGateway::sendDiscordTyping(const LLUUID& session_uuid, bool typing)
{
    if (!typing) return; // Discord's typing endpoint only starts, not stops

    // The session_uuid here is the original Discord UUID (relay_uuid from the
    // static function in fsfloaterim.cpp). Use the same lookup as sendDiscordMessage.
    std::string uuid_key = session_uuid.asString();

    std::string channel_id;
    {
        std::lock_guard<std::mutex> lk(mUserInfoMutex);
        auto it = mUserInfo.find(uuid_key);
        if (it != mUserInfo.end())
            channel_id = it->second.channel_id;
    }

    if (channel_id.empty())
    {
        // Try to create DM channel (same as sendDiscordMessage)
        std::string discord_user_id;
        {
            std::lock_guard<std::mutex> lk(mContactMutex);
            for (const auto& kv : mContactCache)
            {
                if (kv.second.uuid.asString() == uuid_key)
                {
                    discord_user_id = kv.first;
                    break;
                }
            }
        }
        if (discord_user_id.empty()) return;

        boost::json::object dm_body;
        dm_body["recipient_id"] = discord_user_id;
        std::string dm_resp = restPost("/users/@me/channels", boost::json::serialize(dm_body));
        try
        {
            boost::json::value jv = boost::json::parse(dm_resp);
            if (jv.is_object() && jv.as_object().contains("id"))
            {
                channel_id = jv.as_object().at("id").as_string().c_str();
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                mUserInfo[uuid_key] = {discord_user_id, "", channel_id};
            }
        }
        catch (...) { return; }
    }

    if (channel_id.empty()) return;

    restPost("/channels/" + channel_id + "/typing", "{}");
}

std::vector<DiscordContact> FSDiscordGateway::getContacts()
{
    std::lock_guard<std::mutex> lk(mContactMutex);
    std::vector<DiscordContact> result;
    for (const auto& [id, contact] : mContactCache)
        result.push_back(contact);
    return result;
}

void FSDiscordGateway::setMuted(const std::string& discord_id, bool muted)
{
    std::lock_guard<std::mutex> lk(mMuteMutex);
    if (muted)
        mMutedIds.insert(discord_id);
    else
        mMutedIds.erase(discord_id);
}

bool FSDiscordGateway::isMuted(const std::string& discord_id) const
{
    std::lock_guard<std::mutex> lk(mMuteMutex);
    return mMutedIds.count(discord_id) > 0;
}

void FSDiscordGateway::setChannelActive(const std::string& channel_id, bool active)
{
    std::lock_guard<std::mutex> lk(mActiveChannelsMutex);
    if (active)
        mActiveChannels.insert(channel_id);
    else
        mActiveChannels.erase(channel_id);
}

void FSDiscordGateway::setStatus(const std::string& status)
{
    sendPresenceUpdate(status);
}

void FSDiscordGateway::registerSessionUUID(const LLUUID& real_session_id, const LLUUID& original_uuid)
{
    std::lock_guard<std::mutex> lk(mSessionRegistryMutex);
    mSessionOriginalUUIDs[real_session_id] = original_uuid;
}

bool FSDiscordGateway::resolveSessionUUID(const LLUUID& real_session_id, LLUUID& out_original) const
{
    std::lock_guard<std::mutex> lk(mSessionRegistryMutex);
    auto it = mSessionOriginalUUIDs.find(real_session_id);
    if (it == mSessionOriginalUUIDs.end()) return false;
    out_original = it->second;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Gateway thread — WebSocket connection to wss://gateway.discord.gg
// ═════════════════════════════════════════════════════════════════════════════

void FSDiscordGateway::gatewayThreadFunc()
{
    GWLOG("Gateway thread started");
    LL_INFOS("DiscordGateway") << "Gateway thread started" << LL_ENDL;

    while (mRunning.load())
    {
        // Re-read token each attempt (user may have changed it via debug panel)
        mUserToken = gSavedSettings.getString("FSDiscordBotToken");

        mState.store(STATE_CONNECTING);
        if (mUserToken.empty())
        {
            LL_WARNS("DiscordGateway") << "No token set, will retry in 5s" << LL_ENDL;
            mState.store(STATE_FAILED);
            for (int i = 0; i < 50 && mRunning.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        try
        {
            if (!connectGateway())
            {
                mState.store(STATE_FAILED);
                for (int i = 0; i < 50 && mRunning.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        catch (const std::exception& e)
        {
            LL_WARNS("DiscordGateway") << "Gateway thread caught exception: " << e.what() << LL_ENDL;
            mState.store(STATE_FAILED);
            for (int i = 0; i < 50 && mRunning.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        mState.store(STATE_DISCONNECTED);
    }

    LL_INFOS("DiscordGateway") << "Gateway thread ended" << LL_ENDL;
}

bool FSDiscordGateway::connectGateway()
{
    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace websocket = beast::websocket;
    namespace net  = boost::asio;
    namespace ssl  = net::ssl;

    try
    {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        // Load CA certificates from the viewer's bundled bundle
        std::string ca_path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "ca-bundle.crt");
        if (!ca_path.empty() && LLFile::isfile(ca_path))
        {
            ctx.load_verify_file(ca_path);
            ctx.set_verify_mode(ssl::verify_peer);
        }
        else
        {
            // Fallback: verify none (connection is still encrypted)
            LL_WARNS("DiscordGateway") << "CA bundle not found at " << ca_path
                                       << " — SSL verification disabled" << LL_ENDL;
            ctx.set_verify_mode(ssl::verify_none);
        }

        // Resolve DNS
        net::ip::tcp::resolver resolver(ioc);
        auto const results = resolver.resolve(GATEWAY_URL, "443");

        // TCP stream + SSL
        beast::tcp_stream tcp_stream(ioc);
        tcp_stream.connect(results);

        // SSL handshake (by value — stream owns tcp_stream, avoids reference dangling)
        ssl::stream<beast::tcp_stream> ssl_stream(std::move(tcp_stream), ctx);
        ssl_stream.handshake(ssl::stream_base::client);

        // WebSocket upgrade (by value — owns the ssl stream)
        websocket::stream<ssl::stream<beast::tcp_stream>> ws(std::move(ssl_stream));
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req)
            {
                req.set(http::field::user_agent, "Firestorm/1.0 DiscordGateway");
            }));
        ws.handshake(GATEWAY_URL, GATEWAY_PATH);

        // Store the native socket so stop() can interrupt the blocking read
        mGatewaySocketFd.store(beast::get_lowest_layer(ws).socket().native_handle());

        mState.store(STATE_CONNECTED);

        // ── Read Hello (OP 10) ──────────────────────────────────────────────
        // Set a timeout on the Hello read to prevent deadlock on stop()
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        beast::error_code hello_ec;
        ws.read(buffer, hello_ec);
        if (hello_ec)
        {
            LL_WARNS("DiscordGateway") << "Failed to read Hello: " << hello_ec.message() << LL_ENDL;
            return false;
        }
        std::string hello_data = beast::buffers_to_string(buffer.data());
        {
            boost::json::value jv = boost::json::parse(hello_data);
            if (jv.is_object())
            {
                auto& obj = jv.as_object();
                if (obj.contains("op") && obj["op"].as_int64() == OP_HELLO && obj.contains("d"))
                {
                    handleHello(obj["d"].as_object());
                }
            }
        }

        // ── Send Identify (OP 2) — Resume (OP 6) not implemented yet ──────

        // Send identify via WebSocket
        {
            boost::json::object id_obj;
            id_obj["op"] = OP_IDENTIFY;
            boost::json::object d;
            d["token"] = mUserToken;
            boost::json::object props;
            props["os"]      = "Mac OS X";
            props["browser"] = "Discord Client";
            props["device"]  = "";
            d["properties"] = props;
            // capabilities bitfield is required for user account tokens
            // to receive PRESENCE_UPDATE events for friends
            d["capabilities"] = 16381;
            // client_state signals to Discord we're a full client
            boost::json::object client_state;
            client_state["guild_versions"] = boost::json::object{};
            client_state["highest_last_message_id"] = "0";
            client_state["read_state_version"] = 0;
            client_state["user_guild_settings_version"] = -1;
            client_state["user_settings_version"] = -1;
            d["client_state"] = client_state;
            d["compress"] = false;
            boost::json::object presence;
            presence["status"] = "online";
            presence["since"]  = 0;
            presence["afk"]    = false;
            d["presence"] = presence;
            id_obj["d"] = d;

            std::string msg = boost::json::serialize(id_obj);
            ws.write(net::buffer(msg));
            GWLOG("Sent Identify to Discord Gateway");
            LL_INFOS("DiscordGateway") << "Sent Identify" << LL_ENDL;
        }

        // ── Main read loop ──────────────────────────────────────────────────
        mLastHeartbeatAck = std::chrono::steady_clock::now();
        auto last_heartbeat_sent = mLastHeartbeatAck;

        while (mRunning.load())
        {
            // ── Check for pending presence update ───────────────────────────
            {
                std::lock_guard<std::mutex> lk(mPresenceMutex);
                if (!mPendingPresenceStatus.empty())
                {
                    boost::json::object payload;
                    payload["op"] = OP_PRESENCE_UPDATE;
                    boost::json::object d;
                    d["status"]     = mPendingPresenceStatus;
                    d["since"]      = 0;
                    d["afk"]        = false;
                    d["activities"] = boost::json::array();
                    payload["d"] = d;
                    std::string msg = boost::json::serialize(payload);
                    beast::error_code pe_ec;
                    ws.write(net::buffer(msg), pe_ec);
                    mPendingPresenceStatus.clear();
                }
            }

            // ── Non-blocking read with poll-based timeout ──────────────────
            // Use poll() to check for data before reading, to avoid closing
            // the socket on timeout (which expires_after() does).
            auto now = std::chrono::steady_clock::now();
            auto since_last_hb = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_heartbeat_sent).count();
            auto since_last_ack = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mLastHeartbeatAck).count();

            // Check for missed ACKs
            if (since_last_ack > mHeartbeatInterval.count() * HEARTBEAT_GUARD)
            {
                GWLOG("Missed %d heartbeat ACKs, reconnecting...", HEARTBEAT_GUARD);
                break;
            }

            // Send heartbeat if needed (BEFORE the read)
            if (since_last_hb >= mHeartbeatInterval.count())
            {
                boost::json::object hb;
                hb["op"] = OP_HEARTBEAT;
                hb["d"]  = mSession.sequence;
                std::string hb_msg = boost::json::serialize(hb);
                beast::error_code hb_ec;
                ws.write(net::buffer(hb_msg), hb_ec);
                if (hb_ec)
                {
                    GWLOG("Heartbeat send failed: %s — reconnecting", hb_ec.message().c_str());
                    break;
                }
                last_heartbeat_sent = std::chrono::steady_clock::now();
            }

            // Simple blocking read — Discord sends events or heartbeat ACKs
            // frequently enough that this won't block for long. If Discord
            // goes silent for >60s the TCP layer will time out, triggering
            // an error and reconnection.
            buffer.clear();
            beast::error_code ec;
            ws.read(buffer, ec);

            if (ec == beast::websocket::error::closed ||
                ec == net::error::eof ||
                ec == net::error::connection_reset)
            {
                GWLOG("WS closed: %s", ec.message().c_str());
                break;
            }
            if (ec)
            {
                GWLOG("WS read error: %s", ec.message().c_str());
                break;
            }
            std::string data = beast::buffers_to_string(buffer.data());
            if (handleGatewayMessage(data))
            {
                // Server requested reconnect (OP_RECONNECT / OP_INVALID_SESSION)
                GWLOG("Reconnect signal — reconnecting");
                LL_INFOS("DiscordGateway") << "Reconnect signal — reconnecting" << LL_ENDL;
                break;
            }
        } // end while(mRunning)

        // Try clean close (ignore error if already closed)
        beast::error_code close_ec;
        ws.close(websocket::close_code::normal, close_ec);
        mGatewaySocketFd.store(-1);
        return false; // will trigger reconnect
    }
    catch (const std::exception& e)
    {
        LL_WARNS("DiscordGateway") << "Gateway connection error: " << e.what() << LL_ENDL;
        mGatewaySocketFd.store(-1);
        return false;
    }
}

bool FSDiscordGateway::handleGatewayMessage(const std::string& data)
{
    try
    {
        boost::json::value jv = boost::json::parse(data);
        if (!jv.is_object()) return false;
        auto& obj = jv.as_object();

        if (!obj.contains("op")) return false;
        int op = (int)obj["op"].as_int64();
        int seq = obj.contains("s") && !obj["s"].is_null() ? (int)obj["s"].as_int64() : 0;
        if (op == OP_DISPATCH && seq > 0) mSession.sequence = seq;

        switch (op)
        {
        case OP_DISPATCH:
        {
            std::string t = obj.contains("t") && !obj["t"].is_null()
                          ? obj["t"].as_string().c_str() : "";
            if (obj.contains("d") && obj["d"].is_object())
                handleDispatch(obj["d"].as_object(), t, seq);
            break;
        }
        case OP_HEARTBEAT:
            // Server requested a heartbeat — our periodic heartbeat handles it
            break;
        case OP_RECONNECT:
            LL_INFOS("DiscordGateway") << "Server requested reconnect (OP 7)" << LL_ENDL;
            return true;
        case OP_INVALID_SESSION:
            LL_WARNS("DiscordGateway") << "Invalid session (OP 9)" << LL_ENDL;
            mSession.session_id.clear();
            return true;
        case OP_HEARTBEAT_ACK:
            mLastHeartbeatAck = std::chrono::steady_clock::now();
            break;
        case OP_HELLO:
            if (obj.contains("d"))
                handleHello(obj["d"].as_object());
            break;
        default:
            break;
        }
    }
    catch (const std::exception& e)
    {
        // Non-fatal errors (JSON parse failures, etc.)
        LL_WARNS("DiscordGateway") << "Non-fatal error handling message: " << e.what() << LL_ENDL;
    }
    return false;
}

void FSDiscordGateway::handleDispatch(const boost::json::object& d, const std::string& t, int seq)
{
    // Log key dispatch events only
    if (t == "MESSAGE_CREATE" || t == "TYPING_START" || t == "PRESENCE_UPDATE" ||
        t == "SESSIONS_REPLACE" || t == "PRESENCES_REPLACE" || t == "READY_SUPPLEMENTAL")
    {
        GWLOG("DISPATCH type=%s", t.c_str());
    }

    if (t == "READY")
    {
        if (d.contains("session_id"))
            mSession.session_id = d.at("session_id").as_string().c_str();
        if (d.contains("resume_url"))
            mSession.resume_url = d.at("resume_url").as_string().c_str();
        if (d.contains("user"))
        {
            auto& user = d.at("user").as_object();
            if (user.contains("id"))
                mSession.bot_user_id = user.at("id").as_string().c_str();
            if (user.contains("username"))
                mSession.bot_username = user.at("username").as_string().c_str();
        }

        // Populate guild + channel cache from READY
        if (d.contains("guilds"))
        {
            for (const auto& g : d.at("guilds").as_array())
            {
                if (!g.is_object()) continue;
                auto& guild = g.as_object();
                std::string guild_id   = guild.contains("id")   ? guild.at("id").as_string().c_str() : "";
                std::string guild_name = guild.contains("name") ? guild.at("name").as_string().c_str() : "";
                // Channels will be lazy-loaded via getContacts()
                (void)guild_id;
                (void)guild_name;
            }
        }

        // Push initial SSE status
        {
            boost::json::object sse;
            sse["status_update"] = "connected";
            sse["bot_username"]  = mSession.bot_username;
            std::lock_guard<std::mutex> lk(mSSEMutex);
            mSSEQueue.push({sse});
            mSSECV.notify_one();
        }

        GWLOG("READY received | user=%s session=%s",
              mSession.bot_username.c_str(), mSession.session_id.c_str());
        LL_INFOS("DiscordGateway") << "READY received | user=" << mSession.bot_username
                                   << " session=" << mSession.session_id << LL_ENDL;

        // Refresh contact cache in background
        refreshContactCache();
    }
    else if (t == "RESUMED")
    {
        LL_INFOS("DiscordGateway") << "Session resumed" << LL_ENDL;
        {
            boost::json::object sse;
            sse["status_update"] = "connected";
            std::lock_guard<std::mutex> lk(mSSEMutex);
            mSSEQueue.push({sse});
            mSSECV.notify_one();
        }
    }
    else if (t == "MESSAGE_CREATE")
    {
        if (!d.contains("author") || !d.contains("channel_id") || !d.contains("content"))
            return;

        auto& author    = d.at("author").as_object();
        std::string msg_author_id = author.contains("id") ? author.at("id").as_string().c_str() : "";

        // Skip own messages (echo from Discord after we sent them)
        if (msg_author_id == mSession.bot_user_id) return;
        std::string msg_author_name = author.contains("global_name") && !author.at("global_name").is_null()
                                    ? author.at("global_name").as_string().c_str()
                                    : (author.contains("username") ? author.at("username").as_string().c_str() : "Unknown");
        std::string channel_id = d.at("channel_id").as_string().c_str();
        std::string content    = d.at("content").as_string().c_str();

        // Handle attachments
        if (d.contains("attachments"))
        {
            for (const auto& att : d.at("attachments").as_array())
            {
                if (att.is_object() && att.as_object().contains("url"))
                {
                    content += std::string(" ") + att.as_object().at("url").as_string().c_str();
                }
            }
        }

        // Determine if DM or channel
        bool is_dm = (!d.contains("guild_id") || d.at("guild_id").is_null());

        if (is_dm)
        {
            // DM — channel_id IS the DM channel
            std::string session_uuid = discordUUID(msg_author_id).asString();
            std::string display_name = msg_author_name + " (discord)";

            // Store user info for reply routing
            {
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                // Store by Discord user ID (discordUUID) — used for reply routing
                mUserInfo[session_uuid] = {msg_author_id, display_name, channel_id};
                // Also store by the UUID that sDiscordOriginalUUIDs sends via POST
                // (the LLUUID string representation after LLUUID::set parsing)
                LLUUID key_uuid = discordUUID(msg_author_id);
                mUserInfo[key_uuid.asString()] = {msg_author_id, display_name, channel_id};
                GWLOG("DM mUserInfo stored: discord_id=%s uuid=%s channel=%s",
                      msg_author_id.c_str(), key_uuid.asString().c_str(), channel_id.c_str());
            }

            // Queue SSE event
            boost::json::object sse;
            sse["session_uuid"] = session_uuid;
            sse["display_name"] = display_name;
            boost::algorithm::trim(content);
            sse["text"]         = content;
            {
                std::lock_guard<std::mutex> lk(mSSEMutex);
                mSSEQueue.push({sse});
                mSSECV.notify_one();
            }
            LL_INFOS("DiscordGateway") << "DM from " << msg_author_name << LL_ENDL;
        }
        else
        {
            // Channel message — need to know guild name
            std::string guild_id = d.at("guild_id").as_string().c_str();
            std::string guild_name = "Discord";
            std::string channel_name = channel_id;

            // Look up guild/channel names from contact cache
            {
                std::lock_guard<std::mutex> lk(mContactMutex);
                auto it = mChannelIdToName.find(channel_id);
                if (it != mChannelIdToName.end())
                    channel_name = it->second;
                // Guild name lookup from channel prefix
                for (const auto& [id, contact] : mContactCache)
                {
                    if (contact.status == "channel" && !contact.server.empty())
                    {
                        // Find which guild this channel belongs to from the contacts
                        // Channel contacts have server=guild_name
                    }
                }
            }

            // Check if channel is active
            bool is_active = false;
            {
                std::lock_guard<std::mutex> lk(mActiveChannelsMutex);
                is_active = (mActiveChannels.count(channel_id) > 0);
            }
            if (!is_active) return;

            // Check if muted
            if (isMuted(msg_author_id)) return;

            std::string display_name = "#" + channel_name + " / " + guild_name + " (discord)";
            std::string session_uuid = discordUUID(channel_id).asString();

            // Store channel info
            {
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                mUserInfo[session_uuid] = {channel_id, display_name, channel_id};
            }

            // Queue SSE event
            boost::json::object sse;
            sse["session_uuid"]      = session_uuid;
            sse["display_name"]      = display_name;
            boost::algorithm::trim(content);
            sse["text"]              = content;
            sse["author_name"]       = msg_author_name;
            sse["author_discord_id"] = msg_author_id;
            {
                std::lock_guard<std::mutex> lk(mSSEMutex);
                mSSEQueue.push({sse});
                mSSECV.notify_one();
            }

            // Pre-register the author for DM routing
            std::string author_uuid = discordUUID(msg_author_id).asString();
            {
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                if (mUserInfo.find(author_uuid) == mUserInfo.end())
                    mUserInfo[author_uuid] = {msg_author_id, msg_author_name + " (discord)", "0"};
            }

            LL_INFOS("DiscordGateway") << "Channel msg in " << display_name
                                       << " from " << msg_author_name << LL_ENDL;
        }
    }
    else if (t == "TYPING_START")
    {
        if (!d.contains("channel_id") || !d.contains("user_id")) return;

        std::string typing_user_id = d.at("user_id").as_string().c_str();
        std::string channel_id     = d.at("channel_id").as_string().c_str();

        // Skip own typing
        if (typing_user_id == mSession.bot_user_id) return;

        // Check if this channel is active
        bool is_active = false;
        {
            std::lock_guard<std::mutex> lk(mActiveChannelsMutex);
            is_active = (mActiveChannels.count(channel_id) > 0);
        }

        bool is_dm = !d.contains("guild_id") || d.at("guild_id").is_null();

        if (!is_dm && !is_active) return;

        boost::json::object sse;
        sse["typing"] = true;

        if (is_dm)
        {
            std::string session_uuid = discordUUID(typing_user_id).asString();
            sse["session_uuid"] = session_uuid;
        }
        else
        {
            std::string session_uuid = discordUUID(channel_id).asString();
            sse["session_uuid"] = session_uuid;
            // Try to get the display name of the typer
            std::string author_name = typing_user_id;
            {
                std::lock_guard<std::mutex> lk(mUserInfoMutex);
                auto it = mUserInfo.find(discordUUID(typing_user_id).asString());
                if (it != mUserInfo.end())
                    author_name = it->second.display_name;
                // Strip " (discord)" for typing display
                static const std::string suffix = " (discord)";
                if (author_name.size() > suffix.size() &&
                    author_name.compare(author_name.size() - suffix.size(), suffix.size(), suffix) == 0)
                    author_name = author_name.substr(0, author_name.size() - suffix.size());
            }
            sse["author_name"] = author_name;
        }

        {
            std::lock_guard<std::mutex> lk(mSSEMutex);
            mSSEQueue.push({sse});
            mSSECV.notify_one();
        }

        // Note: typing stop after 10s is not implemented in C++ yet.
        // The existing viewer code handles typing timeouts in the IM floater.
        // The relay just needs to signal "typing started" — the viewer's
        // existing processIMTyping() handles the stop automatically.
    }
    else if (t == "PRESENCE_UPDATE")
    {
        if (!d.contains("user") || !d.at("user").is_object()) return;
        auto& user = d.at("user").as_object();
        if (!user.contains("id")) return;

        std::string user_id = user.at("id").as_string().c_str();
        std::string status  = d.contains("status") ? d.at("status").as_string().c_str() : "offline";

        cacheFriendPresence(user_id, status);
    }
    else if (t == "GUILD_CREATE")
    {
        // Populate channels from guild create
        if (d.contains("channels"))
        {
            std::string guild_name = d.contains("name") ? d.at("name").as_string().c_str() : "Discord";
            for (const auto& ch : d.at("channels").as_array())
            {
                if (!ch.is_object()) continue;
                auto& ch_obj = ch.as_object();
                if (!ch_obj.contains("id") || !ch_obj.contains("name")) continue;
                std::string cid   = ch_obj.at("id").as_string().c_str();
                std::string cname = ch_obj.at("name").as_string().c_str();

                cacheChannel(cid, cname, guild_name);
            }
        }
    }
    else if (t == "READY_SUPPLEMENTAL")
    {
        // User account event: contains friend presences in merged_presences.friends
        if (d.contains("merged_presences") && d.at("merged_presences").is_object())
        {
            auto& mp = d.at("merged_presences").as_object();
            if (mp.contains("friends") && mp.at("friends").is_array())
            {
                int count = 0;
                for (const auto& fp : mp.at("friends").as_array())
                {
                    if (!fp.is_object()) continue;
                    auto& f = fp.as_object();
                    std::string uid = f.contains("user_id") ? f.at("user_id").as_string().c_str() : "";
                    std::string st  = f.contains("status")  ? f.at("status").as_string().c_str()  : "offline";
                    if (!uid.empty())
                    {
                        cacheFriendPresence(uid, st);
                        count++;
                    }
                }
                GWLOG("READY_SUPPLEMENTAL: seeded %d friend presences", count);
            }
        }
    }
    else if (t == "SESSIONS_REPLACE")
    {
        // User account event: contains presence data for friends/sessions
        if (d.contains("sessions") && d.at("sessions").is_array())
        {
            for (const auto& s : d.at("sessions").as_array())
            {
                if (!s.is_object()) continue;
                auto& session = s.as_object();
                if (session.contains("user") && session.at("user").is_object())
                {
                    auto& user = session.at("user").as_object();
                    if (user.contains("id"))
                    {
                        std::string uid = user.at("id").as_string().c_str();
                        std::string st = session.contains("status")
                            ? session.at("status").as_string().c_str() : "offline";
                        cacheFriendPresence(uid, st);
                    }
                }
            }
            GWLOG("SESSIONS_REPLACE processed");
        }
    }
    else if (t == "PRESENCES_REPLACE")
    {
        // User account event — initial friend presence batch, stored as array in d.presences
        if (d.contains("presences") && d.at("presences").is_array())
        {
            for (const auto& p : d.at("presences").as_array())
            {
                if (!p.is_object()) continue;
                auto& presence = p.as_object();
                if (presence.contains("user") && presence.at("user").is_object())
                {
                    auto& user = presence.at("user").as_object();
                    if (user.contains("id"))
                    {
                        std::string uid = user.at("id").as_string().c_str();
                        std::string st = presence.contains("status")
                            ? presence.at("status").as_string().c_str() : "offline";
                        cacheFriendPresence(uid, st);
                    }
                }
            }
            GWLOG("PRESENCES_REPLACE processed");
        }
    }
    else if (t == "MESSAGE_DELETE" || t == "MESSAGE_UPDATE")
    {
        // Silently ignore — could be extended for edit/deletion support
    }
    else
    {
        // Unhandled dispatch event — debug log only
        LL_DEBUGS("DiscordGateway") << "Unhandled dispatch: " << t << LL_ENDL;
    }
}

void FSDiscordGateway::handleHello(const boost::json::object& d)
{
    if (d.contains("heartbeat_interval"))
    {
        mHeartbeatInterval = std::chrono::milliseconds(d.at("heartbeat_interval").as_int64());
        GWLOG("Heartbeat interval: %lldms", (long long)mHeartbeatInterval.count());
    LL_INFOS("DiscordGateway") << "Heartbeat interval: " << mHeartbeatInterval.count() << "ms" << LL_ENDL;
    }
}

void FSDiscordGateway::sendPresenceUpdate(const std::string& status)
{
    // Queue the presence update for the gateway thread to send over WebSocket
    {
        std::lock_guard<std::mutex> lk(mPresenceMutex);
        mPendingPresenceStatus = status;
    }
    LL_INFOS("DiscordGateway") << "Presence update queued: " << status << LL_ENDL;
}

void FSDiscordGateway::scheduleReconnect()
{
    // The gateway thread loop handles reconnection automatically
}

// ═════════════════════════════════════════════════════════════════════════════
//  REST API (libcurl)
// ═════════════════════════════════════════════════════════════════════════════

std::string FSDiscordGateway::restGet(const std::string& path)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "{}";

    std::string url = std::string(API_BASE) + path;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + mUserToken).c_str());
    // Note: no Content-Type for GET requests — Discord's API rejects GET with Content-Type
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Firestorm)");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        GWLOG("REST GET ERROR: %s | %s", curl_easy_strerror(res), url.c_str());
        LL_WARNS("DiscordGateway") << "REST GET error: " << curl_easy_strerror(res)
                                   << " (" << url << ")" << LL_ENDL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::string FSDiscordGateway::restPost(const std::string& path, const std::string& body)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "{}";

    std::string url = std::string(API_BASE) + path;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + mUserToken).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Firestorm)");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        GWLOG("REST POST ERROR: %s | %s", curl_easy_strerror(res), url.c_str());
        LL_WARNS("DiscordGateway") << "REST POST error: " << curl_easy_strerror(res)
                                   << " (" << url << ")" << LL_ENDL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Contact cache
// ═════════════════════════════════════════════════════════════════════════════

void FSDiscordGateway::refreshContactCache()
{
    GWLOG("refreshContactCache: fetching friends and guilds...");

    // Fetch friends list
    std::string friends_resp = restGet("/users/@me/relationships");
    GWLOG("refreshContactCache: friends response size=%zu", friends_resp.size());
    // The response is an array of relationship objects
    // Each has: { "id": "user_id", "type": 1 (friend), "user": {...} }

    // Fetch guilds and their channels
    std::string guilds_resp = restGet("/users/@me/guilds");

    // Parse the responses and populate mContactCache
    GWLOG("refreshContactCache: friends raw=[%s]", friends_resp.c_str());
    GWLOG("refreshContactCache: guilds raw=[%s]", guilds_resp.c_str());
    try
    {
        boost::json::value jv = boost::json::parse(friends_resp);
        if (jv.is_array())
        {
            int count = 0;
            for (const auto& rel : jv.as_array())
            {
                if (!rel.is_object()) continue;
                auto& obj = rel.as_object();
                if (!obj.contains("user") || !obj.at("user").is_object()) continue;
                auto& user = obj.at("user").as_object();
                if (!user.contains("id")) continue;
                count++;

                std::string uid   = user.at("id").as_string().c_str();
                std::string uname = user.contains("global_name") && !user.at("global_name").is_null()
                                  ? user.at("global_name").as_string().c_str()
                                  : (user.contains("username") ? user.at("username").as_string().c_str() : "Unknown");

                DiscordContact c;
                c.discord_id   = uid;
                c.display_name = uname;
                c.status       = "offline"; // will be updated on PRESENCE_UPDATE
                c.uuid         = discordUUID(uid);

                std::lock_guard<std::mutex> lk(mContactMutex);
                mContactCache[uid] = c;
            }
            GWLOG("refreshContactCache: found %d friends", count);
        }
        else
        {
            GWLOG("refreshContactCache: friends response is not an array (type=%d)", jv.kind());
        }
    }
    catch (const std::exception& e)
    {
        GWLOG("refreshContactCache: ERROR parsing friends: %s", e.what());
        LL_WARNS("DiscordGateway") << "Error parsing friends: " << e.what() << LL_ENDL;
    }

    GWLOG("refreshContactCache: fetching guilds...");
    try
    {
        // For each guild, fetch channels
        boost::json::value jv = boost::json::parse(guilds_resp);
        if (jv.is_array())
        {
            for (const auto& guild_val : jv.as_array())
            {
                if (!guild_val.is_object()) continue;
                auto& guild = guild_val.as_object();
                if (!guild.contains("id")) continue;
                std::string guild_id   = guild.at("id").as_string().c_str();
                std::string guild_name = guild.contains("name") ? guild.at("name").as_string().c_str() : "Discord";

                // Fetch channels for this guild
                std::string channels_resp = restGet("/guilds/" + guild_id + "/channels");
                try
                {
                    boost::json::value chans = boost::json::parse(channels_resp);
                    if (chans.is_array())
                    {
                        for (const auto& ch : chans.as_array())
                        {
                            if (!ch.is_object()) continue;
                            auto& ch_obj = ch.as_object();
                            if (!ch_obj.contains("id") || !ch_obj.contains("name") || !ch_obj.contains("type"))
                                continue;

                            int ch_type = (int)ch_obj.at("type").as_int64();
                            // Only text (0), news (5), forum (15) channels
                            if (ch_type != 0 && ch_type != 5 && ch_type != 15)
                                continue;

                            std::string cid   = ch_obj.at("id").as_string().c_str();
                            std::string cname = ch_obj.at("name").as_string().c_str();

                            cacheChannel(cid, cname, guild_name);
                        }
                    }
                }
                catch (...) {}
            }
        }
    }
    catch (const std::exception& e)
    {
        GWLOG("refreshContactCache: ERROR parsing guilds: %s", e.what());
        LL_WARNS("DiscordGateway") << "Error parsing guilds: " << e.what() << LL_ENDL;
    }
    GWLOG("refreshContactCache: done — %zu contacts cached", mContactCache.size());
}

void FSDiscordGateway::cacheFriendPresence(const std::string& discord_id, const std::string& status)
{
    std::lock_guard<std::mutex> lk(mContactMutex);
    auto it = mContactCache.find(discord_id);
    if (it != mContactCache.end())
        it->second.status = status;
}

void FSDiscordGateway::cacheChannel(const std::string& channel_id, const std::string& name,
                                    const std::string& guild_name)
{
    {
        std::lock_guard<std::mutex> lk(mContactMutex);
        mChannelIdToName[channel_id] = name;
    }

    DiscordContact c;
    c.discord_id   = channel_id;
    c.display_name = "#" + name;
    c.status       = "channel";
    c.server       = guild_name;
    c.uuid         = discordUUID(channel_id);

    std::lock_guard<std::mutex> lk(mContactMutex);
    // Only insert if not already present
    if (mContactCache.find(channel_id) == mContactCache.end())
        mContactCache[channel_id] = c;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Internal HTTP server (127.0.0.1:3002)
//  Replaces starlette + uvicorn
// ═════════════════════════════════════════════════════════════════════════════

void FSDiscordGateway::httpServerThreadFunc()
{
    GWLOG("HTTP server thread started");
    LL_INFOS("DiscordGateway") << "HTTP server thread started" << LL_ENDL;

    try
    {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        LL_WARNS("DiscordGateway") << "Failed to create HTTP server socket" << LL_ENDL;
        return;
    }

    // Allow immediate reuse of the address
    int reuse = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(HTTP_PORT);
    ::inet_pton(AF_INET, HTTP_HOST, &addr.sin_addr);

    if (::bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LL_WARNS("DiscordGateway") << "Failed to bind HTTP server to port " << HTTP_PORT << LL_ENDL;
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 10) < 0)
    {
        LL_WARNS("DiscordGateway") << "Failed to listen on HTTP server socket" << LL_ENDL;
        ::close(server_fd);
        return;
    }

    mHttpServerFd = server_fd;
    GWLOG("Internal HTTP server listening on %s:%d", HTTP_HOST, HTTP_PORT);
    LL_INFOS("DiscordGateway") << "Internal HTTP server listening on "
                               << HTTP_HOST << ":" << HTTP_PORT << LL_ENDL;

    while (mRunning.load())
    {
        struct pollfd pfd;
        pfd.fd     = server_fd;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 1000); // 1s timeout to check mRunning
        if (ret < 0) break;
        if (ret == 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        // Set a 5-second receive timeout
        struct timeval tv;
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Read the full HTTP request (loop to reassemble fragmented TCP)
        std::string request;
        char buf[4096];
        while (true)
        {
            ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            request.append(buf, n);

            // Check if we have complete headers
            std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos)
            {
                // Parse Content-Length to see if body is complete
                std::size_t cl_pos = request.find("Content-Length: ");
                if (cl_pos != std::string::npos)
                {
                    int content_length = std::atoi(request.c_str() + cl_pos + 16);
                    std::size_t body_start = header_end + 4;
                    if (request.size() >= body_start + (std::size_t)content_length)
                        break; // full request received
                    // Otherwise, continue reading
                }
                else
                {
                    break; // No Content-Length = GET request, headers are enough
                }
            }
        }

        if (request.empty()) { ::close(client_fd); continue; }

        handleHttpRequest(client_fd, request);
    }

    if (server_fd >= 0)
    {
        ::close(server_fd);
        mHttpServerFd = -1;
    }

    LL_INFOS("DiscordGateway") << "Internal HTTP server stopped" << LL_ENDL;
    }
    catch (const std::exception& e)
    {
        LL_WARNS("DiscordGateway") << "HTTP server thread error: " << e.what() << LL_ENDL;
    }
}

void FSDiscordGateway::handleHttpRequest(int client_fd, const std::string& request)
{
    // Parse the first line: METHOD PATH HTTP/1.1
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    if (method == "GET" && path == "/events")
    {
        // SSE is persistent — spawn a thread so the HTTP server can
        // handle other requests (contacts, send, typing, etc.)
        int sse_fd = client_fd;
        std::thread([this, sse_fd]()
        {
            try
            {
                GWLOG("SSE handler thread started for fd=%d", sse_fd);
                serveSSE(sse_fd);
                GWLOG("SSE handler thread ended for fd=%d", sse_fd);
            }
            catch (const std::exception& e)
            {
                GWLOG("SSE handler thread EXCEPTION: %s", e.what());
            }
            catch (...)
            {
                GWLOG("SSE handler thread UNKNOWN EXCEPTION");
            }
        }).detach();
        return;
    }

    if (method == "GET" && path == "/contacts")
    {
        auto contacts = getContacts();
        boost::json::array arr;
        for (const auto& c : contacts)
        {
            boost::json::object jc;
            jc["discord_id"]   = c.discord_id;
            jc["display_name"] = c.display_name;
            jc["status"]       = c.status;
            jc["server"]       = c.server;
            jc["session_uuid"] = c.uuid.asString();
            arr.push_back(jc);
        }
        std::string body = boost::json::serialize(arr);
        GWLOG("GET /contacts returning %zu contacts, body size=%zu",
              contacts.size(), body.size());
        sendHttpResponse(client_fd, 200, "application/json", body);
        return;
    }

    if (method == "POST" && path == "/send")
    {
        // Find body after headers
        std::size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos)
        {
            std::string body = request.substr(body_pos + 4);
            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (jv.is_object())
                {
                    auto& obj = jv.as_object();
                    std::string uuid_str = obj.contains("session_uuid") ? std::string(obj["session_uuid"].as_string()) : "";
                    std::string text     = obj.contains("text")         ? std::string(obj["text"].as_string())         : "";
                    if (!uuid_str.empty() && !text.empty())
                    {
                        LLUUID session_uuid;
                        session_uuid.set(uuid_str);
                        sendDiscordMessage(session_uuid, text);
                    }
                }
            }
            catch (...) {}
        }
        sendHttpResponse(client_fd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (method == "POST" && path == "/typing")
    {
        std::size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos)
        {
            std::string body = request.substr(body_pos + 4);
            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (jv.is_object())
                {
                    auto& obj = jv.as_object();
                    std::string uuid_str = obj.contains("session_uuid") ? obj["session_uuid"].as_string().c_str() : "";
                    bool typing = obj.contains("typing") ? obj["typing"].as_bool() : true;
                    if (!uuid_str.empty())
                    {
                        LLUUID session_uuid;
                        session_uuid.set(uuid_str);
                        sendDiscordTyping(session_uuid, typing);
                    }
                }
            }
            catch (...) {}
        }
        sendHttpResponse(client_fd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (method == "POST" && path == "/mute")
    {
        std::size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos)
        {
            std::string body = request.substr(body_pos + 4);
            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (jv.is_object())
                {
                    auto& obj = jv.as_object();
                    std::string did = obj.contains("discord_id") ? obj["discord_id"].as_string().c_str() : "";
                    bool muted = obj.contains("muted") ? obj["muted"].as_bool() : true;
                    if (!did.empty()) setMuted(did, muted);
                }
            }
            catch (...) {}
        }
        sendHttpResponse(client_fd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (method == "POST" && path == "/channel_active")
    {
        std::size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos)
        {
            std::string body = request.substr(body_pos + 4);
            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (jv.is_object())
                {
                    auto& obj = jv.as_object();
                    std::string cid    = obj.contains("channel_id") ? obj["channel_id"].as_string().c_str() : "";
                    bool active = obj.contains("active") ? obj["active"].as_bool() : true;
                    if (!cid.empty()) setChannelActive(cid, active);
                }
            }
            catch (...) {}
        }
        sendHttpResponse(client_fd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (method == "POST" && path == "/discord_status")
    {
        std::size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos)
        {
            std::string body = request.substr(body_pos + 4);
            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (jv.is_object())
                {
                    auto& obj = jv.as_object();
                    std::string status = obj.contains("status") ? obj["status"].as_string().c_str() : "online";
                    setStatus(status);
                }
            }
            catch (...) {}
        }
        sendHttpResponse(client_fd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (method == "GET" && path == "/status")
    {
        boost::json::object status;
        status["state"] = (int)mState.load();
        status["connected"] = (mState.load() == STATE_CONNECTED);
        sendHttpResponse(client_fd, 200, "application/json", boost::json::serialize(status));
        return;
    }

    // Fallback: 404
    sendHttpResponse(client_fd, 404, "text/plain", "Not Found");
}

void FSDiscordGateway::serveSSE(int client_fd)
{
    // Send SSE headers
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (::send(client_fd, header.c_str(), header.size(), 0) < 0)
    {
        ::close(client_fd);
        return;
    }

    LL_INFOS("DiscordGateway") << "SSE client connected" << LL_ENDL;

    // Set send timeout
    struct timeval tv;
    tv.tv_sec  = 30;
    tv.tv_usec = 0;
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    while (true)
    {
        bool running = mRunning.load();

        boost::json::object event;
        bool have_event = false;

        {
            std::unique_lock<std::mutex> lk(mSSEMutex);
            if (!mSSEQueue.empty())
            {
                event = mSSEQueue.front().payload;
                mSSEQueue.pop();
                have_event = true;
            }
            else if (running)
            {
                // Wait with timeout for keepalive
                if (mSSECV.wait_for(lk, std::chrono::seconds(25)) == std::cv_status::timeout)
                {
                    const char* ka = ": keepalive\n\n";
                    if (::send(client_fd, ka, strlen(ka), 0) < 0)
                        break;
                    continue;
                }
                // Woke up — check for events or shutdown
                if (!mSSEQueue.empty())
                {
                    event = mSSEQueue.front().payload;
                    mSSEQueue.pop();
                    have_event = true;
                }
                else if (!mRunning.load())
                    break; // shutdown
                else
                    continue; // spurious wakeup
            }
            else
            {
                break; // gateway stopped
            }
        }

        if (have_event)
        {
            std::string data = "data: " + boost::json::serialize(event) + "\n\n";
            if (::send(client_fd, data.c_str(), data.size(), 0) < 0)
                break;
        }
    }

    ::close(client_fd);
    LL_INFOS("DiscordGateway") << "SSE client disconnected" << LL_ENDL;
}

void FSDiscordGateway::sendHttpResponse(int client_fd, int status,
                                        const std::string& content_type,
                                        const std::string& body, bool keep_alive)
{
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " "
         << (status == 200 ? "OK" : status == 404 ? "Not Found" : "Error")
         << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "\r\n"
         << body;

    std::string resp_str = resp.str();
    ::send(client_fd, resp_str.c_str(), resp_str.size(), 0);
    if (!keep_alive)
        ::close(client_fd);
}
