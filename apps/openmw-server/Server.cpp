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
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <variant>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Packets/System/PacketGameSettings.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCharGen.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerLoadedCells.hpp>
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
#include <components/openmw-mp/Packets/Worldstate/PacketRecordDynamic.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAI.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimFlags.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimPlay.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAttack.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAttackV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAuthority.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCast.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCellChange.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorDeath.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorEquipment.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorIdentity.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorList.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPosition.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPositionV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPresentationV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCombatRequest.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>
// PacketWorldWeather is defined in PacketWorldTime.hpp

// Encode/decode ESM::Class::CLDTstruct as 15 comma-separated ints.
// Format: specialization, attr[0], attr[1], skills[0..4][0..1], isPlayable, services
namespace
{
    std::string jsonEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (char c : text)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                        out += buffer;
                    }
                    else
                    {
                        out.push_back(c);
                    }
                    break;
            }
        }
        return out;
    }

    std::string makeJsonErrorBody(std::string_view error)
    {
        return std::string("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}";
    }

    std::string makeDynamicRecordKey(std::string_view recordType, std::string_view recordId)
    {
        std::string key;
        key.reserve(recordType.size() + 1 + recordId.size());
        key.append(recordType);
        key.push_back('\x1f');
        key.append(recordId);
        return key;
    }

    std::optional<uint64_t> parseGeneratedRecordNumber(
        std::string_view prefix, std::string_view recordType, std::string_view recordId)
    {
        if (prefix.empty() || recordType.empty())
            return std::nullopt;

        std::string expectedPrefix;
        expectedPrefix.reserve(prefix.size() + recordType.size() + 2);
        expectedPrefix.append(prefix);
        expectedPrefix.push_back('_');
        expectedPrefix.append(recordType);
        expectedPrefix.push_back('_');

        if (!recordId.starts_with(expectedPrefix))
            return std::nullopt;

        std::string_view suffix = recordId.substr(expectedPrefix.size());
        if (suffix.empty())
            return std::nullopt;

        uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), value);
        if (ec != std::errc() || ptr != suffix.data() + suffix.size())
            return std::nullopt;
        return value;
    }

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
                                 uint32_t refNum,
                                 uint32_t mpNum = 0)
    {
        if (mpNum != 0)
            return cellId + "|mp|" + std::to_string(mpNum);
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

    bool sameItemIdentity(const mwmp::Item& left, const mwmp::Item& right)
    {
        return left.refId == right.refId
            && left.charge == right.charge
            && std::abs(left.enchantmentCharge - right.enchantmentCharge) < 0.001f
            && left.soul == right.soul;
    }

    bool inventoryContainsItemIdentity(const std::vector<mwmp::Item>& items, const mwmp::Item& target)
    {
        return std::any_of(items.begin(), items.end(), [&](const mwmp::Item& item) {
            return sameItemIdentity(item, target);
        });
    }

    bool sameItemStack(const mwmp::Item& left, const mwmp::Item& right)
    {
        return sameItemIdentity(left, right) && left.count == right.count;
    }

    bool sameCosmeticItemStack(const mwmp::Item& left, const mwmp::Item& right)
    {
        return left.refId == right.refId && left.count == right.count;
    }

    bool itemStackLess(const mwmp::Item& left, const mwmp::Item& right)
    {
        return std::tie(left.refId, left.charge, left.enchantmentCharge, left.soul, left.count)
            < std::tie(right.refId, right.charge, right.enchantmentCharge, right.soul, right.count);
    }

    bool cosmeticItemStackLess(const mwmp::Item& left, const mwmp::Item& right)
    {
        return std::tie(left.refId, left.count) < std::tie(right.refId, right.count);
    }

    bool sameInventorySnapshot(std::vector<mwmp::Item> left, std::vector<mwmp::Item> right)
    {
        left.erase(std::remove_if(left.begin(), left.end(), [](const mwmp::Item& item) {
            return item.refId.empty() || item.count <= 0;
        }), left.end());
        right.erase(std::remove_if(right.begin(), right.end(), [](const mwmp::Item& item) {
            return item.refId.empty() || item.count <= 0;
        }), right.end());

        std::sort(left.begin(), left.end(), itemStackLess);
        std::sort(right.begin(), right.end(), itemStackLess);
        if (left.size() != right.size())
            return false;

        for (std::size_t i = 0; i < left.size(); ++i)
        {
            if (!sameItemStack(left[i], right[i]))
                return false;
        }
        return true;
    }

    bool sameCosmeticInventorySnapshot(std::vector<mwmp::Item> left, std::vector<mwmp::Item> right)
    {
        left.erase(std::remove_if(left.begin(), left.end(), [](const mwmp::Item& item) {
            return item.refId.empty() || item.count <= 0;
        }), left.end());
        right.erase(std::remove_if(right.begin(), right.end(), [](const mwmp::Item& item) {
            return item.refId.empty() || item.count <= 0;
        }), right.end());

        std::sort(left.begin(), left.end(), cosmeticItemStackLess);
        std::sort(right.begin(), right.end(), cosmeticItemStackLess);
        if (left.size() != right.size())
            return false;

        for (std::size_t i = 0; i < left.size(); ++i)
        {
            if (!sameCosmeticItemStack(left[i], right[i]))
                return false;
        }
        return true;
    }

    bool looksLikeRestoredInventoryRegression(
        const mwmp::BasePlayer::InventoryChanges& incoming,
        const std::vector<mwmp::Item>& restored)
    {
        if (incoming.action != mwmp::BasePlayer::InventoryChanges::Action::Set || restored.empty())
            return false;
        if (sameInventorySnapshot(incoming.items, restored)
            || sameCosmeticInventorySnapshot(incoming.items, restored))
            return false;

        std::size_t restoredPositive = 0;
        std::size_t incomingPositive = 0;
        for (const auto& item : restored)
        {
            if (!item.refId.empty() && item.count > 0)
                ++restoredPositive;
        }
        for (const auto& item : incoming.items)
        {
            if (!item.refId.empty() && item.count > 0)
                ++incomingPositive;
        }

        if (incomingPositive < restoredPositive)
            return true;

        return std::any_of(restored.begin(), restored.end(), [&](const mwmp::Item& item) {
            return !item.refId.empty() && item.count > 0
                && !inventoryContainsItemIdentity(incoming.items, item);
        });
    }

    std::size_t equippedItemCount(
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& equipment)
    {
        return static_cast<std::size_t>(std::count_if(equipment.begin(), equipment.end(),
            [](const mwmp::EquipmentItem& entry) { return !entry.item.refId.empty() && entry.item.count > 0; }));
    }

    bool sameEquipmentSnapshot(
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& left,
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& right)
    {
        for (int slot = 0; slot < mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            if (left[slot].slot != right[slot].slot)
                return false;
            if (!sameItemStack(left[slot].item, right[slot].item))
                return false;
        }
        return true;
    }

    bool sameCosmeticEquipmentSnapshot(
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& left,
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& right)
    {
        for (int slot = 0; slot < mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            if (left[slot].slot != right[slot].slot)
                return false;
            if (!sameCosmeticItemStack(left[slot].item, right[slot].item))
                return false;
        }
        return true;
    }

    bool looksLikeRestoredEquipmentRegression(
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& incoming,
        const std::array<mwmp::EquipmentItem, mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS>& restored)
    {
        const std::size_t restoredCount = equippedItemCount(restored);
        if (restoredCount == 0 || sameEquipmentSnapshot(incoming, restored)
            || sameCosmeticEquipmentSnapshot(incoming, restored))
            return false;
        if (equippedItemCount(incoming) < restoredCount)
            return true;

        for (int slot = 0; slot < mwmp::BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            const mwmp::EquipmentItem& restoredEntry = restored[slot];
            if (restoredEntry.item.refId.empty() || restoredEntry.item.count <= 0)
                continue;

            const mwmp::EquipmentItem& incomingEntry = incoming[slot];
            if (!sameCosmeticItemStack(incomingEntry.item, restoredEntry.item))
                return true;
        }
        return false;
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

    bool isExteriorCellKey(const std::string& cellId)
    {
        return cellId.rfind("EXT:", 0) == 0;
    }

    std::string exteriorCellIdForPosition(const mwmp::Position& position)
    {
        const float cellSize = static_cast<float>(Constants::CellSizeInUnits);
        const int gridX = static_cast<int>(std::floor(position.pos[0] / cellSize));
        const int gridY = static_cast<int>(std::floor(position.pos[1] / cellSize));
        return std::string("EXT:") + std::to_string(gridX) + "," + std::to_string(gridY);
    }

    float exteriorCellBorderDistance(const mwmp::Position& position)
    {
        const float cellSize = static_cast<float>(Constants::CellSizeInUnits);
        const float gridX = std::floor(position.pos[0] / cellSize);
        const float gridY = std::floor(position.pos[1] / cellSize);
        const float localX = position.pos[0] - gridX * cellSize;
        const float localY = position.pos[1] - gridY * cellSize;
        return std::min(std::min(localX, cellSize - localX), std::min(localY, cellSize - localY));
    }

    constexpr std::size_t MaxLoadedActorCells = 25;

    std::string makeActorKey(const mwmp::BaseActor& actor)
    {
        if (actor.mpNum != 0)
            return "mp|" + std::to_string(actor.mpNum);
        return actor.refId + "|" + std::to_string(actor.refNum) + "|" + std::to_string(actor.mpNum);
    }

    bool normalizeActorIdentity(mwmp::BaseActor& actor)
    {
        const bool ambiguous = mwmp::hasAmbiguousActorInstanceIdentity(actor);
        if (actor.mpNum != 0)
            actor.refNum = 0;
        return ambiguous;
    }

    bool isGeneratedSpawnerRefId(const std::string& refId)
    {
        return refId.rfind("spawner_", 0) == 0;
    }

    bool isUnmanagedSpawnerActor(const mwmp::BaseActor& actor)
    {
        return actor.mpNum == 0 && isGeneratedSpawnerRefId(actor.refId);
    }

    bool isNewerEventId(uint32_t incoming, uint32_t previous)
    {
        if (incoming == 0 || previous == 0)
            return true;

        return static_cast<int32_t>(incoming - previous) > 0;
    }

    bool sameDynamicStat(const mwmp::DynamicStat& a, const mwmp::DynamicStat& b)
    {
        return a.base == b.base && a.current == b.current && a.mod == b.mod;
    }

    bool sameDynamicStats(const mwmp::DynamicStats& a, const mwmp::DynamicStats& b)
    {
        return sameDynamicStat(a.health, b.health)
            && sameDynamicStat(a.magicka, b.magicka)
            && sameDynamicStat(a.fatigue, b.fatigue);
    }

    bool sameStatFloat(float a, float b)
    {
        return std::abs(a - b) <= 0.001f;
    }

    bool samePersistentAttribute(const mwmp::Attribute& a, const mwmp::Attribute& b)
    {
        return a.base == b.base
            && sameStatFloat(a.mod, b.mod)
            && sameStatFloat(a.damage, b.damage);
    }

    bool samePersistentSkill(const mwmp::Skill& a, const mwmp::Skill& b)
    {
        return sameStatFloat(a.base, b.base)
            && sameStatFloat(a.mod, b.mod)
            && sameStatFloat(a.damage, b.damage)
            && sameStatFloat(a.progress, b.progress)
            && a.increases == b.increases;
    }

    bool samePersistentPlayerStats(const mwmp::BasePlayer& a, const mwmp::BasePlayer& b)
    {
        if (!sameDynamicStats(a.dynamicStats, b.dynamicStats)
            || a.level != b.level
            || !sameStatFloat(a.levelProgress, b.levelProgress))
            return false;

        for (std::size_t i = 0; i < a.attributes.size(); ++i)
        {
            if (!samePersistentAttribute(a.attributes[i], b.attributes[i]))
                return false;
        }

        for (std::size_t i = 0; i < a.skills.size(); ++i)
        {
            if (!samePersistentSkill(a.skills[i], b.skills[i]))
                return false;
        }

        return true;
    }

    bool looksLikeRestoredStatsRegression(const mwmp::BasePlayer& incoming, const mwmp::BasePlayer& restored)
    {
        for (std::size_t i = 0; i < incoming.attributes.size(); ++i)
        {
            if (restored.attributes[i].base > 100
                && incoming.attributes[i].base <= 100
                && incoming.attributes[i].base < restored.attributes[i].base)
                return true;
        }

        for (std::size_t i = 0; i < incoming.skills.size(); ++i)
        {
            if (restored.skills[i].base > 100.f
                && incoming.skills[i].base <= 100.f
                && incoming.skills[i].base < restored.skills[i].base)
                return true;
        }

        return false;
    }

    void copyPersistentPlayerStats(mwmp::BasePlayer& dst, const mwmp::BasePlayer& src)
    {
        dst.dynamicStats = src.dynamicStats;
        dst.attributes = src.attributes;
        dst.skills = src.skills;
        dst.level = src.level;
        dst.levelProgress = src.levelProgress;
        dst.hasSavedStats = src.hasSavedStats;
    }

    bool isZeroActorPosition(const mwmp::Position& position)
    {
        return std::abs(position.pos[0]) <= 0.001f
            && std::abs(position.pos[1]) <= 0.001f
            && std::abs(position.pos[2]) <= 0.001f;
    }

    bool sameDeadVanillaActorState(const mwmp::BaseActor& a, const mwmp::BaseActor& b)
    {
        return a.refId == b.refId
            && a.refNum == b.refNum
            && a.mpNum == b.mpNum
            && a.cellId == b.cellId
            && a.position.pos[0] == b.position.pos[0]
            && a.position.pos[1] == b.position.pos[1]
            && a.position.pos[2] == b.position.pos[2]
            && a.position.rot[0] == b.position.rot[0]
            && a.position.rot[1] == b.position.rot[1]
            && a.position.rot[2] == b.position.rot[2]
            && sameDynamicStats(a.dynamicStats, b.dynamicStats)
            && a.deathState == b.deathState
            && a.isDead == b.isDead
            && a.isInstantDeath == b.isInstantDeath
            && a.deathAnimGroup == b.deathAnimGroup;
    }

    bool isLocomotionAnimGroup(const std::string& group)
    {
        return group.find("walk") != std::string::npos
            || group.find("run") != std::string::npos
            || group.find("swim") != std::string::npos
            || group.find("sneak") != std::string::npos
            || group.find("turn") != std::string::npos;
    }

    bool isIdleAnimGroup(const std::string& group)
    {
        return group == "idle" || group.rfind("idle", 0) == 0;
    }

    bool isBaseIdleAnimGroup(const std::string& group)
    {
        return group == "idle" || group == "idleswim" || group == "idlesneak";
    }

    bool isReliablePresentationAnimGroup(const std::string& group)
    {
        return !group.empty() && !isLocomotionAnimGroup(group) && !isBaseIdleAnimGroup(group);
    }

    mwmp::ActorPresentationSnapshot makePresentationSnapshot(const mwmp::BaseActor& actor, mwmp::ActorInstanceId actorNetId)
    {
        const bool axisLocomotion = std::abs(actor.animFlags.animFwd) > 0.1f
            || std::abs(actor.animFlags.animSide) > 0.1f;
        const float velX = actor.velocity.linear[0];
        const float velY = actor.velocity.linear[1];
        const float speedSq = velX * velX + velY * velY;
        const bool velocityLocomotion = speedSq > 20.f * 20.f;
        const bool hasLocomotionInput = !actor.isDead && (axisLocomotion || velocityLocomotion);

        float animFwd = hasLocomotionInput ? actor.animFlags.animFwd : 0.f;
        float animSide = hasLocomotionInput ? actor.animFlags.animSide : 0.f;
        if (hasLocomotionInput && !axisLocomotion && velocityLocomotion)
        {
            // NPC AI often moves the actor by transform velocity without leaving
            // useful movement axes behind.  Treat that as ordinary forward
            // locomotion instead of deriving signed strafe/backpedal axes from
            // world velocity, which proved too noisy near cell borders.
            animFwd = 1.f;
            animSide = 0.f;
        }

        mwmp::ActorPresentationSnapshot snapshot;
        snapshot.actorNetId = actorNetId;
        snapshot.isMoving = hasLocomotionInput;
        snapshot.isAttackingOrCasting = actor.isAttackingOrCasting;
        snapshot.hasWeaponDrawn = actor.hasWeaponDrawn;
        snapshot.hasSpellReadied = actor.hasSpellReadied;
        snapshot.isDead = actor.isDead;
        snapshot.movementFlags = static_cast<uint16_t>(actor.animFlags.movementFlags);
        snapshot.animFwd = hasLocomotionInput ? mwmp::quantizeActorAxis(animFwd) : 0;
        snapshot.animSide = hasLocomotionInput ? mwmp::quantizeActorAxis(animSide) : 0;
        mwmp::BaseActor presentationActor = actor;
        presentationActor.isMoving = hasLocomotionInput;
        snapshot.presentationFlags = mwmp::makeActorPresentationFlags(presentationActor);
        snapshot.currentAnimGroup = actor.animFlags.currentAnimGroup;
        if ((actor.isDead || !hasLocomotionInput) && isLocomotionAnimGroup(snapshot.currentAnimGroup))
            snapshot.currentAnimGroup.clear();
        return snapshot;
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
    const std::unordered_set<std::string> oldActorInterestCells = actorInterestCellsForClient(*client);
    client->player.cell = *parsedCell;
    client->player.position = position;
    client->player.position.isTeleporting = true;
    client->player.velocity = {};
    client->loadedActorCells.clear();
    client->loadedActorCellsSequence = 0;

    const std::string newCell = makeCellKey(client->player.cell);
    syncLuaSnapshot();

    for (const std::string& oldActorCellId : oldActorInterestCells)
    {
        if (oldActorCellId != newCell)
            refreshActorAuthorityForCell(oldActorCellId);
    }
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
        {
            PacketPlayerPosition positionPacket;
            positionPacket.setPlayer(&client->player);
            const auto positionEncoded = positionPacket.encode();
            sendTo(client->conn, positionEncoded);
            broadcastToAll(positionEncoded, client->conn);
        }

        if (!newCell.empty())
            sendCellStateToClient(client->conn, newCell);
        sendPlayerStateBootstrapToClient(*client);
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

    mGeneratedRecordIdPrefix = mLua.getString("Config", "GENERATED_RECORD_ID_PREFIX", "$custom");
    if (mGeneratedRecordIdPrefix.empty())
        mGeneratedRecordIdPrefix = "$custom";

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

    mLua.syncGeneratedRecordState(
        mGeneratedRecordIdPrefix, buildGeneratedDynamicRecordCounters(mGeneratedRecordIdPrefix));

    auto normalizeConfiguredCell = [](std::string raw) {
        // Normalise "x, y" coords: strip spaces that follow a comma so
        // findExteriorPosition / std::from_chars can parse the string.
        std::string norm;
        norm.reserve(raw.size());
        bool afterComma = false;
        for (char c : raw)
        {
            if (c == ',')
            {
                norm += c;
                afterComma = true;
            }
            else if (c == ' ' && afterComma)
            {
                // drop
            }
            else
            {
                norm += c;
                afterComma = false;
            }
        }
        return norm;
    };

    // If config.lua set Config.SPAWN_CELL, let it override the C++ default.
    // Config.DEFAULT_SPAWN can additionally provide a full position/rotation.
    {
        std::string raw = mLua.getString("Config", "SPAWN_CELL", "");
        if (!raw.empty())
        {
            mDefaultSpawnCell = normalizeConfiguredCell(std::move(raw));
            Log(Debug::Info) << "[Server] Spawn cell set from config.lua: " << mDefaultSpawnCell;
        }

        if (auto defaultSpawn = mLua.getConfigPlayerMark("DEFAULT_SPAWN"))
        {
            mDefaultSpawnCell = normalizeConfiguredCell(defaultSpawn->cell);
            mDefaultSpawnPosition = defaultSpawn->position;
            mHasDefaultSpawnPosition = true;
            Log(Debug::Info) << "[Server] Default spawn position set from config.lua: " << mDefaultSpawnCell
                             << " (" << mDefaultSpawnPosition.pos[0]
                             << ", " << mDefaultSpawnPosition.pos[1]
                             << ", " << mDefaultSpawnPosition.pos[2] << ")";
        }

        mDefaultPlayerMarks = mLua.getConfigPlayerMarks("DEFAULT_PLAYER_MARKS");
        for (auto& mark : mDefaultPlayerMarks)
            mark.cell = normalizeConfiguredCell(std::move(mark.cell));
        if (!mDefaultPlayerMarks.empty())
            Log(Debug::Info) << "[Server] Default new-character marks loaded: " << mDefaultPlayerMarks.size();
    }

    // Read Config.MAX_CHARS_PER_ACCOUNT from config.lua (0 = unlimited).
    mMaxCharsPerAccount = mLua.getInt("Config", "MAX_CHARS_PER_ACCOUNT", mMaxCharsPerAccount);
    Log(Debug::Info) << "[Server] Max chars per account: "
                     << (mMaxCharsPerAccount == 0 ? "unlimited" : std::to_string(mMaxCharsPerAccount));

    mAdminHttpEnabled = mLua.getBool("Config", "ADMIN_HTTP_ENABLED", true);
    mAdminHttpHost = mLua.getString("Config", "ADMIN_HTTP_HOST", "127.0.0.1");
    mAdminHttpPort = std::max(1, mLua.getInt("Config", "ADMIN_HTTP_PORT", 8081));
    mAdminHttpTimeoutMs = std::max(1, mLua.getInt("Config", "ADMIN_HTTP_TIMEOUT_MS", 250));

    std::string normalizedAdminHttpHost = mAdminHttpHost;
    std::transform(normalizedAdminHttpHost.begin(), normalizedAdminHttpHost.end(), normalizedAdminHttpHost.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalizedAdminHttpHost != "127.0.0.1" && normalizedAdminHttpHost != "localhost")
    {
        Log(Debug::Warning) << "[Server] ADMIN_HTTP_HOST must stay loopback-only; forcing 127.0.0.1 instead of "
                            << mAdminHttpHost;
        mAdminHttpHost = "127.0.0.1";
    }

    syncLuaSnapshot();
    mLua.start();
    mLua.onServerInit();
    startAdminHttpServer();

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
    stopAdminHttpServer();
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

    flushScheduledGeneratedDynamicRecordGc();
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
    const std::unordered_set<std::string> actorInterestCells = actorInterestCellsForClient(client);
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
            // PlayerStatsDynamic is persisted on receipt. Avoid a blind disconnect
            // rewrite from a partially restored/template player during join/quit.
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] disconnect save error: " << e.what();
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
    if (!actorInterestCells.empty())
    {
        for (const std::string& cellId : actorInterestCells)
            refreshActorAuthorityForCell(cellId);
    }
    else if (!actorCell.empty())
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
        return clientHasActorCellLoaded(client, cellId);
    };

    // Stability: keep the current authority owner as long as they are still in the cell.
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

    // Prefer a client whose canonical active cell exactly matches the actor cell.
    if (newAuthorityGuid == 0)
    {
        for (const auto& [conn, client] : mClients)
        {
            if (!isEligible(client) || !cellMatches(client.player.cell, cellId))
                continue;

            if (newAuthorityGuid == 0 || client.guid < newAuthorityGuid)
                newAuthorityGuid = client.guid;
        }
    }

    // For exterior cells visible to several players, prefer the closest active exterior grid.
    if (newAuthorityGuid == 0)
    {
        const auto parsedActorCell = parseCellKey(cellId);
        if (parsedActorCell && parsedActorCell->isExterior)
        {
            int bestDistance = -1;
            for (const auto& [conn, client] : mClients)
            {
                if (!isEligible(client) || !client.player.cell.isExterior)
                    continue;

                const int distance = std::abs(client.player.cell.gridX - parsedActorCell->gridX)
                    + std::abs(client.player.cell.gridY - parsedActorCell->gridY);
                if (bestDistance == -1 || distance < bestDistance
                    || (distance == bestDistance && client.guid < newAuthorityGuid))
                {
                    bestDistance = distance;
                    newAuthorityGuid = client.guid;
                }
            }
        }
    }

    // Current authority is gone — try the preferred GUID after active/closest choices.
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

    // No stronger preference — lowest GUID wins.
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

    if (newAuthorityGuid == 0 && !cellState.actors.empty())
    {
        std::size_t removedRuntimeActors = 0;
        std::vector<ActorRegistryRecord> removedRecords;
        for (auto it = cellState.actors.begin(); it != cellState.actors.end();)
        {
            if (it->second.actor.mpNum == 0 || it->second.persistent)
            {
                ++it;
                continue;
            }

            ensureActorNetId(it->second, cellId);
            removedRecords.push_back(it->second);
            if (mPlayerDb)
                mPlayerDb->deleteSpawnedActorDynamicRecordLink(it->second.actor.mpNum, cellId);
            forgetActorLocation(it->second.actor, cellId);
            it = cellState.actors.erase(it);
            ++removedRuntimeActors;
        }

        if (removedRuntimeActors != 0)
        {
            broadcastActorIdentityRemovalForCell(cellId, cellState, removedRecords);
            for (const ActorRegistryRecord& record : removedRecords)
                forgetActorNetId(record.actorNetId, record.actor);
            if (mPlayerDb)
                scheduleGeneratedDynamicRecordGc("actor_cell_empty");
            Log(Debug::Info) << "[Server] Cleared " << removedRuntimeActors
                             << " runtime spawned actor(s) for inactive cell " << cellId;
        }
    }

    sendActorStateToInterestedClients(cellId);
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
    CellActorState emptyState;
    const CellActorState& state = (it != mWorld.actorCells.end()) ? it->second : emptyState;

    std::unordered_map<std::string, ActorRegistryRecord> actors = state.actors;
    mergeDeadVanillaActorsForCell(cellId, actors);
    if (actors.empty())
        return;

    ActorList actorList;
    actorList.cellId = cellId;
    actorList.isAuthority = false;
    actorList.authorityGuid = state.authorityGuid;
    actorList.authorityGeneration = state.authorityGeneration;
    actorList.snapshotSequence = state.nextSnapshotSequence;
    actorList.serverTimestamp = currentServerTimeMs();

    std::size_t deadVanillaCount = 0;
    bool includesHul = false;
    std::vector<BaseActor> deadBootstrapActors;
    actorList.actors.reserve(actors.size());
    for (const auto& [key, record] : actors)
    {
        if (record.actor.mpNum == 0 && record.actor.isDead)
        {
            ++deadVanillaCount;
            if (record.actor.refId == "hul")
                includesHul = true;
        }
        if (record.actor.isDead)
        {
            BaseActor deadActor = record.actor;
            deadActor.isInstantDeath = true;
            if (deadActor.dynamicStats.health.current > 0.f)
                deadActor.dynamicStats.health.current = 0.f;
            deadBootstrapActors.push_back(std::move(deadActor));
        }
        actorList.actors.push_back(record.actor);
    }

    if (deadVanillaCount != 0)
    {
        auto clientIt = mClients.find(conn);
        Log(Debug::Verbose) << "[Server] sendActorStateToClient dead vanilla snapshot"
                            << " to=" << (clientIt != mClients.end() ? clientIt->second.name : std::string("<unknown>"))
                            << " cell=" << cellId
                            << " actors=" << actorList.actors.size()
                            << " deadVanilla=" << deadVanillaCount
                            << " includesHul=" << includesHul;
    }

    if (it != mWorld.actorCells.end())
        sendActorIdentityToClient(conn, cellId, it->second);

    PacketActorList pkt;
    pkt.setActorList(&actorList);
    sendTo(conn, pkt.encode());

    if (!deadBootstrapActors.empty())
    {
        ActorList statsList = actorList;
        statsList.actors = deadBootstrapActors;
        PacketActorStatsDynamic statsPkt;
        statsPkt.setActorList(&statsList);
        sendTo(conn, statsPkt.encode());

        ActorList deathList = actorList;
        deathList.actors = std::move(deadBootstrapActors);
        PacketActorDeath deathPkt;
        deathPkt.setActorList(&deathList);
        sendTo(conn, deathPkt.encode());
    }
}

void MPServer::sendActorIdentityToClient(HSteamNetConnection conn, const std::string& cellId, CellActorState& cellState)
{
    auto clientIt = mClients.find(conn);
    if (clientIt == mClients.end() || clientIt->second.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
        return;

    std::unordered_map<std::string, ActorRegistryRecord> actors = cellState.actors;
    mergeDeadVanillaActorsForCell(cellId, actors);
    if (actors.empty())
        return;

    ActorIdentityList identityList = buildActorIdentityList(cellId, cellState, actors);
    if (identityList.actors.empty())
        return;

    PacketActorIdentity pkt;
    pkt.setIdentityList(&identityList);
    sendTo(conn, pkt.encode());
    for (const ActorIdentityRecord& record : identityList.actors)
    {
        if (clientIt->second.actorV2IdentitySent.insert(record.actorNetId).second)
            ++clientIt->second.actorV2IdentitySentWindow;
    }

    Log(Debug::Verbose) << "[Server] ActorSync v2 identity sent"
                        << " to=" << clientIt->second.name
                        << " cell=" << cellId
                        << " actors=" << identityList.actors.size()
                        << " seq=" << identityList.sequence;
}

void MPServer::broadcastActorIdentityForCell(
    const std::string& cellId, CellActorState& cellState, HSteamNetConnection except)
{
    std::unordered_map<std::string, ActorRegistryRecord> actors = cellState.actors;
    mergeDeadVanillaActorsForCell(cellId, actors);
    if (actors.empty())
        return;

    ActorIdentityList identityList = buildActorIdentityList(cellId, cellState, actors);
    if (identityList.actors.empty())
        return;

    PacketActorIdentity pkt;
    pkt.setIdentityList(&identityList);
    const std::vector<uint8_t> encoded = pkt.encode();
    for (auto& [conn, client] : mClients)
    {
        if (conn == except
            || !clientHasActorCellLoaded(client, cellId)
            || client.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
            continue;
        sendTo(conn, encoded);
        for (const ActorIdentityRecord& record : identityList.actors)
        {
            if (client.actorV2IdentitySent.insert(record.actorNetId).second)
                ++client.actorV2IdentitySentWindow;
        }
    }
}

void MPServer::broadcastActorIdentityRemovalForCell(
    const std::string& cellId,
    CellActorState& cellState,
    const std::vector<ActorRegistryRecord>& records)
{
    if (records.empty())
        return;

    ActorIdentityList identityList;
    identityList.protocolVersion = ActorSyncProtocolVersionV2;
    identityList.cellId = cellId;
    identityList.authorityGuid = cellState.authorityGuid;
    identityList.authorityGeneration = cellState.authorityGeneration;
    identityList.sequence = cellState.nextSnapshotSequence++;
    identityList.serverTimestamp = currentServerTimeMs();
    identityList.actors.reserve(records.size());

    for (const ActorRegistryRecord& record : records)
    {
        const ActorInstanceId actorNetId = record.actorNetId != 0
            ? record.actorNetId : actorInstanceIdFromActor(record.actor);
        if (actorNetId == 0)
            continue;

        ActorIdentityRecord identity;
        identity.actorNetId = actorNetId;
        identity.persistent = record.persistent;
        identity.serverSpawned = record.actor.mpNum != 0;
        identity.removed = true;
        identity.actor = record.actor;
        identity.actor.cellId = cellId;
        identityList.actors.push_back(std::move(identity));
    }

    if (identityList.actors.empty())
        return;

    PacketActorIdentity pkt;
    pkt.setIdentityList(&identityList);
    const std::vector<uint8_t> encoded = pkt.encode();
    std::size_t sentClients = 0;
    for (auto& [conn, client] : mClients)
    {
        if (client.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
            continue;

        bool shouldSend = clientHasActorCellLoaded(client, cellId);
        if (!shouldSend)
        {
            for (const ActorIdentityRecord& record : identityList.actors)
            {
                if (client.actorV2IdentitySent.count(record.actorNetId) != 0
                    || client.actorV2IdentityAcked.count(record.actorNetId) != 0)
                {
                    shouldSend = true;
                    break;
                }
            }
        }
        if (!shouldSend)
            continue;

        sendTo(conn, encoded);
        ++sentClients;
        for (const ActorIdentityRecord& record : identityList.actors)
        {
            if (client.actorV2IdentitySent.insert(record.actorNetId).second)
                ++client.actorV2IdentitySentWindow;
        }
    }

    Log(Debug::Info) << "[Server] ActorSync v2 identity removed"
                     << " cell=" << cellId
                     << " actors=" << identityList.actors.size()
                     << " sentClients=" << sentClients
                     << " seq=" << identityList.sequence;
}

// ---------------------------------------------------------------------------
bool MPServer::clientHasActorCellLoaded(const ConnectedClient& client, const std::string& cellId) const
{
    if (cellId.empty() || !client.charSelectComplete)
        return false;

    if (cellMatches(client.player.cell, cellId))
        return true;

    if (client.loadedActorCells.empty())
        return false;

    return client.loadedActorCells.find(cellId) != client.loadedActorCells.end();
}

std::unordered_set<std::string> MPServer::actorInterestCellsForClient(const ConnectedClient& client) const
{
    const std::string currentCell = makeCellKey(client.player.cell);
    if (!client.loadedActorCells.empty())
    {
        std::unordered_set<std::string> cells = client.loadedActorCells;
        if (!currentCell.empty())
            cells.insert(currentCell);
        return cells;
    }

    std::unordered_set<std::string> cells;
    if (!currentCell.empty())
        cells.insert(currentCell);
    return cells;
}

void MPServer::sendActorStateToInterestedClients(const std::string& cellId)
{
    for (const auto& [conn, client] : mClients)
    {
        if (!clientHasActorCellLoaded(client, cellId))
            continue;

        sendActorAuthorityToClient(conn, cellId);
        sendActorStateToClient(conn, cellId);
    }
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

    if (std::strcmp(packetName, "ActorPosition") != 0 && !clientHasActorCellLoaded(c, actorList.cellId))
    {
        Log(Debug::Verbose) << "[Server] Rejecting " << packetName << " from " << c.name
                            << " because the actor cell is not loaded: player=" << makeCellKey(c.player.cell)
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

MPServer::ActorRegistryRecord* MPServer::findTrackedActor(CellActorState& cellState,
    const BaseActor& actor,
    const ConnectedClient& sender,
    const char* packetName)
{
    const auto it = cellState.actors.find(makeActorKey(actor));
    if (it != cellState.actors.end())
        return &it->second;

    std::ostringstream message;
    message << "[Server] Ignoring late " << packetName
            << " from " << sender.name
            << " for unknown actor refId=" << actor.refId
            << " refNum=" << actor.refNum
            << " mpNum=" << actor.mpNum
            << " cell=" << actor.cellId;
    const bool noisyPacket = std::strcmp(packetName, "ActorPosition") == 0
        || std::strcmp(packetName, "ActorAttack") == 0;
    Log(noisyPacket ? Debug::Verbose : Debug::Info) << message.str();
    return nullptr;
}

void MPServer::rememberActorLocation(const BaseActor& actor, const std::string& cellId)
{
    if (cellId.empty() || (actor.mpNum == 0 && actor.refId.empty()))
        return;

    mWorld.actorLocations[makeActorKey(actor)] = cellId;
}

void MPServer::forgetActorLocation(const BaseActor& actor, const std::string& cellId)
{
    if (actor.mpNum == 0 && actor.refId.empty())
        return;

    const std::string actorKey = makeActorKey(actor);
    auto locationIt = mWorld.actorLocations.find(actorKey);
    if (locationIt == mWorld.actorLocations.end())
        return;

    if (!cellId.empty() && locationIt->second != cellId)
        return;

    mWorld.actorLocations.erase(locationIt);
}

void MPServer::rememberDeadVanillaActor(const ActorRegistryRecord& record)
{
    if (record.actor.mpNum != 0 || !record.actor.isDead || record.actor.refId.empty() || record.actor.cellId.empty())
        return;

    ActorRegistryRecord remembered = record;
    remembered.actor.cellId = record.actor.cellId;
    const std::string actorKey = makeActorKey(remembered.actor);
    bool changed = false;
    for (auto cellIt = mWorld.deadVanillaActorCells.begin(); cellIt != mWorld.deadVanillaActorCells.end();)
    {
        if (cellIt->first == remembered.actor.cellId)
        {
            ++cellIt;
            continue;
        }

        if (cellIt->second.erase(actorKey) != 0)
            changed = true;
        if (cellIt->second.empty())
            cellIt = mWorld.deadVanillaActorCells.erase(cellIt);
        else
            ++cellIt;
    }

    auto& deadActors = mWorld.deadVanillaActorCells[remembered.actor.cellId];
    const auto previousIt = deadActors.find(actorKey);
    if (previousIt == deadActors.end()
        || !sameDeadVanillaActorState(previousIt->second.actor, remembered.actor))
        changed = true;

    deadActors[actorKey] = remembered;
    rememberActorLocation(remembered.actor, remembered.actor.cellId);

    for (auto& [cellId, cellState] : mWorld.actorCells)
    {
        if (cellId == remembered.actor.cellId)
            continue;

        auto actorIt = cellState.actors.find(actorKey);
        if (actorIt == cellState.actors.end() || actorIt->second.actor.mpNum != 0)
            continue;

        cellState.actors.erase(actorIt);
        cellState.staleLiveVanillaDeathResendMs.erase(actorKey);
        changed = true;

        Log(Debug::Verbose) << "[Server] Removed stale live vanilla record for canonical corpse"
                            << " refId=" << remembered.actor.refId
                            << " refNum=" << remembered.actor.refNum
                            << " liveCell=" << cellId
                            << " deadCell=" << remembered.actor.cellId;
    }

    if (changed && mPlayerDb)
        mPlayerDb->upsertDeadVanillaActor(remembered.actor);
}

void MPServer::forgetDeadVanillaActor(const BaseActor& actor, const std::string& cellId)
{
    if (actor.mpNum != 0 || actor.refId.empty())
        return;

    const std::string actorKey = makeActorKey(actor);
    if (mPlayerDb)
        mPlayerDb->deleteDeadVanillaActor(actor.refId, actor.refNum);

    auto eraseFromCell = [&](const std::string& targetCellId) -> bool
    {
        auto cellIt = mWorld.deadVanillaActorCells.find(targetCellId);
        if (cellIt == mWorld.deadVanillaActorCells.end())
            return false;

        const bool erased = cellIt->second.erase(actorKey) != 0;
        if (cellIt->second.empty())
            mWorld.deadVanillaActorCells.erase(cellIt);
        return erased;
    };

    if (!cellId.empty())
    {
        eraseFromCell(cellId);
        return;
    }

    if (!actor.cellId.empty() && eraseFromCell(actor.cellId))
        return;

    for (auto cellIt = mWorld.deadVanillaActorCells.begin(); cellIt != mWorld.deadVanillaActorCells.end();)
    {
        cellIt->second.erase(actorKey);
        if (cellIt->second.empty())
            cellIt = mWorld.deadVanillaActorCells.erase(cellIt);
        else
            ++cellIt;
    }
}

const MPServer::ActorRegistryRecord* MPServer::findDeadVanillaActor(
    const BaseActor& actor, std::string* cellId) const
{
    if (actor.mpNum != 0 || actor.refId.empty())
        return nullptr;

    const std::string actorKey = makeActorKey(actor);
    auto findInCell = [&](const std::string& targetCellId) -> const ActorRegistryRecord*
    {
        const auto cellIt = mWorld.deadVanillaActorCells.find(targetCellId);
        if (cellIt == mWorld.deadVanillaActorCells.end())
            return nullptr;

        const auto actorIt = cellIt->second.find(actorKey);
        if (actorIt == cellIt->second.end() || !actorIt->second.actor.isDead)
            return nullptr;

        if (cellId)
            *cellId = targetCellId;
        return &actorIt->second;
    };

    if (!actor.cellId.empty())
    {
        if (const ActorRegistryRecord* record = findInCell(actor.cellId))
            return record;
    }

    for (const auto& [targetCellId, actors] : mWorld.deadVanillaActorCells)
    {
        const auto actorIt = actors.find(actorKey);
        if (actorIt == actors.end() || !actorIt->second.actor.isDead)
            continue;

        if (cellId)
            *cellId = targetCellId;
        return &actorIt->second;
    }

    return nullptr;
}

bool MPServer::rejectStaleAliveVanillaActor(
    const BaseActor& actor,
    const std::string& incomingCellId,
    const ConnectedClient& sender,
    const char* packetName) const
{
    if (actor.mpNum != 0 || actor.refId.empty() || actor.isDead)
        return false;

    std::string deadCellId;
    const ActorRegistryRecord* deadRecord = findDeadVanillaActor(actor, &deadCellId);
    if (!deadRecord)
        return false;

    const bool importantPacket = std::strcmp(packetName, "ActorList") == 0
        || std::strcmp(packetName, "ActorAttack") == 0
        || std::strcmp(packetName, "ActorCellChange") == 0;
    Log(importantPacket ? Debug::Info : Debug::Verbose) << "[Server] " << packetName
                        << " ignored stale alive vanilla actor from " << sender.name
                        << " refId=" << actor.refId
                        << " refNum=" << actor.refNum
                        << " incomingCell=" << incomingCellId
                        << " deadCell=" << deadCellId
                        << " deadHp=" << deadRecord->actor.dynamicStats.health.current;
    return true;
}

bool MPServer::rejectResetStaleDeadVanillaActor(
    const BaseActor& actor,
    const std::string& incomingCellId,
    const ConnectedClient& sender,
    const char* packetName) const
{
    if (actor.mpNum != 0 || actor.refId.empty() || !actor.isDead)
        return false;

    const auto cellIt = mWorld.actorCells.find(incomingCellId);
    if (cellIt == mWorld.actorCells.end())
        return false;

    const std::string actorKey = makeActorKey(actor);
    if (cellIt->second.resetSuppressedVanillaDeaths.count(actorKey) == 0)
        return false;

    const bool importantPacket = std::strcmp(packetName, "ActorList") == 0
        || std::strcmp(packetName, "ActorDeath") == 0
        || std::strcmp(packetName, "ActorStatsDynamic") == 0;
    Log(importantPacket ? Debug::Info : Debug::Verbose) << "[Server] " << packetName
                        << " ignored stale dead vanilla actor after reset"
                        << " from=" << sender.name
                        << " refId=" << actor.refId
                        << " refNum=" << actor.refNum
                        << " incomingCell=" << incomingCellId;
    return true;
}

void MPServer::clearResetStaleDeathSuppressionForAliveVanillaActor(
    const BaseActor& actor,
    const std::string& incomingCellId)
{
    if (actor.mpNum != 0 || actor.refId.empty() || actor.isDead)
        return;

    auto cellIt = mWorld.actorCells.find(incomingCellId);
    if (cellIt == mWorld.actorCells.end() || cellIt->second.resetSuppressedVanillaDeaths.empty())
        return;

    const std::string actorKey = makeActorKey(actor);
    cellIt->second.resetSuppressedVanillaDeaths.erase(actorKey);
}

std::size_t MPServer::mergeDeadVanillaActorsForCell(
    const std::string& cellId,
    std::unordered_map<std::string, ActorRegistryRecord>& actors) const
{
    const auto cellIt = mWorld.deadVanillaActorCells.find(cellId);
    if (cellIt == mWorld.deadVanillaActorCells.end())
        return 0;

    std::size_t merged = 0;
    for (const auto& [actorKey, record] : cellIt->second)
    {
        auto actorIt = actors.find(actorKey);
        if (actorIt == actors.end() || !actorIt->second.actor.isDead)
            ++merged;

        ActorRegistryRecord remembered = record;
        remembered.actor.cellId = cellId;
        actors[actorKey] = std::move(remembered);
    }
    return merged;
}

ActorInstanceId MPServer::assignActorNetId(const BaseActor& actor)
{
    const ActorInstanceId actorNetId = actorInstanceIdFromActor(actor);
    if (actorNetId == 0)
        return 0;

    const std::string actorKey = makeActorKey(actor);
    mWorld.actorNetIdsByKey[actorKey] = actorNetId;
    mWorld.actorKeysByNetId[actorNetId] = actorKey;
    return actorNetId;
}

ActorInstanceId MPServer::ensureActorNetId(ActorRegistryRecord& record, const std::string& cellId)
{
    if (!cellId.empty())
        record.actor.cellId = cellId;

    const ActorInstanceId expectedActorNetId = actorInstanceIdFromActor(record.actor);
    if (expectedActorNetId == 0)
        return 0;

    if (record.actorNetId != 0 && record.actorNetId != expectedActorNetId)
    {
        mWorld.actorKeysByNetId.erase(record.actorNetId);
        mWorld.actorNetIdsByKey.erase(makeActorKey(record.actor));
        record.actorNetId = 0;
    }

    if (record.actorNetId == 0)
        record.actorNetId = assignActorNetId(record.actor);
    return record.actorNetId;
}

void MPServer::forgetActorNetId(ActorInstanceId actorNetId, const BaseActor& actor)
{
    if (actorNetId == 0)
        return;

    const auto keyIt = mWorld.actorKeysByNetId.find(actorNetId);
    if (keyIt != mWorld.actorKeysByNetId.end())
    {
        mWorld.actorNetIdsByKey.erase(keyIt->second);
        mWorld.actorKeysByNetId.erase(keyIt);
    }
    else
    {
        mWorld.actorNetIdsByKey.erase(makeActorKey(actor));
    }

    for (auto& clientEntry : mClients)
    {
        ConnectedClient& client = clientEntry.second;
        client.actorV2IdentitySent.erase(actorNetId);
        client.actorV2IdentityAcked.erase(actorNetId);
        client.actorV2LastSentMs.erase(actorNetId);
        client.actorV2MissingIdentityByNetIdWindow.erase(actorNetId);
    }
}

ActorIdentityList MPServer::buildActorIdentityList(
    const std::string& cellId,
    CellActorState& cellState,
    std::unordered_map<std::string, ActorRegistryRecord>& actors)
{
    std::size_t missingIdentity = 0;
    std::size_t ambiguousIdentityNormalized = 0;
    std::size_t unmanagedSpawnerPruned = 0;
    std::vector<std::string> unmanagedSpawnerKeys;
    for (auto& [actorKey, record] : cellState.actors)
    {
        if (normalizeActorIdentity(record.actor))
            ++ambiguousIdentityNormalized;
        if (isUnmanagedSpawnerActor(record.actor))
        {
            unmanagedSpawnerKeys.push_back(actorKey);
            continue;
        }
        ensureActorNetId(record, cellId);
    }
    for (const std::string& actorKey : unmanagedSpawnerKeys)
    {
        auto actorIt = cellState.actors.find(actorKey);
        if (actorIt != cellState.actors.end())
        {
            forgetActorLocation(actorIt->second.actor, cellId);
            cellState.actors.erase(actorIt);
            ++unmanagedSpawnerPruned;
        }
    }

    ActorIdentityList identityList;
    identityList.protocolVersion = ActorSyncProtocolVersionV2;
    identityList.cellId = cellId;
    identityList.authorityGuid = cellState.authorityGuid;
    identityList.authorityGeneration = cellState.authorityGeneration;
    identityList.sequence = cellState.nextSnapshotSequence;
    identityList.serverTimestamp = currentServerTimeMs();
    identityList.actors.reserve(actors.size());

    for (auto& [actorKey, record] : actors)
    {
        if (normalizeActorIdentity(record.actor))
            ++ambiguousIdentityNormalized;
        if (hasMissingActorInstanceIdentity(record.actor))
        {
            ++missingIdentity;
            continue;
        }
        if (isUnmanagedSpawnerActor(record.actor))
        {
            ++unmanagedSpawnerPruned;
            continue;
        }

        const ActorInstanceId actorNetId = ensureActorNetId(record, cellId);
        if (actorNetId == 0)
        {
            ++missingIdentity;
            continue;
        }

        ActorIdentityRecord identity;
        identity.actorNetId = actorNetId;
        identity.persistent = record.persistent;
        identity.serverSpawned = record.actor.mpNum != 0;
        identity.actor = record.actor;
        identity.actor.cellId = cellId;
        identityList.actors.push_back(std::move(identity));
    }

    if (missingIdentity != 0 || ambiguousIdentityNormalized != 0 || unmanagedSpawnerPruned != 0)
    {
        const bool importantIdentityRepair = missingIdentity != 0 || ambiguousIdentityNormalized != 0;
        Log(importantIdentityRepair ? Debug::Info : Debug::Verbose)
            << "[Server] ActorIdentity normalized"
            << " cell=" << cellId
            << " sent=" << identityList.actors.size()
            << " missingIdentity=" << missingIdentity
            << " ambiguousIdentityNormalized=" << ambiguousIdentityNormalized
            << " unmanagedSpawnerPruned=" << unmanagedSpawnerPruned;
    }

    return identityList;
}

std::optional<MPServer::ActorRegistryRecord> MPServer::removeActorFromOtherCells(
    const BaseActor& actor,
    const std::string& destinationCellId,
    std::unordered_set<std::string>& changedCellIds)
{
    if (actor.mpNum == 0 && actor.refId.empty())
        return std::nullopt;

    const std::string actorKey = makeActorKey(actor);
    std::optional<ActorRegistryRecord> migratedRecord;
    bool removedSpawnedActorLink = false;

    auto removeFromCell = [&](const std::string& cellId, CellActorState& cellState)
    {
        if (cellId == destinationCellId)
            return;

        auto actorIt = cellState.actors.find(actorKey);
        if (actorIt == cellState.actors.end())
            return;

        ActorRegistryRecord removedRecord = actorIt->second;
        if (!migratedRecord || removedRecord.lastSnapshotTime >= migratedRecord->lastSnapshotTime)
            migratedRecord = removedRecord;

        if (mPlayerDb && removedRecord.actor.mpNum != 0)
        {
            mPlayerDb->deleteSpawnedActorDynamicRecordLink(removedRecord.actor.mpNum, cellId);
            removedSpawnedActorLink = true;
        }

        cellState.actors.erase(actorIt);
        forgetActorLocation(removedRecord.actor, cellId);
        changedCellIds.insert(cellId);

        Log(removedRecord.actor.mpNum != 0 ? Debug::Info : Debug::Verbose)
            << "[Server] Actor migrated between cells"
            << " refId=" << actor.refId
            << " refNum=" << actor.refNum
            << " mpNum=" << actor.mpNum
            << " from=" << cellId
            << " to=" << destinationCellId;
    };

    const auto locationIt = mWorld.actorLocations.find(actorKey);
    std::string indexedCellId;
    if (locationIt != mWorld.actorLocations.end())
    {
        indexedCellId = locationIt->second;
        auto cellIt = mWorld.actorCells.find(indexedCellId);
        if (cellIt != mWorld.actorCells.end())
            removeFromCell(indexedCellId, cellIt->second);
        else if (indexedCellId != destinationCellId)
            mWorld.actorLocations.erase(locationIt);
    }

    // Fallback scan cleans up duplicate records from older builds or from any
    // path that inserted actor state before the location index existed.
    for (auto& [cellId, cellState] : mWorld.actorCells)
    {
        if (cellId == indexedCellId)
            continue;
        removeFromCell(cellId, cellState);
    }

    if (removedSpawnedActorLink)
        scheduleGeneratedDynamicRecordGc("actor_migration_unlink");

    return migratedRecord;
}

void MPServer::broadcastActorListForCell(const std::string& cellId, CellActorState& cellState)
{
    std::unordered_map<std::string, ActorRegistryRecord> actors = cellState.actors;
    mergeDeadVanillaActorsForCell(cellId, actors);

    ActorList actorList;
    actorList.cellId = cellId;
    actorList.isAuthority = false;
    actorList.authorityGuid = cellState.authorityGuid;
    actorList.authorityGeneration = cellState.authorityGeneration;
    actorList.snapshotSequence = cellState.nextSnapshotSequence++;
    actorList.serverTimestamp = currentServerTimeMs();
    actorList.actors.reserve(actors.size());
    for (const auto& [actorKey, record] : actors)
        actorList.actors.push_back(record.actor);

    PacketActorList pkt;
    pkt.setActorList(&actorList);
    broadcastActorIdentityForCell(cellId, cellState);
    broadcastActorToCell(cellId, pkt.encode());
}

void MPServer::broadcastActorPositionV2ToCell(
    const std::string& cellId, CellActorState& cellState, const ActorList& actorList, HSteamNetConnection except)
{
    static constexpr std::size_t kSnapshotCostBytes = 42;
    static constexpr std::size_t kBudgetBytes = 900;

    const uint64_t now = currentServerTimeMs();
    auto tierForActor = [](const BaseActor& actor) -> std::size_t
    {
        if (actor.isDead)
            return 4;
        if (actor.isAttackingOrCasting
            || (actor.animFlags.actionFlags & (AnimFlags::AF_ATTACKING | AnimFlags::AF_CASTING)) != 0)
            return 0;
        const float speedSq = actor.velocity.linear[0] * actor.velocity.linear[0]
            + actor.velocity.linear[1] * actor.velocity.linear[1]
            + actor.velocity.linear[2] * actor.velocity.linear[2];
        if (actor.isMoving || speedSq > 25.f)
            return 1;
        return 2;
    };
    auto intervalForTier = [](std::size_t tier) -> uint64_t
    {
        switch (tier)
        {
            case 0: return 50;
            case 1: return 50;
            case 2: return 250;
            case 3: return 1000;
            default: return 0;
        }
    };

    for (auto& [conn, client] : mClients)
    {
        if (conn == except
            || !clientHasActorCellLoaded(client, cellId)
            || client.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
            continue;

        ActorPositionV2List positionList;
        positionList.protocolVersion = ActorSyncProtocolVersionV2;
        positionList.authorityGuid = actorList.authorityGuid;
        positionList.authorityGeneration = actorList.authorityGeneration;
        positionList.sequence = actorList.snapshotSequence;
        positionList.serverTimestamp = actorList.serverTimestamp;

        std::size_t budgetUsed = 0;
        for (const BaseActor& actor : actorList.actors)
        {
            auto actorIt = cellState.actors.find(makeActorKey(actor));
            if (actorIt == cellState.actors.end())
                continue;

            ActorRegistryRecord& record = actorIt->second;
            const ActorInstanceId actorNetId = ensureActorNetId(record, cellId);
            if (actorNetId == 0)
                continue;

            if (client.actorV2IdentitySent.count(actorNetId) == 0
                || client.actorV2IdentityAcked.count(actorNetId) == 0)
            {
                ++client.actorV2PositionSuppressedUntilIdentityKnownWindow;
                ++client.actorV2MissingIdentityByNetIdWindow[actorNetId];
                continue;
            }

            const std::size_t tier = tierForActor(actor);
            if (tier < (sizeof(client.actorV2TierCounts) / sizeof(client.actorV2TierCounts[0])))
                ++client.actorV2TierCounts[tier];

            const uint64_t intervalMs = intervalForTier(tier);
            const bool forceSend = actor.position.isTeleporting || tier == 0;
            const uint64_t lastSent = client.actorV2LastSentMs[actorNetId];
            if (!forceSend && intervalMs == 0)
            {
                ++client.actorV2DeferredWindow;
                continue;
            }
            if (!forceSend && lastSent != 0 && now - lastSent < intervalMs)
            {
                ++client.actorV2DeferredWindow;
                continue;
            }
            if (budgetUsed + kSnapshotCostBytes > kBudgetBytes)
            {
                ++client.actorV2DeferredWindow;
                continue;
            }

            positionList.snapshots.push_back(makeCompactActorSnapshot(actor, actorNetId));
            client.actorV2LastSentMs[actorNetId] = now;
            budgetUsed += kSnapshotCostBytes;
        }

        if (!positionList.snapshots.empty())
        {
            PacketActorPositionV2 pkt;
            pkt.setPositionList(&positionList);
            const std::vector<uint8_t> encoded = pkt.encode();
            sendTo(conn, encoded, /*reliable=*/false);
            client.actorV2SnapshotsSentWindow += positionList.snapshots.size();
            client.actorV2BytesSentWindow += encoded.size();
        }

        if (client.actorV2DiagnosticsLastLogMs == 0)
            client.actorV2DiagnosticsLastLogMs = now;
        else if (now - client.actorV2DiagnosticsLastLogMs >= 1000)
        {
            ActorInstanceId noisiestMissingActorNetId = 0;
            std::size_t noisiestMissingCount = 0;
            for (const auto& [actorNetId, count] : client.actorV2MissingIdentityByNetIdWindow)
            {
                if (count > noisiestMissingCount)
                {
                    noisiestMissingActorNetId = actorNetId;
                    noisiestMissingCount = count;
                }
            }
            const bool logAtInfo = client.actorV2IdentitySentWindow != 0
                || client.actorV2IdentityAckedWindow != 0
                || client.actorV2PositionSuppressedUntilIdentityKnownWindow != 0
                || client.actorV2PresentationSuppressedUntilIdentityKnownWindow != 0
                || client.actorV2AttackSuppressedUntilIdentityKnownWindow != 0
                || noisiestMissingCount != 0
                || client.actorV2BytesSentWindow > 12000
                || client.actorV2DeferredWindow > 1200;

            if (logAtInfo)
            {
                Log(Debug::Info) << "[Server] ActorSync v2 budget"
                                 << " receiver=" << client.guid
                                 << " cell=" << cellId
                                 << " interested=" << actorList.actors.size()
                                 << " identitySent=" << client.actorV2IdentitySentWindow
                                 << " identityAcked=" << client.actorV2IdentityAckedWindow
                                 << " snapshots=" << client.actorV2SnapshotsSentWindow
                                 << " bytes=" << client.actorV2BytesSentWindow
                                 << " presentationSent=" << client.actorV2PresentationSentWindow
                                 << " presentationBytes=" << client.actorV2PresentationBytesSentWindow
                                 << " attackSent=" << client.actorV2AttackSentWindow
                                 << " attackSuppressedUntilIdentityKnown=" << client.actorV2AttackSuppressedUntilIdentityKnownWindow
                                 << " budget=" << kBudgetBytes
                                 << " deferred=" << client.actorV2DeferredWindow
                                 << " positionSuppressedUntilIdentityKnown=" << client.actorV2PositionSuppressedUntilIdentityKnownWindow
                                 << " presentationSuppressedUntilIdentityKnown=" << client.actorV2PresentationSuppressedUntilIdentityKnownWindow
                                 << " missingIdentityActorNetId=" << noisiestMissingActorNetId
                                 << " missingIdentityActorKey=" << describeActorInstanceId(noisiestMissingActorNetId)
                                 << " missingIdentityCount=" << noisiestMissingCount
                                 << " tier0=" << client.actorV2TierCounts[0]
                                 << " tier1=" << client.actorV2TierCounts[1]
                                 << " tier2=" << client.actorV2TierCounts[2]
                                 << " tier3=" << client.actorV2TierCounts[3]
                                 << " tier4=" << client.actorV2TierCounts[4];
            }
            client.actorV2DiagnosticsLastLogMs = now;
            client.actorV2IdentitySentWindow = 0;
            client.actorV2IdentityAckedWindow = 0;
            client.actorV2SnapshotsSentWindow = 0;
            client.actorV2BytesSentWindow = 0;
            client.actorV2PresentationSentWindow = 0;
            client.actorV2PresentationBytesSentWindow = 0;
            client.actorV2AttackSentWindow = 0;
            client.actorV2AttackSuppressedUntilIdentityKnownWindow = 0;
            client.actorV2DeferredWindow = 0;
            client.actorV2PositionSuppressedUntilIdentityKnownWindow = 0;
            client.actorV2PresentationSuppressedUntilIdentityKnownWindow = 0;
            client.actorV2MissingIdentityByNetIdWindow.clear();
            for (std::size_t& count : client.actorV2TierCounts)
                count = 0;
        }
    }
}

void MPServer::broadcastActorPresentationV2ToCell(
    const std::string& cellId, CellActorState& cellState, const ActorList& actorList, HSteamNetConnection except)
{
    for (auto& [conn, client] : mClients)
    {
        if (conn == except
            || !clientHasActorCellLoaded(client, cellId)
            || client.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
            continue;

        ActorPresentationV2List presentationList;
        presentationList.protocolVersion = ActorSyncProtocolVersionV2;
        presentationList.authorityGuid = actorList.authorityGuid;
        presentationList.authorityGeneration = actorList.authorityGeneration;
        presentationList.sequence = actorList.snapshotSequence;
        presentationList.serverTimestamp = actorList.serverTimestamp;
        presentationList.snapshots.reserve(actorList.actors.size());

        for (const BaseActor& actor : actorList.actors)
        {
            auto actorIt = cellState.actors.find(makeActorKey(actor));
            if (actorIt == cellState.actors.end())
                continue;

            ActorRegistryRecord& record = actorIt->second;
            const ActorInstanceId actorNetId = ensureActorNetId(record, cellId);
            if (actorNetId == 0)
                continue;

            if (client.actorV2IdentitySent.count(actorNetId) == 0
                || client.actorV2IdentityAcked.count(actorNetId) == 0)
            {
                ++client.actorV2PresentationSuppressedUntilIdentityKnownWindow;
                ++client.actorV2MissingIdentityByNetIdWindow[actorNetId];
                continue;
            }

            presentationList.snapshots.push_back(makePresentationSnapshot(actor, actorNetId));
        }

        if (presentationList.snapshots.empty())
            continue;

        PacketActorPresentationV2 pkt;
        pkt.setPresentationList(&presentationList);
        const std::vector<uint8_t> encoded = pkt.encode();
        sendTo(conn, encoded, /*reliable=*/true);
        client.actorV2PresentationSentWindow += presentationList.snapshots.size();
        client.actorV2PresentationBytesSentWindow += encoded.size();
    }
}

void MPServer::upsertSpawnedActorDynamicRecordLinkIfNeeded(const BaseActor& actor)
{
    if (!mPlayerDb || actor.mpNum == 0 || actor.refId.empty() || actor.cellId.empty())
        return;

    for (const std::string_view recordType : { std::string_view("npc"), std::string_view("creature") })
    {
        if (mWorld.dynamicRecords.find(makeDynamicRecordKey(recordType, actor.refId)) == mWorld.dynamicRecords.end())
            continue;

        mPlayerDb->upsertSpawnedActorDynamicRecordLink(actor.refId, actor.cellId, actor.mpNum);
        return;
    }
}

void MPServer::persistSpawnedActorIfNeeded(ActorRegistryRecord& record, uint64_t now, bool force)
{
    if (!mPlayerDb || !record.persistent || record.actor.mpNum == 0)
        return;

    static constexpr uint64_t kHotPositionPersistIntervalMs = 2000;
    if (now == 0)
        now = currentServerTimeMs();

    if (!force
        && record.lastPersistTime != 0
        && now - record.lastPersistTime < kHotPositionPersistIntervalMs)
    {
        record.pendingPersist = true;
        return;
    }

    PersistedSpawnedActor persisted;
    persisted.actor = record.actor;
    persisted.persistent = true;
    mPlayerDb->upsertSpawnedActor(persisted);
    record.lastPersistTime = now;
    record.pendingPersist = false;
}

void MPServer::deletePersistedSpawnedActor(uint32_t mpNum)
{
    if (!mPlayerDb || mpNum == 0)
        return;

    mPlayerDb->deleteSpawnedActor(mpNum);
}

void MPServer::sendActorLifecycleEvent(const char* eventName, const BaseActor& actor, bool persistent)
{
    if (std::string_view(eventName) == "spawned")
        mLua.onActorSpawned(actor, persistent);
    else if (std::string_view(eventName) == "death")
        mLua.onActorDeath(actor, persistent);
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
        case PacketType::PlayerLoadedCells: handlePlayerLoadedCells(client, data, size); break;
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
        case PacketType::ActorPositionV2:  handleActorPositionV2(client, data, size);    break;
        case PacketType::ActorPresentationV2: handleActorPresentationV2(client, data, size); break;
        case PacketType::ActorIdentityAck: handleActorIdentityAck(client, data, size);   break;
        case PacketType::ActorAnimFlags:   handleActorAnimFlags(client, data, size);     break;
        case PacketType::ActorAnimPlay:    handleActorAnimPlay(client, data, size);      break;
        case PacketType::ActorAttack:      handleActorAttack(client, data, size);        break;
        case PacketType::ActorAttackV2:    handleActorAttackV2(client, data, size);      break;
        case PacketType::ActorCast:        handleActorCast(client, data, size);          break;
        case PacketType::ActorCellChange:  handleActorCellChange(client, data, size);    break;
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

    // Session-only spawned actor links from a previous process lifetime must not
    // keep generated actor records alive. Persistent spawned actors recreate
    // their links after dynamic records are loaded below.
    mPlayerDb->clearSpawnedActorDynamicRecordLinks();

    uint64_t maxMpNum = 0;
    std::size_t objectCount = 0;
    std::size_t spawnedActorCount = 0;
    std::size_t deadVanillaActorCount = 0;
    std::size_t dynamicRecordCount = 0;
    std::vector<DynamicRecordCatalogEntry> dynamicRecordCatalog;

    std::unordered_set<uint32_t> worldObjectMpNums;
    for (const auto& object : mPlayerDb->loadWorldObjects())
    {
        maxMpNum = std::max<uint64_t>(maxMpNum, object.mpNum);
        mWorld.placedObjects[object.cellId].push_back(object);
        if (object.mpNum != 0)
            worldObjectMpNums.insert(object.mpNum);
        ++objectCount;
    }

    for (const auto& record : mPlayerDb->loadContainerRecords())
    {
        if (record.mpNum != 0 && worldObjectMpNums.count(record.mpNum) != 0)
        {
            Log(Debug::Warning) << "[Server] Pruning stale container record for world object mpNum="
                                << record.mpNum
                                << " refId=" << record.refId
                                << " cell=" << record.cellId;
            mPlayerDb->deleteContainerRecord(record.cellId, record.refId, record.refNum);
            continue;
        }

        ContainerRecord normalized = record;
        normalizeContainerItems(normalized.items);
        mWorld.containers[makeContainerKey(
            normalized.cellId, normalized.refId, normalized.refNum, normalized.mpNum)] = std::move(normalized);
    }

    for (const auto& entry : mPlayerDb->loadDoorStates())
        mWorld.doorStates[entry.cellId].push_back(entry);

    dynamicRecordCatalog = mPlayerDb->loadDynamicRecordCatalog();

    for (const auto& record : mPlayerDb->loadDynamicRecords())
    {
        WorldState::StoredDynamicRecord stored;
        stored.recordType = record.recordType;
        stored.recordId = record.recordId;
        stored.recordScope = normalizeDynamicRecordScope(record.recordScope);
        if (stored.recordScope.empty())
            stored.recordScope = "permanent";
        stored.persistent = true;
        stored.data = record.data;
        stored.sequence = mWorld.nextDynamicRecordSequence++;
        mWorld.dynamicRecords[makeDynamicRecordKey(stored.recordType, stored.recordId)] = std::move(stored);
        ++dynamicRecordCount;

        DynamicRecordCatalogEntry catalogEntry;
        catalogEntry.recordType = record.recordType;
        catalogEntry.recordId = record.recordId;
        catalogEntry.recordScope = record.recordScope;
        catalogEntry.persistent = true;
        catalogEntry.createdAt = record.createdAt;
        catalogEntry.updatedAt = record.updatedAt;
        mPlayerDb->upsertDynamicRecordCatalog(catalogEntry);
    }

    std::unordered_set<std::string> sessionRecordIds;
    for (const auto& entry : dynamicRecordCatalog)
    {
        if (!entry.persistent && !entry.recordId.empty())
            sessionRecordIds.insert(entry.recordId);
    }

    if (!sessionRecordIds.empty())
    {
        cleanupDynamicReferences(
            [&](std::string_view refId) -> bool { return sessionRecordIds.count(std::string(refId)) != 0; },
            /*broadcastLive=*/false,
            "startup_session_records");

        for (const auto& entry : dynamicRecordCatalog)
        {
            if (entry.persistent)
                continue;
            mWorld.dynamicRecords.erase(makeDynamicRecordKey(entry.recordType, entry.recordId));
            mPlayerDb->replaceDynamicRecordDependencies(entry.recordType, entry.recordId, {});
            mPlayerDb->deleteDynamicRecord(entry.recordType, entry.recordId);
            mPlayerDb->deleteDynamicRecordLinks(entry.recordId);
            mPlayerDb->deleteDynamicRecordCatalog(entry.recordType, entry.recordId);
        }
    }

    for (const auto& record : mPlayerDb->loadSpawnedActors())
    {
        BaseActor actor = record.actor;
        if (actor.mpNum == 0 || actor.refId.empty() || actor.cellId.empty())
            continue;

        normalizeActorIdentity(actor);
        maxMpNum = std::max<uint64_t>(maxMpNum, actor.mpNum);
        auto& cellState = mWorld.actorCells[actor.cellId];
        // serverSpawnTime = 0: loaded actors are "old"; no spawn-grace needed.
        cellState.actors[makeActorKey(actor)] = { actor, currentServerTimeMs(), /*serverSpawnTime=*/0, /*persistent=*/true };
        rememberActorLocation(actor, actor.cellId);
        ++spawnedActorCount;

        for (const std::string_view recordType : { std::string_view("npc"), std::string_view("creature") })
        {
            const auto it = mWorld.dynamicRecords.find(makeDynamicRecordKey(recordType, actor.refId));
            if (it != mWorld.dynamicRecords.end())
            {
                mPlayerDb->upsertSpawnedActorDynamicRecordLink(actor.refId, actor.cellId, actor.mpNum);
                break;
            }
        }
    }

    for (const auto& persistedActor : mPlayerDb->loadDeadVanillaActors())
    {
        BaseActor actor = persistedActor;
        actor.mpNum = 0;
        if (actor.refId.empty() || actor.cellId.empty())
            continue;

        normalizeActorIdentity(actor);
        actor.isDead = true;
        actor.isInstantDeath = true;
        if (actor.dynamicStats.health.current > 0.f)
            actor.dynamicStats.health.current = 0.f;

        ActorRegistryRecord record { actor, currentServerTimeMs(), /*serverSpawnTime=*/0, /*persistent=*/false };
        const std::string actorKey = makeActorKey(actor);
        auto& cellState = mWorld.actorCells[actor.cellId];
        cellState.actors[actorKey] = record;
        mWorld.deadVanillaActorCells[actor.cellId][actorKey] = record;
        rememberActorLocation(actor, actor.cellId);
        ++deadVanillaActorCount;
    }

    objectCount = 0;
    for (const auto& [cellId, objects] : mWorld.placedObjects)
        objectCount += objects.size();
    dynamicRecordCount = mWorld.dynamicRecords.size();

    const uint64_t minimumNextMpNum = std::max<uint64_t>(1, maxMpNum + 1);
    const uint64_t nextMpNum = mPlayerDb->loadNextMpNum(minimumNextMpNum);
    setNextWorldMpNum(nextMpNum);

    Log(Debug::Info) << "[Server] Loaded persistent world state: objects="
                     << objectCount
                     << " spawnedActors=" << spawnedActorCount
                     << " deadVanillaActors=" << deadVanillaActorCount
                     << " containers=" << mWorld.containers.size()
                     << " doorCells=" << mWorld.doorStates.size()
                     << " dynamicRecords=" << dynamicRecordCount
                     << " nextMpNum=" << nextMpNum;
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
void MPServer::sendDynamicRecordsToClient(HSteamNetConnection conn)
{
    if (mWorld.dynamicRecords.empty())
        return;

    std::vector<const WorldState::StoredDynamicRecord*> ordered;
    ordered.reserve(mWorld.dynamicRecords.size());
    for (const auto& [key, record] : mWorld.dynamicRecords)
        ordered.push_back(&record);

    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->sequence < right->sequence;
    });

    PacketRecordDynamic pkt;
    pkt.action = DynamicRecordAction::Upsert;

    for (const auto* record : ordered)
    {
        if (!pkt.entries.empty() && pkt.recordType != record->recordType)
        {
            sendTo(conn, pkt.encode());
            pkt.entries.clear();
        }

        pkt.recordType = record->recordType;
        pkt.entries.push_back({ record->recordId, record->data });
    }

    if (!pkt.entries.empty())
        sendTo(conn, pkt.encode());
}

std::unordered_map<std::string, uint64_t> MPServer::buildGeneratedDynamicRecordCounters(const std::string& prefix) const
{
    std::unordered_map<std::string, uint64_t> nextGeneratedNumbers;

    for (const auto& [key, record] : mWorld.dynamicRecords)
    {
        if (record.recordScope != "generated")
            continue;

        const auto maybeGeneratedNum = parseGeneratedRecordNumber(prefix, record.recordType, record.recordId);
        if (!maybeGeneratedNum)
            continue;

        uint64_t& nextValue = nextGeneratedNumbers[record.recordType];
        nextValue = std::max(nextValue, *maybeGeneratedNum + 1);
    }

    return nextGeneratedNumbers;
}

std::vector<DynamicRecordCatalogEntry> MPServer::listDynamicRecordCatalog()
{
    std::vector<DynamicRecordCatalogEntry> entries;
    if (mPlayerDb)
        entries = mPlayerDb->loadDynamicRecordCatalog();

    std::unordered_set<std::string> seenKeys;
    seenKeys.reserve(entries.size() + mWorld.dynamicRecords.size());

    for (auto& entry : entries)
    {
        const std::string key = makeDynamicRecordKey(entry.recordType, entry.recordId);
        entry.loaded = mWorld.dynamicRecords.find(key) != mWorld.dynamicRecords.end();
        seenKeys.insert(key);
    }

    for (const auto& [key, record] : mWorld.dynamicRecords)
    {
        if (seenKeys.count(key) != 0)
            continue;

        DynamicRecordCatalogEntry entry;
        entry.recordType = record.recordType;
        entry.recordId = record.recordId;
        entry.recordScope = record.recordScope;
        entry.persistent = record.persistent;
        entry.loaded = true;
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const DynamicRecordCatalogEntry& left, const DynamicRecordCatalogEntry& right) {
        if (left.recordType != right.recordType)
            return left.recordType < right.recordType;
        return left.recordId < right.recordId;
    });

    return entries;
}

std::optional<DynamicRecordCatalogEntry> MPServer::getDynamicRecordInfo(
    std::string_view recordType, std::string_view recordId)
{
    const std::string normalizedType = normalizeDynamicRecordType(recordType);
    if (normalizedType.empty() || recordId.empty())
        return std::nullopt;

    const std::string targetId(recordId);
    for (const auto& entry : listDynamicRecordCatalog())
    {
        if (entry.recordType == normalizedType && entry.recordId == targetId)
            return entry;
    }

    return std::nullopt;
}

std::vector<DatabaseTableInfo> MPServer::listBrowsableTables()
{
    return mPlayerDb ? mPlayerDb->listBrowsableTables() : std::vector<DatabaseTableInfo>{};
}

std::optional<DatabaseBrowsePage> MPServer::browseDatabaseTable(
    std::string_view tableName, int64_t offset, int64_t limit)
{
    if (!mPlayerDb)
        return std::nullopt;

    return mPlayerDb->browseTable(tableName, offset, limit);
}

std::vector<DynamicRecordCatalogEntry> MPServer::collectGeneratedDynamicRecordGcCandidates(
    const std::optional<std::string>& recordType, const std::optional<bool>& persistent)
{
    std::vector<DynamicRecordCatalogEntry> candidates;

    const std::string normalizedType = recordType ? normalizeDynamicRecordType(*recordType) : std::string{};
    if (recordType && normalizedType.empty())
        return candidates;

    for (const auto& entry : listDynamicRecordCatalog())
    {
        if (entry.recordScope != "generated")
            continue;
        if (entry.linkCount > 0)
            continue;
        if (!normalizedType.empty() && entry.recordType != normalizedType)
            continue;
        if (persistent && entry.persistent != *persistent)
            continue;

        candidates.push_back(entry);
    }

    return candidates;
}

std::size_t MPServer::gcGeneratedDynamicRecordsAfterUnlink(std::string_view reason)
{
    const auto candidates = collectGeneratedDynamicRecordGcCandidates();
    if (candidates.empty())
        return 0;

    std::size_t removed = 0;
    std::size_t warnedPersistent = 0;

    for (const auto& entry : candidates)
    {
        if (entry.persistent)
        {
            ++warnedPersistent;
            Log(Debug::Warning) << "[Server] Generated persistent dynamic record became unlinked and is now a GC candidate:"
                                << " type=" << entry.recordType
                                << " id=" << entry.recordId
                                << " reason=" << reason;
        }

        if (removeDynamicRecord(entry.recordType, entry.recordId))
            ++removed;
    }

    if (removed > 0)
    {
        Log(Debug::Info) << "[Server] Auto-GC removed " << removed
                         << " generated dynamic record(s) after unlink reason=" << reason
                         << " warnedPersistent=" << warnedPersistent;
    }

    return removed;
}

void MPServer::scheduleGeneratedDynamicRecordGc(std::string_view reason, std::chrono::milliseconds delay)
{
    mGeneratedRecordGcScheduled = true;
    mGeneratedRecordGcReason.assign(reason.begin(), reason.end());
    mGeneratedRecordGcDueTime = std::chrono::steady_clock::now() + delay;
}

void MPServer::flushScheduledGeneratedDynamicRecordGc()
{
    if (!mGeneratedRecordGcScheduled)
        return;

    if (std::chrono::steady_clock::now() < mGeneratedRecordGcDueTime)
        return;

    const std::string reason = mGeneratedRecordGcReason.empty() ? "deferred_unlink" : mGeneratedRecordGcReason;
    mGeneratedRecordGcScheduled = false;
    mGeneratedRecordGcReason.clear();
    gcGeneratedDynamicRecordsAfterUnlink(reason);
}

MPServer::DynamicReferenceCleanupStats MPServer::cleanupDynamicReferences(
    const std::function<bool(std::string_view)>& shouldRemoveRefId,
    bool broadcastLive,
    std::string_view reason)
{
    DynamicReferenceCleanupStats stats;

    std::vector<PlacedObject> removedObjects;
    for (auto it = mWorld.placedObjects.begin(); it != mWorld.placedObjects.end();)
    {
        auto& objects = it->second;
        for (const auto& object : objects)
        {
            if (shouldRemoveRefId(object.refId))
                removedObjects.push_back(object);
        }

        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [&](const PlacedObject& object) { return shouldRemoveRefId(object.refId); }),
            objects.end());

        if (objects.empty())
            it = mWorld.placedObjects.erase(it);
        else
            ++it;
    }

    for (const auto& object : removedObjects)
    {
        ++stats.placedObjects;
        mLua.removePlacedObject(object.mpNum);
        if (mPlayerDb)
            mPlayerDb->deleteWorldObject(object.mpNum);

        if (broadcastLive)
        {
            PacketObjectDelete pkt;
            pkt.mpNum = object.mpNum;
            pkt.cellId = object.cellId;
            broadcastToCell(object.cellId, pkt.encode());
        }
    }

    std::vector<ContainerRecord> updatedContainers;
    for (auto it = mWorld.containers.begin(); it != mWorld.containers.end();)
    {
        ContainerRecord& record = it->second;
        if (shouldRemoveRefId(record.refId))
        {
            ++stats.containers;
            if (mPlayerDb)
                mPlayerDb->deleteContainerRecord(record.cellId, record.refId, record.refNum);
            it = mWorld.containers.erase(it);
            continue;
        }

        const std::size_t oldCount = record.items.size();
        record.items.erase(std::remove_if(record.items.begin(), record.items.end(),
            [&](const ContainerItem& item) { return shouldRemoveRefId(item.refId); }),
            record.items.end());

        if (record.items.size() != oldCount)
        {
            stats.containerItems += oldCount - record.items.size();
            if (mPlayerDb)
                mPlayerDb->upsertContainerRecord(record);
            if (broadcastLive)
                updatedContainers.push_back(record);
        }

        ++it;
    }

    if (broadcastLive)
    {
        for (const auto& record : updatedContainers)
        {
            PacketContainer pkt;
            pkt.container = record;
            pkt.mAction = static_cast<uint8_t>(ContainerAction::Set);
            broadcastToCell(record.cellId, pkt.encode());
        }
    }

    for (auto it = mWorld.doorStates.begin(); it != mWorld.doorStates.end();)
    {
        auto& entries = it->second;
        std::vector<DoorEntry> removedEntries;
        for (const auto& entry : entries)
        {
            if (shouldRemoveRefId(entry.refId))
                removedEntries.push_back(entry);
        }

        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const DoorEntry& entry) { return shouldRemoveRefId(entry.refId); }),
            entries.end());

        for (const auto& entry : removedEntries)
        {
            ++stats.doorStates;
            if (mPlayerDb)
                mPlayerDb->deleteDoorState(entry.cellId, entry.refId, entry.refNum);
        }

        if (entries.empty())
            it = mWorld.doorStates.erase(it);
        else
            ++it;
    }

    std::unordered_set<int64_t> onlineCharacterIds;
    bool snapshotDirty = false;

    for (auto& [conn, client] : mClients)
    {
        if (client.dbCharacterId != 0)
            onlineCharacterIds.insert(client.dbCharacterId);

        bool inventoryChanged = false;
        auto& inventory = client.player.inventoryChanges.items;
        const std::size_t oldInventoryCount = inventory.size();
        inventory.erase(std::remove_if(inventory.begin(), inventory.end(),
            [&](const Item& item) { return shouldRemoveRefId(item.refId); }),
            inventory.end());
        if (inventory.size() != oldInventoryCount)
        {
            stats.inventoryItems += oldInventoryCount - inventory.size();
            inventoryChanged = true;
        }

        bool equipmentChanged = false;
        for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            auto& entry = client.player.equipment[slot];
            if (entry.item.refId.empty())
                continue;

            if (shouldRemoveRefId(entry.item.refId) || !inventoryContainsItemIdentity(inventory, entry.item))
            {
                ++stats.equipmentItems;
                entry = EquipmentItem{};
                entry.slot = slot;
                equipmentChanged = true;
            }
        }

        if (!inventoryChanged && !equipmentChanged)
            continue;

        ++stats.characters;
        client.player.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
        if (mPlayerDb && client.dbCharacterId != 0)
        {
            mPlayerDb->saveCharacterInventory(client.dbCharacterId, client.player.inventoryChanges.items, false);
            std::vector<EquipmentItem> equipment(client.player.equipment.begin(), client.player.equipment.end());
            mPlayerDb->saveCharacterEquipment(client.dbCharacterId, equipment, false);
        }

        if (broadcastLive)
        {
            sendAuthoritativeInventory(client);
            sendAuthoritativeEquipment(client);
        }
        snapshotDirty = true;
    }

    if (mPlayerDb)
    {
        for (const int64_t characterId : mPlayerDb->listCharactersWithSavedItems())
        {
            if (onlineCharacterIds.count(characterId) != 0)
                continue;

            std::vector<Item> inventory = mPlayerDb->loadCharacterInventory(characterId);
            const std::size_t oldInventoryCount = inventory.size();
            inventory.erase(std::remove_if(inventory.begin(), inventory.end(),
                [&](const Item& item) { return shouldRemoveRefId(item.refId); }),
                inventory.end());
            const std::size_t removedInventory = oldInventoryCount - inventory.size();

            std::vector<EquipmentItem> equipment = mPlayerDb->loadCharacterEquipment(characterId);
            const std::size_t oldEquipmentCount = equipment.size();
            equipment.erase(std::remove_if(equipment.begin(), equipment.end(),
                [&](const EquipmentItem& entry)
                {
                    return shouldRemoveRefId(entry.item.refId)
                        || !inventoryContainsItemIdentity(inventory, entry.item);
                }),
                equipment.end());
            const std::size_t removedEquipment = oldEquipmentCount - equipment.size();

            if (removedInventory == 0 && removedEquipment == 0)
                continue;

            stats.inventoryItems += removedInventory;
            stats.equipmentItems += removedEquipment;
            ++stats.characters;
            mPlayerDb->saveCharacterInventory(characterId, inventory, false);
            mPlayerDb->saveCharacterEquipment(characterId, equipment, false);
        }
    }

    if (snapshotDirty)
        syncLuaSnapshot();

    const std::size_t totalRemoved = stats.placedObjects + stats.containers + stats.containerItems
        + stats.doorStates + stats.inventoryItems + stats.equipmentItems;
    if (totalRemoved > 0)
    {
        Log(Debug::Info) << "[Server] Cleaned dangling dynamic references (" << reason << "):"
                         << " placed=" << stats.placedObjects
                         << " containers=" << stats.containers
                         << " containerItems=" << stats.containerItems
                         << " doors=" << stats.doorStates
                         << " inventoryItems=" << stats.inventoryItems
                         << " equipmentItems=" << stats.equipmentItems
                         << " characters=" << stats.characters;
    }

    return stats;
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

    if (hs.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
    {
        PacketHandshakeResponse rsp;
        rsp.accepted      = false;
        rsp.serverVersion = SERVER_VERSION;
        rsp.rejectReason  = "ActorSync protocol mismatch: server requires v2";
        rsp.actorSyncProtocolVersion = ActorSyncProtocolVersionV2;
        sendTo(c.conn, rsp.encode());
        mInterface->CloseConnection(c.conn, 0, "ActorSync protocol mismatch", true);
        return;
    }
    c.actorSyncProtocolVersion = ActorSyncProtocolVersionV2;

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
    rsp.actorSyncProtocolVersion = c.actorSyncProtocolVersion;
    sendTo(c.conn, rsp.encode());

    Log(Debug::Info) << "[Server] Handshake accepted: " << c.name
                     << " guid=" << c.guid
                     << " actorSyncProtocol=" << c.actorSyncProtocolVersion;

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
    auto applyDefaultSpawn = [this](PacketCharacterData& pkt) {
        pkt.spawnCell = mDefaultSpawnCell;
        if (!mHasDefaultSpawnPosition)
            return;

        pkt.spawnX = mDefaultSpawnPosition.pos[0];
        pkt.spawnY = mDefaultSpawnPosition.pos[1];
        pkt.spawnZ = mDefaultSpawnPosition.pos[2];
        pkt.spawnRotX = mDefaultSpawnPosition.rot[0];
        pkt.spawnRotY = mDefaultSpawnPosition.rot[1];
        pkt.spawnRotZ = mDefaultSpawnPosition.rot[2];
    };
    applyDefaultSpawn(cdPkt);
    bool sendSavedInventory = false;
    bool sendSavedEquipment = false;
    c.hasRestoredStatsSnapshot = false;
    c.acceptedPlayerStatsThisSession = false;
    c.playerStatsRestoreGuardUntilMs = 0;
    c.restoredInventorySnapshot.clear();
    c.restoredEquipmentSnapshot = {};
    c.hasRestoredInventorySnapshot = false;
    c.hasRestoredEquipmentSnapshot = false;
    c.acceptedPlayerInventoryThisSession = false;
    c.acceptedPlayerEquipmentThisSession = false;
    c.playerInventoryRestoreGuardUntilMs = 0;
    c.playerEquipmentRestoreGuardUntilMs = 0;
    c.lastPlayerInventoryRestoreCorrectionLogMs = 0;
    c.lastPlayerEquipmentRestoreCorrectionLogMs = 0;

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
                applyDefaultSpawn(cdPkt);
                cdPkt.characterName  = sel.charName;
                for (const auto& mark : mDefaultPlayerMarks)
                    mPlayerDb->upsertCharacterMark(rec.characterId, mark);
                Log(Debug::Info) << "[Server] New character slot '" << sel.charName
                                 << "' created for " << c.name;
                if (!mDefaultPlayerMarks.empty())
                    Log(Debug::Info) << "[Server] Added " << mDefaultPlayerMarks.size()
                                     << " default mark(s) to '" << sel.charName << "'";
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
                if (rec->isNew)
                {
                    applyDefaultSpawn(cdPkt);
                }
                else
                {
                    cdPkt.spawnCell = rec->cell.empty() ? mDefaultSpawnCell : rec->cell;
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
                        c.restoredInventorySnapshot = c.player.inventoryChanges.items;
                        c.hasRestoredInventorySnapshot = true;
                        c.playerInventoryRestoreGuardUntilMs = currentServerTimeMs() + 5000;
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
                        c.restoredEquipmentSnapshot = c.player.equipment;
                        c.hasRestoredEquipmentSnapshot = true;
                        c.playerEquipmentRestoreGuardUntilMs = currentServerTimeMs() + 5000;
                        sendSavedEquipment = true;
                    }

                    if (rec->hasSavedStats && mPlayerDb->loadCharacterStats(rec->characterId, c.player))
                    {
                        cdPkt.hasSavedStats = true;
                        cdPkt.dynamicStats = c.player.dynamicStats;
                        cdPkt.attributes = c.player.attributes;
                        cdPkt.skills = c.player.skills;
                        cdPkt.level = c.player.level;
                        cdPkt.levelProgress = c.player.levelProgress;
                        c.restoredStatsSnapshot = c.player;
                        c.hasRestoredStatsSnapshot = true;
                        c.playerStatsRestoreGuardUntilMs = currentServerTimeMs() + 5000;
                        const Attribute& strength = c.player.attributes[0];
                        const Skill& blunt = c.player.skills[4];
                        if (strength.base > 100 || blunt.base > 100.f)
                        {
                            Log(Debug::Info) << "[PlayerDB] loaded persistent player stats"
                                             << " charId=" << rec->characterId
                                             << " name=" << sel.charName
                                             << " strength=" << strength.base
                                             << " blunt=" << blunt.base
                                             << " hp=" << c.player.dynamicStats.health.current
                                             << "/" << c.player.dynamicStats.health.base;
                        }
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
        applyDefaultSpawn(cdPkt);
        if (mDefaultPlayerMarks.empty())
            mLua.clearPlayerMarks(c.guid);
        else
            mLua.setPlayerMarks(c.guid, mDefaultPlayerMarks);
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

    sendDynamicRecordsToClient(c.conn);

    if (sendSavedInventory)
    {
        PacketPlayerInventory inventory;
        inventory.setPlayer(&c.player);
        sendTo(c.conn, inventory.encode());
        Log(Debug::Info) << "[PlayerDB] sent player inventory restore"
                         << " guid=" << c.guid
                         << " charId=" << c.dbCharacterId
                         << " name=" << c.slotName
                         << " stacks=" << c.player.inventoryChanges.items.size();
    }

    if (sendSavedEquipment)
    {
        PacketPlayerEquipment equipment;
        equipment.setPlayer(&c.player);
        sendTo(c.conn, equipment.encode());
        Log(Debug::Info) << "[PlayerDB] sent player equipment restore"
                         << " guid=" << c.guid
                         << " charId=" << c.dbCharacterId
                         << " name=" << c.slotName
                         << " equipped=" << equippedItemCount(c.player.equipment);
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
            PacketPlayerPosition positionPacket;
            positionPacket.setPlayer(&existingClient.player);
            sendTo(c.conn, positionPacket.encode());
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

    // Returning players do not include inventory/equipment in their first
    // client full sync; the server has the saved equipment snapshot.
    PacketPlayerEquipment equipment;
    equipment.setPlayer(&c.player);
    broadcastToAll(equipment.encode(), c.conn);
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
    const uint32_t cellChangeSequence = pkt.getSequence();
    c.player.velocity = {};
    c.player.position.isTeleporting = true;

    const std::string newCell = makeCellKey(c.player.cell);
    Log(Debug::Info) << "[Server] " << c.name << " → cell: " << newCell;

    syncLuaSnapshot();
    mLua.onPlayerCellChange(c.guid, c.name, newCell, oldCell);

    if (!oldCell.empty() && oldCell != newCell)
        refreshActorAuthorityForCell(oldCell);
    if (!newCell.empty())
        refreshActorAuthorityForCell(newCell, c.guid);

    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
    {
        PacketPlayerPosition positionPacket;
        positionPacket.setPlayer(&c.player);
        broadcastToAll(positionPacket.encode(cellChangeSequence), c.conn);
    }
    const std::string cellKey = makeCellKey(c.player.cell);
    if (!cellKey.empty())
        sendCellStateToClient(c.conn, cellKey);
    sendPlayerStateBootstrapToClient(c);
}

void MPServer::handlePlayerLoadedCells(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerLoadedCells pkt;
    if (!pkt.decode(data, size))
        return;

    if (!c.charSelectComplete)
        return;

    if (pkt.sequence != 0 && c.loadedActorCellsSequence != 0
        && pkt.sequence <= c.loadedActorCellsSequence)
    {
        Log(Debug::Verbose) << "[Server] Ignoring stale PlayerLoadedCells from " << c.name
                            << " seq=" << pkt.sequence
                            << " current=" << c.loadedActorCellsSequence;
        return;
    }

    const std::string currentCell = makeCellKey(c.player.cell);
    if (currentCell.empty())
        return;

    if (!pkt.activeCellId.empty())
    {
        const auto parsedActive = parseCellKey(pkt.activeCellId);
        if (parsedActive && makeCellKey(*parsedActive) != currentCell)
        {
            Log(Debug::Verbose) << "[Server] PlayerLoadedCells active cell mismatch for " << c.name
                                << ": packet=" << pkt.activeCellId
                                << " server=" << currentCell;
        }
    }

    std::unordered_set<std::string> normalizedCells;
    normalizedCells.reserve(std::min<std::size_t>(pkt.loadedCellIds.size() + 1, MaxLoadedActorCells));

    const bool currentIsExterior = c.player.cell.isExterior;
    for (const std::string& rawCellId : pkt.loadedCellIds)
    {
        if (normalizedCells.size() >= MaxLoadedActorCells)
            break;

        const auto parsedCell = parseCellKey(rawCellId);
        if (!parsedCell)
            continue;

        const std::string normalizedCellId = makeCellKey(*parsedCell);
        if (normalizedCellId.empty())
            continue;

        if (!currentIsExterior)
        {
            if (normalizedCellId != currentCell)
                continue;
        }
        else if (!parsedCell->isExterior)
            continue;

        normalizedCells.insert(normalizedCellId);
    }

    normalizedCells.insert(currentCell);
    for (auto it = normalizedCells.begin(); normalizedCells.size() > MaxLoadedActorCells && it != normalizedCells.end();)
    {
        if (*it == currentCell)
        {
            ++it;
            continue;
        }
        it = normalizedCells.erase(it);
    }

    std::vector<std::string> addedCells;
    std::vector<std::string> removedCells;
    for (const std::string& cellId : normalizedCells)
    {
        if (c.loadedActorCells.find(cellId) == c.loadedActorCells.end())
            addedCells.push_back(cellId);
    }
    for (const std::string& cellId : c.loadedActorCells)
    {
        if (normalizedCells.find(cellId) == normalizedCells.end())
            removedCells.push_back(cellId);
    }

    c.loadedActorCells = std::move(normalizedCells);
    c.loadedActorCellsSequence = pkt.sequence;

    for (const std::string& cellId : addedCells)
        refreshActorAuthorityForCell(cellId, c.guid);
    for (const std::string& cellId : removedCells)
        refreshActorAuthorityForCell(cellId);

    Log(Debug::Verbose) << "[Server] PlayerLoadedCells from " << c.name
                        << " active=" << currentCell
                        << " loaded=" << c.loadedActorCells.size()
                        << " added=" << addedCells.size()
                        << " removed=" << removedCells.size()
                        << " seq=" << pkt.sequence;
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerEquipment(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer incoming = c.player;
    PacketPlayerEquipment pkt;
    pkt.setPlayer(&incoming);
    if (!pkt.decode(data, size)) return;

    const uint64_t nowMs = currentServerTimeMs();
    if (c.hasRestoredEquipmentSnapshot
        && !c.acceptedPlayerEquipmentThisSession
        && nowMs < c.playerEquipmentRestoreGuardUntilMs
        && looksLikeRestoredEquipmentRegression(incoming.equipment, c.restoredEquipmentSnapshot))
    {
        if (c.lastPlayerEquipmentRestoreCorrectionLogMs == 0
            || nowMs - c.lastPlayerEquipmentRestoreCorrectionLogMs >= 1000)
        {
            c.lastPlayerEquipmentRestoreCorrectionLogMs = nowMs;
            Log(Debug::Info) << "[PlayerDB] ignored startup player equipment overwrite"
                             << " charId=" << c.dbCharacterId
                             << " name=" << c.slotName
                             << " incomingEquipped=" << equippedItemCount(incoming.equipment)
                             << " restoredEquipped=" << equippedItemCount(c.restoredEquipmentSnapshot);
        }

        BasePlayer correction = c.player;
        correction.guid = c.guid;
        PacketPlayerEquipment correctionPkt;
        correctionPkt.setPlayer(&correction);
        sendTo(c.conn, correctionPkt.encode());
        return;
    }

    c.player.equipment = incoming.equipment;
    c.acceptedPlayerEquipmentThisSession = true;
    c.restoredEquipmentSnapshot = c.player.equipment;
    c.hasRestoredEquipmentSnapshot = true;
    c.playerEquipmentRestoreGuardUntilMs = 0;

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

    scheduleGeneratedDynamicRecordGc("player_equipment");
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

    const uint64_t nowMs = currentServerTimeMs();
    if (c.hasRestoredInventorySnapshot
        && !c.acceptedPlayerInventoryThisSession
        && nowMs < c.playerInventoryRestoreGuardUntilMs
        && looksLikeRestoredInventoryRegression(incoming.inventoryChanges, c.restoredInventorySnapshot))
    {
        if (c.lastPlayerInventoryRestoreCorrectionLogMs == 0
            || nowMs - c.lastPlayerInventoryRestoreCorrectionLogMs >= 1000)
        {
            c.lastPlayerInventoryRestoreCorrectionLogMs = nowMs;
            Log(Debug::Info) << "[PlayerDB] ignored startup player inventory overwrite"
                             << " charId=" << c.dbCharacterId
                             << " name=" << c.slotName
                             << " incomingItems=" << incoming.inventoryChanges.items.size()
                             << " restoredItems=" << c.restoredInventorySnapshot.size();
        }

        BasePlayer correction = c.player;
        correction.guid = c.guid;
        correction.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
        PacketPlayerInventory correctionPkt;
        correctionPkt.setPlayer(&correction);
        sendTo(c.conn, correctionPkt.encode());
        return;
    }

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

    c.acceptedPlayerInventoryThisSession = true;
    c.restoredInventorySnapshot = c.player.inventoryChanges.items;
    c.hasRestoredInventorySnapshot = true;
    c.playerInventoryRestoreGuardUntilMs = 0;

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
    scheduleGeneratedDynamicRecordGc("player_inventory");
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer incoming = c.player;
    PacketPlayerStatsDynamic pkt;
    pkt.setPlayer(&incoming);
    if (!pkt.decode(data, size)) return;

    const uint64_t nowMs = currentServerTimeMs();
    if (c.hasRestoredStatsSnapshot
        && !c.acceptedPlayerStatsThisSession
        && nowMs < c.playerStatsRestoreGuardUntilMs
        && !samePersistentPlayerStats(incoming, c.restoredStatsSnapshot)
        && looksLikeRestoredStatsRegression(incoming, c.restoredStatsSnapshot))
    {
        const Attribute& incomingStrength = incoming.attributes[0];
        const Skill& incomingBlunt = incoming.skills[4];
        const Attribute& restoredStrength = c.restoredStatsSnapshot.attributes[0];
        const Skill& restoredBlunt = c.restoredStatsSnapshot.skills[4];
        Log(Debug::Info) << "[PlayerDB] ignored startup player stats overwrite"
                         << " charId=" << c.dbCharacterId
                         << " name=" << c.slotName
                         << " incomingStrength=" << incomingStrength.base
                         << " incomingBlunt=" << incomingBlunt.base
                         << " restoredStrength=" << restoredStrength.base
                         << " restoredBlunt=" << restoredBlunt.base;

        BasePlayer correction = c.restoredStatsSnapshot;
        correction.guid = c.guid;
        PacketPlayerStatsDynamic correctionPkt;
        correctionPkt.setPlayer(&correction);
        sendTo(c.conn, correctionPkt.encode());
        return;
    }

    const bool hadPreviousStats = c.hasRestoredStatsSnapshot;
    const BasePlayer previousStats = c.restoredStatsSnapshot;
    copyPersistentPlayerStats(c.player, incoming);
    c.player.hasSavedStats = true;
    c.acceptedPlayerStatsThisSession = true;
    c.restoredStatsSnapshot = c.player;
    c.hasRestoredStatsSnapshot = true;
    c.playerStatsRestoreGuardUntilMs = 0;

    if (mPlayerDb && c.dbCharacterId != 0 && c.charSelectComplete)
    {
        try
        {
            mPlayerDb->saveCharacterStats(c.dbCharacterId, c.player);
            const Attribute& strength = c.player.attributes[0];
            const Skill& blunt = c.player.skills[4];
            const bool loggedTrackedStatsChanged = !hadPreviousStats
                || strength.base != previousStats.attributes[0].base
                || !sameStatFloat(blunt.base, previousStats.skills[4].base);
            if ((strength.base > 100 || blunt.base > 100.f) && loggedTrackedStatsChanged)
            {
                Log(Debug::Info) << "[PlayerDB] saved persistent player stats"
                                 << " charId=" << c.dbCharacterId
                                 << " name=" << c.slotName
                                 << " strength=" << strength.base
                                 << " blunt=" << blunt.base
                                 << " hp=" << c.player.dynamicStats.health.current
                                 << "/" << c.player.dynamicStats.health.base;
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[PlayerDB] saveCharacterStats error: " << e.what();
        }
    }
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

    std::unordered_set<uint32_t> previousSpawnedActorMpNums;
    std::unordered_map<uint32_t, bool> previousSpawnedActorPersistence;
    // Snapshot serverSpawnTime so we can protect recently server-spawned actors
    // from being evicted by an authority ActorList that arrived before the client
    // processed the spawn notification (timing race with the Lua tick thread).
    std::unordered_map<uint32_t, uint64_t> previousSpawnedActorSpawnTime;
    std::unordered_map<uint32_t, ActorRegistryRecord> previousActorRecords;
    std::unordered_map<std::string, ActorRegistryRecord> previousDeadVanillaActorRecords;
    std::vector<ActorRegistryRecord> previousCellRecords;
    std::unordered_set<std::string> previousCellActorKeys;
    for (const auto& [key, record] : cellState.actors)
    {
        previousCellRecords.push_back(record);
        previousCellActorKeys.insert(key);
        if (record.actor.mpNum != 0)
        {
            previousSpawnedActorMpNums.insert(record.actor.mpNum);
            previousSpawnedActorPersistence[record.actor.mpNum] = record.persistent;
            previousSpawnedActorSpawnTime[record.actor.mpNum] = record.serverSpawnTime;
            previousActorRecords[record.actor.mpNum] = record;
        }
        else if (record.actor.isDead)
            previousDeadVanillaActorRecords[key] = record;
    }

    std::unordered_set<uint32_t> currentSpawnedActorMpNums;
    std::unordered_set<std::string> migratedActorCells;
    std::unordered_set<std::string> canonicalDeadVanillaCellsToResend;
    std::size_t staleDeadVanillaCorrections = 0;
    std::size_t missingIdentityDropped = 0;
    std::size_t ambiguousIdentityNormalized = 0;
    std::size_t unmanagedSpawnerDropped = 0;
    std::size_t unknownSpawnedDropped = 0;
    std::size_t deadSpawnedSuppressed = 0;
    std::string firstUnmanagedSpawnerRefId;
    uint32_t firstUnmanagedSpawnerRefNum = 0;
    std::string firstUnknownSpawnedRefId;
    uint32_t firstUnknownSpawnedMpNum = 0;
    std::string firstDeadSpawnedRefId;
    uint32_t firstDeadSpawnedMpNum = 0;
    std::string firstDeadSpawnedKnownCell;
    cellState.actors.clear();

    auto findKnownDeadSpawnedRecord = [&](const BaseActor& actor, std::string* knownCellId = nullptr)
        -> const ActorRegistryRecord*
    {
        if (actor.mpNum == 0)
            return nullptr;

        const auto previousIt = previousActorRecords.find(actor.mpNum);
        if (previousIt != previousActorRecords.end() && previousIt->second.actor.isDead)
        {
            if (knownCellId)
                *knownCellId = incoming.cellId;
            return &previousIt->second;
        }

        const std::string actorKey = makeActorKey(actor);
        const auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt == mWorld.actorLocations.end())
            return nullptr;

        const auto cellIt = mWorld.actorCells.find(locationIt->second);
        if (cellIt == mWorld.actorCells.end())
            return nullptr;

        const auto actorIt = cellIt->second.actors.find(actorKey);
        if (actorIt == cellIt->second.actors.end() || !actorIt->second.actor.isDead)
            return nullptr;

        if (knownCellId)
            *knownCellId = locationIt->second;
        return &actorIt->second;
    };

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        if (normalizeActorIdentity(actor))
            ++ambiguousIdentityNormalized;
        if (hasMissingActorInstanceIdentity(actor))
        {
            ++missingIdentityDropped;
            continue;
        }
        if (isUnmanagedSpawnerActor(actor))
        {
            ++unmanagedSpawnerDropped;
            if (firstUnmanagedSpawnerRefId.empty())
            {
                firstUnmanagedSpawnerRefId = actor.refId;
                firstUnmanagedSpawnerRefNum = actor.refNum;
            }
            continue;
        }
        if (rejectResetStaleDeadVanillaActor(actor, incoming.cellId, c, "ActorList"))
            continue;
        clearResetStaleDeathSuppressionForAliveVanillaActor(actor, incoming.cellId);

        if (actor.mpNum == 0 && !actor.isDead && !actor.refId.empty())
        {
            std::string deadCellId;
            if (const ActorRegistryRecord* deadRecord = findDeadVanillaActor(actor, &deadCellId))
            {
                const std::string actorKey = makeActorKey(actor);
                const uint64_t nowMs = incoming.serverTimestamp != 0
                    ? incoming.serverTimestamp : currentServerTimeMs();
                uint64_t& lastResendMs = cellState.staleLiveVanillaDeathResendMs[actorKey];
                const bool shouldResend = lastResendMs == 0 || nowMs >= lastResendMs + 1000;
                if (shouldResend)
                {
                    lastResendMs = nowMs;
                    ++staleDeadVanillaCorrections;
                    canonicalDeadVanillaCellsToResend.insert(deadCellId);
                    Log(Debug::Info) << "[Server] ActorList suppressed stale live vanilla actor for canonical corpse"
                                     << " from=" << c.name
                                     << " refId=" << deadRecord->actor.refId
                                     << " refNum=" << deadRecord->actor.refNum
                                     << " incomingCell=" << incoming.cellId
                                     << " deadCell=" << deadCellId
                                     << " pos=(" << deadRecord->actor.position.pos[0]
                                     << "," << deadRecord->actor.position.pos[1]
                                     << "," << deadRecord->actor.position.pos[2] << ")"
                                     << " deathAnim='" << deadRecord->actor.deathAnimGroup << "'";
                }
                continue;
            }
        }

        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorList"))
            continue;

        const std::string actorKey = makeActorKey(actor);
        std::string knownDeadSpawnedCellId;
        if (const ActorRegistryRecord* deadSpawnedRecord = findKnownDeadSpawnedRecord(actor, &knownDeadSpawnedCellId))
        {
            ++deadSpawnedSuppressed;
            if (firstDeadSpawnedRefId.empty())
            {
                firstDeadSpawnedRefId = deadSpawnedRecord->actor.refId;
                firstDeadSpawnedMpNum = deadSpawnedRecord->actor.mpNum;
                firstDeadSpawnedKnownCell = knownDeadSpawnedCellId;
            }
            continue;
        }

        if (actor.mpNum != 0 && isExteriorCellKey(incoming.cellId))
        {
            const std::string positionCellId = exteriorCellIdForPosition(actor.position);
            constexpr float kExteriorCellMismatchHysteresis = 64.f;
            if (!positionCellId.empty()
                && positionCellId != incoming.cellId
                && exteriorCellBorderDistance(actor.position) > kExteriorCellMismatchHysteresis
                && clientHasActorCellLoaded(c, positionCellId))
            {
                if (actor.refId == "fargoth" || actor.refId == "heddvild")
                {
                    Log(Debug::Info) << "[Server] ActorList skipped stale spawned exterior cell"
                                     << " from=" << c.name
                                     << " refId=" << actor.refId
                                     << " mpNum=" << actor.mpNum
                                     << " incomingCell=" << incoming.cellId
                                     << " positionCell=" << positionCellId
                                     << " pos=(" << actor.position.pos[0]
                                     << "," << actor.position.pos[1]
                                     << "," << actor.position.pos[2] << ")";
                }
                continue;
            }
        }

        auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt != mWorld.actorLocations.end() && locationIt->second != incoming.cellId)
        {
            // If this actor previously belonged to this cell but has since migrated away,
            // this is a stale ActorList from the old cell. Ignore it.
            if (previousCellActorKeys.count(actorKey) != 0)
                continue;
        }

        std::optional<ActorRegistryRecord> migratedRecord
            = removeActorFromOtherCells(actor, incoming.cellId, migratedActorCells);
        const auto previousRecordIt = previousActorRecords.find(actor.mpNum);
        const ActorRegistryRecord* previousRecord = nullptr;
        if (previousRecordIt != previousActorRecords.end())
            previousRecord = &previousRecordIt->second;
        else if (migratedRecord)
            previousRecord = &*migratedRecord;
        if (actor.mpNum != 0 && previousRecord == nullptr)
        {
            ++unknownSpawnedDropped;
            if (firstUnknownSpawnedRefId.empty())
            {
                firstUnknownSpawnedRefId = actor.refId;
                firstUnknownSpawnedMpNum = actor.mpNum;
            }
            continue;
        }
        if (actor.mpNum != 0)
            currentSpawnedActorMpNums.insert(actor.mpNum);
        const bool hadPreviousRecord = previousRecord != nullptr;
        const bool wasDead = hadPreviousRecord && previousRecord->actor.isDead;
        const bool persistent = actor.mpNum != 0 && previousRecord != nullptr && previousRecord->persistent;
        auto [it, inserted] = cellState.actors.emplace(actorKey,
            ActorRegistryRecord { actor, incoming.serverTimestamp, 0, persistent });
        if (!inserted)
            it->second = { actor, incoming.serverTimestamp, 0, persistent };
        if (previousRecord && previousRecord->actorNetId != 0)
            it->second.actorNetId = previousRecord->actorNetId;
        else if (migratedRecord && migratedRecord->actorNetId != 0)
            it->second.actorNetId = migratedRecord->actorNetId;
        ensureActorNetId(it->second, incoming.cellId);
        // Preserve the original serverSpawnTime so the grace-period logic
        // remains accurate even after later client updates.
        const auto spawnTimeIt = previousSpawnedActorSpawnTime.find(actor.mpNum);
        if (spawnTimeIt != previousSpawnedActorSpawnTime.end())
            it->second.serverSpawnTime = spawnTimeIt->second;
        else if (migratedRecord)
            it->second.serverSpawnTime = migratedRecord->serverSpawnTime;
        persistSpawnedActorIfNeeded(it->second);
        rememberActorLocation(it->second.actor, incoming.cellId);
        if (migratedRecord && it->second.actor.mpNum != 0)
            upsertSpawnedActorDynamicRecordLinkIfNeeded(it->second.actor);

        if (actor.mpNum != 0 && hadPreviousRecord && actor.isDead && !wasDead)
        {
            Log(Debug::Info) << "[Server] ActorList observed death transition"
                             << " refId=" << it->second.actor.refId
                             << " mpNum=" << it->second.actor.mpNum
                             << " cell=" << incoming.cellId;
            sendActorLifecycleEvent("death", it->second.actor, it->second.persistent);
        }
    }

    if (missingIdentityDropped != 0 || ambiguousIdentityNormalized != 0 || unmanagedSpawnerDropped != 0)
    {
        const bool importantActorListRepair = missingIdentityDropped != 0 || ambiguousIdentityNormalized != 0;
        Log(importantActorListRepair ? Debug::Info : Debug::Verbose)
            << "[Server] ActorList normalized identity"
            << " from=" << c.name
            << " cell=" << incoming.cellId
            << " actors=" << incoming.actors.size()
            << " missingIdentityDropped=" << missingIdentityDropped
            << " ambiguousIdentityNormalized=" << ambiguousIdentityNormalized
            << " unmanagedSpawnerDropped=" << unmanagedSpawnerDropped
            << " firstUnmanagedSpawner=" << firstUnmanagedSpawnerRefId
            << " firstUnmanagedSpawnerRefNum=" << firstUnmanagedSpawnerRefNum;
    }

    if (unknownSpawnedDropped != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorList dropped unknown spawned actor(s)"
                            << " from=" << c.name
                            << " cell=" << incoming.cellId
                            << " actors=" << incoming.actors.size()
                            << " dropped=" << unknownSpawnedDropped
                            << " firstRefId=" << firstUnknownSpawnedRefId
                            << " firstMpNum=" << firstUnknownSpawnedMpNum;
    }

    if (deadSpawnedSuppressed != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorList suppressed update(s) for dead spawned actor"
                            << " from=" << c.name
                            << " incomingCell=" << incoming.cellId
                            << " suppressed=" << deadSpawnedSuppressed
                            << " firstRefId=" << firstDeadSpawnedRefId
                            << " firstMpNum=" << firstDeadSpawnedMpNum
                            << " firstKnownCell=" << firstDeadSpawnedKnownCell;
    }

    // Grace period for freshly server-spawned actors: the authority's
    // first ActorList can arrive before the client has processed the spawn
    // notification sent by spawnActor() (Lua tick vs. incoming packet race).
    // If the actor was spawned within the last 10 seconds and the authority
    // didn't include it, re-inject it rather than deleting it.  The authority
    // will acknowledge it once it processes the pending spawn notification.
    // Note: actors removed via mp.removeActor() are already gone from
    // cellState.actors before handleActorList runs, so they won't appear in
    // previousSpawnedActorMpNums and are unaffected by this guard.
    constexpr uint64_t kSpawnGraceMs = 10000; // 10 s
    for (uint32_t previousMpNum : previousSpawnedActorMpNums)
    {
        if (currentSpawnedActorMpNums.count(previousMpNum) != 0)
            continue; // acknowledged by client

        const uint64_t spawnTime = previousSpawnedActorSpawnTime[previousMpNum];
        const auto& prev = previousActorRecords[previousMpNum];
        if (spawnTime == 0 || prev.lastSnapshotTime > spawnTime)
            continue; // already acknowledged once by the authority

        const uint64_t age = (incoming.serverTimestamp > spawnTime)
            ? (incoming.serverTimestamp - spawnTime) : 0;
        if (age > kSpawnGraceMs)
            continue; // old enough that the authority has definitely seen it

        // Re-inject: the actor is too new to have been intentionally removed.
        const std::string actorKey = makeActorKey(prev.actor);
        const auto locationIt2 = mWorld.actorLocations.find(actorKey);
        if (locationIt2 != mWorld.actorLocations.end() && locationIt2->second != incoming.cellId)
            continue;
        cellState.actors[actorKey] = prev;
        rememberActorLocation(prev.actor, incoming.cellId);
        currentSpawnedActorMpNums.insert(previousMpNum);
        Log(Debug::Info) << "[Server] handleActorList re-injected recent actor"
                         << " mpNum=" << previousMpNum
                         << " age=" << age << "ms"
                         << " cell=" << incoming.cellId;
    }

    std::size_t retainedOmittedSpawnedActors = 0;
    for (uint32_t previousMpNum : previousSpawnedActorMpNums)
    {
        if (currentSpawnedActorMpNums.count(previousMpNum) != 0)
            continue;

        const auto previousRecordIt = previousActorRecords.find(previousMpNum);
        if (previousRecordIt == previousActorRecords.end())
            continue;

        const ActorRegistryRecord& previousRecord = previousRecordIt->second;
        const std::string actorKey = makeActorKey(previousRecord.actor);
        const auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt != mWorld.actorLocations.end() && locationIt->second != incoming.cellId)
            continue;

        cellState.actors[actorKey] = previousRecord;
        rememberActorLocation(previousRecord.actor, incoming.cellId);
        currentSpawnedActorMpNums.insert(previousMpNum);
        ++retainedOmittedSpawnedActors;
    }

    if (retainedOmittedSpawnedActors != 0)
    {
        Log(Debug::Verbose) << "[Server] handleActorList retained omitted spawned actor(s)"
                            << " from=" << c.name
                            << " cell=" << incoming.cellId
                            << " count=" << retainedOmittedSpawnedActors;
    }

    std::size_t retainedDeadVanillaActors = 0;
    for (const auto& [actorKey, record] : previousDeadVanillaActorRecords)
    {
        if (cellState.actors.find(actorKey) != cellState.actors.end())
            continue;

        cellState.actors[actorKey] = record;
        rememberActorLocation(record.actor, incoming.cellId);
        rememberDeadVanillaActor(record);
        ++retainedDeadVanillaActors;
    }
    if (retainedDeadVanillaActors != 0)
    {
        Log(Debug::Verbose) << "[Server] handleActorList retained dead vanilla actor(s)"
                            << " count=" << retainedDeadVanillaActors
                            << " cell=" << incoming.cellId;
    }

    const std::size_t restoredDeadVanillaActors = mergeDeadVanillaActorsForCell(incoming.cellId, cellState.actors);
    if (restoredDeadVanillaActors != 0)
    {
        Log(Debug::Verbose) << "[Server] handleActorList restored dead vanilla overlay(s)"
                            << " count=" << restoredDeadVanillaActors
                            << " cell=" << incoming.cellId;
    }

    for (const ActorRegistryRecord& previousRecord : previousCellRecords)
    {
        if (cellState.actors.find(makeActorKey(previousRecord.actor)) != cellState.actors.end())
            continue;
        forgetActorLocation(previousRecord.actor, incoming.cellId);
    }

    if (mPlayerDb)
    {
        bool removedActorLink = false;
        for (uint32_t previousMpNum : previousSpawnedActorMpNums)
        {
            if (currentSpawnedActorMpNums.count(previousMpNum) != 0)
                continue;

            // NEW: retain persistent actors instead of deleting them on relog
            if (previousSpawnedActorPersistence[previousMpNum])
            {
                const auto& prev = previousActorRecords[previousMpNum];
                const std::string actorKey = makeActorKey(prev.actor);
                const auto locationIt = mWorld.actorLocations.find(actorKey);
                if (locationIt != mWorld.actorLocations.end() && locationIt->second != incoming.cellId)
                    continue; // already migrated, do not duplicate into old cell

                cellState.actors[actorKey] = prev;
                rememberActorLocation(prev.actor, incoming.cellId);
                currentSpawnedActorMpNums.insert(previousMpNum);
                Log(Debug::Verbose) << "[Server] retained persistent spawned actor"
                                    << " mpNum=" << previousMpNum
                                    << " cell=" << incoming.cellId;
                continue;
            }

            mPlayerDb->deleteSpawnedActorDynamicRecordLink(previousMpNum, incoming.cellId);
            removedActorLink = true;
        }

        if (removedActorLink)
            scheduleGeneratedDynamicRecordGc("actor_list_unlink");
    }

    for (const std::string& migratedCellId : migratedActorCells)
    {
        auto migratedCellIt = mWorld.actorCells.find(migratedCellId);
        if (migratedCellIt != mWorld.actorCells.end())
            broadcastActorListForCell(migratedCellId, migratedCellIt->second);
    }

    ActorList deduped = incoming;
    deduped.actors.clear();
    deduped.actors.reserve(cellState.actors.size());
    for (const auto& [actorKey, record] : cellState.actors)
        deduped.actors.push_back(record.actor);

    PacketActorList out;
    out.setActorList(&deduped);
    broadcastActorIdentityForCell(incoming.cellId, cellState);
    broadcastActorToCell(incoming.cellId, out.encode(), c.conn);
    if (staleDeadVanillaCorrections != 0)
    {
        Log(Debug::Info) << "[Server] ActorList replaying canonical dead vanilla state"
                         << " staleLiveReports=" << staleDeadVanillaCorrections
                         << " cells=" << canonicalDeadVanillaCellsToResend.size()
                         << " sourceCell=" << incoming.cellId
                         << " count=" << staleDeadVanillaCorrections;
    }
    for (const std::string& deadCellId : canonicalDeadVanillaCellsToResend)
    {
        Log(Debug::Verbose) << "[Server] ActorList resending canonical corpse cell"
                            << " cell=" << deadCellId
                            << " sourceCell=" << incoming.cellId
                            << " staleLiveReports=" << staleDeadVanillaCorrections;
        sendActorStateToInterestedClients(deadCellId);
    }
}

// ---------------------------------------------------------------------------
void MPServer::handleActorPosition(ConnectedClient& c, const uint8_t* data, size_t size)
{
    if (c.actorSyncProtocolVersion >= ActorSyncProtocolVersionV2)
    {
        Log(Debug::Verbose) << "[Server] Ignoring retired legacy ActorPosition from v2 client"
                            << " from=" << c.name
                            << " protocol=" << c.actorSyncProtocolVersion;
        return;
    }

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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectResetStaleDeadVanillaActor(actor, incoming.cellId, c, "ActorPosition"))
            continue;
        clearResetStaleDeathSuppressionForAliveVanillaActor(actor, incoming.cellId);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorPosition"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorPosition");
        if (!stored)
        {
            filtered.actors.push_back(actor);
            continue;
        }
        if (stored->actor.mpNum != 0 && stored->actor.isDead)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.position = actor.position;
        stored->actor.velocity = actor.velocity;
        stored->actor.isMoving = actor.isMoving;
        stored->actor.hasWeaponDrawn = actor.hasWeaponDrawn;
        stored->actor.hasSpellReadied = actor.hasSpellReadied;
        stored->actor.isAttackingOrCasting = actor.isAttackingOrCasting;
        stored->actor.animFlags.movementFlags = actor.animFlags.movementFlags;
        stored->actor.animFlags.actionFlags = actor.animFlags.actionFlags;
        stored->actor.animFlags.animFwd = actor.animFlags.animFwd;
        stored->actor.animFlags.animSide = actor.animFlags.animSide;
        stored->actor.animFlags.currentAnimGroup = actor.animFlags.currentAnimGroup;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        ensureActorNetId(*stored, incoming.cellId);
        persistSpawnedActorIfNeeded(*stored, incoming.serverTimestamp, false);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    broadcastActorPositionV2ToCell(filtered.cellId, cellState, filtered, c.conn);
    broadcastActorPresentationV2ToCell(filtered.cellId, cellState, filtered, c.conn);

    PacketActorPosition out;
    out.setActorList(&filtered);
    const std::vector<uint8_t> encodedLegacy = out.encode();
    for (auto& [conn, client] : mClients)
    {
        if (conn == c.conn
            || !clientHasActorCellLoaded(client, filtered.cellId)
            || client.actorSyncProtocolVersion >= ActorSyncProtocolVersionV2)
            continue;
        sendTo(conn, encodedLegacy, /*reliable=*/false);
    }
}

void MPServer::handleActorIdentityAck(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorIdentityAck ack;
    PacketActorIdentityAck pkt;
    pkt.setAck(&ack);
    if (!pkt.decode(data, size))
        return;
    if (ack.protocolVersion != ActorSyncProtocolVersionV2)
    {
        Log(Debug::Warning) << "[Server] Ignoring ActorIdentityAck from " << c.name
                            << " unsupported protocol=" << ack.protocolVersion;
        return;
    }

    std::size_t newlyAcked = 0;
    std::size_t invalidActorNetId = 0;
    ActorInstanceId firstInvalidActorNetId = 0;
    for (ActorInstanceId actorNetId : ack.actorNetIds)
    {
        if (!isValidActorInstanceId(actorNetId))
        {
            ++invalidActorNetId;
            if (firstInvalidActorNetId == 0)
                firstInvalidActorNetId = actorNetId;
            continue;
        }
        if (c.actorV2IdentitySent.count(actorNetId) == 0)
            continue;
        if (c.actorV2IdentityAcked.insert(actorNetId).second)
        {
            ++newlyAcked;
            ++c.actorV2IdentityAckedWindow;
        }
    }

    if (newlyAcked != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorSync v2 identity ack"
                            << " receiver=" << c.guid
                            << " cell=" << ack.cellId
                            << " acked=" << newlyAcked
                            << " totalAcked=" << c.actorV2IdentityAcked.size()
                            << " seq=" << ack.sequence;
    }

    if (invalidActorNetId != 0)
    {
        Log(Debug::Info) << "[Server] ActorSync v2 identity ack ignored invalid ids"
                         << " receiver=" << c.guid
                         << " cell=" << ack.cellId
                         << " invalidActorNetId=" << invalidActorNetId
                         << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                         << " seq=" << ack.sequence;
    }
}

void MPServer::handleActorPositionV2(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorPositionV2List incoming;
    PacketActorPositionV2 pkt;
    pkt.setPositionList(&incoming);
    if (!pkt.decode(data, size))
        return;
    if (incoming.protocolVersion != ActorSyncProtocolVersionV2)
    {
        Log(Debug::Warning) << "[Server] Rejecting ActorPositionV2 from " << c.name
                            << " unsupported protocol=" << incoming.protocolVersion;
        return;
    }

    const uint64_t timestamp = currentServerTimeMs();
    std::unordered_map<std::string, ActorList> updatesByCell;
    std::size_t accepted = 0;
    std::size_t invalidActorNetId = 0;
    std::size_t missingIdentity = 0;
    std::size_t wrongAuthority = 0;
    std::size_t deadVanillaSuppressed = 0;
    std::size_t deadSpawnedSuppressed = 0;
    std::size_t migratedByPositionCell = 0;
    ActorInstanceId firstInvalidActorNetId = 0;
    ActorInstanceId firstMissingActorNetId = 0;
    ActorInstanceId firstDeadVanillaActorNetId = 0;
    ActorInstanceId firstDeadSpawnedActorNetId = 0;
    std::unordered_set<std::string> migratedActorCells;
    std::unordered_set<std::string> canonicalDeadVanillaCellsToResend;

    auto applyPositionSnapshot = [](BaseActor& actor, const CompactActorSnapshot& snapshot,
                                   const std::string& cellId)
    {
        applyCompactActorSnapshotState(actor, snapshot, false);
        actor.cellId = cellId;
    };

    for (const CompactActorSnapshot& snapshot : incoming.snapshots)
    {
        if (!isValidActorInstanceId(snapshot.actorNetId))
        {
            ++invalidActorNetId;
            if (firstInvalidActorNetId == 0)
                firstInvalidActorNetId = snapshot.actorNetId;
            continue;
        }

        auto keyIt = mWorld.actorKeysByNetId.find(snapshot.actorNetId);
        if (keyIt == mWorld.actorKeysByNetId.end())
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = snapshot.actorNetId;
            continue;
        }

        const std::string& actorKey = keyIt->second;
        std::string cellId;
        CellActorState* cellState = nullptr;
        ActorRegistryRecord* record = nullptr;

        auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt != mWorld.actorLocations.end())
        {
            auto cellIt = mWorld.actorCells.find(locationIt->second);
            if (cellIt != mWorld.actorCells.end())
            {
                auto actorIt = cellIt->second.actors.find(actorKey);
                if (actorIt != cellIt->second.actors.end())
                {
                    cellId = cellIt->first;
                    cellState = &cellIt->second;
                    record = &actorIt->second;
                }
            }
        }

        if (!record)
        {
            for (auto& [candidateCellId, candidateCellState] : mWorld.actorCells)
            {
                auto actorIt = candidateCellState.actors.find(actorKey);
                if (actorIt == candidateCellState.actors.end())
                    continue;

                cellId = candidateCellId;
                cellState = &candidateCellState;
                record = &actorIt->second;
                rememberActorLocation(record->actor, cellId);
                break;
            }
        }

        if (!record || !cellState)
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = snapshot.actorNetId;
            continue;
        }

        if (cellState->authorityGuid != c.guid)
        {
            ++wrongAuthority;
            continue;
        }

        if (record->actor.mpNum == 0)
        {
            std::string deadCellId;
            if (const ActorRegistryRecord* deadRecord = findDeadVanillaActor(record->actor, &deadCellId))
            {
                if (deadCellId != cellId)
                {
                    forgetActorLocation(record->actor, cellId);
                    cellState->actors.erase(actorKey);
                    migratedActorCells.insert(cellId);
                    canonicalDeadVanillaCellsToResend.insert(deadCellId);
                }
                else
                {
                    BaseActor& actor = record->actor;
                    actor = deadRecord->actor;
                    actor.cellId = deadCellId;
                    actor.isDead = true;
                    actor.isInstantDeath = true;
                    actor.isMoving = false;
                    actor.isAttackingOrCasting = false;
                    actor.velocity = Velocity {};
                    actor.animFlags.animFwd = 0.f;
                    actor.animFlags.animSide = 0.f;
                    actor.animFlags.movementFlags = 0;
                    record->lastSnapshotTime = timestamp;
                    ensureActorNetId(*record, deadCellId);
                }
                ++deadVanillaSuppressed;
                if (firstDeadVanillaActorNetId == 0)
                    firstDeadVanillaActorNetId = snapshot.actorNetId;
                continue;
            }
        }

        if (record->actor.mpNum != 0 && record->actor.isDead)
        {
            ++deadSpawnedSuppressed;
            if (firstDeadSpawnedActorNetId == 0)
                firstDeadSpawnedActorNetId = snapshot.actorNetId;
            continue;
        }

        BaseActor& actor = record->actor;
        std::string destinationCellId = cellId;
        if (actor.mpNum != 0 && isExteriorCellKey(cellId))
        {
            const std::string positionCellId = exteriorCellIdForPosition(snapshot.position);
            constexpr float kExteriorCellMismatchHysteresis = 64.f;
            if (!positionCellId.empty()
                && positionCellId != cellId
                && exteriorCellBorderDistance(snapshot.position) > kExteriorCellMismatchHysteresis
                && clientHasActorCellLoaded(c, positionCellId))
            {
                destinationCellId = positionCellId;
            }
        }

        if (destinationCellId != cellId)
        {
            ActorRegistryRecord movedRecord = *record;
            applyPositionSnapshot(movedRecord.actor, snapshot, destinationCellId);
            movedRecord.lastSnapshotTime = timestamp;

            forgetActorLocation(record->actor, cellId);
            cellState->actors.erase(actorKey);
            migratedActorCells.insert(cellId);
            if (mPlayerDb && movedRecord.actor.mpNum != 0)
                mPlayerDb->deleteSpawnedActorDynamicRecordLink(movedRecord.actor.mpNum, cellId);

            std::unordered_set<std::string> changedCellIds;
            removeActorFromOtherCells(movedRecord.actor, destinationCellId, changedCellIds);
            migratedActorCells.insert(changedCellIds.begin(), changedCellIds.end());

            auto& destinationCellState = mWorld.actorCells[destinationCellId];
            auto [destIt, inserted] = destinationCellState.actors.emplace(actorKey, movedRecord);
            if (!inserted)
                destIt->second = movedRecord;
            ensureActorNetId(destIt->second, destinationCellId);
            rememberActorLocation(destIt->second.actor, destinationCellId);
            persistSpawnedActorIfNeeded(destIt->second, timestamp, false);
            upsertSpawnedActorDynamicRecordLinkIfNeeded(destIt->second.actor);
            if (destinationCellState.authorityGuid == 0)
                refreshActorAuthorityForCell(destinationCellId, c.guid);

            ActorList& outgoing = updatesByCell[destinationCellId];
            if (outgoing.cellId.empty())
            {
                outgoing.cellId = destinationCellId;
                outgoing.isAuthority = true;
                outgoing.authorityGuid = destinationCellState.authorityGuid;
                outgoing.authorityGeneration = destinationCellState.authorityGeneration;
                outgoing.snapshotSequence = destinationCellState.nextSnapshotSequence++;
                outgoing.serverTimestamp = timestamp;
            }
            outgoing.actors.push_back(destIt->second.actor);
            ++accepted;
            ++migratedByPositionCell;

            Log(Debug::Info) << "[Server] ActorPositionV2 migrated spawned actor by position cell"
                             << " refId=" << destIt->second.actor.refId
                             << " mpNum=" << destIt->second.actor.mpNum
                             << " from=" << cellId
                             << " to=" << destinationCellId
                             << " pos=(" << snapshot.position.pos[0]
                             << "," << snapshot.position.pos[1]
                             << "," << snapshot.position.pos[2] << ")";
            continue;
        }

        applyPositionSnapshot(actor, snapshot, cellId);
        record->lastSnapshotTime = timestamp;
        ensureActorNetId(*record, cellId);
        persistSpawnedActorIfNeeded(*record, timestamp, false);

        ActorList& outgoing = updatesByCell[cellId];
        if (outgoing.cellId.empty())
        {
            outgoing.cellId = cellId;
            outgoing.isAuthority = true;
            outgoing.authorityGuid = cellState->authorityGuid;
            outgoing.authorityGeneration = cellState->authorityGeneration;
            outgoing.snapshotSequence = cellState->nextSnapshotSequence++;
            outgoing.serverTimestamp = timestamp;
        }
        outgoing.actors.push_back(actor);
        ++accepted;
    }

    for (auto& [cellId, actorList] : updatesByCell)
    {
        auto cellIt = mWorld.actorCells.find(cellId);
        if (cellIt == mWorld.actorCells.end())
            continue;
        broadcastActorPositionV2ToCell(cellId, cellIt->second, actorList, c.conn);
    }

    for (const std::string& migratedCellId : migratedActorCells)
    {
        auto migratedCellIt = mWorld.actorCells.find(migratedCellId);
        if (migratedCellIt != mWorld.actorCells.end())
            broadcastActorListForCell(migratedCellId, migratedCellIt->second);
    }

    for (const std::string& deadCellId : canonicalDeadVanillaCellsToResend)
        sendActorStateToInterestedClients(deadCellId);

    if (invalidActorNetId != 0 || missingIdentity != 0 || wrongAuthority != 0
        || deadVanillaSuppressed != 0 || deadSpawnedSuppressed != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorPositionV2 filtered"
                            << " from=" << c.name
                            << " snapshots=" << incoming.snapshots.size()
                            << " accepted=" << accepted
                            << " invalidActorNetId=" << invalidActorNetId
                            << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                            << " missingIdentity=" << missingIdentity
                            << " firstMissingActorNetId=" << firstMissingActorNetId
                            << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
                            << " wrongAuthority=" << wrongAuthority
                            << " deadVanillaSuppressed=" << deadVanillaSuppressed
                            << " firstDeadVanillaActorNetId=" << firstDeadVanillaActorNetId
                            << " firstDeadVanillaActorKey=" << describeActorInstanceId(firstDeadVanillaActorNetId)
                            << " deadSpawnedSuppressed=" << deadSpawnedSuppressed
                            << " firstDeadSpawnedActorNetId=" << firstDeadSpawnedActorNetId
                            << " firstDeadSpawnedActorKey=" << describeActorInstanceId(firstDeadSpawnedActorNetId);
    }

    if (migratedByPositionCell != 0)
    {
        Log(Debug::Info) << "[Server] ActorPositionV2 coordinate migration summary"
                         << " from=" << c.name
                         << " migrated=" << migratedByPositionCell
                         << " snapshots=" << incoming.snapshots.size();
    }
}

void MPServer::handleActorPresentationV2(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorPresentationV2List incoming;
    PacketActorPresentationV2 pkt;
    pkt.setPresentationList(&incoming);
    if (!pkt.decode(data, size))
        return;
    if (incoming.protocolVersion != ActorSyncProtocolVersionV2)
    {
        Log(Debug::Warning) << "[Server] Rejecting ActorPresentationV2 from " << c.name
                            << " unsupported protocol=" << incoming.protocolVersion;
        return;
    }

    const uint64_t timestamp = currentServerTimeMs();
    std::unordered_map<std::string, ActorList> updatesByCell;
    std::size_t accepted = 0;
    std::size_t invalidActorNetId = 0;
    std::size_t missingIdentity = 0;
    std::size_t wrongAuthority = 0;
    std::size_t deadVanillaSuppressed = 0;
    std::size_t deadSpawnedSuppressed = 0;
    ActorInstanceId firstInvalidActorNetId = 0;
    ActorInstanceId firstMissingActorNetId = 0;
    ActorInstanceId firstDeadVanillaActorNetId = 0;
    ActorInstanceId firstDeadSpawnedActorNetId = 0;
    std::unordered_set<std::string> staleLiveVanillaCellsChanged;
    std::unordered_set<std::string> canonicalDeadVanillaCellsToResend;

    for (const ActorPresentationSnapshot& snapshot : incoming.snapshots)
    {
        if (!isValidActorInstanceId(snapshot.actorNetId))
        {
            ++invalidActorNetId;
            if (firstInvalidActorNetId == 0)
                firstInvalidActorNetId = snapshot.actorNetId;
            continue;
        }

        auto keyIt = mWorld.actorKeysByNetId.find(snapshot.actorNetId);
        if (keyIt == mWorld.actorKeysByNetId.end())
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = snapshot.actorNetId;
            continue;
        }

        const std::string& actorKey = keyIt->second;
        std::string cellId;
        CellActorState* cellState = nullptr;
        ActorRegistryRecord* record = nullptr;

        auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt != mWorld.actorLocations.end())
        {
            auto cellIt = mWorld.actorCells.find(locationIt->second);
            if (cellIt != mWorld.actorCells.end())
            {
                auto actorIt = cellIt->second.actors.find(actorKey);
                if (actorIt != cellIt->second.actors.end())
                {
                    cellId = cellIt->first;
                    cellState = &cellIt->second;
                    record = &actorIt->second;
                }
            }
        }

        if (!record)
        {
            for (auto& [candidateCellId, candidateCellState] : mWorld.actorCells)
            {
                auto actorIt = candidateCellState.actors.find(actorKey);
                if (actorIt == candidateCellState.actors.end())
                    continue;

                cellId = candidateCellId;
                cellState = &candidateCellState;
                record = &actorIt->second;
                rememberActorLocation(record->actor, cellId);
                break;
            }
        }

        if (!record || !cellState)
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = snapshot.actorNetId;
            continue;
        }

        if (cellState->authorityGuid != c.guid)
        {
            ++wrongAuthority;
            continue;
        }

        const bool snapshotIsDead = snapshot.isDead || ((snapshot.presentationFlags & ActorPresentationDead) != 0);
        if (record->actor.mpNum == 0)
        {
            std::string deadCellId;
            if (const ActorRegistryRecord* deadRecord = findDeadVanillaActor(record->actor, &deadCellId))
            {
                if (deadCellId != cellId)
                {
                    forgetActorLocation(record->actor, cellId);
                    cellState->actors.erase(actorKey);
                    staleLiveVanillaCellsChanged.insert(cellId);
                    canonicalDeadVanillaCellsToResend.insert(deadCellId);
                }
                else
                {
                    BaseActor& actor = record->actor;
                    actor = deadRecord->actor;
                    actor.cellId = deadCellId;
                    actor.isDead = true;
                    actor.isInstantDeath = true;
                    actor.isMoving = false;
                    actor.isAttackingOrCasting = false;
                    actor.velocity = Velocity {};
                    actor.animFlags.animFwd = 0.f;
                    actor.animFlags.animSide = 0.f;
                    actor.animFlags.movementFlags = 0;
                    record->lastSnapshotTime = timestamp;
                    ensureActorNetId(*record, deadCellId);
                }
                ++deadVanillaSuppressed;
                if (firstDeadVanillaActorNetId == 0)
                    firstDeadVanillaActorNetId = snapshot.actorNetId;
                continue;
            }
        }

        if (record->actor.mpNum != 0 && record->actor.isDead && !snapshotIsDead)
        {
            ++deadSpawnedSuppressed;
            if (firstDeadSpawnedActorNetId == 0)
                firstDeadSpawnedActorNetId = snapshot.actorNetId;
            continue;
        }

        BaseActor& actor = record->actor;
        const bool wasDead = actor.isDead;
        actor.isAttackingOrCasting = snapshot.isAttackingOrCasting;
        actor.hasWeaponDrawn = snapshot.hasWeaponDrawn;
        actor.hasSpellReadied = snapshot.hasSpellReadied;
        actor.isDead = snapshotIsDead;
        actor.position.isTeleporting = (snapshot.presentationFlags & ActorPresentationTeleporting) != 0;
        static constexpr uint32_t kReliablePresentationMovementFlags =
            AnimFlags::MF_KNOCKED_DOWN | AnimFlags::MF_KNOCKED_OUT | AnimFlags::MF_RECOVERY;
        actor.animFlags.movementFlags =
            (actor.animFlags.movementFlags & ~kReliablePresentationMovementFlags)
            | (snapshot.movementFlags & kReliablePresentationMovementFlags);
        if (isReliablePresentationAnimGroup(snapshot.currentAnimGroup))
            actor.animFlags.currentAnimGroup = snapshot.currentAnimGroup;
        if (snapshotIsDead)
        {
            actor.isMoving = false;
            actor.isAttackingOrCasting = false;
            actor.velocity = Velocity {};
            actor.animFlags.animFwd = 0.f;
            actor.animFlags.animSide = 0.f;
            actor.animFlags.movementFlags = 0;
        }
        actor.cellId = cellId;
        record->lastSnapshotTime = timestamp;
        ensureActorNetId(*record, cellId);
        if (actor.mpNum != 0 && actor.isDead && !wasDead)
        {
            Log(Debug::Info) << "[Server] ActorPresentationV2 observed spawned death transition"
                             << " refId=" << actor.refId
                             << " mpNum=" << actor.mpNum
                             << " cell=" << cellId;
            sendActorLifecycleEvent("death", actor, record->persistent);
        }

        ActorList& outgoing = updatesByCell[cellId];
        if (outgoing.cellId.empty())
        {
            outgoing.cellId = cellId;
            outgoing.isAuthority = true;
            outgoing.authorityGuid = cellState->authorityGuid;
            outgoing.authorityGeneration = cellState->authorityGeneration;
            outgoing.snapshotSequence = cellState->nextSnapshotSequence++;
            outgoing.serverTimestamp = timestamp;
        }
        outgoing.actors.push_back(actor);
        ++accepted;
    }

    for (auto& [cellId, actorList] : updatesByCell)
    {
        auto cellIt = mWorld.actorCells.find(cellId);
        if (cellIt == mWorld.actorCells.end())
            continue;
        broadcastActorPresentationV2ToCell(cellId, cellIt->second, actorList, c.conn);
    }

    for (const std::string& changedCellId : staleLiveVanillaCellsChanged)
    {
        auto changedCellIt = mWorld.actorCells.find(changedCellId);
        if (changedCellIt != mWorld.actorCells.end())
            broadcastActorListForCell(changedCellId, changedCellIt->second);
    }

    for (const std::string& deadCellId : canonicalDeadVanillaCellsToResend)
        sendActorStateToInterestedClients(deadCellId);

    if (invalidActorNetId != 0 || missingIdentity != 0 || wrongAuthority != 0
        || deadVanillaSuppressed != 0 || deadSpawnedSuppressed != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorPresentationV2 filtered"
                            << " from=" << c.name
                            << " snapshots=" << incoming.snapshots.size()
                            << " accepted=" << accepted
                            << " invalidActorNetId=" << invalidActorNetId
                            << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                            << " missingIdentity=" << missingIdentity
                            << " firstMissingActorNetId=" << firstMissingActorNetId
                            << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
                            << " wrongAuthority=" << wrongAuthority
                            << " deadVanillaSuppressed=" << deadVanillaSuppressed
                            << " firstDeadVanillaActorNetId=" << firstDeadVanillaActorNetId
                            << " firstDeadVanillaActorKey=" << describeActorInstanceId(firstDeadVanillaActorNetId)
                            << " deadSpawnedSuppressed=" << deadSpawnedSuppressed
                            << " firstDeadSpawnedActorNetId=" << firstDeadSpawnedActorNetId
                            << " firstDeadSpawnedActorKey=" << describeActorInstanceId(firstDeadSpawnedActorNetId);
    }
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorAnimFlags"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorAnimFlags");
        if (!stored)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.animFlags = actor.animFlags;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    PacketActorAnimFlags out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn, /*reliable=*/false);
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorAnimPlay"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorAnimPlay");
        if (!stored)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.animPlay = actor.animPlay;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    PacketActorAnimPlay out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAttack(ConnectedClient& c, const uint8_t* data, size_t size)
{
    if (c.actorSyncProtocolVersion >= ActorSyncProtocolVersionV2)
    {
        Log(Debug::Verbose) << "[Server] Ignoring retired legacy ActorAttack from v2 client"
                            << " from=" << c.name
                            << " protocol=" << c.actorSyncProtocolVersion;
        return;
    }

    ActorList incoming;
    PacketActorAttack pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    Log(Debug::Info) << "[Server] Received ActorAttack from " << c.name << " cellId=" << incoming.cellId
                     << " actors=" << incoming.actors.size();
    if (!validateActorUpdate(c, incoming, "ActorAttack"))
    {
        Log(Debug::Info) << "[Server] ActorAttack rejected by validateActorUpdate from " << c.name
                         << " cellId=" << incoming.cellId;
        return;
    }
    Log(Debug::Info) << "[Server] ActorAttack accepted from " << c.name
                     << " cell=" << incoming.cellId
                     << " actors=" << incoming.actors.size();

    auto& cellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = cellState.authorityGuid;
    incoming.authorityGeneration = cellState.authorityGeneration;
    incoming.snapshotSequence = cellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorAttack"))
            continue;

        Log(Debug::Verbose) << "[Server] ActorAttack candidate from " << c.name
                            << " refId=" << actor.refId
                            << " refNum=" << actor.refNum
                            << " mpNum=" << actor.mpNum
                            << " targetMpNum=" << actor.attack.targetMpNum
                            << " damage=" << actor.attack.damage
                            << " healthDamage=" << actor.attack.healthDamage;
        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorAttack");
        if (!stored)
        {
            Log(Debug::Warning) << "[Server] ActorAttack dropped for untracked actor"
                                << " refId=" << actor.refId
                                << " refNum=" << actor.refNum
                                << " mpNum=" << actor.mpNum
                                << " cell=" << incoming.cellId;
            continue;
        }
        Log(Debug::Verbose) << "[Server] ActorAttack matched tracked actor"
                            << " refId=" << stored->actor.refId
                            << " refNum=" << stored->actor.refNum
                            << " mpNum=" << stored->actor.mpNum
                            << " persistent=" << stored->persistent
                            << " wasDead=" << stored->actor.isDead;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.attack = actor.attack;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
    {
        Log(Debug::Info) << "[Server] ActorAttack dropped after filtering"
                         << " from=" << c.name
                         << " cell=" << incoming.cellId
                         << " actors=" << incoming.actors.size();
        return;
    }

    PacketActorAttack out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorAttack to cell=" << filtered.cellId
                     << " actors=" << filtered.actors.size();
}

// ---------------------------------------------------------------------------
void MPServer::handleActorAttackV2(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorAttackV2List incoming;
    PacketActorAttackV2 pkt;
    pkt.setAttackList(&incoming);
    if (!pkt.decode(data, size))
        return;
    if (incoming.protocolVersion != ActorSyncProtocolVersionV2)
    {
        Log(Debug::Warning) << "[Server] Rejecting ActorAttackV2 from " << c.name
                            << " unsupported protocol=" << incoming.protocolVersion;
        return;
    }

    const uint64_t timestamp = currentServerTimeMs();
    std::unordered_map<std::string, ActorAttackV2List> updatesByCell;
    std::size_t accepted = 0;
    std::size_t invalidActorNetId = 0;
    std::size_t missingIdentity = 0;
    std::size_t wrongAuthority = 0;
    std::size_t unloadedCell = 0;
    std::size_t deadVanillaSuppressed = 0;
    ActorInstanceId firstInvalidActorNetId = 0;
    ActorInstanceId firstMissingActorNetId = 0;
    ActorInstanceId firstDeadVanillaActorNetId = 0;
    std::unordered_set<std::string> staleLiveVanillaCellsChanged;
    std::unordered_set<std::string> canonicalDeadVanillaCellsToResend;

    for (const ActorAttackV2Event& event : incoming.events)
    {
        if (!isValidActorInstanceId(event.actorNetId))
        {
            ++invalidActorNetId;
            if (firstInvalidActorNetId == 0)
                firstInvalidActorNetId = event.actorNetId;
            continue;
        }

        auto keyIt = mWorld.actorKeysByNetId.find(event.actorNetId);
        if (keyIt == mWorld.actorKeysByNetId.end())
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = event.actorNetId;
            continue;
        }

        const std::string& actorKey = keyIt->second;
        std::string cellId;
        CellActorState* cellState = nullptr;
        ActorRegistryRecord* record = nullptr;

        auto locationIt = mWorld.actorLocations.find(actorKey);
        if (locationIt != mWorld.actorLocations.end())
        {
            auto cellIt = mWorld.actorCells.find(locationIt->second);
            if (cellIt != mWorld.actorCells.end())
            {
                auto actorIt = cellIt->second.actors.find(actorKey);
                if (actorIt != cellIt->second.actors.end())
                {
                    cellId = cellIt->first;
                    cellState = &cellIt->second;
                    record = &actorIt->second;
                }
            }
        }

        if (!record)
        {
            for (auto& [candidateCellId, candidateCellState] : mWorld.actorCells)
            {
                auto actorIt = candidateCellState.actors.find(actorKey);
                if (actorIt == candidateCellState.actors.end())
                    continue;

                cellId = candidateCellId;
                cellState = &candidateCellState;
                record = &actorIt->second;
                rememberActorLocation(record->actor, cellId);
                break;
            }
        }

        if (!record || !cellState)
        {
            ++missingIdentity;
            if (firstMissingActorNetId == 0)
                firstMissingActorNetId = event.actorNetId;
            continue;
        }

        if (!clientHasActorCellLoaded(c, cellId))
        {
            ++unloadedCell;
            continue;
        }

        if (cellState->authorityGuid != c.guid)
        {
            ++wrongAuthority;
            continue;
        }

        if (record->actor.mpNum == 0)
        {
            std::string deadCellId;
            if (const ActorRegistryRecord* deadRecord = findDeadVanillaActor(record->actor, &deadCellId))
            {
                if (deadCellId != cellId)
                {
                    forgetActorLocation(record->actor, cellId);
                    cellState->actors.erase(actorKey);
                    staleLiveVanillaCellsChanged.insert(cellId);
                    canonicalDeadVanillaCellsToResend.insert(deadCellId);
                }
                else
                {
                    BaseActor& actor = record->actor;
                    actor = deadRecord->actor;
                    actor.cellId = deadCellId;
                    actor.isDead = true;
                    actor.isInstantDeath = true;
                    actor.isMoving = false;
                    actor.isAttackingOrCasting = false;
                    actor.velocity = Velocity {};
                    actor.animFlags.animFwd = 0.f;
                    actor.animFlags.animSide = 0.f;
                    actor.animFlags.movementFlags = 0;
                    record->lastSnapshotTime = timestamp;
                    ensureActorNetId(*record, deadCellId);
                }
                ++deadVanillaSuppressed;
                if (firstDeadVanillaActorNetId == 0)
                    firstDeadVanillaActorNetId = event.actorNetId;
                continue;
            }
        }

        BaseActor& actor = record->actor;
        actor.attack = event.attack;
        actor.cellId = cellId;
        record->lastSnapshotTime = timestamp;
        const ActorInstanceId actorNetId = ensureActorNetId(*record, cellId);
        persistSpawnedActorIfNeeded(*record);

        ActorAttackV2List& outgoing = updatesByCell[cellId];
        if (outgoing.events.empty())
        {
            outgoing.protocolVersion = ActorSyncProtocolVersionV2;
            outgoing.cellId = cellId;
            outgoing.authorityGuid = cellState->authorityGuid;
            outgoing.authorityGeneration = cellState->authorityGeneration;
            outgoing.sequence = cellState->nextSnapshotSequence++;
            outgoing.serverTimestamp = timestamp;
        }

        ActorAttackV2Event outgoingEvent = event;
        outgoingEvent.actorNetId = actorNetId;
        outgoing.events.push_back(outgoingEvent);
        ++accepted;
    }

    std::size_t sent = 0;
    std::size_t suppressedUntilIdentityKnown = 0;
    for (auto& [cellId, attackList] : updatesByCell)
    {
        for (auto& [conn, client] : mClients)
        {
            if (conn == c.conn
                || !clientHasActorCellLoaded(client, cellId)
                || client.actorSyncProtocolVersion < ActorSyncProtocolVersionV2)
                continue;

            ActorAttackV2List filtered = attackList;
            filtered.events.clear();
            filtered.events.reserve(attackList.events.size());
            for (const ActorAttackV2Event& event : attackList.events)
            {
                if (client.actorV2IdentitySent.count(event.actorNetId) == 0
                    || client.actorV2IdentityAcked.count(event.actorNetId) == 0)
                {
                    ++suppressedUntilIdentityKnown;
                    ++client.actorV2AttackSuppressedUntilIdentityKnownWindow;
                    ++client.actorV2MissingIdentityByNetIdWindow[event.actorNetId];
                    continue;
                }

                filtered.events.push_back(event);
            }

            if (filtered.events.empty())
                continue;

            PacketActorAttackV2 out;
            out.setAttackList(&filtered);
            sendTo(conn, out.encode(), /*reliable=*/true);
            client.actorV2AttackSentWindow += filtered.events.size();
            sent += filtered.events.size();
        }
    }

    for (const std::string& changedCellId : staleLiveVanillaCellsChanged)
    {
        auto changedCellIt = mWorld.actorCells.find(changedCellId);
        if (changedCellIt != mWorld.actorCells.end())
            broadcastActorListForCell(changedCellId, changedCellIt->second);
    }

    for (const std::string& deadCellId : canonicalDeadVanillaCellsToResend)
        sendActorStateToInterestedClients(deadCellId);

    Log((invalidActorNetId != 0 || missingIdentity != 0 || wrongAuthority != 0 || unloadedCell != 0
            || deadVanillaSuppressed != 0 || suppressedUntilIdentityKnown != 0) ? Debug::Info : Debug::Verbose)
        << "[Server] ActorAttackV2"
        << " from=" << c.name
        << " packetCell=" << incoming.cellId
        << " events=" << incoming.events.size()
        << " accepted=" << accepted
        << " sent=" << sent
        << " invalidActorNetId=" << invalidActorNetId
        << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
        << " missingIdentity=" << missingIdentity
        << " firstMissingActorNetId=" << firstMissingActorNetId
        << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
        << " wrongAuthority=" << wrongAuthority
        << " unloadedCell=" << unloadedCell
        << " deadVanillaSuppressed=" << deadVanillaSuppressed
        << " firstDeadVanillaActorNetId=" << firstDeadVanillaActorNetId
        << " firstDeadVanillaActorKey=" << describeActorInstanceId(firstDeadVanillaActorNetId)
        << " suppressedUntilIdentityKnown=" << suppressedUntilIdentityKnown;
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorCast"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorCast");
        if (!stored)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.cast = actor.cast;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    PacketActorCast out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorCast to cell=" << filtered.cellId;
}

// ---------------------------------------------------------------------------
void MPServer::handleActorCellChange(ConnectedClient& c, const uint8_t* data, size_t size)
{
    ActorList incoming;
    PacketActorCellChange pkt;
    pkt.setActorList(&incoming);
    if (!pkt.decode(data, size)) return;
    if (!validateActorUpdate(c, incoming, "ActorCellChange")) return;

    auto& sourceCellState = mWorld.actorCells[incoming.cellId];
    incoming.isAuthority = true;
    incoming.authorityGuid = sourceCellState.authorityGuid;
    incoming.authorityGeneration = sourceCellState.authorityGeneration;
    incoming.snapshotSequence = sourceCellState.nextSnapshotSequence++;
    incoming.serverTimestamp = currentServerTimeMs();

    ActorList accepted = incoming;
    accepted.actors.clear();
    accepted.actors.reserve(incoming.actors.size());
    std::unordered_set<std::string> destinationCellIds;
    std::unordered_set<std::string> changedCellIds;

    for (auto& actor : incoming.actors)
    {
        const std::string destinationCellId = actor.cellId;
        if (destinationCellId.empty() || destinationCellId == incoming.cellId)
        {
            Log(Debug::Warning) << "[Server] Rejecting ActorCellChange from " << c.name
                                << " for actor " << actor.refId
                                << " because destination cell is invalid: " << destinationCellId;
            continue;
        }

        if (!clientHasActorCellLoaded(c, destinationCellId))
        {
            Log(Debug::Warning) << "[Server] Rejecting ActorCellChange from " << c.name
                                << " for actor " << actor.refId
                                << " because destination cell is not loaded: " << destinationCellId;
            continue;
        }

        normalizeActorIdentity(actor);
        actor.cellId = destinationCellId;
        if (isUnmanagedSpawnerActor(actor))
        {
            Log(Debug::Info) << "[Server] Rejecting unmanaged spawner ActorCellChange from " << c.name
                             << " refId=" << actor.refId
                             << " refNum=" << actor.refNum
                             << " from=" << incoming.cellId
                             << " to=" << destinationCellId;
            continue;
        }
        if (rejectStaleAliveVanillaActor(actor, destinationCellId, c, "ActorCellChange"))
            continue;

        const std::string actorKey = makeActorKey(actor);
        auto sourceActorIt = sourceCellState.actors.find(actorKey);
        if (sourceActorIt == sourceCellState.actors.end())
        {
            findTrackedActor(sourceCellState, actor, c, "ActorCellChange");
            continue;
        }

        ActorRegistryRecord movedRecord = sourceActorIt->second;
        const bool wasDead = movedRecord.actor.isDead;
        sourceCellState.actors.erase(sourceActorIt);
        forgetActorLocation(movedRecord.actor, incoming.cellId);
        if (mPlayerDb && movedRecord.actor.mpNum != 0)
            mPlayerDb->deleteSpawnedActorDynamicRecordLink(movedRecord.actor.mpNum, incoming.cellId);

        removeActorFromOtherCells(actor, destinationCellId, changedCellIds);

        movedRecord.actor = actor;
        movedRecord.actor.cellId = destinationCellId;
        movedRecord.lastSnapshotTime = incoming.serverTimestamp;

        auto& destinationCellState = mWorld.actorCells[destinationCellId];
        destinationCellState.actors[actorKey] = movedRecord;
        rememberActorLocation(movedRecord.actor, destinationCellId);
        rememberDeadVanillaActor(movedRecord);
        persistSpawnedActorIfNeeded(movedRecord);
        upsertSpawnedActorDynamicRecordLinkIfNeeded(movedRecord.actor);

        if (movedRecord.actor.mpNum != 0 && movedRecord.actor.isDead && !wasDead)
            sendActorLifecycleEvent("death", movedRecord.actor, movedRecord.persistent);

        accepted.actors.push_back(movedRecord.actor);
        destinationCellIds.insert(destinationCellId);

        Log(Debug::Info) << "[Server] ActorCellChange migrated actor"
                         << " refId=" << movedRecord.actor.refId
                         << " mpNum=" << movedRecord.actor.mpNum
                         << " from=" << incoming.cellId
                         << " to=" << destinationCellId;
    }

    if (accepted.actors.empty())
        return;

    PacketActorCellChange out;
    out.setActorList(&accepted);
    const std::vector<uint8_t> encoded = out.encode();
    for (auto& [conn, client] : mClients)
    {
        if (conn == c.conn)
            continue;

        bool interested = clientHasActorCellLoaded(client, incoming.cellId);
        if (!interested)
        {
            for (const std::string& destinationCellId : destinationCellIds)
            {
                if (clientHasActorCellLoaded(client, destinationCellId))
                {
                    interested = true;
                    break;
                }
            }
        }

        if (interested)
            sendTo(conn, encoded);
    }

    for (const std::string& destinationCellId : destinationCellIds)
    {
        auto destinationCellIt = mWorld.actorCells.find(destinationCellId);
        if (destinationCellIt != mWorld.actorCells.end() && destinationCellIt->second.authorityGuid == 0)
            refreshActorAuthorityForCell(destinationCellId, c.guid);
    }

    for (const std::string& changedCellId : changedCellIds)
    {
        auto changedCellIt = mWorld.actorCells.find(changedCellId);
        if (changedCellIt != mWorld.actorCells.end())
            broadcastActorListForCell(changedCellId, changedCellIt->second);
    }
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());
    std::unordered_set<std::string> changedCellIds;

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectResetStaleDeadVanillaActor(actor, incoming.cellId, c, "ActorDeath"))
            continue;
        clearResetStaleDeathSuppressionForAliveVanillaActor(actor, incoming.cellId);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorDeath"))
            continue;

        Log(Debug::Info) << "[Server] ActorDeath candidate from " << c.name
                         << " refId=" << actor.refId
                         << " refNum=" << actor.refNum
                         << " mpNum=" << actor.mpNum
                         << " eventId=" << actor.deathEventId
                         << " isDead=" << actor.isDead
                         << " hp=" << actor.dynamicStats.health.current
                         << " deathAnim='" << actor.deathAnimGroup << "'";
        const std::string actorKeyForDeath = makeActorKey(actor);
        auto storedIt = cellState.actors.find(actorKeyForDeath);
        ActorRegistryRecord* stored = storedIt != cellState.actors.end() ? &storedIt->second : nullptr;
        if (!stored)
        {
            const std::string& actorKey = actorKeyForDeath;
            std::string sourceCellId;
            CellActorState* sourceCellState = nullptr;
            ActorRegistryRecord* sourceRecord = nullptr;

            const auto locationIt = mWorld.actorLocations.find(actorKey);
            if (locationIt != mWorld.actorLocations.end() && locationIt->second != incoming.cellId)
            {
                auto sourceCellIt = mWorld.actorCells.find(locationIt->second);
                if (sourceCellIt != mWorld.actorCells.end())
                {
                    auto sourceActorIt = sourceCellIt->second.actors.find(actorKey);
                    if (sourceActorIt != sourceCellIt->second.actors.end())
                    {
                        sourceCellId = sourceCellIt->first;
                        sourceCellState = &sourceCellIt->second;
                        sourceRecord = &sourceActorIt->second;
                    }
                }
            }

            if (!sourceRecord)
            {
                for (auto& [candidateCellId, candidateCellState] : mWorld.actorCells)
                {
                    if (candidateCellId == incoming.cellId)
                        continue;

                    auto sourceActorIt = candidateCellState.actors.find(actorKey);
                    if (sourceActorIt == candidateCellState.actors.end())
                        continue;

                    sourceCellId = candidateCellId;
                    sourceCellState = &candidateCellState;
                    sourceRecord = &sourceActorIt->second;
                    break;
                }
            }

            if (sourceRecord && sourceRecord->actor.isDead)
            {
                Log(Debug::Verbose) << "[Server] ActorDeath ignored cross-cell duplicate for already-dead actor"
                                    << " refId=" << actor.refId
                                    << " refNum=" << actor.refNum
                                    << " mpNum=" << actor.mpNum
                                    << " sourceCell=" << sourceCellId
                                    << " incomingCell=" << incoming.cellId;
                continue;
            }

            if (sourceRecord && sourceCellState)
            {
                ActorRegistryRecord movedRecord = *sourceRecord;
                sourceCellState->actors.erase(actorKey);
                forgetActorLocation(movedRecord.actor, sourceCellId);
                if (mPlayerDb && movedRecord.actor.mpNum != 0)
                {
                    mPlayerDb->deleteSpawnedActorDynamicRecordLink(movedRecord.actor.mpNum, sourceCellId);
                    scheduleGeneratedDynamicRecordGc("actor_death_migration_unlink");
                }
                changedCellIds.insert(sourceCellId);

                auto [movedIt, inserted] = cellState.actors.emplace(actorKey, movedRecord);
                if (!inserted)
                    movedIt->second = movedRecord;
                stored = &movedIt->second;

                Log(Debug::Info) << "[Server] ActorDeath moved tracked actor to death cell"
                                 << " refId=" << actor.refId
                                 << " refNum=" << actor.refNum
                                 << " mpNum=" << actor.mpNum
                                 << " from=" << sourceCellId
                                 << " to=" << incoming.cellId;
            }
        }
        if (!stored)
        {
            Log(Debug::Warning) << "[Server] ActorDeath dropped for untracked actor"
                                << " refId=" << actor.refId
                                << " refNum=" << actor.refNum
                                << " mpNum=" << actor.mpNum
                                << " cell=" << incoming.cellId;
            continue;
        }
        const bool wasDead = stored->actor.isDead;
        Log(Debug::Info) << "[Server] ActorDeath matched tracked actor"
                         << " refId=" << stored->actor.refId
                         << " refNum=" << stored->actor.refNum
                         << " mpNum=" << stored->actor.mpNum
                         << " persistent=" << stored->persistent
                         << " wasDead=" << wasDead
                         << " lastDeathEventId=" << stored->lastDeathEventId
                         << " prevHp=" << stored->actor.dynamicStats.health.current;
        if (actor.deathEventId != 0
            && stored->lastDeathEventId != 0
            && !isNewerEventId(actor.deathEventId, stored->lastDeathEventId))
        {
            Log(Debug::Verbose) << "[Server] ActorDeath ignored duplicate event"
                                << " refId=" << stored->actor.refId
                                << " refNum=" << stored->actor.refNum
                                << " mpNum=" << stored->actor.mpNum
                                << " eventId=" << actor.deathEventId
                                << " lastDeathEventId=" << stored->lastDeathEventId
                                << " cell=" << incoming.cellId;
            continue;
        }

        const bool alreadyRememberedDeadVanilla = actor.mpNum == 0 && findDeadVanillaActor(actor) != nullptr;
        const bool duplicateAlreadyDead = wasDead
            && actor.isDead
            && (alreadyRememberedDeadVanilla
                || (actor.mpNum != 0 && !stored->actor.deathAnimGroup.empty()));
        if (duplicateAlreadyDead)
        {
            Log(Debug::Verbose) << "[Server] ActorDeath ignored duplicate already-dead actor"
                                << " refId=" << stored->actor.refId
                                << " refNum=" << stored->actor.refNum
                                << " mpNum=" << stored->actor.mpNum
                                << " cell=" << incoming.cellId;
            continue;
        }
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.position = actor.position;
        stored->actor.velocity = Velocity {};
        stored->actor.deathEventId = actor.deathEventId;
        stored->actor.deathState = actor.deathState;
        stored->actor.isDead = actor.isDead;
        stored->actor.isInstantDeath = actor.isInstantDeath;
        stored->actor.deathAnimGroup = actor.deathAnimGroup;
        stored->actor.dynamicStats.health.current = actor.dynamicStats.health.current;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        if (actor.deathEventId != 0)
            stored->lastDeathEventId = actor.deathEventId;
        rememberActorLocation(stored->actor, incoming.cellId);
        persistSpawnedActorIfNeeded(*stored);
        upsertSpawnedActorDynamicRecordLinkIfNeeded(stored->actor);
        if (stored->actor.mpNum == 0 && stored->actor.isDead)
        {
            Log(Debug::Info) << "[Server] ActorDeath stored vanilla corpse transform"
                             << " refId=" << stored->actor.refId
                             << " refNum=" << stored->actor.refNum
                             << " eventId=" << stored->actor.deathEventId
                             << " cell=" << incoming.cellId
                             << " pos=(" << stored->actor.position.pos[0]
                             << "," << stored->actor.position.pos[1]
                             << "," << stored->actor.position.pos[2] << ")"
                             << " deathAnim='" << stored->actor.deathAnimGroup << "'";
            rememberDeadVanillaActor(*stored);
        }
        else if (wasDead)
            forgetDeadVanillaActor(stored->actor, incoming.cellId);
        if (actor.isDead && !wasDead)
            sendActorLifecycleEvent("death", stored->actor, stored->persistent);
        filtered.actors.push_back(actor);
    }

    auto broadcastChangedDeathCells = [&]()
    {
        for (const std::string& changedCellId : changedCellIds)
        {
            auto changedCellIt = mWorld.actorCells.find(changedCellId);
            if (changedCellIt != mWorld.actorCells.end())
                broadcastActorListForCell(changedCellId, changedCellIt->second);
        }
    };

    if (filtered.actors.empty())
    {
        broadcastChangedDeathCells();
        return;
    }

    PacketActorDeath out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
    Log(Debug::Info) << "[Server] Broadcast ActorDeath to cell=" << filtered.cellId;
    broadcastChangedDeathCells();
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorEquipment"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorEquipment");
        if (!stored)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.equipment = actor.equipment;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    PacketActorEquipment out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    std::size_t deadSpawnedSuppressed = 0;
    std::string firstDeadSpawnedRefId;
    uint32_t firstDeadSpawnedMpNum = 0;

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectResetStaleDeadVanillaActor(actor, incoming.cellId, c, "ActorStatsDynamic"))
            continue;
        clearResetStaleDeathSuppressionForAliveVanillaActor(actor, incoming.cellId);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorStatsDynamic"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorStatsDynamic");
        if (!stored)
            continue;
        if (stored->actor.mpNum != 0 && stored->actor.isDead && !actor.isDead)
        {
            ++deadSpawnedSuppressed;
            if (firstDeadSpawnedRefId.empty())
            {
                firstDeadSpawnedRefId = stored->actor.refId;
                firstDeadSpawnedMpNum = stored->actor.mpNum;
            }
            continue;
        }
        const bool wasDead = stored->actor.isDead;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.dynamicStats = actor.dynamicStats;
        stored->actor.isDead = actor.isDead;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        if (wasDead && !stored->actor.isDead)
            forgetDeadVanillaActor(stored->actor, incoming.cellId);
        if (actor.isDead && !wasDead)
            sendActorLifecycleEvent("death", stored->actor, stored->persistent);
        filtered.actors.push_back(actor);
    }

    if (deadSpawnedSuppressed != 0)
    {
        Log(Debug::Verbose) << "[Server] ActorStatsDynamic suppressed live update(s) for dead spawned actor"
                            << " from=" << c.name
                            << " cell=" << incoming.cellId
                            << " suppressed=" << deadSpawnedSuppressed
                            << " firstRefId=" << firstDeadSpawnedRefId
                            << " firstMpNum=" << firstDeadSpawnedMpNum;
    }

    if (filtered.actors.empty())
        return;

    PacketActorStatsDynamic out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
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

    ActorList filtered = incoming;
    filtered.actors.clear();
    filtered.actors.reserve(incoming.actors.size());

    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
        if (rejectStaleAliveVanillaActor(actor, incoming.cellId, c, "ActorAI"))
            continue;

        ActorRegistryRecord* stored = findTrackedActor(cellState, actor, c, "ActorAI");
        if (!stored)
            continue;
        stored->actor.refId = actor.refId;
        stored->actor.refNum = actor.refNum;
        stored->actor.mpNum = actor.mpNum;
        stored->actor.cellId = incoming.cellId;
        stored->actor.ai = actor.ai;
        stored->lastSnapshotTime = incoming.serverTimestamp;
        persistSpawnedActorIfNeeded(*stored);
        filtered.actors.push_back(actor);
    }

    if (filtered.actors.empty())
        return;

    PacketActorAI out;
    out.setActorList(&filtered);
    broadcastActorToCell(filtered.cellId, out.encode(), c.conn);
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
    for (auto& actor : incoming.actors)
    {
        actor.cellId = incoming.cellId;
        normalizeActorIdentity(actor);
    }

    PacketActorCombatRequest out;
    out.setActorList(&incoming);

    if (incoming.cellId.empty() || !clientHasActorCellLoaded(c, incoming.cellId))
    {
        Log(Debug::Verbose) << "[Server] Rejecting ActorCombatRequest from " << c.name
                            << " because the actor cell is not loaded: player=" << makeCellKey(c.player.cell)
                            << " packet=" << incoming.cellId;
        return;
    }

    auto cellIt = mWorld.actorCells.find(incoming.cellId);
    if (cellIt == mWorld.actorCells.end()) return;

    const uint32_t authorityGuid = cellIt->second.authorityGuid;

    // NPC->player damage: routed by the cell authority to the victim player.
    // This must be checked BEFORE the authority guard because the sender IS
    // the authority in this case (authority forwards NPC hits to the victim).
    if (incoming.victimPlayerGuid != 0)
    {
        if (authorityGuid == 0 || authorityGuid != c.guid)
            return;

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

    for (const auto& [actorCellId, cellState] : mWorld.actorCells)
    {
        for (const auto& [actorKey, actorRecord] : cellState.actors)
        {
            if (actorRecord.actor.mpNum == pkt.mpNum)
            {
                const BaseActor& actor = actorRecord.actor;
                if (!cellMatches(c.player.cell, actorCellId))
                {
                    Log(Debug::Verbose) << "[Server] Ignoring ObjectDelete from " << c.name
                                        << " mpNum=" << pkt.mpNum
                                        << " cell=" << pkt.cellId
                                        << " because spawned corpse is not in the player's current cell";
                    return;
                }

                if (!actor.isDead)
                {
                    Log(Debug::Verbose) << "[Server] Ignoring ObjectDelete from " << c.name
                                        << " mpNum=" << pkt.mpNum
                                        << " cell=" << pkt.cellId
                                        << " because spawned actor is still alive";
                    return;
                }

                const std::string containerKey = makeContainerKey(actorCellId, actor.refId, actor.refNum, actor.mpNum);
                auto containerIt = mWorld.containers.find(containerKey);
                if (containerIt != mWorld.containers.end() && !containerIt->second.items.empty())
                {
                    Log(Debug::Verbose) << "[Server] Ignoring ObjectDelete from " << c.name
                                        << " mpNum=" << pkt.mpNum
                                        << " cell=" << pkt.cellId
                                        << " because spawned corpse container is not empty";
                    return;
                }

                if (!removeActor(actor.mpNum, actorCellId))
                {
                    Log(Debug::Verbose) << "[Server] Ignoring ObjectDelete from " << c.name
                                        << " mpNum=" << pkt.mpNum
                                        << " cell=" << pkt.cellId
                                        << " because corpse removal failed";
                    return;
                }

                Log(Debug::Info) << "[Server] Corpse dispose accepted: player=" << c.name
                                 << " mpNum=" << pkt.mpNum
                                 << " cell=" << actorCellId;
                return;
            }
        }
    }

    if (!removePlacedObjectAuthoritative(pkt.mpNum, pkt.cellId))
    {
        Log(Debug::Verbose) << "[Server] Ignoring ObjectDelete from " << c.name
                            << " mpNum=" << pkt.mpNum
                            << " cell=" << pkt.cellId
                            << " because no matching placed object exists";
        return;
    }

    Log(Debug::Info) << "[Server] ObjectDelete accepted: player=" << c.name
                     << " mpNum=" << pkt.mpNum
                     << " cell=" << pkt.cellId;
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

    const std::string key = makeContainerKey(
        pkt.container.cellId, pkt.container.refId, pkt.container.refNum, pkt.container.mpNum);
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
                             << " mpNum=" << authoritative.mpNum
                             << " items=" << authoritative.items.size();
            return;
        }

        authoritative = pkt.container;
        authoritative.hasAuthority = true;
        if (mPlayerDb)
            mPlayerDb->upsertContainerRecord(authoritative);

        scheduleGeneratedDynamicRecordGc("container_set");

        PacketContainer accepted;
        accepted.container = authoritative;
        accepted.mAction = static_cast<uint8_t>(ContainerAction::Set);
        sendTo(c.conn, accepted.encode());
        broadcastToCell(authoritative.cellId, accepted.encode(), c.conn);
        Log(Debug::Info) << "[Server] Container(Set accepted): player=" << c.name
                         << " refId=" << authoritative.refId
                         << " refNum=" << authoritative.refNum
                         << " mpNum=" << authoritative.mpNum
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
    else if (authoritative.mpNum == 0 && pkt.container.mpNum != 0)
    {
        authoritative.mpNum = pkt.container.mpNum;
    }

    normalizeContainerItems(pkt.container.items);
    for (const auto& item : pkt.container.items)
        applyContainerDelta(authoritative.items, item, action);

    normalizeContainerItems(authoritative.items);

    if (mPlayerDb)
        mPlayerDb->upsertContainerRecord(authoritative);

    scheduleGeneratedDynamicRecordGc("container_delta");

    const ContainerItem* firstDelta = pkt.container.items.empty() ? nullptr : &pkt.container.items.front();
    Log(Debug::Info) << "[Server] Container(" << static_cast<int>(action) << "): player=" << c.name
                     << " refId=" << authoritative.refId
                     << " refNum=" << authoritative.refNum
                     << " mpNum=" << authoritative.mpNum
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
    return removePlacedObjectAuthoritativeAnyCell(mpNum, cellId);
}

bool MPServer::worldMpNumInUse(uint32_t mpNum) const
{
    if (mpNum == 0)
        return false;

    for (const auto& [cellId, objects] : mWorld.placedObjects)
    {
        for (const auto& object : objects)
        {
            if (object.mpNum == mpNum)
                return true;
        }
    }

    for (const auto& [cellId, cellState] : mWorld.actorCells)
    {
        for (const auto& [actorKey, record] : cellState.actors)
        {
            if (record.actor.mpNum == mpNum)
                return true;
        }
    }

    for (const auto& [containerKey, record] : mWorld.containers)
    {
        if (record.mpNum == mpNum)
            return true;
    }

    for (const auto& [cellId, doors] : mWorld.doorStates)
    {
        for (const auto& door : doors)
        {
            if (door.mpNum == mpNum)
                return true;
        }
    }

    return false;
}

void MPServer::setNextWorldMpNum(uint64_t nextMpNum)
{
    nextMpNum = std::max<uint64_t>(nextMpNum, 1);
    mWorld.nextObjectMpNum = nextMpNum;
    mWorld.nextActorMpNum = nextMpNum;
    if (mPlayerDb)
        mPlayerDb->saveNextMpNum(nextMpNum);
}

std::optional<uint32_t> MPServer::reserveWorldMpNum()
{
    uint64_t candidate = std::max<uint64_t>(mWorld.nextObjectMpNum, mWorld.nextActorMpNum);
    candidate = std::max<uint64_t>(candidate, 1);

    constexpr uint64_t maxMpNum = std::numeric_limits<uint32_t>::max();
    if (candidate > maxMpNum)
        return std::nullopt;

    setNextWorldMpNum(candidate + 1);
    return static_cast<uint32_t>(candidate);
}

void MPServer::advanceWorldMpNumPast(uint32_t mpNum)
{
    if (mpNum == 0)
        return;

    const uint64_t minimumNext = static_cast<uint64_t>(mpNum) + 1;
    const uint64_t nextMpNum = std::max({ mWorld.nextObjectMpNum, mWorld.nextActorMpNum, minimumNext });
    if (nextMpNum != mWorld.nextObjectMpNum || nextMpNum != mWorld.nextActorMpNum)
        setNextWorldMpNum(nextMpNum);
}

bool MPServer::removeGameObject(uint32_t mpNum, const std::string& cellId)
{
    if (mpNum == 0)
        return false;

    for (const auto& [actorCellId, cellState] : mWorld.actorCells)
    {
        if (!cellId.empty() && actorCellId != cellId)
            continue;

        for (const auto& [actorKey, record] : cellState.actors)
        {
            if (record.actor.mpNum == mpNum)
                return removeActor(mpNum, actorCellId);
        }
    }

    return removePlacedObjectAuthoritativeAnyCell(mpNum, cellId);
}

bool MPServer::spawnActor(
    const std::string& refId, uint32_t refNum, uint32_t mpNum, const std::string& cellId, const Position& position,
    bool persistent)
{
    if (refId.empty() || cellId.empty())
        return false;

    const WorldState::StoredDynamicRecord* dynamicActorRecord = nullptr;
    for (const std::string_view recordType : { std::string_view("npc"), std::string_view("creature") })
    {
        auto it = mWorld.dynamicRecords.find(makeDynamicRecordKey(recordType, refId));
        if (it != mWorld.dynamicRecords.end())
        {
            dynamicActorRecord = &it->second;
            break;
        }
    }

    if (dynamicActorRecord == nullptr)
    {
        for (const auto& [key, record] : mWorld.dynamicRecords)
        {
            if (record.recordId != refId)
                continue;

            Log(Debug::Warning) << "[Server] Rejecting Script ActorSpawn for non-actor dynamic record type="
                                << record.recordType << " id=" << refId;
            return false;
        }
    }

    uint32_t assignedMpNum = mpNum;
    if (assignedMpNum == 0)
    {
        do
        {
            const std::optional<uint32_t> reservedMpNum = reserveWorldMpNum();
            if (!reservedMpNum)
            {
                Log(Debug::Warning) << "[Server] Rejecting Script ActorSpawn because mpNum space is exhausted";
                return false;
            }

            assignedMpNum = *reservedMpNum;
        }
        while (worldMpNumInUse(assignedMpNum));
    }
    else
    {
        if (worldMpNumInUse(assignedMpNum))
        {
            Log(Debug::Warning) << "[Server] Rejecting Script ActorSpawn for duplicate actor mpNum=" << assignedMpNum;
            return false;
        }

        advanceWorldMpNumPast(assignedMpNum);
    }

    BaseActor actor;
    actor.refId = refId;
    actor.mpNum = assignedMpNum;
    actor.refNum = assignedMpNum != 0 ? 0 : refNum;
    actor.cellId = cellId;
    actor.position = position;
    actor.equipment.resize(BaseActor::NUM_EQUIPMENT_SLOTS);

    std::size_t staleContainerCount = 0;
    for (auto it = mWorld.containers.begin(); it != mWorld.containers.end();)
    {
        const ContainerRecord& record = it->second;
        if (record.mpNum != assignedMpNum)
        {
            ++it;
            continue;
        }

        if (mPlayerDb)
            mPlayerDb->deleteContainerRecord(record.cellId, record.refId, record.refNum);
        it = mWorld.containers.erase(it);
        ++staleContainerCount;
    }

    if (staleContainerCount != 0)
        Log(Debug::Info) << "[Server] Script ActorSpawn cleared stale container authority"
                         << " mpNum=" << assignedMpNum
                         << " count=" << staleContainerCount;

    auto& cellState = mWorld.actorCells[cellId];
    if (cellState.authorityGuid == 0)
        refreshActorAuthorityForCell(cellId);

    const uint64_t timestamp = currentServerTimeMs();
    ActorRegistryRecord registryRecord;
    registryRecord.actor           = actor;
    registryRecord.lastSnapshotTime = timestamp;
    registryRecord.serverSpawnTime  = timestamp;   // never updated by client
    registryRecord.persistent       = persistent;
    ensureActorNetId(registryRecord, cellId);
    cellState.actors[makeActorKey(actor)] = registryRecord;
    rememberActorLocation(actor, cellId);

    if (dynamicActorRecord != nullptr)
    {
        PacketRecordDynamic recordPkt;
        recordPkt.action = DynamicRecordAction::Upsert;
        recordPkt.recordType = dynamicActorRecord->recordType;
        recordPkt.entries.push_back({ dynamicActorRecord->recordId, dynamicActorRecord->data });
        broadcastActorToCell(cellId, recordPkt.encode());

        if (mPlayerDb)
            mPlayerDb->upsertSpawnedActorDynamicRecordLink(actor.refId, cellId, actor.mpNum);
    }

    persistSpawnedActorIfNeeded(registryRecord);

    ActorList actorList;
    actorList.cellId = cellId;
    actorList.isAuthority = false;
    actorList.authorityGuid = cellState.authorityGuid;
    actorList.authorityGeneration = cellState.authorityGeneration;
    actorList.snapshotSequence = cellState.nextSnapshotSequence++;
    actorList.serverTimestamp = timestamp;
    actorList.actors.reserve(cellState.actors.size());
    for (const auto& [actorKey, record] : cellState.actors)
        actorList.actors.push_back(record.actor);

    PacketActorList pkt;
    pkt.setActorList(&actorList);
    broadcastActorIdentityForCell(cellId, cellState);
    broadcastActorToCell(cellId, pkt.encode());

    Log(Debug::Info) << "[Server] Script ActorSpawn accepted: refId=" << actor.refId
                     << " refNum=" << actor.refNum
                     << " mpNum=" << actor.mpNum
                     << " actorNetId=" << registryRecord.actorNetId
                     << " cell=" << actor.cellId
                     << " persistent=" << persistent;
    sendActorLifecycleEvent("spawned", actor, persistent);
    return true;
}

bool MPServer::removeActor(uint32_t mpNum, const std::string& cellId)
{
    if (mpNum == 0)
        return false;

    auto cellIt = cellId.empty() ? mWorld.actorCells.end() : mWorld.actorCells.find(cellId);
    std::string resolvedCellId = cellId;

    auto actorPresentInCell = [&](const CellActorState& state)
    {
        for (const auto& [actorKey, record] : state.actors)
        {
            if (record.actor.mpNum == mpNum)
                return true;
        }
        return false;
    };

    if (cellIt == mWorld.actorCells.end() || !actorPresentInCell(cellIt->second))
    {
        cellIt = mWorld.actorCells.end();
        resolvedCellId.clear();
        for (auto it = mWorld.actorCells.begin(); it != mWorld.actorCells.end(); ++it)
        {
            if (!actorPresentInCell(it->second))
                continue;

            cellIt = it;
            resolvedCellId = it->first;
            break;
        }
    }

    if (cellIt == mWorld.actorCells.end() || resolvedCellId.empty())
        return false;

    auto& actors = cellIt->second.actors;
    bool removed = false;
    std::vector<ActorRegistryRecord> removedRecords;
    for (auto it = actors.begin(); it != actors.end();)
    {
        if (it->second.actor.mpNum == mpNum)
        {
            ensureActorNetId(it->second, resolvedCellId);
            removedRecords.push_back(it->second);
            forgetActorLocation(it->second.actor, resolvedCellId);
            it = actors.erase(it);
            removed = true;
        }
        else
        {
            ++it;
        }
    }

    if (!removed)
        return false;

    broadcastActorIdentityRemovalForCell(resolvedCellId, cellIt->second, removedRecords);

    for (const ActorRegistryRecord& record : removedRecords)
        forgetActorNetId(record.actorNetId, record.actor);

    for (auto it = mWorld.containers.begin(); it != mWorld.containers.end();)
    {
        const ContainerRecord& record = it->second;
        if (record.mpNum == mpNum)
        {
            if (mPlayerDb)
                mPlayerDb->deleteContainerRecord(record.cellId, record.refId, record.refNum);
            it = mWorld.containers.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (mPlayerDb)
    {
        mPlayerDb->deleteSpawnedActorDynamicRecordLink(mpNum, resolvedCellId);
        mPlayerDb->deleteSpawnedActor(mpNum);
        scheduleGeneratedDynamicRecordGc("remove_actor");
    }

    ActorList actorList;
    actorList.cellId = resolvedCellId;
    actorList.isAuthority = false;
    actorList.authorityGuid = cellIt->second.authorityGuid;
    actorList.authorityGeneration = cellIt->second.authorityGeneration;
    actorList.snapshotSequence = cellIt->second.nextSnapshotSequence++;
    actorList.serverTimestamp = currentServerTimeMs();
    actorList.actors.reserve(cellIt->second.actors.size());
    for (const auto& [actorKey, record] : cellIt->second.actors)
        actorList.actors.push_back(record.actor);

    PacketActorList pkt;
    pkt.setActorList(&actorList);
    broadcastActorToCell(resolvedCellId, pkt.encode());

    Log(Debug::Info) << "[Server] Script ActorRemove accepted: mpNum=" << mpNum
                     << " cell=" << resolvedCellId;
    return true;
}

bool MPServer::resetCellStateForTesting(const std::string& cellId)
{
    if (cellId.empty())
        return false;

    std::vector<ActorRegistryRecord> removedRecords;
    std::unordered_set<std::string> resetSuppressedVanillaDeaths;
    std::size_t runtimeSpawnedActors = 0;
    std::size_t runtimeVanillaActors = 0;
    std::size_t deadVanillaActors = 0;

    auto cellIt = mWorld.actorCells.find(cellId);
    if (cellIt != mWorld.actorCells.end())
    {
        removedRecords.reserve(cellIt->second.actors.size());
        for (auto& [actorKey, record] : cellIt->second.actors)
        {
            ensureActorNetId(record, cellId);
            removedRecords.push_back(record);
            if (record.actor.mpNum != 0)
                ++runtimeSpawnedActors;
            else
            {
                ++runtimeVanillaActors;
                resetSuppressedVanillaDeaths.insert(actorKey);
            }
            forgetActorLocation(record.actor, cellId);
        }
        cellIt->second.actors.clear();
        cellIt->second.staleLiveVanillaDeathResendMs.clear();
    }

    auto deadCellIt = mWorld.deadVanillaActorCells.find(cellId);
    if (deadCellIt != mWorld.deadVanillaActorCells.end())
    {
        for (auto& [actorKey, record] : deadCellIt->second)
        {
            ActorRegistryRecord removedRecord = record;
            ensureActorNetId(removedRecord, cellId);
            removedRecords.push_back(removedRecord);
            forgetActorLocation(removedRecord.actor, cellId);
            resetSuppressedVanillaDeaths.insert(actorKey);
            ++deadVanillaActors;
        }
        mWorld.deadVanillaActorCells.erase(deadCellIt);
    }

    if (cellIt == mWorld.actorCells.end())
        cellIt = mWorld.actorCells.emplace(cellId, CellActorState {}).first;
    cellIt->second.staleLiveVanillaDeathResendMs.clear();
    cellIt->second.resetSuppressedVanillaDeaths.insert(
        resetSuppressedVanillaDeaths.begin(), resetSuppressedVanillaDeaths.end());

    if (!removedRecords.empty())
    {
        broadcastActorIdentityRemovalForCell(cellId, cellIt->second, removedRecords);
        for (const ActorRegistryRecord& record : removedRecords)
            forgetActorNetId(record.actorNetId, record.actor);

        ActorList actorList;
        actorList.cellId = cellId;
        actorList.isAuthority = false;
        actorList.authorityGuid = cellIt->second.authorityGuid;
        actorList.authorityGeneration = cellIt->second.authorityGeneration;
        actorList.snapshotSequence = cellIt->second.nextSnapshotSequence++;
        actorList.serverTimestamp = currentServerTimeMs();

        PacketActorList pkt;
        pkt.setActorList(&actorList);
        broadcastActorToCell(cellId, pkt.encode());
    }

    std::size_t runtimePlacedObjects = 0;
    auto objectsIt = mWorld.placedObjects.find(cellId);
    if (objectsIt != mWorld.placedObjects.end())
    {
        runtimePlacedObjects = objectsIt->second.size();
        for (const PlacedObject& object : objectsIt->second)
            mLua.removePlacedObject(object.mpNum);
        mWorld.placedObjects.erase(objectsIt);
    }

    std::size_t runtimeContainers = 0;
    for (auto it = mWorld.containers.begin(); it != mWorld.containers.end();)
    {
        if (it->second.cellId != cellId)
        {
            ++it;
            continue;
        }

        it = mWorld.containers.erase(it);
        ++runtimeContainers;
    }

    const std::size_t runtimeDoorStates = mWorld.doorStates.erase(cellId);

    std::size_t dbPlacedObjects = 0;
    std::size_t dbSpawnedActors = 0;
    std::size_t dbDeadVanillaActors = 0;
    std::size_t dbContainers = 0;
    std::size_t dbDoorStates = 0;
    std::size_t dbSpawnedActorLinks = 0;
    if (mPlayerDb)
    {
        dbPlacedObjects = mPlayerDb->deleteWorldObjectsForCell(cellId);
        dbSpawnedActors = mPlayerDb->deleteSpawnedActorsForCell(cellId);
        dbDeadVanillaActors = mPlayerDb->deleteDeadVanillaActorsForCell(cellId);
        dbContainers = mPlayerDb->deleteContainerRecordsForCell(cellId);
        dbDoorStates = mPlayerDb->deleteDoorStatesForCell(cellId);
        dbSpawnedActorLinks = mPlayerDb->deleteSpawnedActorDynamicRecordLinksForCell(cellId);
        scheduleGeneratedDynamicRecordGc("reset_cell");
    }

    refreshActorAuthorityForCell(cellId);
    sendActorStateToInterestedClients(cellId);
    syncLuaSnapshot();

    Log(Debug::Info) << "[Server] Reset cell state"
                     << " cell=" << cellId
                     << " runtimeSpawnedActors=" << runtimeSpawnedActors
                     << " runtimeVanillaActors=" << runtimeVanillaActors
                     << " deadVanillaActors=" << deadVanillaActors
                     << " resetSuppressedVanillaDeaths=" << resetSuppressedVanillaDeaths.size()
                     << " runtimePlacedObjects=" << runtimePlacedObjects
                     << " runtimeContainers=" << runtimeContainers
                     << " runtimeDoorStates=" << runtimeDoorStates
                     << " dbPlacedObjects=" << dbPlacedObjects
                     << " dbSpawnedActors=" << dbSpawnedActors
                     << " dbDeadVanillaActors=" << dbDeadVanillaActors
                     << " dbContainers=" << dbContainers
                     << " dbDoorStates=" << dbDoorStates
                     << " dbSpawnedActorLinks=" << dbSpawnedActorLinks;

    return true;
}

bool MPServer::upsertDynamicRecord(const std::string& recordType, const std::string& recordId, const std::string& data,
    const std::string& recordScope, bool persistent)
{
    const std::string normalizedType = normalizeDynamicRecordType(recordType);
    const std::string normalizedScope = normalizeDynamicRecordScope(recordScope);
    if (normalizedType.empty() || recordId.empty())
        return false;
    if (normalizedScope.empty())
        return false;

    auto& record = mWorld.dynamicRecords[makeDynamicRecordKey(normalizedType, recordId)];
    record.recordType = normalizedType;
    record.recordId = recordId;
    record.data = data;
    record.recordScope = normalizedScope;
    record.persistent = persistent;
    record.sequence = mWorld.nextDynamicRecordSequence++;

    if (normalizedScope == "generated")
        mLua.observeGeneratedRecordId(normalizedType, recordId);

    if (mPlayerDb)
    {
        DynamicRecordCatalogEntry catalogRecord;
        catalogRecord.recordType = normalizedType;
        catalogRecord.recordId = recordId;
        catalogRecord.recordScope = normalizedScope;
        catalogRecord.persistent = persistent;
        mPlayerDb->upsertDynamicRecordCatalog(catalogRecord);

        if (persistent)
        {
            PersistedDynamicRecord persisted;
            persisted.recordType = normalizedType;
            persisted.recordId = recordId;
            persisted.recordScope = normalizedScope;
            persisted.data = data;
            mPlayerDb->upsertDynamicRecord(persisted);
        }
        else
        {
            mPlayerDb->deleteDynamicRecord(normalizedType, recordId);
        }
    }

    PacketRecordDynamic pkt;
    pkt.action = DynamicRecordAction::Upsert;
    pkt.recordType = normalizedType;
    pkt.entries.push_back({ recordId, data });
    broadcastToAll(pkt.encode());

    Log(Debug::Info) << "[Server] Upserted dynamic record type=" << normalizedType
                     << " id=" << recordId
                     << " scope=" << normalizedScope
                     << " persistent=" << (persistent ? "true" : "false");
    return true;
}

bool MPServer::removeDynamicRecord(const std::string& recordType, const std::string& recordId)
{
    const std::string normalizedType = normalizeDynamicRecordType(recordType);
    if (normalizedType.empty() || recordId.empty())
        return false;

    auto it = mWorld.dynamicRecords.find(makeDynamicRecordKey(normalizedType, recordId));
    if (it == mWorld.dynamicRecords.end())
        return false;

    cleanupDynamicReferences(
        [&](std::string_view refId) -> bool { return refId == recordId; },
        /*broadcastLive=*/true,
        recordId);

    mWorld.dynamicRecords.erase(it);

    if (mPlayerDb)
    {
        mPlayerDb->replaceDynamicRecordDependencies(normalizedType, recordId, {});
        mPlayerDb->deleteDynamicRecord(normalizedType, recordId);
        mPlayerDb->deleteDynamicRecordCatalog(normalizedType, recordId);
        mPlayerDb->deleteDynamicRecordLinks(recordId);
    }

    PacketRecordDynamic pkt;
    pkt.action = DynamicRecordAction::Remove;
    pkt.recordType = normalizedType;
    pkt.entries.push_back({ recordId, {} });
    broadcastToAll(pkt.encode());

    Log(Debug::Info) << "[Server] Removed dynamic record type=" << normalizedType
                     << " id=" << recordId;
    return true;
}

bool MPServer::setDynamicRecordDependencies(
    const std::string& recordType, const std::string& recordId, const std::vector<std::string>& dependencyRecordIds)
{
    const std::string normalizedType = normalizeDynamicRecordType(recordType);
    if (normalizedType.empty() || recordId.empty())
        return false;

    if (mWorld.dynamicRecords.find(makeDynamicRecordKey(normalizedType, recordId)) == mWorld.dynamicRecords.end())
        return false;

    std::vector<std::string> uniqueDependencies;
    std::unordered_set<std::string> seen;

    for (const auto& dependencyRecordId : dependencyRecordIds)
    {
        if (dependencyRecordId.empty() || dependencyRecordId == recordId)
            continue;
        if (!seen.insert(dependencyRecordId).second)
            continue;

        bool found = false;
        for (const auto& [key, record] : mWorld.dynamicRecords)
        {
            if (record.recordId == dependencyRecordId)
            {
                found = true;
                break;
            }
        }

        if (!found)
            return false;

        uniqueDependencies.push_back(dependencyRecordId);
    }

    if (mPlayerDb)
        mPlayerDb->replaceDynamicRecordDependencies(normalizedType, recordId, uniqueDependencies);

    Log(Debug::Info) << "[Server] Set dynamic record dependencies type=" << normalizedType
                     << " id=" << recordId
                     << " count=" << uniqueDependencies.size();
    return true;
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

    do
    {
        const std::optional<uint32_t> reservedMpNum = reserveWorldMpNum();
        if (!reservedMpNum)
        {
            Log(Debug::Warning) << "[Server] Rejecting ObjectPlace because mpNum space is exhausted";
            return false;
        }

        object.mpNum = *reservedMpNum;
    }
    while (worldMpNumInUse(object.mpNum));

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

    std::vector<LuaActorSnapshot> actors;
    for (const auto& [cellId, cellState] : mWorld.actorCells)
    {
        for (const auto& [actorKey, record] : cellState.actors)
        {
            if (record.actor.mpNum == 0)
                continue;

            LuaActorSnapshot snapshot;
            snapshot.actor = record.actor;
            if (snapshot.actor.cellId.empty())
                snapshot.actor.cellId = cellId;
            snapshot.persistent = record.persistent;
            actors.push_back(std::move(snapshot));
        }
    }
    mLua.syncActors(std::move(actors));
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
void MPServer::sendAuthoritativeEquipment(ConnectedClient& c, bool includeOthers)
{
    PacketPlayerEquipment equipment;
    equipment.setPlayer(&c.player);
    const std::vector<uint8_t> encoded = equipment.encode();
    sendTo(c.conn, encoded);
    if (includeOthers)
        broadcastToAll(encoded, c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::sendPlayerStateBootstrapToClient(ConnectedClient& receiver)
{
    if (!receiver.charSelectComplete)
        return;

    std::size_t sent = 0;
    for (auto& [conn, client] : mClients)
    {
        if (conn == receiver.conn || !client.charSelectComplete)
            continue;

        PacketPlayerBaseInfo baseInfo;
        baseInfo.setPlayer(&client.player);
        sendTo(receiver.conn, baseInfo.encode());

        PacketPlayerCellChange cellChange;
        cellChange.setPlayer(&client.player);
        sendTo(receiver.conn, cellChange.encode());

        PacketPlayerEquipment equipment;
        equipment.setPlayer(&client.player);
        sendTo(receiver.conn, equipment.encode());

        if (client.player.position.pos[0] != 0.f
            || client.player.position.pos[1] != 0.f
            || client.player.position.pos[2] != 0.f)
        {
            PacketPlayerPosition positionPacket;
            positionPacket.setPlayer(&client.player);
            sendTo(receiver.conn, positionPacket.encode());
        }

        ++sent;
    }

    if (sent != 0)
    {
        Log(Debug::Verbose) << "[Server] Sent player bootstrap to " << receiver.name
                            << " players=" << sent
                            << " cell=" << makeCellKey(receiver.player.cell);
    }
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
    scheduleGeneratedDynamicRecordGc("grant_inventory_item");
    return true;
}

// ---------------------------------------------------------------------------
bool MPServer::removePlacedObjectAuthoritative(uint32_t mpNum, const std::string& cellId)
{
    if (mpNum == 0 || cellId.empty())
        return false;

    auto objectsIt = mWorld.placedObjects.find(cellId);
    if (objectsIt == mWorld.placedObjects.end())
        return false;

    auto& objects = objectsIt->second;
    const auto matchIt = std::find_if(objects.begin(), objects.end(),
        [&](const PlacedObject& object) { return object.mpNum == mpNum; });
    if (matchIt == objects.end())
        return false;

    mLua.removePlacedObject(mpNum);

    objects.erase(std::remove_if(objects.begin(), objects.end(),
        [&](const PlacedObject& object) { return object.mpNum == mpNum; }),
        objects.end());
    if (objects.empty())
    {
        mWorld.placedObjects.erase(objectsIt);
    }

    if (mPlayerDb)
        mPlayerDb->deleteWorldObject(mpNum);

    scheduleGeneratedDynamicRecordGc("remove_placed_object_authoritative");

    PacketObjectDelete pkt;
    pkt.mpNum = mpNum;
    pkt.cellId = cellId;
    broadcastToCell(cellId, pkt.encode());
    return true;
}

bool MPServer::removePlacedObjectAuthoritativeAnyCell(uint32_t mpNum, const std::string& preferredCellId)
{
    if (mpNum == 0)
        return false;

    if (!preferredCellId.empty() && removePlacedObjectAuthoritative(mpNum, preferredCellId))
        return true;

    for (const auto& [cellId, objects] : mWorld.placedObjects)
    {
        if (cellId == preferredCellId)
            continue;

        const auto it = std::find_if(objects.begin(), objects.end(),
            [&](const PlacedObject& object) { return object.mpNum == mpNum; });
        if (it != objects.end())
            return removePlacedObjectAuthoritative(mpNum, cellId);
    }

    return false;
}

// ---------------------------------------------------------------------------
void MPServer::startAdminHttpServer()
{
    if (!mAdminHttpEnabled)
        return;

    if (!mAdminHttpServer)
    {
        mAdminHttpServer = std::make_unique<AdminHttpServer>(
            [this](std::string_view action, const std::map<std::string, std::string>& query) {
                return handleAdminHttpRequest(action, query);
            });
    }

    std::string error;
    if (!mAdminHttpServer->start(mAdminHttpHost, mAdminHttpPort, &error))
    {
        Log(Debug::Warning) << "[Server] Failed to start admin HTTP listener on "
                            << mAdminHttpHost << ":" << mAdminHttpPort
                            << " error=" << error;
        mAdminHttpServer.reset();
        return;
    }

    Log(Debug::Info) << "[Server] Admin HTTP browser listening at " << mAdminHttpServer->url();
}

void MPServer::stopAdminHttpServer()
{
    if (!mAdminHttpServer)
        return;

    mAdminHttpServer->stop();
    mAdminHttpServer.reset();
}

AdminHttpServer::Response MPServer::handleAdminHttpRequest(
    std::string_view action, const std::map<std::string, std::string>& query)
{
    AdminHttpServer::Response response;
    response.contentType = "application/json; charset=utf-8";

    if (!mLua.isLoaded() || !mLua.isRunning())
    {
        response.status = 503;
        response.body = makeJsonErrorBody("lua_service_unavailable");
        return response;
    }

    LuaWireTable payload;
    payload.emplace_back("action", std::string(action));
    for (const auto& [key, value] : query)
        payload.emplace_back(key, value);

    std::string error;
    const auto resultData = mLua.callSynchronousInterface(
        "IntentPolicy", "handleAdminUiHttp", serializeLuaWireTable(payload), mAdminHttpTimeoutMs, &error);
    if (!resultData)
    {
        response.status = error == "timeout" ? 504 : 500;
        response.body = makeJsonErrorBody(error.empty() ? "admin_http_request_failed" : error);
        return response;
    }

    try
    {
        const LuaWireTable result = parseLuaWireTable(*resultData);
        response.status = std::max(100, std::min(599, static_cast<int>(getLuaNumberField(result, "status", 200.0))));
        response.contentType = getLuaStringField(result, "contentType", response.contentType);
        response.body = getLuaStringField(result, "body", "{}");
        if (response.body.empty())
            response.body = "{}";
    }
    catch (const std::exception& e)
    {
        response.status = 500;
        response.body = makeJsonErrorBody(std::string("invalid_admin_http_response:") + e.what());
    }

    return response;
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

void MPServer::broadcastActorToCell(const std::string& cellId,
                                    const std::vector<uint8_t>& data,
                                    HSteamNetConnection except,
                                    bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    for (auto& [conn, client] : mClients)
    {
        if (conn == except || !clientHasActorCellLoaded(client, cellId)) continue;
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
