#include "MasterServerClient.hpp"

#include <cstdio>
#include <components/debug/debuglog.hpp>

// cpp-httplib vendored single-header.
// Plain HTTP (no TLS) — BCrypt crypto backend in use on Windows, OpenSSL not available.
// Master server operators should run behind a TLS-terminating proxy if HTTPS is needed.
#include "httplib.h"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace mwmp
{

// ============================================================================
//  Internal helpers
// ============================================================================

namespace
{
    /// Escape a string for embedding as a JSON value.
    /// Handles the common cases; full RFC 8259 escaping for unusual chars.
    std::string jsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x",
                                 static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += buf;
                    }
                    else
                        out += c;
            }
        }
        return out;
    }

    /// Build the JSON body for POST /register.
    std::string buildRegisterBody(const MasterServerClient::Config& cfg)
    {
        std::ostringstream ss;
        ss << "{"
           << "\"name\":\""             << jsonEscape(cfg.serverName) << "\","
           << "\"port\":"               << cfg.port                   << ","
           << "\"max_players\":"        << cfg.maxPlayers             << ","
           << "\"version\":\""          << jsonEscape(cfg.version)    << "\","
           << "\"game_mode\":\""        << jsonEscape(cfg.gameMode)   << "\","
           << "\"password_protected\":" << (cfg.passwordProtected ? "true" : "false")
           << "}";
        return ss.str();
    }

    /// Build the JSON body for POST /heartbeat.
    std::string buildHeartbeatBody(const std::string& token, int players)
    {
        std::ostringstream ss;
        ss << "{"
           << "\"token\":\""          << jsonEscape(token) << "\","
           << "\"current_players\":"  << players
           << "}";
        return ss.str();
    }

    /// Build the JSON body for POST /unregister.
    std::string buildUnregisterBody(const std::string& token)
    {
        return "{\"token\":\"" + jsonEscape(token) + "\"}";
    }

    /// Extract a quoted string value for the given key from a flat JSON object.
    /// e.g. jsonStringValue(body, "token") on {"token":"abc123"} returns "abc123".
    std::string jsonStringValue(const std::string& body, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        auto pos = body.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) ++pos;
        if (pos >= body.size() || body[pos] != '"') return {};
        ++pos;
        std::string val;
        while (pos < body.size() && body[pos] != '"') val += body[pos++];
        return val;
    }

} // anonymous namespace

// ============================================================================
//  Constructor / Destructor
// ============================================================================

MasterServerClient::MasterServerClient() = default;

MasterServerClient::~MasterServerClient()
{
    // Worker thread must not outlive this object.
    if (mWorker.joinable())
        mWorker.join();
}

// ============================================================================
//  Public API
// ============================================================================

void MasterServerClient::registerAsync(const Config& config)
{
    if (config.masterUrl.empty())
        return;

    // Wait for any in-flight operation before starting a new one.
    if (mWorker.joinable())
        mWorker.join();

    {
        std::lock_guard<std::mutex> lk(mMutex);
        mLastConfig = config;
        mMasterUrl  = config.masterUrl;
        mRegistered = false;
        mToken.clear();
    }

    mWorker = std::thread([this, config]() { doRegister(config); });
}

void MasterServerClient::tickHeartbeat(float dt, int currentPlayers)
{
    {
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mRegistered || mToken.empty())
            return;
    }

    mHeartbeatAccum += dt;
    if (mHeartbeatAccum < HEARTBEAT_INTERVAL_S)
        return;

    mHeartbeatAccum = 0.f;

    // Fire and forget — copy token under lock, then send on a detached thread.
    std::string token;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        token = mToken;
    }

    std::thread([this, token, currentPlayers]()
    {
        doHeartbeat(currentPlayers);
    }).detach();
}

void MasterServerClient::unregister()
{
    // Drain any in-flight register thread first.
    if (mWorker.joinable())
        mWorker.join();

    doUnregister();
}

// ============================================================================
//  Private: HTTP operations (run on background thread)
// ============================================================================

void MasterServerClient::doRegister(const Config& config)
{
    try
    {
        httplib::Client cli(config.masterUrl);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);

        const std::string body = buildRegisterBody(config);
        auto res = cli.Post("/register", body, "application/json");

        if (!res)
        {
            Log(Debug::Warning) << "[MasterServer] POST /register failed: no response";
            return;
        }
        if (res->status != 200 && res->status != 201)
        {
            Log(Debug::Warning) << "[MasterServer] POST /register HTTP "
                                 << res->status << ": " << res->body;
            return;
        }

        const std::string token = jsonStringValue(res->body, "token");
        if (token.empty())
        {
            Log(Debug::Warning) << "[MasterServer] /register: no token in response: "
                                 << res->body;
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mMutex);
            mToken      = token;
            mRegistered = true;
        }

        Log(Debug::Info) << "[MasterServer] registered as \"" << config.serverName
                          << "\" on " << config.masterUrl;
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MasterServer] doRegister exception: " << e.what();
    }
}

void MasterServerClient::doHeartbeat(int currentPlayers)
{
    std::string token;
    std::string url;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        token = mToken;
        url   = mMasterUrl;
    }
    if (token.empty() || url.empty()) return;

    try
    {
        httplib::Client cli(url);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(8);

        const std::string body = buildHeartbeatBody(token, currentPlayers);
        auto res = cli.Post("/heartbeat", body, "application/json");

        if (!res)
        {
            Log(Debug::Warning) << "[MasterServer] POST /heartbeat: no response";
            return;
        }

        // 401 / unknown token → master server lost our registration
        // (e.g. it restarted). Re-register.
        if (res->status == 401 ||
            res->body.find("unknown token") != std::string::npos)
        {
            Log(Debug::Info) << "[MasterServer] token expired, re-registering...";
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mToken.clear();
                mRegistered = false;
            }
            // Re-register synchronously on this heartbeat thread.
            Config cfg;
            {
                std::lock_guard<std::mutex> lk(mMutex);
                cfg = mLastConfig;
            }
            doRegister(cfg);
            return;
        }

        if (res->status != 200)
            Log(Debug::Warning) << "[MasterServer] /heartbeat HTTP "
                                  << res->status << ": " << res->body;
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MasterServer] doHeartbeat exception: " << e.what();
    }
}

void MasterServerClient::doUnregister()
{
    std::string token;
    std::string url;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        token = mToken;
        url   = mMasterUrl;
    }
    if (token.empty() || url.empty()) return;

    try
    {
        httplib::Client cli(url);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);

        const std::string body = buildUnregisterBody(token);
        auto res = cli.Post("/unregister", body, "application/json");

        if (!res || res->status != 200)
            Log(Debug::Warning) << "[MasterServer] /unregister HTTP "
                                  << (res ? res->status : -1);
        else
            Log(Debug::Info) << "[MasterServer] unregistered cleanly";
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MasterServer] doUnregister exception: " << e.what();
    }

    std::lock_guard<std::mutex> lk(mMutex);
    mToken.clear();
    mRegistered = false;
}

} // namespace mwmp
