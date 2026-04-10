#include "MpNetworkBridge.hpp"

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaEvent.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwlua/context.hpp"
#include "Main.hpp"
#include "network/Client.hpp"

namespace mwmp
{
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

        return LuaUtil::makeReadOnly(mp);
    }
}
