#include "Server.hpp"
#include "MasterServerClient.hpp"
#include "bcrypt.h"  // extern/bcrypt/bcrypt.h — password hashing wrapper

// GNS C++ crypto API — CECSigningPublicKey::VerifySignature for challenge-response auth.
// Include paths: extern/GameNetworkingSockets/src/common + src/public
#include <crypto_25519.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "bindings/PlayerBindings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <variant>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Packets/System/PacketGameSettings.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCharGen.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimFlags.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimPlay.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAttack.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCast.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerInventory.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerDeath.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerResurrect.hpp>
#include <components/openmw-mp/Packets/Player/PacketChatMessage.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaEvent.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectPlace.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectDelete.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectMove.hpp>
#include <components/openmw-mp/Packets/Object/PacketContainer.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAI.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimFlags.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimPlay.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAttack.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAuthority.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCast.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorDeath.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorEquipment.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorList.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPosition.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCombatRequest.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>
// PacketWorldWeather is defined in PacketWorldTime.hpp

// Encode/decode ESM::Class::CLDTstruct as 15 comma-separated ints.
// Format: specialization, attr[0], attr[1], skills[0..4][0..1], isPlayable, services
namespace
{
    constexpr unsigned char sLuaFormatVersion = 0;
    constexpr unsigned char sLuaShortStringFlag = 0x20;

    enum class SerializedLuaType : unsigned char
    {
        Number = 0x0,
        LongString = 0x1,
        Boolean = 0x2,
        TableStart = 0x3,
        TableEnd = 0x4,
    };

    struct LuaWireValue;
    using LuaWireTable = std::vector<std::pair<std::string, LuaWireValue>>;

    struct LuaWireValue
    {
        using Variant = std::variant<std::monostate, double, bool, std::string, LuaWireTable>;
        Variant value;

        LuaWireValue() = default;
        LuaWireValue(double v)
            : value(v)
        {
        }
        LuaWireValue(bool v)
            : value(v)
        {
        }
        LuaWireValue(std::string v)
            : value(std::move(v))
        {
        }
        LuaWireValue(const char* v)
            : value(std::string(v))
        {
        }
        LuaWireValue(LuaWireTable v)
            : value(std::move(v))
        {
        }
    };

    template <typename T>
    T readLuaValue(std::string_view& data)
    {
        if (data.size() < sizeof(T))
            throw std::runtime_error("Unexpected end of Lua event payload");

        T value;
        std::memcpy(&value, data.data(), sizeof(T));
        data.remove_prefix(sizeof(T));
        return value;
    }

    std::string readLuaString(std::string_view& data, std::size_t size)
    {
        if (data.size() < size)
            throw std::runtime_error("Unexpected end of Lua string payload");

        std::string value(data.substr(0, size));
        data.remove_prefix(size);
        return value;
    }

    LuaWireValue parseLuaWireValue(std::string_view& data)
    {
        if (data.empty())
            throw std::runtime_error("Unexpected end of Lua payload");

        const unsigned char type = static_cast<unsigned char>(data.front());
        data.remove_prefix(1);

        if (type & sLuaShortStringFlag)
            return LuaWireValue(readLuaString(data, type & 0x1f));

        switch (static_cast<SerializedLuaType>(type))
        {
            case SerializedLuaType::Number:
                return LuaWireValue(readLuaValue<double>(data));
            case SerializedLuaType::LongString:
                return LuaWireValue(readLuaString(data, readLuaValue<std::uint32_t>(data)));
            case SerializedLuaType::Boolean:
                return LuaWireValue(readLuaValue<char>(data) != 0);
            case SerializedLuaType::TableStart:
            {
                LuaWireTable table;
                while (!data.empty() && static_cast<unsigned char>(data.front()) != static_cast<unsigned char>(SerializedLuaType::TableEnd))
                {
                    LuaWireValue key = parseLuaWireValue(data);
                    LuaWireValue value = parseLuaWireValue(data);
                    if (const auto* stringKey = std::get_if<std::string>(&key.value))
                        table.emplace_back(*stringKey, std::move(value));
                }

                if (data.empty())
                    throw std::runtime_error("Unexpected end of Lua table payload");

                data.remove_prefix(1);
                return LuaWireValue(std::move(table));
            }
            case SerializedLuaType::TableEnd:
                throw std::runtime_error("Unexpected end-of-table marker in Lua payload");
        }

        throw std::runtime_error("Unsupported Lua payload type");
    }

    LuaWireTable parseLuaWireTable(const std::string& data)
    {
        std::string_view view(data);
        if (view.empty())
            return {};
        if (static_cast<unsigned char>(view.front()) != sLuaFormatVersion)
            throw std::runtime_error("Unsupported Lua payload format version");

        view.remove_prefix(1);
        LuaWireValue root = parseLuaWireValue(view);
        if (!view.empty())
            throw std::runtime_error("Unexpected trailing bytes in Lua payload");

        if (const auto* table = std::get_if<LuaWireTable>(&root.value))
            return *table;

        throw std::runtime_error("Expected table payload");
    }

    const LuaWireValue* findLuaField(const LuaWireTable& table, std::string_view key)
    {
        for (const auto& [field, value] : table)
        {
            if (field == key)
                return &value;
        }
        return nullptr;
    }

    const LuaWireTable* getLuaTableField(const LuaWireTable& table, std::string_view key)
    {
        const LuaWireValue* value = findLuaField(table, key);
        return value ? std::get_if<LuaWireTable>(&value->value) : nullptr;
    }

    std::string getLuaStringField(const LuaWireTable& table, std::string_view key, std::string defaultValue = {})
    {
        const LuaWireValue* value = findLuaField(table, key);
        if (!value)
            return defaultValue;
        if (const auto* str = std::get_if<std::string>(&value->value))
            return *str;
        return defaultValue;
    }

    double getLuaNumberField(const LuaWireTable& table, std::string_view key, double defaultValue = 0.0)
    {
        const LuaWireValue* value = findLuaField(table, key);
        if (!value)
            return defaultValue;
        if (const auto* number = std::get_if<double>(&value->value))
            return *number;
        return defaultValue;
    }

    bool getLuaBoolField(const LuaWireTable& table, std::string_view key, bool defaultValue = false)
    {
        const LuaWireValue* value = findLuaField(table, key);
        if (!value)
            return defaultValue;
        if (const auto* boolean = std::get_if<bool>(&value->value))
            return *boolean;
        return defaultValue;
    }

    void appendLuaBytes(std::string& out, const void* bytes, std::size_t size)
    {
        out.append(static_cast<const char*>(bytes), size);
    }

    template <typename T>
    void appendLuaPod(std::string& out, T value)
    {
        appendLuaBytes(out, &value, sizeof(T));
    }

    void serializeLuaWireValue(std::string& out, const LuaWireValue& value);

    void appendLuaString(std::string& out, std::string_view value)
    {
        if (value.size() < 32)
        {
            out.push_back(static_cast<char>(sLuaShortStringFlag | static_cast<unsigned char>(value.size())));
        }
        else
        {
            out.push_back(static_cast<char>(SerializedLuaType::LongString));
            appendLuaPod<std::uint32_t>(out, static_cast<std::uint32_t>(value.size()));
        }

        out.append(value.data(), value.size());
    }

    void serializeLuaWireTable(std::string& out, const LuaWireTable& table)
    {
        out.push_back(static_cast<char>(SerializedLuaType::TableStart));
        for (const auto& [key, value] : table)
        {
            appendLuaString(out, key);
            serializeLuaWireValue(out, value);
        }
        out.push_back(static_cast<char>(SerializedLuaType::TableEnd));
    }

    void serializeLuaWireValue(std::string& out, const LuaWireValue& value)
    {
        if (std::holds_alternative<std::monostate>(value.value))
            throw std::runtime_error("Can not serialize nil into ActivateResult payload");

        if (const auto* number = std::get_if<double>(&value.value))
        {
            out.push_back(static_cast<char>(SerializedLuaType::Number));
            appendLuaPod<double>(out, *number);
            return;
        }

        if (const auto* boolean = std::get_if<bool>(&value.value))
        {
            out.push_back(static_cast<char>(SerializedLuaType::Boolean));
            out.push_back(*boolean ? 1 : 0);
            return;
        }

        if (const auto* stringValue = std::get_if<std::string>(&value.value))
        {
            appendLuaString(out, *stringValue);
            return;
        }

        if (const auto* table = std::get_if<LuaWireTable>(&value.value))
        {
            serializeLuaWireTable(out, *table);
            return;
        }

        throw std::runtime_error("Unsupported ActivateResult payload value");
    }

    std::string serializeLuaWireTable(const LuaWireTable& table)
    {
        std::string out;
        out.push_back(static_cast<char>(sLuaFormatVersion));
        serializeLuaWireTable(out, table);
        return out;
    }

    std::string encodeClassData(const ESM::Class::CLDTstruct& d)
    {
        std::ostringstream ss;
        ss << d.mSpecialization
           << ',' << d.mAttribute[0] << ',' << d.mAttribute[1];
        for (const auto& row : d.mSkills)
            for (auto v : row)
                ss << ',' << v;
        ss << ',' << d.mIsPlayable << ',' << d.mServices;
        return ss.str();
    }

    void decodeClassData(const std::string& s, ESM::Class::CLDTstruct& d)
    {
        if (s.empty()) return;
        std::istringstream ss(s);
        char comma;
        ss >> d.mSpecialization
           >> comma >> d.mAttribute[0] >> comma >> d.mAttribute[1];
        for (auto& row : d.mSkills)
            for (auto& v : row)
                ss >> comma >> v;
        ss >> comma >> d.mIsPlayable >> comma >> d.mServices;
    }

    std::string makeContainerKey(const std::string& cellId,
                                 const std::string& refId,
                                 uint32_t refNum)
    {
        return cellId + "|" + refId + "|" + std::to_string(refNum);
    }

    void appendOrMergeContainerItem(std::vector<mwmp::ContainerItem>& items, const mwmp::ContainerItem& item)
    {
        if (item.refId.empty() || item.count <= 0)
            return;

        auto it = std::find_if(items.begin(), items.end(),
            [&](const mwmp::ContainerItem& current)
            {
                return current.refId == item.refId && current.charge == item.charge;
            });

        if (it == items.end())
            items.push_back(item);
        else
            it->count += item.count;
    }

    void normalizeContainerItems(std::vector<mwmp::ContainerItem>& items)
    {
        std::vector<mwmp::ContainerItem> normalized;
        normalized.reserve(items.size());

        for (const auto& item : items)
            appendOrMergeContainerItem(normalized, item);

        items = std::move(normalized);
    }

    void applyContainerDelta(std::vector<mwmp::ContainerItem>& items,
                             const mwmp::ContainerItem& item,
                             mwmp::ContainerAction action)
    {
        if (item.refId.empty() || item.count <= 0)
            return;

        auto it = std::find_if(items.begin(), items.end(),
            [&](const mwmp::ContainerItem& current)
            {
                return current.refId == item.refId && current.charge == item.charge;
            });

        if (action == mwmp::ContainerAction::Add)
        {
            if (it == items.end())
                items.push_back(item);
            else
                it->count += item.count;
            return;
        }

        if (action != mwmp::ContainerAction::Remove)
            return;

        int remaining = item.count;

        if (it != items.end())
        {
            const int removed = std::min(it->count, remaining);
            it->count -= removed;
            remaining -= removed;
            if (it->count <= 0)
                items.erase(it);
        }

        for (auto current = items.begin(); remaining > 0 && current != items.end();)
        {
            if (current->refId != item.refId)
            {
                ++current;
                continue;
            }

            const int removed = std::min(current->count, remaining);
            current->count -= removed;
            remaining -= removed;

            if (current->count <= 0)
                current = items.erase(current);
            else
                ++current;
        }
    }

    std::string makeCellKey(const mwmp::CellId& cell)
    {
        if (!cell.isExterior)
            return cell.cellName;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "EXT:%d,%d", cell.gridX, cell.gridY);
        return buf;
    }

    std::optional<mwmp::CellId> parseCellKey(std::string_view cellId)
    {
        if (cellId.empty())
            return std::nullopt;

        mwmp::CellId parsed;
        if (cellId.rfind("EXT:", 0) == 0)
        {
            int gridX = 0;
            int gridY = 0;
            if (std::sscanf(cellId.data(), "EXT:%d,%d", &gridX, &gridY) != 2)
                return std::nullopt;

            parsed.isExterior = true;
            parsed.gridX = gridX;
            parsed.gridY = gridY;
            return parsed;
        }

        parsed.cellName = std::string(cellId);
        return parsed;
    }

    bool cellMatches(const mwmp::CellId& playerCell, const std::string& cellId)
    {
        if (cellId.rfind("EXT:", 0) == 0)
        {
            int gridX = 0;
            int gridY = 0;
            if (std::sscanf(cellId.c_str(), "EXT:%d,%d", &gridX, &gridY) != 2)
                return false;

            return playerCell.isExterior
                && playerCell.gridX == gridX
                && playerCell.gridY == gridY;
        }

        return !playerCell.isExterior && playerCell.cellName == cellId;
    }

    std::string makeActorKey(const mwmp::BaseActor& actor)
    {
        return actor.refId + "|" + std::to_string(actor.refNum) + "|" + std::to_string(actor.mpNum);
    }

    uint64_t currentServerTimeMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

namespace mwmp
{

MPServer* MPServer::sInstance = nullptr;

// ---------------------------------------------------------------------------
double MPServer::getUptime() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now() - mStartTime).count();
}

// ---------------------------------------------------------------------------
void MPServer::broadcastServerMessage(const std::string& text)
{
    PacketChatMessage pkt;
    BasePlayer serverPlayer;
    serverPlayer.guid = 0;
    serverPlayer.name = "Server";
    pkt.setPlayer(&serverPlayer);
    pkt.message = text;
    pkt.channel = "";
    broadcastToAll(pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::broadcastServerMessageToCell(const std::string& cellId, const std::string& text)
{
    PacketChatMessage pkt;
    BasePlayer serverPlayer;
    serverPlayer.guid = 0;
    serverPlayer.name = "Server";
    pkt.setPlayer(&serverPlayer);
    pkt.message = text;
    pkt.channel = "";
    broadcastToCell(cellId, pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::broadcastGameSettingsToCell(const std::string& cellId)
{
    if (cellId.empty())
        return;

    for (auto& [conn, client] : mClients)
    {
        if (!client.handshakeComplete || !cellMatches(client.player.cell, cellId))
            continue;
        sendGameSettingsToClient(conn, cellId);
    }
}

void MPServer::broadcastGameSettingsToAllPlayers()
{
    for (auto& [conn, client] : mClients)
    {
        if (!client.handshakeComplete)
            continue;

        sendGameSettingsToClient(conn, makeCellKey(client.player.cell));
    }
}

void MPServer::sendGameSettingsToPlayer(uint32_t guid)
{
    for (auto& [conn, client] : mClients)
    {
        if (!client.handshakeComplete || client.guid != guid)
            continue;

        sendGameSettingsToClient(conn, makeCellKey(client.player.cell));
        return;
    }
}

bool MPServer::teleportPlayer(uint32_t guid, const std::string& cellId, const Position& position)
{
    ConnectedClient* client = findClientByGuid(guid);
    if (!client || !client->handshakeComplete || !client->charSelectComplete)
        return false;

    auto parsedCell = parseCellKey(cellId);
    if (!parsedCell)
        return false;

    const std::string oldCell = makeCellKey(client->player.cell);
    client->player.cell = *parsedCell;
    client->player.position = position;
    client->player.position.isTeleporting = true;
    client->player.velocity = {};

    const std::string newCell = makeCellKey(client->player.cell);
    syncLuaSnapshot();

    if (!oldCell.empty() && oldCell != newCell)
        refreshActorAuthorityForCell(oldCell);
    if (!newCell.empty())
        refreshActorAuthorityForCell(newCell, client->guid);

    if (oldCell != newCell)
    {
        Log(Debug::Info) << "[Server] Teleport " << client->name << " -> cell: " << newCell;
        mLua.onPlayerCellChange(client->guid, client->name, newCell, oldCell);

        PacketPlayerCellChange cellChange;
        cellChange.setPlayer(&client->player);
        const auto encoded = cellChange.encode();
        sendTo(client->conn, encoded);
        broadcastToAll(encoded, client->conn);

        if (!newCell.empty())
            sendCellStateToClient(client->conn, newCell);
    }
    else
    {
        PacketPlayerPosition pkt;
        pkt.setPlayer(&client->player);
        const auto encoded = pkt.encode();
        sendTo(client->conn, encoded);
        broadcastToAll(encoded, client->conn);
    }

    client->player.position.isTeleporting = false;
    return true;
}

bool MPServer::upsertPlayerMark(uint32_t guid, const PlayerMark& mark)
{
    ConnectedClient* client = findClientByGuid(guid);
    if (!client || !mPlayerDb || client->dbCharacterId == 0 || mark.name.empty() || mark.cell.empty())
        return false;

    mPlayerDb->upsertCharacterMark(client->dbCharacterId, mark);
    return true;
}

bool MPServer::deletePlayerMark(uint32_t guid, std::string_view name)
{
    ConnectedClient* client = findClientByGuid(guid);
    if (!client || !mPlayerDb || client->dbCharacterId == 0 || name.empty())
        return false;

    mPlayerDb->deleteCharacterMark(client->dbCharacterId, name);
    return true;
}

// ---------------------------------------------------------------------------
void MPServer::sendServerMessage(uint32_t guid, const std::string& text)
{
    for (auto& [conn, client] : mClients)
    {
        if (client.guid == guid && client.handshakeComplete)
        {
            PacketChatMessage pkt;
            BasePlayer serverPlayer;
            serverPlayer.guid = 0;
            serverPlayer.name = "Server";
            pkt.setPlayer(&serverPlayer);
            pkt.message = text;
            pkt.channel = "";
            sendTo(conn, pkt.encode());
            return;
        }
    }
}

// ---------------------------------------------------------------------------
void MPServer::relayPlayerChat(uint32_t guid, const std::string& text)
{
    ConnectedClient* c = findClientByGuid(guid);
    if (!c || !c->handshakeComplete)
        return;

    PacketChatMessage pkt;
    c->player.name = c->name;
    pkt.setPlayer(&c->player);
    pkt.message = text;
    pkt.channel = "";
    broadcastToAll(pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::broadcastLuaEvent(uint32_t pid, const std::string& eventName, const std::string& eventData)
{
    PacketLuaEvent pkt;
    pkt.pid = pid;
    pkt.eventName = eventName;
    pkt.eventData = eventData;
    broadcastToAll(pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::broadcastLuaEventToCell(
    const std::string& cellId, uint32_t pid, const std::string& eventName, const std::string& eventData)
{
    PacketLuaEvent pkt;
    pkt.pid = pid;
    pkt.eventName = eventName;
    pkt.eventData = eventData;
    broadcastToCell(cellId, pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::sendLuaEvent(
    uint32_t guid, uint32_t pid, const std::string& eventName, const std::string& eventData)
{
    ConnectedClient* client = findClientByGuid(guid);
    if (!client || !client->handshakeComplete)
        return;

    PacketLuaEvent pkt;
    pkt.pid = pid;
    pkt.eventName = eventName;
    pkt.eventData = eventData;
    sendTo(client->conn, pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::broadcastLuaStorage(
    LuaStorageAction action, const std::string& section, const std::vector<LuaStorageEntry>& entries)
{
    PacketLuaStorage pkt;
    pkt.action = action;
    pkt.section = section;
    pkt.entries = entries;
    broadcastToAll(pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::sendLuaStorage(uint32_t guid, LuaStorageAction action,
    const std::string& section, const std::vector<LuaStorageEntry>& entries)
{
    ConnectedClient* client = findClientByGuid(guid);
    if (!client || !client->charSelectComplete)
        return;

    PacketLuaStorage pkt;
    pkt.action = action;
    pkt.section = section;
    pkt.entries = entries;
    sendTo(client->conn, pkt.encode());
}

// ---------------------------------------------------------------------------
MPServer::MPServer(uint16_t port) : mPort(port)
{
    if (sInstance)
        throw std::runtime_error("MPServer: only one instance allowed");

    SteamDatagramErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg))
        throw std::runtime_error(std::string("GNS init failed: ") + errMsg);

    mInterface = SteamNetworkingSockets();
    if (!mInterface)
        throw std::runtime_error("MPServer: failed to get ISteamNetworkingSockets");

    sInstance  = this;
    mStartTime = std::chrono::steady_clock::now();
    Log(Debug::Info) << "[Server] Initialised";
}

// ---------------------------------------------------------------------------
MPServer::~MPServer()
{
    shutdown();
    GameNetworkingSockets_Kill();
    sInstance = nullptr;
}

// ---------------------------------------------------------------------------
void MPServer::run()
{
    // Create listen socket
    SteamNetworkingIPAddr listenAddr;
    listenAddr.Clear();
    listenAddr.m_port = mPort;

    SteamNetworkingConfigValue_t opts[2];
    opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&staticConnectionStatusChanged));
    // Allow connections without Steam certificate authentication (no Steam backend in dev)
    opts[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

    mListenSocket = mInterface->CreateListenSocketIP(listenAddr, 2, opts);
    if (mListenSocket == k_HSteamListenSocket_Invalid)
        throw std::runtime_error("MPServer: CreateListenSocketIP failed");

    mPollGroup = mInterface->CreatePollGroup();
    if (mPollGroup == k_HSteamNetPollGroup_Invalid)
        throw std::runtime_error("MPServer: CreatePollGroup failed");

    Log(Debug::Info) << "[Server] Listening on port " << mPort;

    // Open player database.
    try
    {
        mPlayerDb.emplace(mDbPath);
        loadPersistentWorldState();
        syncLuaAuthorityState();
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "[Server] PlayerDatabase failed to open: " << e.what();
        // Non-fatal — server runs without persistence if DB unavailable.
    }

    // If config.lua set Config.SPAWN_CELL, let it override the C++ default.
    // mDefaultSpawnCell may already be set by main.cpp (CLI flag); only
    // override when it is still the compiled-in default ("toddtest").
    {
        std::string raw = mLua.getString("Config", "SPAWN_CELL", "");
        if (!raw.empty())
        {
            // Normalise "x, y" coords: strip spaces that follow a comma so
            // findExteriorPosition / std::from_chars can parse the string.
            std::string norm;
            norm.reserve(raw.size());
            bool afterComma = false;
            for (char c : raw)
            {
                if      (c == ',')               { norm += c; afterComma = true;  }
                else if (c == ' ' && afterComma) { /* drop */                     }
                else                             { norm += c; afterComma = false; }
            }
            mDefaultSpawnCell = std::move(norm);
            Log(Debug::Info) << "[Server] Spawn cell set from config.lua: " << mDefaultSpawnCell;
        }
    }

    // Read Config.MAX_CHARS_PER_ACCOUNT from config.lua (0 = unlimited).
    mMaxCharsPerAccount = mLua.getInt("Config", "MAX_CHARS_PER_ACCOUNT", mMaxCharsPerAccount);
    Log(Debug::Info) << "[Server] Max chars per account: "
                     << (mMaxCharsPerAccount == 0 ? "unlimited" : std::to_string(mMaxCharsPerAccount));

    syncLuaSnapshot();
    mLua.start();
    mLua.onServerInit();

    // Register with the master server (async — does not block the tick loop).
    if (!mMasterUrl.empty())
    {
        MasterServerClient::Config cfg;
        cfg.masterUrl         = mMasterUrl;
        cfg.serverName        = mServerName;
        cfg.port              = mPort;
        cfg.maxPlayers        = mMaxPlayersConfig;
        cfg.version           = SERVER_VERSION;
        cfg.gameMode          = mGameMode;
        cfg.passwordProtected = mPasswordProtected;
        mMasterClient.registerAsync(cfg);
    }

    mRunning = true;
    using Clock = std::chrono::steady_clock;
    auto last   = Clock::now();

    while (mRunning)
    {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        mInterface->RunCallbacks();
        processIncomingMessages();
        tick(dt);
        mLua.drainOutbound();
        syncLuaSnapshot();

        // 20 Hz server tick
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    shutdown();
}

// ---------------------------------------------------------------------------
void MPServer::shutdown()
{
    mLua.stop();

    // Tell the master server we are gone immediately (synchronous, best-effort).
    mMasterClient.unregister();

    if (mListenSocket != k_HSteamListenSocket_Invalid)
    {
        // Close all client connections gracefully
        for (auto& [conn, client] : mClients)
            mInterface->CloseConnection(conn, 0, "Server shutdown", true);
        mClients.clear();

        mInterface->CloseListenSocket(mListenSocket);
        mListenSocket = k_HSteamListenSocket_Invalid;
    }
    if (mPollGroup != k_HSteamNetPollGroup_Invalid)
    {
        mInterface->DestroyPollGroup(mPollGroup);
        mPollGroup = k_HSteamNetPollGroup_Invalid;
    }
    Log(Debug::Info) << "[Server] Shutdown complete";
}

// ---------------------------------------------------------------------------
void MPServer::tick(float dt)
{
    // Advance world time — carry over into day/month/year when hour wraps.
    // 30-day months, 12-month year (Morrowind calendar approximation).
    mWorld.gameHour += (dt * mWorld.timeScale) / 3600.f;
    while (mWorld.gameHour >= 24.f)
    {
        mWorld.gameHour -= 24.f;
        if (++mWorld.day > 30)
        {
            mWorld.day = 1;
            if (++mWorld.month > 11)
            {
                mWorld.month = 0;
                ++mWorld.year;
            }
        }
    }

    // Periodic world-time broadcast so connected clients stay in sync.
    mWorld.timeSyncTimer += dt;
    if (mWorld.timeSyncTimer >= WorldState::TIME_SYNC_RATE)
    {
        mWorld.timeSyncTimer = 0.f;
        if (!mClients.empty())
            broadcastToAll(buildWorldTimePacket());
    }

    // Send a heartbeat to the master server at most once every 30 seconds.
    mMasterClient.tickHeartbeat(dt, getPlayerCount());
}

// ---------------------------------------------------------------------------
void MPServer::processIncomingMessages()
{
    static constexpr int MAX_MSGS = 512;
    ISteamNetworkingMessage* msgs[MAX_MSGS];

    int n = mInterface->ReceiveMessagesOnPollGroup(mPollGroup, msgs, MAX_MSGS);
    for (int i = 0; i < n; ++i)
    {
        auto* msg = msgs[i];
        auto  it  = mClients.find(msg->m_conn);
        if (it != mClients.end())
            onClientMessage(it->second,
                            static_cast<const uint8_t*>(msg->m_pData),
                            static_cast<size_t>(msg->m_cbSize));
        msg->Release();
    }
}

// ---------------------------------------------------------------------------
void MPServer::onClientConnected(HSteamNetConnection conn)
{
    if ((int)mClients.size() >= mMaxPlayersConfig)
    {
        mInterface->CloseConnection(conn, 0, "Server full", false);
        return;
    }

    ConnectedClient client;
    client.conn = conn;
    client.guid = mNextGuid++;
    mClients.emplace(conn, client);

    mInterface->SetConnectionPollGroup(conn, mPollGroup);
    Log(Debug::Info) << "[Server] Client connected, conn=" << conn;
}

// Note: OnPlayerConnect fires after handshake completes (in handleHandshake),
// not here — the client has no name yet at this point.

// ---------------------------------------------------------------------------
void MPServer::onClientDisconnected(HSteamNetConnection conn, const std::string& reason)
{
    auto it = mClients.find(conn);
    if (it == mClients.end()) return;

    const auto& client = it->second;
    const std::string actorCell = makeCellKey(client.player.cell);
    Log(Debug::Info) << "[Server] Client disconnected: "
                     << client.name << " (" << reason << ")";

    // Persist last known position before removing the client.
    if (mPlayerDb && client.dbCharacterId != 0 && client.charSelectComplete)
    {
        const auto& pos = client.player.position;
        try
        {
            Log(Debug::Info) << "[PlayerDB] savePosition: charId=" << client.dbCharacterId
                             << " cell='" << client.player.cell.cellName
                             << "' pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
            mPlayerDb->savePosition(client.dbCharacterId,
                                    client.player.cell.cellName,
                                    pos.pos[0], pos.pos[1], pos.pos[2],
                                    pos.rot[0], pos.rot[1], pos.rot[2]);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] savePosition error: " << e.what();
        }
    }

    if (client.charSelectComplete)
    {
        mLua.onPlayerDisconnect(client.guid, client.name, reason);

        // Notify all others
        PacketDisconnect pkt;
        pkt.guid   = client.guid;
        pkt.reason = reason;
        broadcastToAll(pkt.encode(), conn);
    }

    mInterface->CloseConnection(conn, 0, nullptr, false);
    mLua.clearPlayerMarks(client.guid);
    mLua.clearPlayerData(client.guid);
    mClients.erase(it);
    if (!actorCell.empty())
        refreshActorAuthorityForCell(actorCell);
    syncLuaSnapshot();
}

// ---------------------------------------------------------------------------
void MPServer::refreshActorAuthorityForCell(const std::string& cellId, uint32_t preferredGuid)
{
    if (cellId.empty())
        return;

    auto& cellState = mWorld.actorCells[cellId];
    uint32_t newAuthorityGuid = 0;

    auto isEligible = [&](const ConnectedClient& client)
    {
        return client.charSelectComplete && cellMatches(client.player.cell, cellId);
    };

    // Stability: keep the current authority owner as long as they are still in the cell.
    // A newly entering player must not steal authority from whoever already holds it.
    // The preferredGuid hint only takes effect when the cell has no active owner.
    if (cellState.authorityGuid != 0)
    {
        for (const auto& [conn, client] : mClients)
        {
            if (client.guid == cellState.authorityGuid && isEligible(client))
            {
                newAuthorityGuid = cellState.authorityGuid;
                break;
            }
        }
    }

    // Current authority is gone — try the preferred GUID first.
    if (newAuthorityGuid == 0 && preferredGuid != 0)
    {
        for (const auto& [conn, client] : mClients)
        {
            if (client.guid == preferredGuid && isEligible(client))
            {
                newAuthorityGuid = client.guid;
                break;
            }
        }
    }

    // No preferred or preferred not eligible — lowest GUID wins.
    if (newAuthorityGuid == 0)
    {
        for (const auto& [conn, client] : mClients)
        {
            if (!isEligible(client))
                continue;

            if (newAuthorityGuid == 0 || client.guid < newAuthorityGuid)
                newAuthorityGuid = client.guid;
        }
    }

    if (cellState.authorityGuid != newAuthorityGuid)
    {
        cellState.authorityGuid = newAuthorityGuid;
        ++cellState.authorityGeneration;
        Log(Debug::Info) << "[Server] Actor authority for " << cellId
                         << " -> guid=" << newAuthorityGuid
                         << " generation=" << cellState.authorityGeneration;
    }

    for (const auto& [conn, client] : mClients)
    {
        if (!isEligible(client))
            continue;

        sendActorAuthorityToClient(conn, cellId);
        sendActorStateToClient(conn, cellId);
    }
}

// ---------------------------------------------------------------------------
void MPServer::sendActorAuthorityToClient(HSteamNetConnection conn, const std::string& cellId)
{
    auto it = mWorld.actorCells.find(cellId);
    CellActorState emptyState;
    const CellActorState& state = (it != mWorld.actorCells.end()) ? it->second : emptyState;

    ActorList actorList;
    actorList.cellId = cellId;
    actorList.isAuthority = state.authorityGuid != 0;
    actorList.authorityGuid = state.authorityGuid;
    actorList.authorityGeneration = state.authorityGeneration;
    actorList.snapshotSequence = state.nextSnapshotSequence;
    actorList.serverTimestamp = currentServerTimeMs();

    PacketActorAuthority pkt;
    pkt.setActorList(&actorList);
    sendTo(conn, pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::sendActorStateToClient(HSteamNetConnection conn, const std::string& cellId)
{
    auto it = mWorld.actorCells.find(cellId);
    if (it == mWorld.actorCells.end() || it->second.actors.empty())
        return;

    const CellActorState& state = it->second;

    ActorList actorList;
    actorList.cellId = cellId;
    actorList.isAuthority = false;
    actorList.authorityGuid = state.authorityGuid;
    actorList.authorityGeneration = state.authorityGeneration;
    actorList.snapshotSequence = state.nextSnapshotSequence;
    actorList.serverTimestamp = currentServerTimeMs();

    actorList.actors.reserve(state.actors.size());
    for (const auto& [key, record] : state.actors)
        actorList.actors.push_back(record.actor);

    PacketActorList pkt;
    pkt.setActorList(&actorList);
    sendTo(conn, pkt.encode());
}

// ---------------------------------------------------------------------------
bool MPServer::validateActorUpdate(const ConnectedClient& c, const ActorList& actorList, const char* packetName)
{
    if (actorList.cellId.empty())
    {
        Log(Debug::Warning) << "[Server] Rejecting " << packetName << " from " << c.name
                            << " because the actor cellId is empty";
        return false;
    }

    if (!cellMatches(c.player.cell, actorList.cellId))
    {
        Log(Debug::Warning) << "[Server] Rejecting " << packetName << " from " << c.name
                            << " due to cell mismatch: player=" << makeCellKey(c.player.cell)
                            << " packet=" << actorList.cellId;
        return false;
    }

    auto cellIt = mWorld.actorCells.find(actorList.cellId);
    if (cellIt == mWorld.actorCells.end() || cellIt->second.authorityGuid == 0)
    {
        refreshActorAuthorityForCell(actorList.cellId, c.guid);
        cellIt = mWorld.actorCells.find(actorList.cellId);
    }

    if (cellIt == mWorld.actorCells.end() || cellIt->second.authorityGuid != c.guid)
    {
        const uint32_t authorityGuid = (cellIt != mWorld.actorCells.end()) ? cellIt->second.authorityGuid : 0;
        Log(Debug::Warning) << "[Server] Rejecting " << packetName << " from " << c.name
                            << " because guid=" << c.guid
                            << " is not actor authority for " << actorList.cellId
                            << " (authority=" << authorityGuid << ")";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
void MPServer::onClientMessage(ConnectedClient& client,
                               const uint8_t* data, size_t size)
{
    PacketHeader hdr;
    if (!BasePacket::peekHeader(data, size, hdr))
        return;

    auto type = static_cast<PacketType>(hdr.type);

    // Must complete handshake before any other packet is processed.
    if (!client.handshakeComplete
        && type != PacketType::Handshake
        && type != PacketType::ChallengeResponse)
    {
        Log(Debug::Warning) << "[Server] Pre-handshake packet from conn="
                            << client.conn << ", ignoring";
        return;
    }

    // Must select a character before any world/gameplay packets are processed.
    // CharacterSelect and PlayerCharGen are the only exceptions.
    if (client.handshakeComplete && !client.charSelectComplete
        && type != PacketType::CharacterSelect
        && type != PacketType::ChallengeResponse
        && type != PacketType::PlayerCharGen
        && type != PacketType::LinkKeyRequest    // allowed during charselect flow
        && type != PacketType::UnlinkKeyRequest  // allowed during charselect flow
        && type != PacketType::DeleteCharRequest  // allowed during charselect flow
        && type != PacketType::Handshake)
    {
        Log(Debug::Verbose) << "[Server] Pre-charselect packet type=" << (int)type
                            << " from " << client.name << ", ignoring";
        return;
    }

    switch (type)
    {
        case PacketType::Handshake:        handleHandshake(client, data, size);          break;
        case PacketType::CharacterSelect:  handleCharacterSelect(client, data, size);    break;
        case PacketType::ChallengeResponse:handleChallengeResponse(client, data, size); break;
        case PacketType::LinkKeyRequest:   handleLinkKeyRequest(client, data, size);    break;
        case PacketType::UnlinkKeyRequest: handleUnlinkKeyRequest(client, data, size);  break;
        case PacketType::DeleteCharRequest:handleDeleteCharRequest(client, data, size);  break;
        case PacketType::PlayerCharGen:    handlePlayerCharGen(client, data, size);      break;
        case PacketType::PlayerBaseInfo:   handlePlayerBaseInfo(client, data, size);     break;
        case PacketType::PlayerPosition:   handlePlayerPosition(client, data, size);     break;
        case PacketType::PlayerCellChange: handlePlayerCellChange(client, data, size);   break;
        case PacketType::PlayerEquipment:  handlePlayerEquipment(client, data, size);    break;
        case PacketType::PlayerAnimFlags:  handlePlayerAnimFlags(client, data, size);    break;
        case PacketType::PlayerAnimPlay:   handlePlayerAnimPlay(client, data, size);     break;
        case PacketType::PlayerAttack:     handlePlayerAttack(client, data, size);       break;
        case PacketType::PlayerCast:       handlePlayerCast(client, data, size);         break;
        case PacketType::PlayerInventory:  handlePlayerInventory(client, data, size);    break;
        case PacketType::PlayerStatsDynamic: handlePlayerStatsDynamic(client, data, size); break;
        case PacketType::PlayerDeath:      handlePlayerDeath(client, data, size);        break;
        case PacketType::PlayerResurrect:  handlePlayerResurrect(client, data, size);    break;
        case PacketType::ChatMessage:      handleChatMessage(client, data, size);        break;
        case PacketType::PacketLuaEvent:   handleLuaEvent(client, data, size);           break;
        case PacketType::ObjectPlace:      handleObjectPlace(client, data, size);        break;
        case PacketType::ObjectDelete:     handleObjectDelete(client, data, size);       break;
        case PacketType::ObjectMove:       handleObjectMove(client, data, size);         break;
        case PacketType::Container:        handleContainer(client, data, size);          break;
        case PacketType::DoorState:        handleDoorState(client, data, size);          break;
        case PacketType::WorldWeather:     handleWeather(client, data, size);            break;
        case PacketType::ActorList:        handleActorList(client, data, size);          break;
        case PacketType::ActorPosition:    handleActorPosition(client, data, size);      break;
        case PacketType::ActorAnimFlags:   handleActorAnimFlags(client, data, size);     break;
        case PacketType::ActorAnimPlay:    handleActorAnimPlay(client, data, size);      break;
        case PacketType::ActorAttack:      handleActorAttack(client, data, size);        break;
        case PacketType::ActorCast:        handleActorCast(client, data, size);          break;
        case PacketType::ActorDeath:       handleActorDeath(client, data, size);         break;
        case PacketType::ActorEquipment:   handleActorEquipment(client, data, size);     break;
        case PacketType::ActorStatsDynamic: handleActorStatsDynamic(client, data, size); break;
        case PacketType::ActorAI:          handleActorAI(client, data, size);            break;
        case PacketType::ActorCombatRequest: handleActorCombatRequest(client, data, size); break;
        default:
            Log(Debug::Verbose) << "[Server] Unhandled packet type " << hdr.type;
            break;
    }
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> MPServer::buildWorldTimePacket() const
{
    PacketWorldTime pkt;
    pkt.time.hour      = mWorld.gameHour;
    pkt.time.day       = mWorld.day;
    pkt.time.month     = mWorld.month;
    pkt.time.year      = mWorld.year;
    pkt.time.gameHour  = mWorld.gameHour;
    pkt.timeScale      = mWorld.timeScale;
    return pkt.encode();
}

// ---------------------------------------------------------------------------
void MPServer::loadPersistentWorldState()
{
    if (!mPlayerDb) return;

    uint32_t maxMpNum = 0;
    std::size_t objectCount = 0;

    for (const auto& object : mPlayerDb->loadWorldObjects())
    {
        maxMpNum = std::max(maxMpNum, object.mpNum);
        mWorld.placedObjects[object.cellId].push_back(object);
        ++objectCount;
    }

    for (const auto& record : mPlayerDb->loadContainerRecords())
    {
        ContainerRecord normalized = record;
        normalizeContainerItems(normalized.items);
        mWorld.containers[makeContainerKey(normalized.cellId, normalized.refId, normalized.refNum)] = std::move(normalized);
    }

    for (const auto& entry : mPlayerDb->loadDoorStates())
        mWorld.doorStates[entry.cellId].push_back(entry);

    mWorld.nextObjectMpNum = std::max<uint32_t>(1, maxMpNum + 1);

    Log(Debug::Info) << "[Server] Loaded persistent world state: objects="
                     << objectCount
                     << " containers=" << mWorld.containers.size()
                     << " doorCells=" << mWorld.doorStates.size();
}

// ---------------------------------------------------------------------------
void MPServer::sendCellStateToClient(HSteamNetConnection conn, const std::string& cellId)
{
    sendGameSettingsToClient(conn, cellId);
    sendActorAuthorityToClient(conn, cellId);
    sendActorStateToClient(conn, cellId);

    auto objectsIt = mWorld.placedObjects.find(cellId);
    if (objectsIt != mWorld.placedObjects.end())
    {
        for (const auto& object : objectsIt->second)
        {
            PacketObjectPlace pkt;
            pkt.object = object;
            sendTo(conn, pkt.encode());
        }
    }

    for (const auto& [key, record] : mWorld.containers)
    {
        if (record.cellId != cellId || !record.hasAuthority) continue;
        PacketContainer pkt;
        pkt.container = record;
        pkt.mAction = static_cast<uint8_t>(ContainerAction::Set);
        sendTo(conn, pkt.encode());
    }

    auto doorsIt = mWorld.doorStates.find(cellId);
    if (doorsIt != mWorld.doorStates.end() && !doorsIt->second.empty())
    {
        PacketDoorState pkt;
        pkt.authorGuid = 0;
        pkt.cellId = cellId;
        pkt.doors = doorsIt->second;
        sendTo(conn, pkt.encode());
    }
}

// ---------------------------------------------------------------------------
void MPServer::sendGameSettingsToClient(HSteamNetConnection conn, const std::string& cellId)
{
    if (cellId.empty())
        return;

    PacketGameSettings pkt;
    auto clientIt = mClients.find(conn);
    if (clientIt != mClients.end())
        pkt.settings = mLua.getEffectiveSurfPhysicsSettings(clientIt->second.guid, cellId);
    else
        pkt.settings = mLua.getCellSurfPhysicsSettings(cellId);
    sendTo(conn, pkt.encode());
}

// ---------------------------------------------------------------------------
void MPServer::handleHandshake(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketHandshake hs;
    if (!hs.decode(data, size))
    {
        mInterface->CloseConnection(c.conn, 0, "Bad handshake", false);
        return;
    }

    // Version check
    if (hs.clientVersion != SERVER_VERSION)
    {
        PacketHandshakeResponse rsp;
        rsp.accepted      = false;
        rsp.serverVersion = SERVER_VERSION;
        rsp.rejectReason  = "Version mismatch: server=" + std::string(SERVER_VERSION)
                          + " client=" + hs.clientVersion;
        sendTo(c.conn, rsp.encode());
        mInterface->CloseConnection(c.conn, 0, "Version mismatch", true);
        return;
    }

    // ── Ed25519 keypair path ──────────────────────────────────────────────────
    // If the client presents a public key and it maps to a known account,
    // issue a challenge instead of asking for a password.
    if (mPlayerDb && !hs.publicKey.empty())
    {
        try
        {
            const int64_t accountId = mPlayerDb->lookupAccountByKeypair(hs.publicKey);
            if (accountId >= 0)
            {
                // Recognised key — generate a random 32-byte challenge nonce.
                // Store the challenge in ConnectedClient so handleChallengeResponse
                // can verify the signature.
                std::memset(c.pendingChallenge, 0, 32);
#ifdef _WIN32
                typedef BOOLEAN (WINAPI *PfnRtlGenRandom)(void*, ULONG);
                static PfnRtlGenRandom rng = nullptr;
                if (!rng) rng = reinterpret_cast<PfnRtlGenRandom>(
                    GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036"));
                if (rng) rng(c.pendingChallenge, 32);
#else
                {   std::ifstream r("/dev/urandom", std::ios::binary);
                    r.read(reinterpret_cast<char*>(c.pendingChallenge), 32); }
#endif
                c.pendingPublicKey = hs.publicKey;
                // Store login name so the accept path (in handleChallengeResponse)
                // can set c.loginName.
                c.loginName = hs.playerName.empty()
                    ? mPlayerDb->getUsernameForAccount(accountId)
                    : hs.playerName;
                c.dbAccountId = accountId;

                PacketChallenge pkt;
                std::memcpy(pkt.nonce, c.pendingChallenge, 32);
                sendTo(c.conn, pkt.encode());
                Log(Debug::Info) << "[Auth] Keypair challenge sent to " << c.loginName;
                return; // wait for PacketChallengeResponse
            }
            // Unknown key — reject immediately with a clear message.
            // The client sent a keypair auth request (empty passwordHash) so
            // falling through to password auth would always fail with
            // "Incorrect password" which is misleading.
            Log(Debug::Warning) << "[Auth] Keypair not recognised for conn=" << c.conn;
            PacketHandshakeResponse rsp;
            rsp.accepted     = false;
            rsp.rejectReason = "Key not recognised on this server. Please log in with your password to re-link.";
            sendTo(c.conn, rsp.encode());
            mInterface->CloseConnection(c.conn, 0, "Unknown key", true);
            return;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[Auth] Keypair lookup error: " << e.what();
        }
    }

    // ── Authentication ────────────────────────────────────────────────────────
    if (mPlayerDb)
    {
        try
        {
            if (hs.isRegistration)
            {
                // Registration: username must not already exist
                if (mPlayerDb->lookupAccount(hs.playerName) >= 0)
                {
                    PacketHandshakeResponse rsp;
                    rsp.accepted     = false;
                    rsp.rejectReason = "Username '" + hs.playerName + "' is already taken.";
                    sendTo(c.conn, rsp.encode());
                    mInterface->CloseConnection(c.conn, 0, "Username taken", true);
                    return;
                }
                // Hash the password and create the account
                const std::string hash = Bcrypt::hash(hs.passwordHash);
                const int64_t accountId = mPlayerDb->createAccount(hs.playerName);
                mPlayerDb->setPasswordHash(accountId, hash);
                Log(Debug::Info) << "[Auth] Registered new account: " << hs.playerName;
            }
            else
            {
                // Login: account must exist and password must match
                const int64_t accountId = mPlayerDb->lookupAccount(hs.playerName);
                if (accountId < 0)
                {
                    PacketHandshakeResponse rsp;
                    rsp.accepted     = false;
                    rsp.rejectReason = "Account not found. Did you mean to register?";
                    sendTo(c.conn, rsp.encode());
                    mInterface->CloseConnection(c.conn, 0, "Account not found", true);
                    return;
                }
                const std::string storedHash = mPlayerDb->getPasswordHash(accountId);
                if (storedHash.empty() || !Bcrypt::verify(hs.passwordHash, storedHash))
                {
                    PacketHandshakeResponse rsp;
                    rsp.accepted     = false;
                    rsp.rejectReason = "Incorrect password.";
                    sendTo(c.conn, rsp.encode());
                    mInterface->CloseConnection(c.conn, 0, "Bad password", true);
                    return;
                }
                Log(Debug::Info) << "[Auth] Login verified: " << hs.playerName;
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[Auth] Auth error for " << hs.playerName << ": " << e.what();
            PacketHandshakeResponse rsp;
            rsp.accepted     = false;
            rsp.rejectReason = "Server authentication error. Please try again.";
            sendTo(c.conn, rsp.encode());
            mInterface->CloseConnection(c.conn, 0, "Auth error", true);
            return;
        }
    }

    // ── Accept — look up or create the player's character record ─────────────
    c.loginName         = hs.playerName;
    c.name              = hs.playerName;  // overwritten to charName after charselect
    c.player.guid       = c.guid;
    c.player.name       = hs.playerName;
    c.handshakeComplete = true;

    // Resolve account id — needed for CharacterList and later for CharacterSelect.
    if (mPlayerDb)
    {
        try { c.dbAccountId = mPlayerDb->lookupOrCreateAccount(hs.playerName); }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] account lookup error: " << e.what();
        }
    }

    // First player to complete handshake becomes the weather host.
    if (mWorld.hostGuid == 0)
        mWorld.hostGuid = c.guid;

    // Send the minimal handshake acceptance (no chargen data — that comes
    // via PacketCharacterData after the player picks a character).
    PacketHandshakeResponse rsp;
    rsp.accepted      = true;
    rsp.assignedGuid  = c.guid;
    rsp.serverVersion = SERVER_VERSION;
    sendTo(c.conn, rsp.encode());

    Log(Debug::Info) << "[Server] Handshake accepted: " << c.name
                     << " guid=" << c.guid;

    // Build and send the character list so the client can show the
    // CharacterSelectDialog with one row per character.
    PacketCharacterList charListPkt;
    if (mPlayerDb && c.dbAccountId > 0)
    {
        try
        {
            for (const auto& cs : mPlayerDb->listCharacters(c.dbAccountId))
            {
                CharacterEntry entry;
                entry.name      = cs.name;
                entry.race      = cs.race;
                entry.className = cs.className;
                entry.lastSeen  = cs.lastSeen;
                entry.isNew     = cs.isNew;
                charListPkt.characters.push_back(std::move(entry));
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] listCharacters error: " << e.what();
        }
    }
    sendTo(c.conn, charListPkt.encode());
    Log(Debug::Info) << "[Server] Sent " << charListPkt.characters.size()
                     << " character(s) to " << c.name;
}

// ---------------------------------------------------------------------------
void MPServer::handleCharacterSelect(ConnectedClient& c, const uint8_t* data, size_t size)
{
    if (!c.handshakeComplete)
        return;

    PacketCharacterSelect sel;
    if (!sel.decode(data, size)) return;

    // Reject empty names — the old "" = new shorthand is gone.
    if (sel.charName.empty())
    {
        PacketCharacterSelectError err;
        err.reason = "Character name cannot be empty.";
        sendTo(c.conn, err.encode());
        return;
    }

    // Basic name validation: 2–24 printable ASCII characters.
    if (sel.charName.size() < 2 || sel.charName.size() > 24
        || sel.charName.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyz"
               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
               "0123456789 '-") != std::string::npos)
    {
        PacketCharacterSelectError err;
        err.reason = "Invalid character name. Use 2-24 letters, numbers, spaces, hyphens, or apostrophes.";
        sendTo(c.conn, err.encode());
        return;
    }

    PacketCharacterData cdPkt;
    cdPkt.spawnCell = mDefaultSpawnCell;
    bool sendSavedInventory = false;
    bool sendSavedEquipment = false;

    for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        c.player.equipment[slot].slot = slot;

    if (mPlayerDb && c.dbAccountId > 0)
    {
        try
        {
            if (sel.isNew)
            {
                // Enforce per-account character limit (0 = unlimited).
                if (mMaxCharsPerAccount > 0)
                {
                    const auto existing = mPlayerDb->listCharacters(c.dbAccountId);
                    if ((int)existing.size() >= mMaxCharsPerAccount)
                    {
                        PacketCharacterSelectError err;
                        err.reason = "Character limit reached ("
                                   + std::to_string(mMaxCharsPerAccount)
                                   + " per account). Delete a character to create a new one.";
                        sendTo(c.conn, err.encode());
                        return;
                    }
                }

                // New character slot — name must not already exist on this account.
                if (mPlayerDb->characterNameTaken(c.dbAccountId, sel.charName))
                {
                    PacketCharacterSelectError err;
                    err.reason = "You already have a character named '" + sel.charName + "'.";
                    sendTo(c.conn, err.encode());
                    return;
                }
                const PlayerRecord rec = mPlayerDb->createCharacter(c.dbAccountId, sel.charName);
                c.dbCharacterId      = rec.characterId;
                cdPkt.isNewCharacter = true;
                cdPkt.spawnCell      = mDefaultSpawnCell;
                cdPkt.characterName  = sel.charName;
                Log(Debug::Info) << "[Server] New character slot '" << sel.charName
                                 << "' created for " << c.name;
            }
            else
            {
                // Existing character — check it isn't already in use by a live session.
                for (const auto& [existingConn, existingClient] : mClients)
                {
                    if (existingConn != c.conn
                        && existingClient.charSelectComplete
                        && existingClient.loginName == c.loginName
                        && existingClient.slotName == sel.charName)
                    {
                        PacketCharacterSelectError err;
                        err.reason = "'" + sel.charName + "' is already in use by another session.";
                        sendTo(c.conn, err.encode());
                        return;
                    }
                }

                // Look up by (account_id, name).
                auto rec = mPlayerDb->lookupCharacter(c.dbAccountId, sel.charName);
                if (!rec)
                {
                    PacketCharacterSelectError err;
                    err.reason = "Character '" + sel.charName + "' not found on this account.";
                    sendTo(c.conn, err.encode());
                    return;
                }
                c.dbCharacterId      = rec->characterId;
                cdPkt.isNewCharacter = rec->isNew;
                cdPkt.spawnCell      = rec->cell.empty() ? mDefaultSpawnCell : rec->cell;
                if (!rec->isNew)
                {
                    cdPkt.race      = rec->race;
                    cdPkt.headMesh  = rec->headMesh;
                    cdPkt.hairMesh  = rec->hairMesh;
                    cdPkt.isMale    = rec->isMale;
                    cdPkt.classId   = rec->classId;
                    cdPkt.className = rec->className;
                    cdPkt.birthSign = rec->birthSign;
                    cdPkt.classData = rec->classData;
                    cdPkt.spawnX    = rec->posX;
                    cdPkt.spawnY    = rec->posY;
                    cdPkt.spawnZ    = rec->posZ;
                    cdPkt.spawnRotX = rec->rotX;
                    cdPkt.spawnRotY = rec->rotY;
                    cdPkt.spawnRotZ = rec->rotZ;

                    if (rec->hasSavedInventory)
                    {
                        c.player.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
                        c.player.inventoryChanges.items = mPlayerDb->loadCharacterInventory(rec->characterId);
                        sendSavedInventory = true;
                    }

                    if (rec->hasSavedEquipment)
                    {
                        for (auto& slotEntry : c.player.equipment)
                            slotEntry.item = {};

                        for (const auto& entry : mPlayerDb->loadCharacterEquipment(rec->characterId))
                        {
                            if (entry.slot < 0 || entry.slot >= BasePlayer::NUM_EQUIPMENT_SLOTS)
                                continue;
                            c.player.equipment[entry.slot] = entry;
                        }
                        sendSavedEquipment = true;
                    }
                }
                cdPkt.characterName  = sel.charName;
                Log(Debug::Info) << "[Server] Character '" << sel.charName
                                 << "' selected for " << c.name
                                 << " (new=" << rec->isNew << ")";
            }
            mPlayerDb->touch(c.dbCharacterId);
            mLua.setPlayerMarks(c.guid, mPlayerDb->loadCharacterMarks(c.dbCharacterId));
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] CharacterSelect error: " << e.what();
            PacketCharacterSelectError err;
            err.reason = "Server error processing character selection.";
            sendTo(c.conn, err.encode());
            return;
        }
    }
    else
    {
        // No DB — run as new character (dev/offline mode).
        cdPkt.isNewCharacter  = true;
        cdPkt.characterName   = sel.charName;
        mLua.clearPlayerMarks(c.guid);
    }

    sendTo(c.conn, cdPkt.encode());
    c.charSelectComplete = true;

    // Update the display name now that the character slot is known.
    // slotName is the permanent DB key; name/player.name use nickname if set.
    if (!cdPkt.characterName.empty())
    {
        c.slotName = cdPkt.characterName;
        // Load nickname from the DB record (empty string if never set).
        if (mPlayerDb)
        {
            auto rec = mPlayerDb->lookupCharacter(c.dbAccountId, cdPkt.characterName);
            if (rec) c.nickname = rec->nickname;
        }
        const std::string displayName = c.nickname.empty() ? c.slotName : c.nickname;
        c.name        = displayName;
        c.player.name = displayName;
    }

    if (sendSavedInventory)
    {
        PacketPlayerInventory inventory;
        inventory.setPlayer(&c.player);
        sendTo(c.conn, inventory.encode());
    }

    if (sendSavedEquipment)
    {
        PacketPlayerEquipment equipment;
        equipment.setPlayer(&c.player);
        sendTo(c.conn, equipment.encode());
    }

    // Late-join catch-up: send state of all in-world players to the new joiner.
    for (auto& [existingConn, existingClient] : mClients)
    {
        if (existingConn == c.conn || !existingClient.charSelectComplete)
            continue;

        PacketPlayerBaseInfo baseInfo;
        baseInfo.setPlayer(&existingClient.player);
        sendTo(c.conn, baseInfo.encode());

        PacketPlayerCellChange cellChange;
        cellChange.setPlayer(&existingClient.player);
        sendTo(c.conn, cellChange.encode());

        PacketPlayerEquipment equipment;
        equipment.setPlayer(&existingClient.player);
        sendTo(c.conn, equipment.encode());

        if (existingClient.player.position.pos[0] != 0.f
            || existingClient.player.position.pos[1] != 0.f
            || existingClient.player.position.pos[2] != 0.f)
        {
            PacketPlayerPosition position;
            position.setPlayer(&existingClient.player);
            sendTo(c.conn, position.encode());
        }
    }

    if (!cdPkt.spawnCell.empty())
        refreshActorAuthorityForCell(cdPkt.spawnCell, c.guid);

    sendTo(c.conn, buildWorldTimePacket());
    if (!cdPkt.spawnCell.empty())
        sendCellStateToClient(c.conn, cdPkt.spawnCell);

    if (mWorld.hasWeather)
        sendTo(c.conn, buildWorldWeatherPacket());

    syncLuaSnapshot();
    mLua.requestGlobalStorageSnapshot(c.guid);
    mLua.onPlayerConnect(c.guid, c.name);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void MPServer::handlePlayerCharGen(ConnectedClient& c, const uint8_t* data, size_t size)
{
    // Decode the packet — it now carries the full chargen result.
    PacketPlayerCharGen pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    if (mPlayerDb && c.dbCharacterId != 0)
    {
        try
        {
            // Persist race/class/birthsign so they can be restored on next login.
            mPlayerDb->saveChargenData(c.dbCharacterId,
                c.player.race,
                c.player.headMesh,
                c.player.hairMesh,
                c.player.isMale,
                c.player.charClass.mId.serializeText(),
                c.player.charClass.mName,
                c.player.birthSign,
                encodeClassData(c.player.charClass.mData));

            mPlayerDb->markChargenComplete(c.dbCharacterId);
            Log(Debug::Info) << "[Server] Chargen complete for " << c.name
                             << " race=" << c.player.race
                             << " class=" << c.player.charClass.mId.toString()
                             << " birthSign=" << c.player.birthSign;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] chargen save error: " << e.what();
        }
    }
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerBaseInfo(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerBaseInfo pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    // Stamp the server-authoritative display name (nickname if set, else character
    // name) before rebroadcasting.  The client sends its raw character name in its
    // own forceFullSync, but other clients must always see the canonical c.name.
    // This also keeps c.player.name in sync so the late-join catch-up loop
    // (which re-encodes from existingClient.player) sends the right name too.
    c.player.name = c.name;

    // Re-encode with the corrected name so all receivers get the nickname.
    broadcastToAll(pkt.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerPosition(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer proposed = c.player;
    PacketPlayerPosition pkt;
    pkt.setPlayer(&proposed);
    if (!pkt.decode(data, size)) return;

    if (!validateMovement(c, proposed))
    {
        // Send correction back
        PacketPlayerPosition correction;
        correction.setPlayer(&c.player);
        sendTo(c.conn, correction.encode());
        return;
    }

    c.player.position = proposed.position;
    c.player.velocity = proposed.velocity;

    // Relay to all other clients (unreliable is fine — we use raw broadcast)
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerCellChange(ConnectedClient& c, const uint8_t* data, size_t size)
{
    std::string oldCell = makeCellKey(c.player.cell);
    PacketPlayerCellChange pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    const std::string newCell = makeCellKey(c.player.cell);
    Log(Debug::Info) << "[Server] " << c.name << " → cell: " << newCell;

    syncLuaSnapshot();
    mLua.onPlayerCellChange(c.guid, c.name, newCell, oldCell);

    if (!oldCell.empty() && oldCell != newCell)
        refreshActorAuthorityForCell(oldCell);
    if (!newCell.empty())
        refreshActorAuthorityForCell(newCell, c.guid);

    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
    const std::string cellKey = makeCellKey(c.player.cell);
    if (!cellKey.empty())
        sendCellStateToClient(c.conn, cellKey);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerEquipment(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer incoming = c.player;
    PacketPlayerEquipment pkt;
    pkt.setPlayer(&incoming);
    if (!pkt.decode(data, size)) return;
    c.player.equipment = incoming.equipment;

    if (mPlayerDb && c.dbCharacterId != 0)
    {
        try
        {
            std::vector<EquipmentItem> equipment(c.player.equipment.begin(), c.player.equipment.end());
            mPlayerDb->saveCharacterEquipment(c.dbCharacterId, equipment);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] saveCharacterEquipment error: " << e.what();
        }
    }

    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerAnimFlags(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerAnimFlags pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerAnimPlay(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerAnimPlay pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerAttack(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerAttack pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerCast(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerCast pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerInventory(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer incoming = c.player;
    PacketPlayerInventory pkt;
    pkt.setPlayer(&incoming);
    if (!pkt.decode(data, size)) return;

    using InventoryAction = BasePlayer::InventoryChanges::Action;
    auto sameStack = [](const Item& left, const Item& right) {
        return left.refId == right.refId
            && left.charge == right.charge
            && std::abs(left.enchantmentCharge - right.enchantmentCharge) < 0.001f
            && left.soul == right.soul;
    };

    if (c.player.inventoryChanges.action != InventoryAction::Set)
        c.player.inventoryChanges.action = InventoryAction::Set;

    if (incoming.inventoryChanges.action == InventoryAction::Set)
    {
        c.player.inventoryChanges = incoming.inventoryChanges;
    }
    else if (incoming.inventoryChanges.action == InventoryAction::Add)
    {
        for (const auto& item : incoming.inventoryChanges.items)
        {
            auto it = std::find_if(
                c.player.inventoryChanges.items.begin(),
                c.player.inventoryChanges.items.end(),
                [&](const Item& existing) { return sameStack(existing, item); });
            if (it != c.player.inventoryChanges.items.end())
                it->count += item.count;
            else
                c.player.inventoryChanges.items.push_back(item);
        }
    }
    else if (incoming.inventoryChanges.action == InventoryAction::Remove)
    {
        for (const auto& item : incoming.inventoryChanges.items)
        {
            auto it = std::find_if(
                c.player.inventoryChanges.items.begin(),
                c.player.inventoryChanges.items.end(),
                [&](const Item& existing) { return sameStack(existing, item); });
            if (it == c.player.inventoryChanges.items.end())
                continue;

            it->count -= item.count;
            if (it->count <= 0)
                c.player.inventoryChanges.items.erase(it);
        }
    }

    if (mPlayerDb && c.dbCharacterId != 0)
    {
        try
        {
            mPlayerDb->saveCharacterInventory(c.dbCharacterId, c.player.inventoryChanges.items);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] saveCharacterInventory error: " << e.what();
        }
    }

    syncLuaSnapshot();
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerStatsDynamic pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    syncLuaSnapshot();
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerDeath(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerDeath pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    c.player.isDead = true;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);

    Log(Debug::Info) << "[Server] Relayed PlayerDeath for " << c.name
                     << " anim='" << c.player.deathAnimationGroup << "'"
                     << " killerGuid=" << pkt.killerGuid
                     << " killerRefId='" << pkt.killerRefId << "'";
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerResurrect(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerResurrect pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    c.player.isDead = false;
    c.player.deathAnimationGroup.clear();
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);

    Log(Debug::Info) << "[Server] Relayed PlayerResurrect for " << c.name;
}

// ---------------------------------------------------------------------------
void MPServer::handleActorList(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorList pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorList")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    cellState.actors.clear();
    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        cellState.actors[makeActorKey(actor)] = { actor, incoming.serverTimestamp };
    }

    PacketActorList out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorPosition(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorPosition pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorPosition")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.position = actor.position;
        stored.actor.velocity = actor.velocity;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorPosition out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAnimFlags(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorAnimFlags pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorAnimFlags")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.animFlags = actor.animFlags;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorAnimFlags out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAnimPlay(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorAnimPlay pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorAnimPlay")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.animPlay = actor.animPlay;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorAnimPlay out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAttack(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorAttack pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    Log(Debug::Info) << "[Server] Received ActorAttack from " << c.name << " cellId=" << incoming.cellId;
    if (!validateActorUpdate(c, incoming, "ActorAttack")) return;
    Log(Debug::Info) << "[Server] ActorAttack from " << c.name << " cell=" << incoming.cellId << " actors=" << incoming.actors.size();

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.attack = actor.attack;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorAttack out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorAttack to cell=" << incoming.cellId;
}

// ---------------------------------------------------------------------------
void MPServer::handleActorCast(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorCast pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    Log(Debug::Info) << "[Server] Received ActorCast from " << c.name << " cellId=" << incoming.cellId;
    if (!validateActorUpdate(c, incoming, "ActorCast")) return;
    Log(Debug::Info) << "[Server] ActorCast from " << c.name << " cell=" << incoming.cellId << " actors=" << incoming.actors.size();

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.cast = actor.cast;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorCast out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorCast to cell=" << incoming.cellId;
}

// ---------------------------------------------------------------------------
void MPServer::handleActorDeath(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorDeath pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    Log(Debug::Info) << "[Server] Received ActorDeath from " << c.name << " cellId=" << incoming.cellId;
    if (!validateActorUpdate(c, incoming, "ActorDeath")) return;
    Log(Debug::Info) << "[Server] ActorDeath from " << c.name << " cell=" << incoming.cellId << " actors=" << incoming.actors.size();

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.deathState = actor.deathState;
        stored.actor.isDead = actor.isDead;
        stored.actor.isInstantDeath = actor.isInstantDeath;
        stored.actor.deathAnimGroup = actor.deathAnimGroup;
        stored.actor.dynamicStats.health.current = actor.dynamicStats.health.current;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorDeath out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorDeath to cell=" << incoming.cellId;
}

// ---------------------------------------------------------------------------
void MPServer::handleActorEquipment(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorEquipment pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorEquipment")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.equipment = actor.equipment;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorEquipment out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorStatsDynamic pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorStatsDynamic")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.dynamicStats = actor.dynamicStats;
        stored.actor.isDead = actor.isDead;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorStatsDynamic out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAI(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorAI pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorAI")) return;

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        auto& stored = cellState.actors[makeActorKey(actor)];
        stored.actor.refId = actor.refId;
        stored.actor.refNum = actor.refNum;
        stored.actor.mpNum = actor.mpNum;
        stored.actor.cellId = incoming.cellId;
        stored.actor.ai = actor.ai;
        stored.lastSnapshotTime = incoming.serverTimestamp;
    }

    PacketActorAI out;
    out.setActorList(&incoming);
    broadcastToCell(incoming.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorCombatRequest(ConnectedClient& c, const uint8_t* data, size_t size)
{
    // Decode the incoming request.
    ActorList incoming;
    PacketActorCombatRequest pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;

    // Tag with the sending client's guid before any routing decisions.
    incoming.authorityGuid = c.guid;

    PacketActorCombatRequest out;
    out.setActorList(&incoming);

    // NPC->player damage: routed by the cell authority to the victim player.
    // This must be checked BEFORE the authority guard because the sender IS
    // the authority in this case (authority forwards NPC hits to the victim).
    if (incoming.victimPlayerGuid != 0)
    {
        for (auto& [conn, client] : mClients)
        {
            if (client.guid == incoming.victimPlayerGuid)
            {
                sendTo(conn, out.encode(), true);
                Log(Debug::Info) << "[Server] Routed NpcPlayerDamage from guid=" << c.guid
                                 << " to victim guid=" << incoming.victimPlayerGuid;
                break;
            }
        }
        return;
    }

    // Normal non-authority->authority: find the cell and validate sender is not authority.
    auto cellIt = mWorld.actorCells.find(incoming.cellId);
    if (cellIt == mWorld.actorCells.end()) return;

    const uint32_t authorityGuid = cellIt->second.authorityGuid;
    if (authorityGuid == 0 || authorityGuid == c.guid)
        return; // sender already has authority or no one does

    // Route to cell authority.
    for (auto& [conn, client] : mClients)
    {
        if (client.guid == authorityGuid)
        {
            sendTo(conn, out.encode(), true);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> MPServer::buildWorldWeatherPacket() const
{
    PacketWorldWeather pkt;
    pkt.currentWeather   = mWorld.weatherCurrent;
    pkt.nextWeather      = mWorld.weatherNext;
    pkt.transitionFactor = mWorld.weatherTransition;
    pkt.regionName       = mWorld.weatherRegion;
    return pkt.encode();
}

// ---------------------------------------------------------------------------
void MPServer::handleWeather(ConnectedClient& c, const uint8_t* data, size_t size)
{
    // Only the host is trusted to report weather.
    // Ignore packets from any other client — they should not be sending these.
    if (c.guid != mWorld.hostGuid)
    {
        Log(Debug::Verbose) << "[Server] Ignoring weather from non-host " << c.name;
        return;
    }

    PacketWorldWeather pkt;
    if (!pkt.decode(data, size)) return;

    mWorld.weatherCurrent    = pkt.currentWeather;
    mWorld.weatherNext       = pkt.nextWeather;
    mWorld.weatherTransition = pkt.transitionFactor;
    mWorld.weatherRegion     = pkt.regionName;
    mWorld.hasWeather        = true;

    Log(Debug::Verbose) << "[Server] Weather from host " << c.name
                        << ": current=" << pkt.currentWeather
                        << " region=" << pkt.regionName;

    // Relay to all non-host clients.
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);

    mLua.onWorldWeather(
        mWorld.weatherRegion, mWorld.weatherCurrent, mWorld.weatherNext, mWorld.weatherTransition);
}

// ---------------------------------------------------------------------------
void MPServer::handleObjectPlace(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketObjectPlace pkt;
    if (!pkt.decode(data, size)) return;

    if (!acceptPlacedObject(pkt.object))
        return;

    Log(Debug::Info) << "[Server] ObjectPlace accepted: player=" << c.name
                     << " refId=" << pkt.object.refId
                     << " mpNum=" << pkt.object.mpNum
                     << " cell=" << pkt.object.cellId
                     << " count=" << pkt.object.count;

    sendTo(c.conn, pkt.encode());
    broadcastToCell(pkt.object.cellId, pkt.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleObjectDelete(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketObjectDelete pkt;
    if (!pkt.decode(data, size)) return;

    mLua.removePlacedObject(pkt.mpNum);

    auto objectsIt = mWorld.placedObjects.find(pkt.cellId);
    if (objectsIt != mWorld.placedObjects.end())
    {
        auto& objects = objectsIt->second;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [&](const PlacedObject& object) { return object.mpNum == pkt.mpNum; }),
            objects.end());
        if (objects.empty())
            mWorld.placedObjects.erase(objectsIt);
    }

    if (mPlayerDb)
        mPlayerDb->deleteWorldObject(pkt.mpNum);

    broadcastToCell(pkt.cellId, std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleObjectMove(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketObjectMove pkt;
    if (!pkt.decode(data, size)) return;

    auto objectsIt = mWorld.placedObjects.find(pkt.cellId);
    if (objectsIt != mWorld.placedObjects.end())
    {
        for (auto& object : objectsIt->second)
        {
            if (object.mpNum != pkt.mpNum) continue;
            object.position = pkt.position;
            mLua.upsertPlacedObject(object);
            if (mPlayerDb)
                mPlayerDb->upsertWorldObject(object);
            break;
        }
    }

    broadcastToCell(pkt.cellId, std::vector<uint8_t>(data, data + size), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handleContainer(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketContainer pkt;
    if (!pkt.decode(data, size)) return;

    const auto action = static_cast<ContainerAction>(pkt.mAction);
    if (action != ContainerAction::Set
        && action != ContainerAction::Add
        && action != ContainerAction::Remove)
        return;

    if (pkt.container.cellId.empty() || pkt.container.refId.empty())
        return;

    if (!cellMatches(c.player.cell, pkt.container.cellId))
    {
        Log(Debug::Warning) << "[Server] Rejecting Container from " << c.name
                            << " due to cell mismatch: player="
                            << makeCellKey(c.player.cell)
                            << " packet=" << pkt.container.cellId;
        return;
    }

    const std::string key = makeContainerKey(pkt.container.cellId, pkt.container.refId, pkt.container.refNum);
    auto& authoritative = mWorld.containers[key];

    if (action == ContainerAction::Set)
    {
        normalizeContainerItems(pkt.container.items);

        if (authoritative.hasAuthority)
        {
            PacketContainer current;
            current.container = authoritative;
            current.mAction = static_cast<uint8_t>(ContainerAction::Set);
            sendTo(c.conn, current.encode());
            Log(Debug::Info) << "[Server] Container(Set replay): player=" << c.name
                             << " refId=" << authoritative.refId
                             << " refNum=" << authoritative.refNum
                             << " items=" << authoritative.items.size();
            return;
        }

        authoritative = pkt.container;
        authoritative.hasAuthority = true;
        if (mPlayerDb)
            mPlayerDb->upsertContainerRecord(authoritative);

        PacketContainer accepted;
        accepted.container = authoritative;
        accepted.mAction = static_cast<uint8_t>(ContainerAction::Set);
        sendTo(c.conn, accepted.encode());
        broadcastToCell(authoritative.cellId, accepted.encode(), c.conn);
        Log(Debug::Info) << "[Server] Container(Set accepted): player=" << c.name
                         << " refId=" << authoritative.refId
                         << " refNum=" << authoritative.refNum
                         << " items=" << authoritative.items.size();
        return;
    }

    if (!authoritative.hasAuthority)
    {
        Log(Debug::Verbose) << "[Server] Ignoring Container delta before Set from " << c.name
                            << " refId=" << pkt.container.refId
                            << " refNum=" << pkt.container.refNum;
        return;
    }

    authoritative.hasAuthority = true;
    if (authoritative.cellId.empty())
    {
        authoritative.cellId = pkt.container.cellId;
        authoritative.refId = pkt.container.refId;
        authoritative.refNum = pkt.container.refNum;
        authoritative.mpNum = pkt.container.mpNum;
    }

    normalizeContainerItems(pkt.container.items);
    for (const auto& item : pkt.container.items)
        applyContainerDelta(authoritative.items, item, action);

    normalizeContainerItems(authoritative.items);

    if (mPlayerDb)
        mPlayerDb->upsertContainerRecord(authoritative);

    const ContainerItem* firstDelta = pkt.container.items.empty() ? nullptr : &pkt.container.items.front();
    Log(Debug::Info) << "[Server] Container(" << static_cast<int>(action) << "): player=" << c.name
                     << " refId=" << authoritative.refId
                     << " refNum=" << authoritative.refNum
                     << " deltaItems=" << pkt.container.items.size()
                     << " totalItems=" << authoritative.items.size()
                     << " firstDeltaRefId=" << (firstDelta ? firstDelta->refId : "")
                     << " firstDeltaCount=" << (firstDelta ? firstDelta->count : 0)
                     << " firstDeltaCharge=" << (firstDelta ? firstDelta->charge : -999);
    broadcastToCell(authoritative.cellId, std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleDoorState(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketDoorState pkt;
    if (!pkt.decode(data, size)) return;

    Log(Debug::Verbose) << "[Server] DoorState from " << c.name
                        << " cell=" << pkt.cellId
                        << " doors=" << pkt.doors.size();

    // Store each entry as authoritative state, keyed by cellId.
    // Last write wins — the server is the authority.
    for (const auto& entry : pkt.doors)
    {
        auto& cellDoors = mWorld.doorStates[entry.cellId];

        // Update existing entry for this refId/refNum, or append.
        bool found = false;
        for (auto& existing : cellDoors)
        {
            if (existing.refId == entry.refId
                && (entry.refNum == 0 || existing.refNum == entry.refNum))
            {
                existing = entry;
                found = true;
                break;
            }
        }
        if (!found)
            cellDoors.push_back(entry);

        if (mPlayerDb)
            mPlayerDb->upsertDoorState(entry);
    }

    // Relay to all other clients so they apply the state immediately.
    broadcastToCell(pkt.cellId, std::vector<uint8_t>(data, data + size), c.conn);

    // Notify scripts — fire once per door entry.
    for (const auto& entry : pkt.doors)
        mLua.onDoorState(pkt.cellId, entry.refId, entry.isOpen);
}

// ---------------------------------------------------------------------------
void MPServer::handleChatMessage(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketChatMessage pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    // The client encodes its local player name into the packet, which may be
    // the slot name (before a nickname is set) rather than the current display
    // name. Re-assert the server-authoritative name so the relay uses the
    // nickname if one has been set, and to prevent the decode from corrupting
    // c.player.name for subsequent operations.
    c.player.name = c.name;

    Log(Debug::Info) << "[Server] Chat [" << c.name << "] "
                     << "(server time " << mWorld.gameHour << "h "
                     << "day=" << mWorld.day << " mo=" << mWorld.month
                     << " yr=" << mWorld.year << "): "
                     << pkt.message;

    if (mLua.isLoaded())
        mLua.onPlayerSendMessage(c.guid, c.name, pkt.message);
    else
        broadcastToAll(pkt.encode());  // re-encoded with authoritative name
}

// ---------------------------------------------------------------------------
void MPServer::handleLuaEvent(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketLuaEvent pkt;
    if (!pkt.decode(data, size)) return;

    Log(Debug::Verbose) << "[Server] LuaEvent from " << c.name
                        << " pid=" << c.guid
                        << " name=" << pkt.eventName
                        << " bytes=" << pkt.eventData.size();

    if (pkt.eventName == "Activate")
    {
        std::string error;
        const std::optional<LuaUtil::BinaryData> resultData = mLua.evaluateImmediateIntent(c.guid, pkt.eventName, pkt.eventData, &error);
        if (resultData)
        {
            mLua.drainOutbound();

            const std::string playerCell = makeCellKey(c.player.cell);
            if (!playerCell.empty())
                broadcastLuaEventToCell(playerCell, 0, "ActivateResult", *resultData);
            else
                broadcastLuaEvent(0, "ActivateResult", *resultData);

            try
            {
                const LuaWireTable result = parseLuaWireTable(*resultData);
                Log(Debug::Info) << "[Server] Immediate Activate seq=" << getLuaNumberField(result, "seq", 0.0)
                                 << " by " << c.name
                                 << " action=" << getLuaStringField(result, "action")
                                 << " object=" << getLuaStringField(result, "objectId")
                                 << " recordId=" << getLuaStringField(result, "objectRecordId")
                                 << " accepted=" << (getLuaBoolField(result, "accepted") ? "true" : "false")
                                 << " verified=" << (getLuaBoolField(result, "serverVerified") ? "true" : "false")
                                 << " reason=" << getLuaStringField(result, "reason")
                                 << " mutation=" << getLuaStringField(result, "mutation");
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "[Server] Immediate Activate log parse failed: " << e.what();
            }
            return;
        }

        Log(Debug::Warning) << "[Server] Immediate Activate failed for " << c.name
                            << ": " << (error.empty() ? "unknown" : error)
                            << "; falling back to queued Lua event";
    }

    mLua.onLuaEvent(c.guid, pkt.eventName, pkt.eventData);
}

// ---------------------------------------------------------------------------
bool MPServer::validateMovement(const ConnectedClient& /*c*/,
                                const BasePlayer& /*proposed*/) const
{
    // TODO: re-enable anti-cheat once position sync is stable.
    // The per-tick distance check was causing false positives during
    // load transitions and initial position sync.
    return true;
}

// ---------------------------------------------------------------------------
void MPServer::kickClient(uint32_t guid, const std::string& reason)
{
    for (auto& [conn, client] : mClients)
    {
        if (client.guid == guid)
        {
            mInterface->CloseConnection(conn, 0, reason.c_str(), true);
            return;
        }
    }
}

ConnectedClient* MPServer::findClientByGuid(uint32_t guid)
{
    for (auto& [conn, client] : mClients)
        if (client.guid == guid)
            return &client;
    return nullptr;
}

bool MPServer::grantPlayerInventoryItem(uint32_t guid, const std::string& refId, int count)
{
    ConnectedClient* client = findClientByGuid(guid);
    return client ? grantInventoryItem(*client, refId, count) : false;
}

bool MPServer::placeObject(const std::string& refId, int count, const std::string& cellId, const Position& position)
{
    PlacedObject object;
    object.refId = refId;
    object.count = count;
    object.cellId = cellId;
    object.position = position;

    if (!acceptPlacedObject(object))
        return false;

    Log(Debug::Info) << "[Server] Script ObjectPlace accepted: refId=" << object.refId
                     << " mpNum=" << object.mpNum
                     << " cell=" << object.cellId
                     << " count=" << object.count;

    PacketObjectPlace pkt;
    pkt.object = object;
    broadcastToCell(object.cellId, pkt.encode());
    return true;
}

bool MPServer::removePlacedObjectByMpNum(uint32_t mpNum, const std::string& cellId)
{
    return removePlacedObjectAuthoritative(mpNum, cellId);
}

// ---------------------------------------------------------------------------
void MPServer::setPlayerNickname(uint32_t guid, const std::string& nickname)
{
    ConnectedClient* c = findClientByGuid(guid);
    if (!c || !c->charSelectComplete) return;

    // Clamp to 32 chars to prevent abuse
    const std::string nick = nickname.substr(0, 32);

    c->nickname = nick;
    const std::string displayName = nick.empty() ? c->slotName : nick;
    c->name        = displayName;
    c->player.name = displayName;

    // Persist to DB
    if (mPlayerDb && c->dbCharacterId != 0)
        mPlayerDb->setNickname(c->dbCharacterId, nick);

    // Broadcast updated base info so all clients update their nameplate
    PacketPlayerBaseInfo pkt;
    pkt.setPlayer(&c->player);
    broadcastToAll(pkt.encode());
    syncLuaSnapshot();

    Log(Debug::Info) << "[Server] " << c->slotName
                     << " nickname set to '" << displayName << "'";
}

int MPServer::getPlayerCount() const
{
    int count = 0;
    for (const auto& [conn, client] : mClients)
        if (client.charSelectComplete)
            ++count;
    return count;
}

bool MPServer::acceptPlacedObject(PlacedObject& object)
{
    if (object.refId.empty() || object.cellId.empty() || object.count <= 0)
        return false;

    object.mpNum = mWorld.nextObjectMpNum++;

    auto& objects = mWorld.placedObjects[object.cellId];
    objects.push_back(object);
    mLua.upsertPlacedObject(object);

    if (mPlayerDb)
        mPlayerDb->upsertWorldObject(object);

    return true;
}

// ---------------------------------------------------------------------------
void MPServer::syncLuaSnapshot()
{
    if (!mLua.isLoaded())
        return;

    std::vector<LuaPlayerSnapshot> players;
    players.reserve(mClients.size());

    for (const auto& [conn, client] : mClients)
    {
        if (!client.charSelectComplete)
            continue;

        LuaPlayerSnapshot snapshot;
        snapshot.guid = client.guid;
        snapshot.name = client.name;
        snapshot.cell = makeCellKey(client.player.cell);
        snapshot.nickname = client.nickname;
        snapshot.x = client.player.position.pos[0];
        snapshot.y = client.player.position.pos[1];
        snapshot.z = client.player.position.pos[2];
        snapshot.rx = client.player.position.rot[0];
        snapshot.ry = client.player.position.rot[1];
        snapshot.rz = client.player.position.rot[2];
        snapshot.dynamicStats = client.player.dynamicStats;
        snapshot.skills = client.player.skills;
        snapshot.inventory = client.player.inventoryChanges.items;
        players.push_back(std::move(snapshot));
    }

    mLua.syncSnapshot(getUptime(), mWorld.gameHour, players);
}

// ---------------------------------------------------------------------------
void MPServer::sendAuthoritativeInventory(ConnectedClient& c)
{
    c.player.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
    PacketPlayerInventory inventory;
    inventory.setPlayer(&c.player);
    sendTo(c.conn, inventory.encode());
}

// ---------------------------------------------------------------------------
bool MPServer::grantInventoryItem(ConnectedClient& c, const std::string& refId, int count)
{
    if (refId.empty() || count <= 0)
        return false;

    auto& items = c.player.inventoryChanges.items;
    auto it = std::find_if(items.begin(), items.end(), [&](const Item& item) {
        return item.refId == refId && item.charge == -1 && item.soul.empty();
    });

    if (it != items.end())
    {
        it->count += count;
    }
    else
    {
        Item item;
        item.refId = refId;
        item.count = count;
        item.charge = -1;
        item.enchantmentCharge = -1.f;
        items.push_back(std::move(item));
    }

    c.player.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;

    if (mPlayerDb && c.dbCharacterId != 0)
        mPlayerDb->saveCharacterInventory(c.dbCharacterId, c.player.inventoryChanges.items);

    sendAuthoritativeInventory(c);
    syncLuaSnapshot();
    return true;
}

// ---------------------------------------------------------------------------
bool MPServer::removePlacedObjectAuthoritative(uint32_t mpNum, const std::string& cellId)
{
    if (mpNum == 0 || cellId.empty())
        return false;

    mLua.removePlacedObject(mpNum);

    auto objectsIt = mWorld.placedObjects.find(cellId);
    if (objectsIt != mWorld.placedObjects.end())
    {
        auto& objects = objectsIt->second;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [&](const PlacedObject& object) { return object.mpNum == mpNum; }),
            objects.end());
        if (objects.empty())
            mWorld.placedObjects.erase(objectsIt);
    }

    if (mPlayerDb)
        mPlayerDb->deleteWorldObject(mpNum);

    PacketObjectDelete pkt;
    pkt.mpNum = mpNum;
    pkt.cellId = cellId;
    broadcastToCell(cellId, pkt.encode());
    return true;
}

// ---------------------------------------------------------------------------
void MPServer::syncLuaAuthorityState()
{
    std::vector<PlacedObject> placedObjects;
    for (const auto& [cellId, objects] : mWorld.placedObjects)
    {
        placedObjects.insert(placedObjects.end(), objects.begin(), objects.end());
    }

    mLua.syncPlacedObjects(std::move(placedObjects));
}

// ---------------------------------------------------------------------------
void MPServer::broadcastToAll(const std::vector<uint8_t>& data,
                              HSteamNetConnection except, bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    for (auto& [conn, client] : mClients)
    {
        if (conn == except || !client.charSelectComplete) continue;
        mInterface->SendMessageToConnection(
            conn, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
    }
}

void MPServer::sendTo(HSteamNetConnection conn,
                      const std::vector<uint8_t>& data, bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    mInterface->SendMessageToConnection(
        conn, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
}

void MPServer::broadcastToCell(const std::string& cellId,
                               const std::vector<uint8_t>& data,
                               HSteamNetConnection except,
                               bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    for (auto& [conn, client] : mClients)
    {
        if (conn == except || !client.charSelectComplete) continue;
        if (!cellMatches(client.player.cell, cellId)) continue;
        mInterface->SendMessageToConnection(
            conn, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
    }
}

// ---------------------------------------------------------------------------
void MPServer::staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    if (sInstance) sInstance->onConnectionStatusChanged(info);
}

void MPServer::onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    switch (info->m_info.m_eState)
    {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Accept all incoming connections
            if (mInterface->AcceptConnection(info->m_hConn) != k_EResultOK)
            {
                mInterface->CloseConnection(info->m_hConn, 0, "Accept failed", false);
                return;
            }
            onClientConnected(info->m_hConn);
            break;

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            onClientDisconnected(info->m_hConn, info->m_info.m_szEndDebug);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
void MPServer::handleChallengeResponse(ConnectedClient& c,
                                        const uint8_t* data, size_t size)
{
    // Must have an outstanding challenge — ignore if there isn't one.
    if (c.pendingPublicKey.empty()) return;

    PacketChallengeResponse pkt;
    if (!pkt.decode(data, size)) return;

    // Load the stored public key from its base64 representation directly into
    // a GNS key object — no manual base64 decode needed.
    CECSigningPublicKey pubKey;
    if (!pubKey.SetFromBase64EncodedString(c.pendingPublicKey.c_str()) || !pubKey.IsValid())
    {
        mInterface->CloseConnection(c.conn, 0, "Bad keypair", true);
        return;
    }

    // Verify the Ed25519 signature using the GNS C++ API.
    CryptoSignature_t sig;
    std::memcpy(sig, pkt.signature, 64);
    if (!pubKey.VerifySignature(c.pendingChallenge, 32, sig))
    {
        PacketHandshakeResponse rsp;
        rsp.accepted     = false;
        rsp.rejectReason = "Keypair verification failed.";
        sendTo(c.conn, rsp.encode());
        mInterface->CloseConnection(c.conn, 0, "Bad signature", true);
        Log(Debug::Warning) << "[Auth] Bad signature from " << c.loginName;
        return;
    }

    Log(Debug::Info) << "[Auth] Keypair auth verified for " << c.loginName;
    c.pendingPublicKey.clear();

    // Auth succeeded via keypair — proceed exactly as a normal accepted handshake.
    c.player.guid       = c.guid;
    c.player.name       = c.loginName;
    c.handshakeComplete = true;

    if (mWorld.hostGuid == 0)
        mWorld.hostGuid = c.guid;

    PacketHandshakeResponse rsp;
    rsp.accepted      = true;
    rsp.assignedGuid  = c.guid;
    rsp.serverVersion = SERVER_VERSION;
    sendTo(c.conn, rsp.encode());

    Log(Debug::Info) << "[Server] Keypair handshake accepted: " << c.name
                     << " guid=" << c.guid;

    PacketCharacterList charListPkt;
    if (mPlayerDb && c.dbAccountId > 0)
    {
        try
        {
            for (const auto& cs : mPlayerDb->listCharacters(c.dbAccountId))
            {
                CharacterEntry entry;
                entry.name      = cs.name;
                entry.race      = cs.race;
                entry.className = cs.className;
                entry.lastSeen  = cs.lastSeen;
                entry.isNew     = cs.isNew;
                charListPkt.characters.push_back(std::move(entry));
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] listCharacters error: " << e.what();
        }
    }
    sendTo(c.conn, charListPkt.encode());
    Log(Debug::Info) << "[Server] Sent " << charListPkt.characters.size()
                     << " character(s) to " << c.name;
}

// ---------------------------------------------------------------------------
void MPServer::handleLinkKeyRequest(ConnectedClient& c,
                                     const uint8_t* data, size_t size)
{
    if (!c.handshakeComplete || !mPlayerDb || c.dbAccountId <= 0) return;

    PacketLinkKeyRequest pkt;
    if (!pkt.decode(data, size)) return;

    if (pkt.publicKey.empty()) return;

    // Check the key isn't already registered globally.
    if (mPlayerDb->lookupAccountByKeypair(pkt.publicKey) >= 0)
    {
        Log(Debug::Warning) << "[Auth] LinkKey: key already registered for "
                            << c.loginName;
        return; // silently ignore — client considers itself linked already
    }

    try
    {
        mPlayerDb->addKeypair(c.dbAccountId, pkt.publicKey,
                              pkt.label.empty() ? "linked machine" : pkt.label);
        Log(Debug::Info) << "[Auth] Keypair linked for " << c.loginName
                         << " label='" << pkt.label << "'";
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[Auth] addKeypair error: " << e.what();
    }
}

// ---------------------------------------------------------------------------
void MPServer::handleUnlinkKeyRequest(ConnectedClient& c,
                                       const uint8_t* data, size_t size)
{
    if (!c.handshakeComplete || !mPlayerDb || c.dbAccountId <= 0) return;

    PacketUnlinkKeyRequest pkt;
    if (!pkt.decode(data, size)) return;

    if (pkt.publicKey.empty()) return;

    // Only allow removing a key that belongs to this account.
    const int64_t owner = mPlayerDb->lookupAccountByKeypair(pkt.publicKey);
    if (owner != c.dbAccountId)
    {
        Log(Debug::Warning) << "[Auth] UnlinkKey: key not owned by " << c.loginName;
        return;
    }

    try
    {
        // Simple DELETE — use a prepared statement via exec since we don't have
        // a dedicated removeKeypair method; add one to PlayerDatabase.
        // For now find and delete by public_key.
        mPlayerDb->removeKeypair(pkt.publicKey);
        Log(Debug::Info) << "[Auth] Keypair unlinked for " << c.loginName;
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[Auth] removeKeypair error: " << e.what();
    }
}


// ---------------------------------------------------------------------------
void MPServer::handleDeleteCharRequest(ConnectedClient& c,
                                        const uint8_t* data, size_t size)
{
    if (!c.handshakeComplete || c.charSelectComplete || !mPlayerDb || c.dbAccountId <= 0)
        return;

    PacketDeleteCharRequest pkt;
    if (!pkt.decode(data, size) || pkt.charName.empty()) return;

    // Refuse to delete a character that is live in another session.
    for (const auto& [conn, other] : mClients)
    {
        if (&other == &c) continue;
        if (other.handshakeComplete && other.charSelectComplete
            && other.dbAccountId == c.dbAccountId
            && other.slotName == pkt.charName)
        {
            PacketDeleteCharResponse rsp;
            rsp.success  = false;
            rsp.charName = pkt.charName;
            rsp.error    = "'" + pkt.charName + "' is currently in-world in another session.";
            sendTo(c.conn, rsp.encode());
            return;
        }
    }

    PacketDeleteCharResponse rsp;
    rsp.charName = pkt.charName;
    try
    {
        rsp.success = mPlayerDb->deleteCharacter(c.dbAccountId, pkt.charName);
        if (!rsp.success)
            rsp.error = "Character '" + pkt.charName + "' not found on this account.";
        else
            Log(Debug::Info) << "[Server] Deleted character '" << pkt.charName
                             << "' for " << c.loginName;
    }
    catch (const std::exception& e)
    {
        rsp.success = false;
        rsp.error   = "Server error during deletion.";
        Log(Debug::Warning) << "[Server] deleteCharacter error: " << e.what();
    }
    sendTo(c.conn, rsp.encode());
}

} // namespace mwmp
