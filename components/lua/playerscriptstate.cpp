#include "playerscriptstate.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <components/vfs/pathutil.hpp>

#include "configuration.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace LuaUtil
{
    namespace
    {
        constexpr std::string_view sMagic = "OMWMPPS1";
        constexpr std::uint32_t sMaximumScripts = 4096;
        constexpr std::uint32_t sMaximumTimersPerScript = 65536;
        constexpr std::uint32_t sMaximumStringSize = 8 * 1024 * 1024;
        constexpr std::uint64_t sFnvOffset = 14695981039346656037ull;
        constexpr std::uint64_t sFnvPrime = 1099511628211ull;

        std::uint64_t fnv1a(std::span<const char> data, std::uint64_t hash = sFnvOffset)
        {
            for (const unsigned char value : data)
            {
                hash ^= value;
                hash *= sFnvPrime;
            }
            return hash;
        }

        std::uint64_t fnv1a(std::string_view data, std::uint64_t hash = sFnvOffset)
        {
            return fnv1a(std::span<const char>(data.data(), data.size()), hash);
        }

        void appendU8(std::string& out, std::uint8_t value)
        {
            out.push_back(static_cast<char>(value));
        }

        void appendU32(std::string& out, std::uint32_t value)
        {
            for (unsigned i = 0; i < 4; ++i)
                appendU8(out, static_cast<std::uint8_t>(value >> (i * 8)));
        }

        void appendU64(std::string& out, std::uint64_t value)
        {
            for (unsigned i = 0; i < 8; ++i)
                appendU8(out, static_cast<std::uint8_t>(value >> (i * 8)));
        }

        void appendString(std::string& out, std::string_view value)
        {
            if (value.size() > sMaximumStringSize)
                throw std::runtime_error("Player Lua script state value exceeds the size limit");
            appendU32(out, static_cast<std::uint32_t>(value.size()));
            out.append(value);
        }

        class Reader
        {
        public:
            explicit Reader(std::string_view data)
                : mData(data)
            {
            }

            std::uint8_t readU8()
            {
                require(1);
                return static_cast<std::uint8_t>(mData[mPosition++]);
            }

            std::uint32_t readU32()
            {
                std::uint32_t value = 0;
                for (unsigned i = 0; i < 4; ++i)
                    value |= static_cast<std::uint32_t>(readU8()) << (i * 8);
                return value;
            }

            std::uint64_t readU64()
            {
                std::uint64_t value = 0;
                for (unsigned i = 0; i < 8; ++i)
                    value |= static_cast<std::uint64_t>(readU8()) << (i * 8);
                return value;
            }

            std::string readString()
            {
                const std::uint32_t size = readU32();
                if (size > sMaximumStringSize)
                    throw std::runtime_error("Player Lua script state value exceeds the size limit");
                require(size);
                std::string result(mData.substr(mPosition, size));
                mPosition += size;
                return result;
            }

            bool atEnd() const { return mPosition == mData.size(); }

        private:
            void require(std::size_t size) const
            {
                if (size > mData.size() - mPosition)
                    throw std::runtime_error("Player Lua script state is truncated");
            }

            std::string_view mData;
            std::size_t mPosition = 0;
        };

        std::filesystem::path makeTemporaryPath(const std::filesystem::path& path)
        {
            static std::atomic<std::uint64_t> sequence{ 0 };
#ifdef _WIN32
            const auto processId = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
            const auto processId = static_cast<std::uint64_t>(getpid());
#endif
            return path.parent_path()
                / (path.filename().string() + ".tmp-" + std::to_string(processId) + "-"
                    + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        }

        void replaceFile(const std::filesystem::path& temporaryPath, const std::filesystem::path& destinationPath)
        {
#ifdef _WIN32
            if (!MoveFileExW(
                    temporaryPath.c_str(), destinationPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                    "Unable to replace player Lua script state file");
            }
#else
            std::filesystem::rename(temporaryPath, destinationPath);
#endif
        }

        bool isPlayerScript(const ScriptsConfiguration& configuration, int id)
        {
            return id >= 0 && static_cast<std::size_t>(id) < configuration.size()
                && (configuration[id].mFlags & ESM::LuaScriptCfg::sPlayer) != 0;
        }
    }

    std::uint64_t PlayerScriptState::configurationFingerprint(const ScriptsConfiguration& configuration)
    {
        std::uint64_t hash = sFnvOffset;
        for (std::size_t id = 0; id < configuration.size(); ++id)
        {
            const ESM::LuaScriptCfg& script = configuration[id];
            if ((script.mFlags & ESM::LuaScriptCfg::sPlayer) == 0)
                continue;
            hash = fnv1a(VFS::Path::NormalizedView(script.mScriptPath).value(), hash);
            const char separator = '\0';
            hash = fnv1a(std::span<const char>(&separator, 1), hash);
            hash = fnv1a(std::string_view(script.mInitializationData), hash);
            const std::uint32_t flags = script.mFlags;
            for (unsigned byte = 0; byte < sizeof(flags); ++byte)
            {
                const char value = static_cast<char>(flags >> (byte * 8));
                hash = fnv1a(std::span<const char>(&value, 1), hash);
            }
        }
        return hash;
    }

    bool PlayerScriptState::load(const std::filesystem::path& path, const ScriptsConfiguration& configuration,
        PlayerScriptStateLoadResult& result, std::string& error)
    {
        result = {};
        error.clear();
        try
        {
            if (!std::filesystem::exists(path))
                return true;
            result.mExists = true;

            const std::uintmax_t fileSize = std::filesystem::file_size(path);
            if (fileSize == 0)
                throw std::runtime_error("Player Lua script state file has zero length");
            if (fileSize > sMaximumFileSize)
                throw std::runtime_error("Player Lua script state file exceeds the size limit");

            std::ifstream input(path, std::ios::binary);
            if (!input)
                throw std::runtime_error("Unable to open player Lua script state file");
            std::string fileData((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (fileData.size() < sMagic.size() + 4 + 8 + 8)
                throw std::runtime_error("Player Lua script state header is truncated");
            if (std::string_view(fileData.data(), sMagic.size()) != sMagic)
                throw std::runtime_error("Player Lua script state has an invalid header");

            Reader header(std::string_view(fileData).substr(sMagic.size()));
            const std::uint32_t version = header.readU32();
            if (version != sFormatVersion)
                throw std::runtime_error("Unsupported player Lua script state version " + std::to_string(version));
            const std::uint64_t savedFingerprint = header.readU64();
            const std::uint64_t savedChecksum = header.readU64();
            constexpr std::size_t headerSize = sMagic.size() + 4 + 8 + 8;
            const std::string_view payload(fileData.data() + headerSize, fileData.size() - headerSize);
            if (fnv1a(payload) != savedChecksum)
                throw std::runtime_error("Player Lua script state checksum mismatch");

            result.mConfigurationMatches = savedFingerprint == configurationFingerprint(configuration);
            Reader reader(payload);
            result.mSimulationTime = std::bit_cast<double>(reader.readU64());
            result.mGameTime = std::bit_cast<double>(reader.readU64());
            if (!std::isfinite(result.mSimulationTime) || !std::isfinite(result.mGameTime))
                throw std::runtime_error("Player Lua script state contains an invalid clock value");
            const std::uint32_t scriptCount = reader.readU32();
            if (scriptCount > sMaximumScripts)
                throw std::runtime_error("Player Lua script state contains too many scripts");

            std::set<std::string, std::less<>> seenPaths;
            for (std::uint32_t i = 0; i < scriptCount; ++i)
            {
                const std::string pathValue = reader.readString();
                if (!VFS::Path::isNormalized(pathValue))
                    throw std::runtime_error("Player Lua script state contains an invalid script path");
                if (!seenPaths.insert(pathValue).second)
                    throw std::runtime_error("Player Lua script state contains a duplicate script path");

                ESM::LuaScript script;
                script.mData = reader.readString();
                const std::uint32_t timerCount = reader.readU32();
                if (timerCount > sMaximumTimersPerScript)
                    throw std::runtime_error("Player Lua script state contains too many timers");
                script.mTimers.reserve(timerCount);
                for (std::uint32_t timerIndex = 0; timerIndex < timerCount; ++timerIndex)
                {
                    ESM::LuaTimer timer;
                    const std::uint8_t type = reader.readU8();
                    if (type > 1)
                        throw std::runtime_error("Player Lua script state contains an invalid timer type");
                    timer.mType = static_cast<ESM::LuaTimer::Type>(type);
                    timer.mTime = std::bit_cast<double>(reader.readU64());
                    if (!std::isfinite(timer.mTime))
                        throw std::runtime_error("Player Lua script state contains an invalid timer value");
                    timer.mCallbackName = reader.readString();
                    timer.mCallbackArgument = reader.readString();
                    script.mTimers.push_back(std::move(timer));
                }

                const auto currentId = configuration.findId(VFS::Path::NormalizedView(pathValue.c_str()));
                if (!currentId || !isPlayerScript(configuration, *currentId))
                    continue;
                script.mScriptId = *currentId;
                result.mScripts.mScripts.push_back(std::move(script));
            }
            if (!reader.atEnd())
                throw std::runtime_error("Player Lua script state contains trailing data");
            return true;
        }
        catch (const std::exception& e)
        {
            result.mScripts.mScripts.clear();
            error = e.what();
            return false;
        }
    }

    bool PlayerScriptState::save(const std::filesystem::path& path, const ScriptsConfiguration& configuration,
        const ESM::LuaScripts& scripts, double simulationTime, double gameTime, std::string& error)
    {
        error.clear();
        std::filesystem::path temporaryPath;
        try
        {
            std::vector<const ESM::LuaScript*> playerScripts;
            playerScripts.reserve(scripts.mScripts.size());
            for (const ESM::LuaScript& script : scripts.mScripts)
            {
                if (isPlayerScript(configuration, script.mScriptId))
                    playerScripts.push_back(&script);
            }
            if (playerScripts.size() > sMaximumScripts)
                throw std::runtime_error("Too many player Lua scripts to save");
            if (!std::isfinite(simulationTime) || !std::isfinite(gameTime))
                throw std::runtime_error("Cannot save invalid player Lua script clock values");

            std::string payload;
            appendU64(payload, std::bit_cast<std::uint64_t>(simulationTime));
            appendU64(payload, std::bit_cast<std::uint64_t>(gameTime));
            appendU32(payload, static_cast<std::uint32_t>(playerScripts.size()));
            for (const ESM::LuaScript* script : playerScripts)
            {
                const ESM::LuaScriptCfg& scriptCfg = configuration[script->mScriptId];
                appendString(payload, VFS::Path::NormalizedView(scriptCfg.mScriptPath).value());
                appendString(payload, script->mData);
                if (script->mTimers.size() > sMaximumTimersPerScript)
                    throw std::runtime_error("Too many player Lua timers to save");
                appendU32(payload, static_cast<std::uint32_t>(script->mTimers.size()));
                for (const ESM::LuaTimer& timer : script->mTimers)
                {
                    appendU8(payload, static_cast<std::uint8_t>(timer.mType));
                    appendU64(payload, std::bit_cast<std::uint64_t>(timer.mTime));
                    appendString(payload, timer.mCallbackName);
                    appendString(payload, timer.mCallbackArgument);
                }
            }

            std::string fileData(sMagic);
            appendU32(fileData, sFormatVersion);
            appendU64(fileData, configurationFingerprint(configuration));
            appendU64(fileData, fnv1a(std::string_view(payload)));
            fileData.append(payload);
            if (fileData.size() > sMaximumFileSize)
                throw std::runtime_error("Player Lua script state file exceeds the size limit");

            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            temporaryPath = makeTemporaryPath(path);
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            output.write(fileData.data(), fileData.size());
            output.flush();
            output.close();
            replaceFile(temporaryPath, path);
            return true;
        }
        catch (const std::exception& e)
        {
            if (!temporaryPath.empty())
            {
                std::error_code ignored;
                std::filesystem::remove(temporaryPath, ignored);
            }
            error = e.what();
            return false;
        }
    }
}
