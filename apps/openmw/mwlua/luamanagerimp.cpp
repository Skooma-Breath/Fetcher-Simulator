#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "luamanagerimp.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <MyGUI_InputManager.h>
#include <osg/Stats>
#include <osg/Timer>

#include <sol/object.hpp>
#include <sol/table.hpp>
#include <sol/types.hpp>

#include <components/debug/debuglog.hpp>

#include <components/esm/luascripts.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

#include <components/settings/values.hpp>

#include <components/l10n/manager.hpp>

#include <components/lua_ui/registerscriptsettings.hpp>
#include <components/lua_ui/util.hpp>

#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#ifdef BUILD_MULTIPLAYER
#include "../mwmp/Main.hpp"
#include "../mwmp/MpNetworkBridge.hpp"
#endif

#include "../mwrender/bonegroup.hpp"
#include "../mwrender/postprocessor.hpp"

#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luabindings.hpp"
#include "playerscripts.hpp"
#include "types/types.hpp"
#include "userdataserializer.hpp"

#ifdef _WIN32
#include <windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#endif

namespace MWLua
{
    class PlayerStorageLock
    {
    public:
        PlayerStorageLock() = default;
        PlayerStorageLock(const PlayerStorageLock&) = delete;
        PlayerStorageLock& operator=(const PlayerStorageLock&) = delete;

        ~PlayerStorageLock()
        {
#ifdef _WIN32
            if (mHandle != INVALID_HANDLE_VALUE)
                CloseHandle(mHandle);
#else
            if (mFd >= 0)
            {
                flock(mFd, LOCK_UN);
                close(mFd);
            }
#endif
        }

        bool acquire(const std::filesystem::path& path, std::string& error, bool waitForOwner = false)
        {
            try
            {
                std::filesystem::create_directories(path.parent_path());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (true)
                {
#ifdef _WIN32
                    mHandle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (mHandle != INVALID_HANDLE_VALUE)
                        return true;
                    const DWORD lockError = GetLastError();
                    if (!waitForOwner || (lockError != ERROR_SHARING_VIOLATION && lockError != ERROR_LOCK_VIOLATION)
                        || std::chrono::steady_clock::now() >= deadline)
                    {
                        error = std::system_error(static_cast<int>(lockError), std::system_category()).what();
                        return false;
                    }
#else
                    mFd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
                    if (mFd < 0)
                    {
                        error = std::error_code(errno, std::generic_category()).message();
                        return false;
                    }
                    if (flock(mFd, LOCK_EX | LOCK_NB) == 0)
                        return true;
                    const int lockError = errno;
                    close(mFd);
                    mFd = -1;
                    if (!waitForOwner || (lockError != EWOULDBLOCK && lockError != EAGAIN)
                        || std::chrono::steady_clock::now() >= deadline)
                    {
                        error = std::error_code(lockError, std::generic_category()).message();
                        return false;
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
        }

    private:
#ifdef _WIN32
        HANDLE mHandle = INVALID_HANDLE_VALUE;
#else
        int mFd = -1;
#endif
    };

    namespace
    {
#ifdef BUILD_MULTIPLAYER
        constexpr double luaPerfLogThresholdMs = 1.5;
        constexpr double luaPerfLogIntervalSec = 1.0;

        double elapsedMs(const osg::Timer& timer, osg::Timer_t start, osg::Timer_t end)
        {
            return timer.delta_s(start, end) * 1000.0;
        }

        bool shouldLogLuaPerf(const osg::Timer& timer, osg::Timer_t end, double totalMs, osg::Timer_t& lastLogTick,
            unsigned& suppressed)
        {
            if (totalMs < luaPerfLogThresholdMs)
                return false;

            if (lastLogTick != 0 && timer.delta_s(lastLogTick, end) < luaPerfLogIntervalSec)
            {
                ++suppressed;
                return false;
            }

            return true;
        }

        struct LuaUpdatePerf
        {
            double gc = 0.0;
            double objectLists = 0.0;
            double autoScripts = 0.0;
            double purgeInactive = 0.0;
            double stats = 0.0;
            double network = 0.0;
            double inboundEvents = 0.0;
            double finalizeEvents = 0.0;
            double timers = 0.0;
            double eventHandlers = 0.0;
            double callbacks = 0.0;
            double engineHandlers = 0.0;
            double localUpdates = 0.0;
            double globalUpdate = 0.0;
            double unloadInactive = 0.0;
        };

        struct LuaSyncPerf
        {
            double newGame = 0.0;
            double menuInput = 0.0;
            double playerInput = 0.0;
            double menuEvents = 0.0;
            double inputActions = 0.0;
            double menuFrame = 0.0;
            double playerFrame = 0.0;
            double uiMessages = 0.0;
            double consoleMessages = 0.0;
            double delayedActions = 0.0;
            double reloadScripts = 0.0;
            double uiModeChanged = 0.0;
        };

        void logLuaUpdatePerf(const osg::Timer& timer, osg::Timer_t end, double totalMs, const LuaUpdatePerf& perf,
            std::size_t activeLocalScripts, std::size_t queuedAutoScripts, std::size_t inboundEvents,
            std::size_t queuedCallbacks)
        {
            static osg::Timer_t lastLogTick = 0;
            static unsigned suppressed = 0;

            if (!shouldLogLuaPerf(timer, end, totalMs, lastLogTick, suppressed))
                return;

            Log(Debug::Info) << "[Perf] Lua update spike"
                             << " totalMs=" << totalMs
                             << " activeLocalScripts=" << activeLocalScripts
                             << " queuedAutoScripts=" << queuedAutoScripts
                             << " inboundEvents=" << inboundEvents
                             << " queuedCallbacks=" << queuedCallbacks
                             << " gcMs=" << perf.gc
                             << " objectListsMs=" << perf.objectLists
                             << " autoScriptsMs=" << perf.autoScripts
                             << " purgeInactiveMs=" << perf.purgeInactive
                             << " statsMs=" << perf.stats
                             << " networkMs=" << perf.network
                             << " inboundEventsMs=" << perf.inboundEvents
                             << " finalizeEventsMs=" << perf.finalizeEvents
                             << " timersMs=" << perf.timers
                             << " eventHandlersMs=" << perf.eventHandlers
                             << " callbacksMs=" << perf.callbacks
                             << " engineHandlersMs=" << perf.engineHandlers
                             << " localUpdatesMs=" << perf.localUpdates
                             << " globalUpdateMs=" << perf.globalUpdate
                             << " unloadInactiveMs=" << perf.unloadInactive
                             << " suppressed=" << suppressed;

            suppressed = 0;
            lastLogTick = end;
        }

        void logLuaSyncPerf(const osg::Timer& timer, osg::Timer_t end, double totalMs, const LuaSyncPerf& perf,
            std::size_t menuInputEvents, std::size_t playerInputEvents, std::size_t uiMessages,
            std::size_t consoleMessages, std::size_t delayedActions, bool hadTeleportAction, bool reloadRequested,
            bool hadUiModeChanged)
        {
            static osg::Timer_t lastLogTick = 0;
            static unsigned suppressed = 0;

            if (!shouldLogLuaPerf(timer, end, totalMs, lastLogTick, suppressed))
                return;

            Log(Debug::Info) << "[Perf] Lua sync spike"
                             << " totalMs=" << totalMs
                             << " menuInputEvents=" << menuInputEvents
                             << " playerInputEvents=" << playerInputEvents
                             << " uiMessages=" << uiMessages
                             << " consoleMessages=" << consoleMessages
                             << " delayedActions=" << delayedActions
                             << " hadTeleportAction=" << hadTeleportAction
                             << " reloadRequested=" << reloadRequested
                             << " hadUiModeChanged=" << hadUiModeChanged
                             << " newGameMs=" << perf.newGame
                             << " menuInputMs=" << perf.menuInput
                             << " playerInputMs=" << perf.playerInput
                             << " menuEventsMs=" << perf.menuEvents
                             << " inputActionsMs=" << perf.inputActions
                             << " menuFrameMs=" << perf.menuFrame
                             << " playerFrameMs=" << perf.playerFrame
                             << " uiMessagesMs=" << perf.uiMessages
                             << " consoleMessagesMs=" << perf.consoleMessages
                             << " delayedActionsMs=" << perf.delayedActions
                             << " reloadScriptsMs=" << perf.reloadScripts
                             << " uiModeChangedMs=" << perf.uiModeChanged
                             << " suppressed=" << suppressed;

            suppressed = 0;
            lastLogTick = end;
        }
#endif

        struct BoolScopeGuard
        {
            bool& mValue;
            BoolScopeGuard(bool& value)
                : mValue(value)
            {
                mValue = true;
            }

            ~BoolScopeGuard() { mValue = false; }
        };
    }

    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {
        if (!Settings::lua().mLuaProfiler)
            LuaUtil::LuaState::disableProfiler();
        return { .mInstructionLimit = Settings::lua().mInstructionLimitPerCall,
            .mMemoryLimit = Settings::lua().mMemoryLimit,
            .mSmallAllocMaxSize = Settings::lua().mSmallAllocMaxSize,
            .mLogMemoryUsage = Settings::lua().mLogMemoryUsage };
    }

    LuaManager::LuaManager(const VFS::Manager* vfs, const std::filesystem::path& libsDir)
        : mLua(vfs, &mConfiguration, createLuaStateSettings())
    {
        Log(Debug::Info) << "Lua version: " << LuaUtil::getLuaVersion();
        mLua.addInternalLibSearchPath(libsDir);

        mGlobalSerializer = createUserdataSerializer(false);
        mLocalSerializer = createUserdataSerializer(true);
        mGlobalLoader = createUserdataSerializer(false, &mContentFileMapping);
        mLocalLoader = createUserdataSerializer(true, &mContentFileMapping);

        mGlobalScripts.setSerializer(mGlobalSerializer.get());
    }

    LuaManager::~LuaManager()
    {
        LuaUi::clearSettings();
    }

    void LuaManager::initConfiguration(bool reload)
    {
        mConfiguration.init(MWBase::Environment::get().getESMStore()->getLuaScriptsCfg(), reload);
        Log(Debug::Verbose) << "Lua scripts configuration (" << mConfiguration.size() << " scripts):";
        for (size_t i = 0; i < mConfiguration.size(); ++i)
            Log(Debug::Verbose) << "#" << i << " " << LuaUtil::scriptCfgToString(mConfiguration[i]);
        mMenuScripts.setAutoStartConf(mConfiguration.getMenuConf());
        mGlobalScripts.setAutoStartConf(mConfiguration.getGlobalConf());
    }

    void LuaManager::initPreLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context context;
            context.mType = Context::Load;
            context.mLuaManager = this;
            context.mLua = &mLua;

            for (const auto& [name, package] : initCommonPackages(context))
                mLua.addCommonPackage(name, package);

            for (const auto& [name, package] : initLoadPackages(context))
                mLoadScripts.addPackage(name, package);

            mLoadScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initLoadPackage(view, &mPlayerStorage));

            LuaUtil::LuaStorage::initLuaBindings(view);
        });
    }

    void LuaManager::contentFilesLoaded()
    {
        initConfiguration(false);
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());
        mLoadScripts.addAutoStartedScripts();
        mLoadScripts.contentFilesLoaded();
        mLoadScripts.removeAllScripts();
    }

    void LuaManager::initPostLoad()
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            Context globalContext;
            globalContext.mType = Context::Global;
            globalContext.mLuaManager = this;
            globalContext.mLua = &mLua;
            globalContext.mObjectLists = &mObjectLists;
            globalContext.mLuaEvents = &mLuaEvents;
            globalContext.mSerializer = mGlobalSerializer.get();

            Context localContext = globalContext;
            localContext.mType = Context::Local;
            localContext.mSerializer = mLocalSerializer.get();

            Context menuContext = globalContext;
            menuContext.mType = Context::Menu;

            for (const auto& [name, package] : initGlobalPackages(globalContext))
                mGlobalScripts.addPackage(name, package);
            for (const auto& [name, package] : initMenuPackages(menuContext))
                mMenuScripts.addPackage(name, package);

            mLocalPackages = initLocalPackages(localContext);

            mPlayerPackages = initPlayerPackages(localContext);
            mPlayerPackages.insert(mLocalPackages.begin(), mLocalPackages.end());

            mGlobalScripts.addPackage("openmw.storage", LuaUtil::LuaStorage::initGlobalPackage(view, &mGlobalStorage));
            mMenuScripts.addPackage(
                "openmw.storage", LuaUtil::LuaStorage::initMenuPackage(view, &mGlobalStorage, &mPlayerStorage));
            mLocalPackages["openmw.storage"] = LuaUtil::LuaStorage::initLocalPackage(view, &mGlobalStorage);
            mPlayerPackages["openmw.storage"]
                = LuaUtil::LuaStorage::initPlayerPackage(view, &mGlobalStorage, &mPlayerStorage);

            mPlayerStorage.setActive(true);
            mGlobalStorage.setActive(false);

            mInitialized = true;
            mMenuScripts.addAutoStartedScripts();
        });
    }

    void LuaManager::loadPermanentStorage(const std::filesystem::path& userConfigPath,
        bool deferMultiplayerPlayerStorage, bool isolatedMultiplayerProfile)
    {
        mGlobalStorageMirroredFromServer = false;
        mPlayerStorage.setActive(true);
        mGlobalStorage.setActive(true);
        const auto globalPath = userConfigPath / "global_storage.bin";
        mDefaultPlayerStoragePath = userConfigPath / "player_storage.bin";
        mMultiplayerPlayerStorage = deferMultiplayerPlayerStorage || isolatedMultiplayerProfile;
        mIsolatedMultiplayerProfile = isolatedMultiplayerProfile;

        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (std::filesystem::exists(globalPath))
                mGlobalStorage.load(view.sol(), globalPath);
            if (!deferMultiplayerPlayerStorage && std::filesystem::exists(mDefaultPlayerStoragePath))
            {
                mPlayerStorage.load(view.sol(), mDefaultPlayerStoragePath);
                mLoadedPlayerStoragePath = mDefaultPlayerStoragePath;
            }
        });
    }

    void LuaManager::savePermanentStorage(const std::filesystem::path& userConfigPath)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (mGlobalScriptsStarted && !mGlobalStorageMirroredFromServer)
                mGlobalStorage.save(view.sol(), userConfigPath / "global_storage.bin");
            else if (mGlobalScriptsStarted)
                Log(Debug::Info) << "Skipping Lua global storage save because it is mirrored from the multiplayer server";

            if (mMultiplayerPlayerStorage)
            {
                if (mBoundPlayerStoragePath)
                    mPlayerStorage.save(view.sol(), *mBoundPlayerStoragePath);
                else
                    Log(Debug::Verbose) << "Skipping unbound multiplayer player Lua storage save";
            }
            else
                mPlayerStorage.save(view.sol(), userConfigPath / "player_storage.bin");
        });
    }

    void LuaManager::prepareMultiplayerPlayerStorage()
    {
        mMultiplayerPlayerStorage = true;
    }

    bool LuaManager::bindMultiplayerPlayerStorage(std::string_view storageNamespace,
        std::string_view characterKey, std::string_view characterName, std::string& error)
    {
        const auto isSafePathComponent = [](std::string_view value) {
            return !value.empty()
                && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
                    == std::string_view::npos;
        };
        if (!isSafePathComponent(storageNamespace) || !isSafePathComponent(characterKey))
        {
            error = "Invalid multiplayer player storage identity";
            return false;
        }

        prepareMultiplayerPlayerStorage();
        const std::filesystem::path storagePath = mIsolatedMultiplayerProfile
            ? mDefaultPlayerStoragePath
            : mDefaultPlayerStoragePath.parent_path() / "multiplayer-characters"
                / std::string(storageNamespace) / std::string(characterKey) / "player_storage.bin";

        if (mBoundPlayerStoragePath && *mBoundPlayerStoragePath == storagePath && mPlayerStorageLock)
            return true;

        auto nextLock = std::make_unique<PlayerStorageLock>();
        if (!nextLock->acquire(storagePath.parent_path() / "player_storage.lock", error))
        {
            error = "Character '" + std::string(characterName)
                + "' is already open in another client, or its storage lock is unavailable: " + error;
            return false;
        }

        try
        {
            if (!mIsolatedMultiplayerProfile && !std::filesystem::exists(storagePath)
                && std::filesystem::exists(mDefaultPlayerStoragePath))
            {
                PlayerStorageLock migrationLock;
                std::string migrationError;
                if (!migrationLock.acquire(
                        mDefaultPlayerStoragePath.parent_path() / "player_storage.migration.lock",
                        migrationError, true))
                {
                    throw std::runtime_error("Unable to acquire shared player storage migration lock: " + migrationError);
                }

                if (!std::filesystem::exists(storagePath) && std::filesystem::exists(mDefaultPlayerStoragePath))
                {
                    if (mLoadedPlayerStoragePath && *mLoadedPlayerStoragePath == mDefaultPlayerStoragePath)
                    {
                        mLua.protectedCall([&](LuaUtil::LuaView& view) {
                            mPlayerStorage.save(view.sol(), mDefaultPlayerStoragePath);
                        });
                    }
                    std::filesystem::create_directories(storagePath.parent_path());
                    std::filesystem::rename(mDefaultPlayerStoragePath, storagePath);
                    Log(Debug::Info) << "[MP] Migrated shared player Lua storage to character-bound path: "
                                     << storagePath;
                }
            }

            if (mBoundPlayerStoragePath && *mBoundPlayerStoragePath != storagePath)
            {
                mLua.protectedCall(
                    [&](LuaUtil::LuaView& view) { mPlayerStorage.save(view.sol(), *mBoundPlayerStoragePath); });
            }

            if (!mLoadedPlayerStoragePath || *mLoadedPlayerStoragePath != storagePath)
            {
                bool loaded = false;
                std::string loadError;
                mLua.protectedCall([&](LuaUtil::LuaView& view) {
                    loaded = mPlayerStorage.replaceFromFile(view.sol(), storagePath, loadError);
                });
                if (!loaded)
                {
                    error = "Could not load player storage for character '" + std::string(characterName)
                        + "': " + loadError;
                    return false;
                }
            }
        }
        catch (const std::exception& e)
        {
            error = "Could not bind player storage for character '" + std::string(characterName) + "': " + e.what();
            return false;
        }

        mLoadedPlayerStoragePath = storagePath;
        mBoundPlayerStoragePath = storagePath;
        mPlayerStorageLock = std::move(nextLock);
        Log(Debug::Info) << "[MP] Bound character player Lua storage"
                         << " character=" << characterName
                         << " path=" << storagePath;
        return true;
    }

    void LuaManager::sendLocalEvent(
        const MWWorld::Ptr& target, const std::string& name, const std::optional<sol::table>& data)
    {
        LuaUtil::BinaryData binary = {};
        if (data)
        {
            binary = LuaUtil::serialize(*data, mLocalSerializer.get());
        }
        mLuaEvents.addLocalEvent({ getId(target), name, std::move(binary) });
    }

    void LuaManager::update()
    {
#ifdef BUILD_MULTIPLAYER
        const osg::Timer* const perfTimer = osg::Timer::instance();
        const osg::Timer_t perfStart = perfTimer->tick();
        osg::Timer_t perfPhaseStart = perfStart;
        LuaUpdatePerf perf;
        auto markPerf = [&](double& bucket) {
            const osg::Timer_t now = perfTimer->tick();
            bucket += elapsedMs(*perfTimer, perfPhaseStart, now);
            perfPhaseStart = now;
        };
        const std::size_t activeLocalScriptCount = mActiveLocalScripts.size();
        const std::size_t queuedAutoScriptCount = mQueuedAutoStartedScripts.size();
        const std::size_t queuedCallbackCount = mQueuedCallbacks.size();
        std::size_t inboundEventCount = 0;
#endif

        if (const int steps = Settings::lua().mGcStepsPerFrame; steps > 0)
            lua_gc(mLua.unsafeState(), LUA_GCSTEP, steps);
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.gc);
#endif

        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!(getId(mPlayer) == getId(newPlayerPtr)))
            throw std::logic_error("Player RefNum was changed unexpectedly");
        if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
        {
            mPlayer = newPlayerPtr; // player was moved to another cell, update ptr in registry
            MWBase::Environment::get().getWorldModel()->registerPtr(mPlayer);
        }

        mObjectLists.update();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.objectLists);
#endif

        for (auto scripts : mQueuedAutoStartedScripts)
            scripts->addAutoStartedScripts();
        mQueuedAutoStartedScripts.clear();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.autoScripts);
#endif

        std::erase_if(mActiveLocalScripts,
            [](const LocalScripts* l) { return l->getPtrOrEmpty().isEmpty() || l->getPtrOrEmpty().mRef->isDeleted(); });
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.purgeInactive);
#endif

        mGlobalScripts.statsNextFrame();
        for (LocalScripts* scripts : mActiveLocalScripts)
            scripts->statsNextFrame();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.stats);
#endif

#ifdef BUILD_MULTIPLAYER
        if (mwmp::Main::isInitialised())
            mwmp::Main::get().getNetworkBridge().processIncoming(*this);
        markPerf(perf.network);
#endif

        std::vector<LuaEvents::Global> inboundEvents;
        {
            std::lock_guard<std::mutex> lock(mInboundGlobalEventsMutex);
            inboundEvents.swap(mInboundGlobalEvents);
        }
#ifdef BUILD_MULTIPLAYER
        inboundEventCount = inboundEvents.size();
#endif
        for (auto& event : inboundEvents)
            mLuaEvents.addGlobalEvent(std::move(event));
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.inboundEvents);
#endif

        mLuaEvents.finalizeEventBatch();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.finalizeEvents);
#endif

        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        bool luaPaused = timeManager.isPaused();
#ifdef BUILD_MULTIPLAYER
        if (luaPaused && mwmp::Main::isConnected())
            luaPaused = false;
#endif
        if (!luaPaused)
        {
            mMenuScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            mGlobalScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            for (LocalScripts* scripts : mActiveLocalScripts)
                scripts->processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
        }
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.timers);
#endif

        // Run event handlers for events that were sent before `finalizeEventBatch`.
        mLuaEvents.callEventHandlers();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.eventHandlers);
#endif

        mLua.protectedCall([&](LuaUtil::LuaView& lua) {
            // Run queued callbacks
            for (CallbackWithData& c : mQueuedCallbacks)
                c.mCallback.tryCall(c.mArg);
            mQueuedCallbacks.clear();
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.callbacks);
#endif

            // Run engine handlers
            mEngineEvents.callEngineHandlers();
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.engineHandlers);
#endif
            bool isPaused = luaPaused;

            float frameDuration = MWBase::Environment::get().getFrameDuration();
            for (LocalScripts* scripts : mActiveLocalScripts)
                scripts->update(isPaused ? 0 : frameDuration);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.localUpdates);
#endif
            mGlobalScripts.update(isPaused ? 0 : frameDuration);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.globalUpdate);
#endif

            mScriptTracker.unloadInactiveScripts(lua);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.unloadInactive);
#endif
        });

#ifdef BUILD_MULTIPLAYER
        const osg::Timer_t perfEnd = perfTimer->tick();
        logLuaUpdatePerf(*perfTimer, perfEnd, elapsedMs(*perfTimer, perfStart, perfEnd), perf, activeLocalScriptCount,
            queuedAutoScriptCount, inboundEventCount, queuedCallbackCount);
#endif
    }

    void LuaManager::objectTeleported(const MWWorld::Ptr& ptr)
    {
        if (ptr == mPlayer)
        {
            // For player run the onTeleported handler immediately,
            // so it can adjust camera position after teleporting.
            PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
            if (playerScripts)
                playerScripts->onTeleported();
        }
        else
            mEngineEvents.addToQueue(EngineEvents::OnTeleported{ getId(ptr) });
    }

    void LuaManager::questUpdated(const ESM::RefId& questId, int stage)
    {
        if (mPlayer.isEmpty())
            return; // The game is not started yet.
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
        {
            playerScripts->onQuestUpdate(questId.serializeText(), stage);
        }
    }

    void LuaManager::synchronizedUpdate()
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) { synchronizedUpdateUnsafe(); });
    }

    void LuaManager::synchronizedUpdateUnsafe()
    {
#ifdef BUILD_MULTIPLAYER
        const osg::Timer* const perfTimer = osg::Timer::instance();
        const osg::Timer_t perfStart = perfTimer->tick();
        osg::Timer_t perfPhaseStart = perfStart;
        LuaSyncPerf perf;
        auto markPerf = [&](double& bucket) {
            const osg::Timer_t now = perfTimer->tick();
            bucket += elapsedMs(*perfTimer, perfPhaseStart, now);
            perfPhaseStart = now;
        };
        const std::size_t menuInputEventCount = mMenuInputEvents.size();
        const std::size_t playerInputEventCount = mInputEvents.size();
        const std::size_t uiMessageCount = mUIMessages.size();
        const std::size_t consoleMessageCount = mInGameConsoleMessages.size();
        const std::size_t delayedActionCount = mActionQueue.size();
        const bool hadTeleportAction = mTeleportPlayerAction.has_value();
        const bool reloadRequested = mReloadAllScriptsRequested;
        const bool hadUiModeChanged = mDelayedUiModeChangedArg.has_value();
#endif

        if (mNewGameStarted)
        {
            mNewGameStarted = false;
            // Run onNewGame handler in synchronizedUpdate (at the beginning of the frame), so it
            // can teleport the player to the starting location before the first frame is rendered.
            mGlobalScripts.newGameStarted();
        }
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.newGame);
#endif
        BoolScopeGuard updateGuard(mRunningSynchronizedUpdates);

        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        // We apply input events in `synchronizedUpdate` rather than in `update` in order to reduce input latency.
        {
            BoolScopeGuard processingGuard(mProcessingInputEvents);

            for (const auto& event : mMenuInputEvents)
                mMenuScripts.processInputEvent(event);
            mMenuInputEvents.clear();
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.menuInput);
#endif
            if (playerScripts && !windowManager->containsMode(MWGui::GM_MainMenu))
            {
                for (const auto& event : mInputEvents)
                    playerScripts->processInputEvent(event);
            }
            mInputEvents.clear();
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.playerInput);
#endif
            mLuaEvents.callMenuEventHandlers();
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.menuEvents);
#endif
            bool luaPaused = MWBase::Environment::get().getWorld()->getTimeManager()->isPaused();
#ifdef BUILD_MULTIPLAYER
            if (luaPaused && mwmp::Main::isConnected())
                luaPaused = false;
#endif
            float frameDuration = luaPaused
                ? 0.f
                : MWBase::Environment::get().getFrameDuration();
            mInputActions.update(frameDuration);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.inputActions);
#endif
            mMenuScripts.onFrame(frameDuration);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.menuFrame);
#endif
            if (playerScripts)
                playerScripts->onFrame(frameDuration);
#ifdef BUILD_MULTIPLAYER
            markPerf(perf.playerFrame);
#endif
        }

        for (const auto& [message, mode] : mUIMessages)
            windowManager->messageBox(message, mode);
        mUIMessages.clear();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.uiMessages);
#endif
        for (auto& [msg, color] : mInGameConsoleMessages)
            windowManager->printToConsole(msg, "#" + color.toHex());
        mInGameConsoleMessages.clear();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.consoleMessages);
#endif

        applyDelayedActions();
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.delayedActions);
#endif

        if (mReloadAllScriptsRequested)
        {
            // Reloading right after `applyDelayedActions` to guarantee that no delayed actions are currently queued.
            reloadAllScriptsImpl();
            mReloadAllScriptsRequested = false;
        }
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.reloadScripts);
#endif

        if (mDelayedUiModeChangedArg)
        {
            if (playerScripts)
                playerScripts->uiModeChanged(*mDelayedUiModeChangedArg, true);
            mDelayedUiModeChangedArg = std::nullopt;
        }
#ifdef BUILD_MULTIPLAYER
        markPerf(perf.uiModeChanged);
        const osg::Timer_t perfEnd = perfTimer->tick();
        logLuaSyncPerf(*perfTimer, perfEnd, elapsedMs(*perfTimer, perfStart, perfEnd), perf, menuInputEventCount,
            playerInputEventCount, uiMessageCount, consoleMessageCount, delayedActionCount, hadTeleportAction,
            reloadRequested, hadUiModeChanged);
#endif
    }

    void LuaManager::applyDelayedActions()
    {
        BoolScopeGuard applyingGuard(mApplyingDelayedActions);
        for (DelayedAction& action : mActionQueue)
            action.apply();
        mActionQueue.clear();

        if (mTeleportPlayerAction)
            mTeleportPlayerAction->apply();
        mTeleportPlayerAction.reset();
    }

    void LuaManager::clear()
    {
        LuaUi::clearGameInterface();
        mUiResourceManager.clear();
        MWBase::Environment::get().getWorld()->getPostProcessor()->disableDynamicShaders();
        mActiveLocalScripts.clear();
        mLuaEvents.clear();
        mEngineEvents.clear();
        mInputEvents.clear();
        mMenuInputEvents.clear();
        mObjectLists.clear();
        mGlobalScripts.removeAllScripts();
        mGlobalScriptsStarted = false;
        mNewGameStarted = false;
        mDelayedUiModeChangedArg = std::nullopt;
        if (!mPlayer.isEmpty())
        {
            mPlayer.getCellRef().unsetRefNum();
            mPlayer.getRefData().setLuaScripts(nullptr);
            mPlayer = MWWorld::Ptr();
        }
        mGlobalStorage.setActive(true);
        mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        mGlobalStorage.setActive(false);
        mPlayerStorage.clearTemporaryAndRemoveCallbacks();
        mInputActions.clear();
        mInputTriggers.clear();
        mQueuedAutoStartedScripts.clear();
        for (int i = 0; i < 5; ++i)
            lua_gc(mLua.unsafeState(), LUA_GCCOLLECT, 0);
    }

    void LuaManager::setupPlayer(const MWWorld::Ptr& ptr)
    {
        if (!mInitialized)
            return;
        if (!mPlayer.isEmpty())
            throw std::logic_error("Player is initialized twice");
        mObjectLists.objectAddedToScene(ptr);
        mObjectLists.setPlayer(ptr);
        mPlayer = ptr;
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            mQueuedAutoStartedScripts.push_back(localScripts);
        }
        mActiveLocalScripts.insert(localScripts);
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });
    }

    void LuaManager::newGameStarted()
    {
        mGlobalStorage.setActive(true);
        mInputEvents.clear();
        mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mNewGameStarted = true;
    }

    void LuaManager::gameLoaded()
    {
        mGlobalStorage.setActive(true);
        if (!mGlobalScriptsStarted)
            mGlobalScripts.addAutoStartedScripts();
        mGlobalScriptsStarted = true;
        mMenuScripts.stateChanged();
    }

    void LuaManager::gameEnded()
    {
        // TODO: disable scripts and global storage when the game is actually unloaded
        // mGlobalStorage.setActive(false);
        mMenuScripts.stateChanged();
    }

    void LuaManager::noGame()
    {
        clear();
        mMenuScripts.stateChanged();
    }

    void LuaManager::uiModeChanged(const MWWorld::Ptr& arg)
    {
        if (mPlayer.isEmpty())
            return;
        ObjectId argId = arg.isEmpty() ? ObjectId() : getId(arg);
        if (mApplyingDelayedActions)
        {
            mDelayedUiModeChangedArg = argId;
            return;
        }
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
            playerScripts->uiModeChanged(argId, false);
    }

    void LuaManager::actorDied(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return;
        mLuaEvents.addLocalEvent({ getId(actor), "Died", {} });
    }

    void LuaManager::onDialogueResponse(
        const MWWorld::Ptr& actor, const ESM::DialInfo& info, const ESM::Dialogue& record)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["actor"] = LObject(actor);
            if (record.mType == ESM::Dialogue::Type::Greeting)
                data["type"] = "greeting";
            else if (record.mType == ESM::Dialogue::Type::Journal)
                data["type"] = "journal";
            else if (record.mType == ESM::Dialogue::Type::Persuasion)
                data["type"] = "persuasion";
            else if (record.mType == ESM::Dialogue::Type::Topic)
                data["type"] = "topic";
            else if (record.mType == ESM::Dialogue::Type::Voice)
                data["type"] = "voice";
            data["infoId"] = info.mId.serializeText();
            data["recordId"] = record.mId.serializeText();
            sendLocalEvent(mPlayer, "DialogueResponse", data);
        });
    }

    void LuaManager::useItem(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, bool force)
    {
        MWBase::Environment::get().getWorldModel()->registerPtr(object);
        mEngineEvents.addToQueue(EngineEvents::OnUseItem{ getId(actor), getId(object), force });
    }

    void LuaManager::animationTextKey(const MWWorld::Ptr& actor, const std::string& key)
    {
        auto pos = key.find(": ");
        if (pos != std::string::npos)
            mEngineEvents.addToQueue(
                EngineEvents::OnAnimationTextKey{ getId(actor), key.substr(0, pos), key.substr(pos + 2) });
    }

    void LuaManager::playAnimation(const MWWorld::Ptr& actor, const std::string& groupname,
        const MWRender::AnimPriority& priority, int blendMask, bool autodisable, float speedmult,
        std::string_view start, std::string_view stop, float startpoint, uint32_t loops, bool loopfallback)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table options = view.newTable();
            options["blendMask"] = blendMask;
            options["autoDisable"] = autodisable;
            options["speed"] = speedmult;
            options["startKey"] = start;
            options["stopKey"] = stop;
            options["startPoint"] = startpoint;
            options["loops"] = loops;
            options["forceLoop"] = loopfallback;

            bool priorityAsTable = false;
            for (uint32_t i = 1; i < MWRender::sNumBlendMasks; i++)
                if (priority[static_cast<MWRender::BoneGroup>(i)] != priority[static_cast<MWRender::BoneGroup>(0)])
                    priorityAsTable = true;
            if (priorityAsTable)
            {
                sol::table priorityTable = view.newTable();
                for (uint32_t i = 0; i < MWRender::sNumBlendMasks; i++)
                    priorityTable[static_cast<MWRender::BoneGroup>(i)] = priority[static_cast<MWRender::BoneGroup>(i)];
                options["priority"] = priorityTable;
            }
            else
                options["priority"] = priority[MWRender::BoneGroup_LowerBody];

            // mEngineEvents.addToQueue(event);
            //  Has to be called immediately, otherwise engine details that depend on animations playing immediately
            //  break.
            if (auto* scripts = actor.getRefData().getLuaScripts())
                scripts->onPlayAnimation(groupname, options);
        });
    }

    void LuaManager::animationEnded(const MWWorld::Ptr& actor, std::string_view groupname, float time, float completion,
        std::string_view startKey, std::string_view stopKey)
    {
        mEngineEvents.addToQueue(EngineEvents::OnAnimationEnded{
            getId(actor), std::string(groupname), std::string(startKey), std::string(stopKey), time, completion });
    }

    void LuaManager::skillUse(const MWWorld::Ptr& actor, ESM::RefId skillId, int useType, float scale)
    {
        mEngineEvents.addToQueue(EngineEvents::OnSkillUse{ getId(actor), skillId.serializeText(), useType, scale });
    }

    void LuaManager::skillLevelUp(const MWWorld::Ptr& actor, ESM::RefId skillId, std::string_view source)
    {
        mEngineEvents.addToQueue(
            EngineEvents::OnSkillLevelUp{ getId(actor), skillId.serializeText(), std::string(source) });
    }

    void LuaManager::jailTimeServed(const MWWorld::Ptr& actor, int days)
    {
        mEngineEvents.addToQueue(EngineEvents::OnJailTimeServed{ getId(actor), days });
    }

    void LuaManager::onHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& weapon,
        const MWWorld::Ptr& ammo, int attackType, float attackStrength, float damage, bool isHealth,
        const osg::Vec3f& hitPos, bool successful, MWMechanics::DamageSourceType sourceType)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table damageTable = view.newTable();
            if (isHealth)
                damageTable["health"] = damage;
            else
                damageTable["fatigue"] = damage;

            sol::table data = view.newTable();
            if (!attacker.isEmpty())
                data["attacker"] = LObject(attacker);
            if (!weapon.isEmpty())
                data["weapon"] = LObject(weapon);
            if (!ammo.isEmpty())
                data["ammo"] = ammo.getCellRef().getRefId().serializeText();
            data["type"] = attackType;
            data["strength"] = attackStrength;
            data["damage"] = damageTable;
            data["hitPos"] = hitPos;
            data["successful"] = successful;
            switch (sourceType)
            {
                case MWMechanics::DamageSourceType::Unspecified:
                    data["sourceType"] = "unspecified";
                    break;
                case MWMechanics::DamageSourceType::Melee:
                    data["sourceType"] = "melee";
                    break;
                case MWMechanics::DamageSourceType::Ranged:
                    data["sourceType"] = "ranged";
                    break;
                case MWMechanics::DamageSourceType::Magical:
                    data["sourceType"] = "magic";
                    break;
            }

            sendLocalEvent(victim, "Hit", data);
        });
    }

    void LuaManager::objectAddedToScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectAddedToScene(ptr); // assigns generated RefNum if it is not set yet.
        mEngineEvents.addToQueue(EngineEvents::OnActive{ getId(ptr) });

        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            LuaUtil::ScriptIdsWithInitializationData autoStartConf
                = mConfiguration.getLocalConf(getLiveCellRefType(ptr.mRef), ptr.getCellRef().getRefId(), getId(ptr));
            if (!autoStartConf.empty())
            {
                localScripts = createLocalScripts(ptr, std::move(autoStartConf));
                mQueuedAutoStartedScripts.push_back(localScripts);
            }
        }
        if (localScripts)
            mActiveLocalScripts.insert(localScripts);
    }

    void LuaManager::objectRemovedFromScene(const MWWorld::Ptr& ptr)
    {
        mObjectLists.objectRemovedFromScene(ptr);
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (localScripts)
        {
            mActiveLocalScripts.erase(localScripts);
            if (!MWBase::Environment::get().getWorldModel()->getPtr(getId(ptr)).isEmpty())
                mEngineEvents.addToQueue(EngineEvents::OnInactive{ getId(ptr) });
        }
    }

    void LuaManager::inputEvent(const InputEvent& event)
    {
        if (!MyGUI::InputManager::getInstance().isModalAny()
            && !MWBase::Environment::get().getWindowManager()->isConsoleMode())
        {
            mInputEvents.push_back(event);
        }
        mMenuInputEvents.push_back(event);
    }

    MWBase::LuaManager::ActorControls* LuaManager::getActorControls(const MWWorld::Ptr& ptr) const
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
            return nullptr;
        return localScripts->getActorControls();
    }

    void LuaManager::addCustomLocalScript(const MWWorld::Ptr& ptr, int scriptId, std::string_view initData)
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            localScripts->addAutoStartedScripts();
            if (ptr.isInCell() && MWBase::Environment::get().getWorldScene()->isCellActive(*ptr.getCell()))
            {
                localScripts->setActive(true, false);
                mActiveLocalScripts.insert(localScripts);
            }
        }
        localScripts->addCustomScript(scriptId, initData);
    }

    LocalScripts* LuaManager::createLocalScripts(
        const MWWorld::Ptr& ptr, std::optional<LuaUtil::ScriptIdsWithInitializationData> autoStartConf)
    {
        assert(mInitialized);
        std::shared_ptr<LocalScripts> scripts;
        const uint32_t type = getLiveCellRefType(ptr.mRef);
        if (type == ESM::REC_STAT)
            throw std::runtime_error("Lua scripts on static objects are not allowed");
        else if (type == ESM::REC_INTERNAL_PLAYER)
        {
            scripts = std::make_shared<PlayerScripts>(&mLua, LObject(getId(ptr)));
            scripts->setAutoStartConf(mConfiguration.getPlayerConf());
            for (const auto& [name, package] : mPlayerPackages)
                scripts->addPackage(name, package);
        }
        else
        {
            scripts = std::make_shared<LocalScripts>(&mLua, LObject(getId(ptr)), &mScriptTracker);
            if (!autoStartConf.has_value())
                autoStartConf = mConfiguration.getLocalConf(type, ptr.getCellRef().getRefId(), getId(ptr));
            scripts->setAutoStartConf(std::move(*autoStartConf));
            for (const auto& [name, package] : mLocalPackages)
                scripts->addPackage(name, package);
        }
        scripts->setSerializer(mLocalSerializer.get());

        MWWorld::RefData& refData = ptr.getRefData();
        refData.setLuaScripts(std::move(scripts));
        return refData.getLuaScripts();
    }

    void LuaManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        writer.startRecord(ESM::REC_LUAM);

        writer.writeHNT<double>("LUAW", MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime());
        writer.writeFormId(MWBase::Environment::get().getWorldModel()->getLastGeneratedRefNum(), true);
        mConfiguration.write(writer);
        ESM::LuaScripts globalScripts;
        mGlobalScripts.save(globalScripts);
        globalScripts.save(writer);
        mLuaEvents.save(writer);

        writer.endRecord(ESM::REC_LUAM);
    }

    void LuaManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type != ESM::REC_LUAM)
            throw std::runtime_error("ESM::REC_LUAM is expected");

        double simulationTime;
        reader.getHNT(simulationTime, "LUAW");
        MWBase::Environment::get().getWorld()->getTimeManager()->setSimulationTime(simulationTime);
        ESM::FormId lastGenerated = reader.getFormId(true);
        if (lastGenerated.hasContentFile())
            throw std::runtime_error("Last generated RefNum is invalid");
        MWBase::Environment::get().getWorldModel()->setLastGeneratedRefNum(lastGenerated);

        mConfiguration.read(reader);

        // TODO: don't execute scripts right away, it will be necessary in multiplayer where global storage requires
        // initialization. For now just set global storage as active slightly before it would be set by gameLoaded()
        mGlobalStorage.setActive(true);

        ESM::LuaScripts globalScripts;
        globalScripts.load(reader);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            mLuaEvents.load(view.sol(), reader, mContentFileMapping, mGlobalLoader.get());
        });

        mGlobalScripts.setSavedDataDeserializer(mGlobalLoader.get());
        mGlobalScripts.load(globalScripts);
        mGlobalScriptsStarted = true;
    }

    void LuaManager::saveLocalScripts(const MWWorld::Ptr& ptr, ESM::LuaScripts& data)
    {
        if (ptr.getRefData().getLuaScripts())
            ptr.getRefData().getLuaScripts()->save(data);
        else
            data.mScripts.clear();
    }

    void LuaManager::loadLocalScripts(const MWWorld::Ptr& ptr, const ESM::LuaScripts& data)
    {
        if (data.mScripts.empty())
        {
            if (ptr.getRefData().getLuaScripts())
                ptr.getRefData().setLuaScripts(nullptr);
            return;
        }

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        LocalScripts* scripts = createLocalScripts(ptr);

        scripts->setSerializer(mLocalSerializer.get());
        scripts->setSavedDataDeserializer(mLocalLoader.get());
        scripts->load(data);
    }

    void LuaManager::reloadAllScriptsImpl()
    {
        Log(Debug::Info) << "Reload Lua";

        LuaUi::clearGameInterface();
        LuaUi::clearMenuInterface();
        LuaUi::clearSettings();
        MWBase::Environment::get().getWindowManager()->setConsoleMode("");
        MWBase::Environment::get().getL10nManager()->dropCache();
        mUiResourceManager.clear();
        mLua.dropScriptCache();
        mInputActions.clear(true);
        mInputTriggers.clear(true);

        ESM::LuaScripts globalData;

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.setSavedDataDeserializer(mGlobalSerializer.get());
            mGlobalScripts.save(globalData);
            mGlobalStorage.clearTemporaryAndRemoveCallbacks();
        }

        std::unordered_map<ESM::RefNum, ESM::LuaScripts> localData;

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->setSavedDataDeserializer(mLocalSerializer.get());
            ESM::LuaScripts data;
            scripts->save(data);
            localData[id] = std::move(data);
        }

        initConfiguration(true);

        mMenuScripts.removeAllScripts();

        mPlayerStorage.clearTemporaryAndRemoveCallbacks();

        mMenuScripts.addAutoStartedScripts();

        for (const auto& [id, ptr] : MWBase::Environment::get().getWorldModel()->getPtrRegistryView())
        {
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            scripts->load(localData[id]);
        }

        for (LocalScripts* scripts : mActiveLocalScripts)
            scripts->setActive(true);

        if (mGlobalScriptsStarted)
        {
            mGlobalScripts.load(globalData);
        }
    }

    void LuaManager::handleConsoleCommand(
        const std::string& consoleMode, const std::string& command, const MWWorld::Ptr& selectedPtr)
    {
        PlayerScripts* playerScripts = nullptr;
        if (!mPlayer.isEmpty())
            playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        bool processed = mMenuScripts.consoleCommand(consoleMode, command);
        if (playerScripts)
        {
            sol::object selected = sol::nil;
            if (!selectedPtr.isEmpty())
                mLua.protectedCall([&](LuaUtil::LuaView& view) {
                    selected = sol::make_object(view.sol(), LObject(getId(selectedPtr)));
                });
            if (playerScripts->consoleCommand(consoleMode, command, selected))
                processed = true;
        }
        if (!processed)
            MWBase::Environment::get().getWindowManager()->printToConsole(
                "No Lua handlers for console\n", MWBase::WindowManager::sConsoleColor_Error);
    }

    void LuaManager::receiveGlobalEvent(std::string eventName, std::string eventData)
    {
        std::lock_guard<std::mutex> lock(mInboundGlobalEventsMutex);
        mInboundGlobalEvents.push_back({ std::move(eventName), std::move(eventData) });
    }

    void LuaManager::receiveGlobalStorageSnapshot(std::vector<MWBase::LuaManager::GlobalStorageValue> values)
    {
        if (!mInitialized)
            return;

        mGlobalStorageMirroredFromServer = true;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table existingSections = mGlobalStorage.getAllSections(view.sol().lua_state());
            for (const auto& [sectionName, _] : existingSections)
                mGlobalStorage.setSectionValues(LuaUtil::cast<std::string>(sectionName), sol::nullopt);

            std::map<std::string, sol::table> sections;
            for (const auto& value : values)
            {
                auto [it, inserted] = sections.emplace(value.mSection, sol::table(view.sol(), sol::create));
                it->second[value.mKey] = LuaUtil::deserialize(
                    view.sol().lua_state(), value.mValue, mGlobalSerializer.get());
            }

            for (const auto& [section, sectionValues] : sections)
            {
                sol::optional<sol::table> optionalValues = sectionValues;
                mGlobalStorage.setSectionValues(section, optionalValues);
            }
        });
    }

    void LuaManager::receiveGlobalStorageDelta(MWBase::LuaManager::GlobalStorageValue value)
    {
        if (!mInitialized)
            return;

        mGlobalStorageMirroredFromServer = true;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::object object = LuaUtil::deserialize(view.sol().lua_state(), value.mValue, mGlobalSerializer.get());
            mGlobalStorage.setSingleValue(value.mSection, value.mKey, object);
        });
    }

    void LuaManager::receiveGlobalStorageSection(
        std::string section, std::vector<MWBase::LuaManager::GlobalStorageValue> values)
    {
        if (!mInitialized)
            return;

        mGlobalStorageMirroredFromServer = true;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            if (values.empty())
            {
                mGlobalStorage.setSectionValues(section, sol::nullopt);
                return;
            }

            sol::table sectionValues(view.sol(), sol::create);
            for (const auto& value : values)
            {
                sectionValues[value.mKey] = LuaUtil::deserialize(
                    view.sol().lua_state(), value.mValue, mGlobalSerializer.get());
            }
            sol::optional<sol::table> optionalValues = sectionValues;
            mGlobalStorage.setSectionValues(section, optionalValues);
        });
    }

    LuaManager::DelayedAction::DelayedAction(LuaUtil::LuaState* state, std::function<void()> fn, std::string_view name)
        : mFn(std::move(fn))
        , mName(name)
    {
        if (Settings::lua().mLuaDebug)
            mCallerTraceback = state->debugTraceback();
    }

    void LuaManager::DelayedAction::apply() const
    {
        try
        {
            mFn();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Error in DelayedAction " << mName << ": " << e.what();

            if (mCallerTraceback.empty())
                Log(Debug::Error) << "Set 'lua debug=true' in settings.cfg to enable action tracebacks";
            else
                Log(Debug::Error) << "Caller " << mCallerTraceback;
        }
    }

    void LuaManager::addAction(std::function<void()> action, std::string_view name)
    {
        if (mApplyingDelayedActions)
            throw std::runtime_error("DelayedAction is not allowed to create another DelayedAction");
        mActionQueue.emplace_back(&mLua, std::move(action), name);
    }

    void LuaManager::addTeleportPlayerAction(std::function<void()> action)
    {
        mTeleportPlayerAction = DelayedAction(&mLua, std::move(action), "TeleportPlayer");
    }

    void LuaManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Lua UsedMemory", static_cast<double>(mLua.getTotalMemoryUsage()));
    }

    std::string LuaManager::formatResourceUsageStats() const
    {
        if (!LuaUtil::LuaState::isProfilerEnabled())
            return "Lua profiler is disabled";

        std::stringstream out;

        constexpr unsigned nameW = 50;
        constexpr int valueW = 12;

        auto outMemSize = [&](size_t bytes) {
            constexpr size_t limit = 10000;
            out << std::right << std::setw(valueW - 3);
            if (bytes < limit)
                out << bytes << " B ";
            else if (bytes < limit * 1024)
                out << (bytes / 1024) << " KB";
            else if (bytes < limit * 1024 * 1024)
                out << (bytes / (1024 * 1024)) << " MB";
            else
                out << (bytes / (1024 * 1024 * 1024)) << " GB";
        };

        const uint64_t smallAllocSize = Settings::lua().mSmallAllocMaxSize;
        out << "Total memory usage:";
        outMemSize(mLua.getTotalMemoryUsage());
        out << "\n";
        out << "LuaUtil::ScriptsContainer count: " << LuaUtil::ScriptsContainer::getInstanceCount() << "\n";
        out << "\n";
        out << "small alloc max size = " << smallAllocSize << " (section [Lua] in settings.cfg)\n";
        out << "Smaller values give more information for the profiler, but increase performance overhead.\n";
        out << "  Memory allocations <= " << smallAllocSize << " bytes:";
        outMemSize(mLua.getSmallAllocMemoryUsage());
        out << " (not tracked)\n";
        out << "  Memory allocations >  " << smallAllocSize << " bytes:";
        outMemSize(mLua.getTotalMemoryUsage() - mLua.getSmallAllocMemoryUsage());
        out << " (see the table below)\n\n";

        using Stats = LuaUtil::ScriptsContainer::ScriptStats;

        std::vector<Stats> activeStats;
        mGlobalScripts.collectStats(activeStats);
        for (LocalScripts* scripts : mActiveLocalScripts)
            scripts->collectStats(activeStats);

        std::vector<Stats> selectedStats;
        MWWorld::Ptr selectedPtr = MWBase::Environment::get().getWindowManager()->getConsoleSelectedObject();
        LocalScripts* selectedScripts = nullptr;
        if (!selectedPtr.isEmpty())
        {
            selectedScripts = selectedPtr.getRefData().getLuaScripts();
            if (selectedScripts)
                selectedScripts->collectStats(selectedStats);
            out << "Profiled object (selected in the in-game console): " << selectedPtr.toString() << "\n";
        }
        else
            out << "No selected object. Use the in-game console to select an object for detailed profile.\n";
        out << "\n";

        out << "Legend\n";
        out << "  ops:        Averaged number of Lua instruction per frame;\n";
        out << "  memory:     Aggregated size of Lua allocations > " << smallAllocSize << " bytes;\n";
        out << "  [all]:      Sum over all instances of each script;\n";
        out << "  [active]:   Sum over all active (i.e. currently in scene) instances of each script;\n";
        out << "  [inactive]: Sum over all inactive instances of each script;\n";
        out << "  [for selected object]: Only for the object that is selected in the console;\n";
        out << "\n";

        out << std::left;
        out << " " << std::setw(nameW + 2) << "*** Resource usage per script";
        out << std::right;
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "memory";
        out << std::setw(valueW) << "ops";
        out << std::setw(valueW) << "memory";
        out << "\n";
        out << std::left << " " << std::setw(nameW + 2) << "[name]" << std::right;
        out << std::setw(valueW) << "[all]";
        out << std::setw(valueW) << "[active]";
        out << std::setw(valueW) << "[inactive]";
        out << std::setw(valueW * 2) << "[for selected object]";
        out << "\n";

        for (size_t i = 0; i < mConfiguration.size(); ++i)
        {
            bool isGlobal = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sGlobal;
            bool isMenu = mConfiguration[i].mFlags & ESM::LuaScriptCfg::sMenu;

            out << std::left;
            out << " " << std::setw(nameW) << mConfiguration[i].mScriptPath.value();
            if (mConfiguration[i].mScriptPath.value().size() > nameW)
                out << "\n " << std::setw(nameW) << ""; // if path is too long, break line
            out << std::right;
            out << std::setw(valueW) << static_cast<int64_t>(activeStats[i].mAvgInstructionCount);
            outMemSize(static_cast<size_t>(activeStats[i].mMemoryUsage));
            outMemSize(mLua.getMemoryUsageByScriptIndex(static_cast<unsigned>(i))
                - static_cast<uint64_t>(activeStats[i].mMemoryUsage));

            if (isGlobal)
                out << std::setw(valueW * 2) << "NA (global script)";
            else if (isMenu && (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i))))
                out << std::setw(valueW * 2) << "NA (menu script)";
            else if (selectedPtr.isEmpty())
                out << std::setw(valueW * 2) << "NA (not selected) ";
            else if (!selectedScripts || !selectedScripts->hasScript(static_cast<int>(i)))
                out << std::setw(valueW * 2) << "NA";
            else
            {
                out << std::setw(valueW) << static_cast<int64_t>(selectedStats[i].mAvgInstructionCount);
                outMemSize(static_cast<size_t>(selectedStats[i].mMemoryUsage));
            }
            out << "\n";
        }

        return out.str();
    }
}
