#include "MpNetworkBridge.hpp"

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaEvent.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwlua/context.hpp"
#include "../mwlua/object.hpp"
#include "Main.hpp"
#include "network/Client.hpp"
#include "sync/ActorSync.hpp"
#include "sync/WorldObjectSync.hpp"

namespace mwmp
{
    namespace
    {
        template <class ObjectT>
        sol::optional<uint32_t> getObjectMpNum(const ObjectT& object)
        {
            if (!Main::isInitialised())
                return sol::nullopt;

            const MWWorld::Ptr& ptr = object.ptrOrEmpty();
            if (ptr.isEmpty())
                return sol::nullopt;

            const uint32_t mpNum = Main::get().getWorldObjectSync().getMpNumForObject(ptr);
            if (mpNum == 0)
                return sol::nullopt;

            return mpNum;
        }
    }

    void MpNetworkBridge::queueInbound(LuaEvent event)
    {
        std::lock_guard<std::mutex> lock(mInboundMutex);
        mInboundEvents.push_back(std::move(event));
    }

    void MpNetworkBridge::queueStorage(
        LuaStorageAction action, std::string section, std::vector<LuaStorageEntry> entries)
    {
        std::lock_guard<std::mutex> lock(mStorageMutex);
        mStorageUpdates.push_back({ action, std::move(section), std::move(entries) });
    }

    void MpNetworkBridge::queueOutbound(std::string eventName, LuaUtil::BinaryData eventData)
    {
        std::lock_guard<std::mutex> lock(mOutboundMutex);
        mOutboundEvents.push_back({ 0, std::move(eventName), std::move(eventData) });
    }

    void MpNetworkBridge::processIncoming(MWBase::LuaManager& luaManager)
    {
        std::vector<LuaEvent> events;
        {
            std::lock_guard<std::mutex> lock(mInboundMutex);
            events.swap(mInboundEvents);
        }

        std::vector<LuaStorageUpdate> storageUpdates;
        {
            std::lock_guard<std::mutex> lock(mStorageMutex);
            storageUpdates.swap(mStorageUpdates);
        }

        for (auto& update : storageUpdates)
        {
            std::vector<MWBase::LuaManager::GlobalStorageValue> values;
            values.reserve(update.entries.size());
            for (auto& entry : update.entries)
            {
                values.push_back({
                    std::move(entry.section),
                    std::move(entry.key),
                    std::move(entry.value),
                });
            }

            switch (update.action)
            {
                case LuaStorageAction::Snapshot:
                    luaManager.receiveGlobalStorageSnapshot(std::move(values));
                    break;
                case LuaStorageAction::Delta:
                    for (auto& value : values)
                        luaManager.receiveGlobalStorageDelta(std::move(value));
                    break;
                case LuaStorageAction::ResetSection:
                    luaManager.receiveGlobalStorageSection(std::move(update.section), std::move(values));
                    break;
            }
        }

        for (auto& event : events)
        {
            luaManager.receiveGlobalEvent(std::move(event.eventName), std::move(event.eventData));
        }
    }

    void MpNetworkBridge::drainOutgoing(NetworkClient& client)
    {
        std::vector<LuaEvent> events;
        {
            std::lock_guard<std::mutex> lock(mOutboundMutex);
            events.swap(mOutboundEvents);
        }

        for (auto& event : events)
        {
            PacketLuaEvent pkt;
            pkt.pid = event.pid;
            pkt.eventName = event.eventName;
            pkt.eventData = event.eventData;
            client.sendReliable(pkt.encode());
        }
    }

    sol::table initClientMpPackage(const MWLua::Context& context)
    {
        sol::state_view lua = context.sol();
        sol::table mp(lua, sol::create);

        mp.set_function("sendToServer", [context](std::string eventName, const sol::object& eventData) {
            if (!Main::isInitialised() || !Main::isConnected())
                return;

            Main::get().getNetworkBridge().queueOutbound(
                std::move(eventName), LuaUtil::serialize(eventData, context.mSerializer));
        });

        mp.set_function("isConnected", []() -> bool {
            return Main::isConnected();
        });

        mp.set_function("isServer", []() -> bool {
            return false;
        });

        mp.set_function("hasActorAuthority", [](const std::string& cellId) -> bool {
            return Main::isInitialised() && Main::isConnected()
                && Main::get().getActorSync().hasAuthority(cellId);
        });

        mp.set_function("getActorAuthorityCell", sol::overload(
            [](const MWLua::LObject& object) -> std::string {
                if (!Main::isInitialised() || !Main::isConnected())
                    return {};
                return Main::get().getActorSync().getActorAuthorityCellId(object.ptr());
            },
            [](const MWLua::GObject& object) -> std::string {
                if (!Main::isInitialised() || !Main::isConnected())
                    return {};
                return Main::get().getActorSync().getActorAuthorityCellId(object.ptr());
            }));

        mp.set_function("hasActorAuthorityForObject", sol::overload(
            [](const MWLua::LObject& object) -> bool {
                if (!Main::isInitialised() || !Main::isConnected())
                    return false;
                return Main::get().getActorSync().hasAuthorityForObject(object.ptr());
            },
            [](const MWLua::GObject& object) -> bool {
                if (!Main::isInitialised() || !Main::isConnected())
                    return false;
                return Main::get().getActorSync().hasAuthorityForObject(object.ptr());
            }));

        mp.set_function("hasActorAuthorityForMpNum", [](uint32_t mpNum, const std::string& cellId) -> bool {
            if (!Main::isInitialised() || !Main::isConnected())
                return false;
            return Main::get().getActorSync().hasAuthorityForMpNum(mpNum, cellId);
        });

        mp.set_function("getObjectMpNum", sol::overload(
            [](const MWLua::LObject& object) -> sol::optional<uint32_t> {
                return getObjectMpNum(object);
            },
            [](const MWLua::GObject& object) -> sol::optional<uint32_t> {
                return getObjectMpNum(object);
            }));

        return LuaUtil::makeReadOnly(mp);
    }
}
