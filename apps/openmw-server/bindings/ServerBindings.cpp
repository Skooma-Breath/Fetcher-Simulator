#include "ServerBindings.hpp"

#include <components/lua/serialization.hpp>
#include <components/lua/storage.hpp>
#include <components/lua/luastate.hpp>
#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Base/DynamicRecord.hpp>
#include <sol/sol.hpp>

#include <optional>
#include <tuple>

#include "PlayerBindings.hpp"
#include "../LuaServerContext.hpp"

namespace mwmp
{
namespace
{
    sol::table makePositionTable(sol::state_view lua, const Position& position)
    {
        sol::table table(lua, sol::create);
        table["x"] = position.pos[0];
        table["y"] = position.pos[1];
        table["z"] = position.pos[2];
        table["rx"] = position.rot[0];
        table["ry"] = position.rot[1];
        table["rz"] = position.rot[2];
        return LuaUtil::makeReadOnly(table);
    }

    sol::object makePlacedObjectValue(sol::this_state thisState, const PlacedObject& object)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["mpNum"] = object.mpNum;
        table["refId"] = object.refId;
        table["count"] = object.count;
        table["cell"] = object.cellId;
        table["position"] = makePositionTable(lua, object.position);
        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    sol::object makeSurfPhysicsValue(sol::this_state thisState, const SurfPhysicsSettings& settings)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["cellId"] = settings.cellId;
        table["enabled"] = settings.enabled;
        table["airAccel"] = settings.airAcceleration;
        table["maxAirSpeed"] = settings.maxAirSpeed;
        table["friction"] = settings.groundFriction;
        table["groundAccel"] = settings.groundAcceleration;
        table["jumpSpeed"] = settings.jumpSpeed;
        table["gravityMult"] = settings.gravityMultiplier;
        table["gravity"] = settings.gravityMultiplier;
        table["overbounce"] = settings.overbounce;
        table["rampAngle"] = settings.rampAngle;
        table["impactOverbounce"] = settings.impactOverbounce;
        table["impactVelocityThreshold"] = settings.impactVelocityThreshold;
        table["surfPhysicsEnabled"] = settings.enabled;
        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    sol::object makePlayerMarkValue(sol::this_state thisState, const PlayerMark& mark)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["name"] = mark.name;
        table["cell"] = mark.cell;
        table["position"] = makePositionTable(lua, mark.position);
        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    sol::object makeDynamicRecordInfoValue(sol::this_state thisState, const DynamicRecordCatalogEntry& record)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["recordType"] = record.recordType;
        table["recordId"] = record.recordId;
        table["scope"] = record.recordScope;
        table["persistent"] = record.persistent;
        table["createdAt"] = record.createdAt;
        table["updatedAt"] = record.updatedAt;
        table["linkCount"] = record.linkCount;
        table["loaded"] = record.loaded;
        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    sol::object makeDatabaseTableInfoValue(sol::this_state thisState, const DatabaseTableInfo& info)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["name"] = info.name;
        table["rowCount"] = info.rowCount;
        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    sol::object makeDatabaseBrowsePageValue(sol::this_state thisState, const DatabaseBrowsePage& page)
    {
        sol::state_view lua(thisState);
        sol::table table(lua, sol::create);
        table["tableName"] = page.tableName;
        table["totalRows"] = page.totalRows;
        table["offset"] = page.offset;
        table["limit"] = page.limit;

        sol::table columns(lua, sol::create);
        for (std::size_t i = 0; i < page.columns.size(); ++i)
            columns[static_cast<int>(i + 1)] = page.columns[i];
        table["columns"] = LuaUtil::makeReadOnly(columns);

        sol::table rows(lua, sol::create);
        for (std::size_t rowIndex = 0; rowIndex < page.rows.size(); ++rowIndex)
        {
            sol::table row(lua, sol::create);
            const auto& rowValues = page.rows[rowIndex];
            for (std::size_t columnIndex = 0; columnIndex < page.columns.size() && columnIndex < rowValues.size(); ++columnIndex)
            {
                if (rowValues[columnIndex])
                    row[page.columns[columnIndex]] = *rowValues[columnIndex];
                else
                    row[page.columns[columnIndex]] = sol::lua_nil;
            }
            rows[static_cast<int>(rowIndex + 1)] = LuaUtil::makeReadOnly(row);
        }
        table["rows"] = LuaUtil::makeReadOnly(rows);

        return sol::make_object(thisState, LuaUtil::makeReadOnly(table));
    }

    std::optional<Position> parsePositionTable(const sol::table& table)
    {
        const auto x = table.get<sol::optional<double>>("x");
        const auto y = table.get<sol::optional<double>>("y");
        const auto z = table.get<sol::optional<double>>("z");
        if (!x || !y || !z)
            return std::nullopt;

        Position position;
        position.pos[0] = static_cast<float>(*x);
        position.pos[1] = static_cast<float>(*y);
        position.pos[2] = static_cast<float>(*z);
        position.rot[0] = static_cast<float>(table.get_or("rx", 0.0));
        position.rot[1] = static_cast<float>(table.get_or("ry", 0.0));
        position.rot[2] = static_cast<float>(table.get_or("rz", 0.0));
        position.isTeleporting = table.get_or("isTeleporting", false);
        return position;
    }
}

sol::table initMpPackage(LuaUtil::LuaView& view, LuaServerContext* context, LuaUtil::LuaStorage* storage)
{
    sol::table mp = view.newTable();

    initPlayerBindings(view, mp, context);

    // ── Logging ──────────────────────────────────────────────────────────
    // mp.log(text) — routes through the OpenMW log system so script output
    // gets the same timestamp/level prefix as C++ output.
    mp.set_function("log", [](const std::string& msg)
    {
        Log(Debug::Info) << "[Script] " << msg;
    });

    // ── Messaging ────────────────────────────────────────────────────────
    // mp.broadcast(text) — send a chat message from "Server" to all players.
    mp.set_function("broadcast", sol::overload(
        [context](const std::string& text)
        {
            if (context) context->queueBroadcastServerMessage(text);
        },
        [context](const std::string& eventName, const sol::table& data)
        {
            if (context) context->queueBroadcastLuaEvent(eventName, LuaUtil::serialize(data));
        }));

    mp.set_function("broadcastToCell", sol::overload(
        [context](const std::string& cellId, const std::string& text)
        {
            if (context) context->queueBroadcastServerMessageToCell(cellId, text);
        },
        [context](const std::string& cellId, const std::string& eventName, const sol::table& data)
        {
            if (context) context->queueBroadcastLuaEventToCell(cellId, eventName, LuaUtil::serialize(data));
        }));

    mp.set_function("send", [context](uint32_t guid, const std::string& eventName, const sol::table& data)
    {
        if (context) context->queueSendLuaEvent(guid, eventName, LuaUtil::serialize(data));
    });

    mp.set_function("kick", [context](uint32_t guid, const std::string& reason)
    {
        if (context) context->queueKickClient(guid, reason);
    });

    mp.set_function("isServer", []() -> bool
    {
        return true;
    });

    // ── Player queries ───────────────────────────────────────────────────
    // mp.getPlayerCount() — number of fully connected (post-handshake) players.
    mp.set_function("getPlayerCount", [context]() -> int
    {
        return context ? context->getPlayerCount() : 0;
    });

    mp.set_function("getObjectByMpNum", [context](uint32_t mpNum, sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        auto object = context->getPlacedObject(mpNum);
        if (!object)
            return sol::make_object(ts, sol::nil);

        return makePlacedObjectValue(ts, *object);
    });

    mp.set_function("grantInventoryItem", [context](uint32_t guid, const std::string& refId, int count) -> bool
    {
        if (!context || guid == 0 || refId.empty() || count <= 0)
            return false;

        context->queueGrantInventoryItem(guid, refId, count);
        return true;
    });

    mp.set_function("removePlacedObject", [context](uint32_t mpNum, const std::string& cellId) -> bool
    {
        if (!context || mpNum == 0 || cellId.empty())
            return false;

        context->queueRemovePlacedObject(mpNum, cellId);
        return true;
    });

    mp.set_function("spawnActor",
        [context](const std::string& refId, uint32_t refNum, uint32_t mpNum, const std::string& cellId,
            const sol::table& positionTable) -> bool
    {
        if (!context || refId.empty() || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        context->queueSpawnActor(refId, refNum, mpNum, cellId, *position);
        return true;
    });
    mp.set_function("SpawnActor",
        [context](const std::string& refId, uint32_t refNum, uint32_t mpNum, const std::string& cellId,
            const sol::table& positionTable) -> bool
    {
        if (!context || refId.empty() || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        context->queueSpawnActor(refId, refNum, mpNum, cellId, *position);
        return true;
    });

    mp.set_function("removeActor", [context](uint32_t mpNum, const std::string& cellId) -> bool
    {
        if (!context || mpNum == 0 || cellId.empty())
            return false;

        context->queueRemoveActor(mpNum, cellId);
        return true;
    });
    mp.set_function("RemoveActor", [context](uint32_t mpNum, const std::string& cellId) -> bool
    {
        if (!context || mpNum == 0 || cellId.empty())
            return false;

        context->queueRemoveActor(mpNum, cellId);
        return true;
    });

    auto resolveDynamicRecordOptions
        = [context](const std::string& normalizedType, const std::string& recordId, const sol::optional<sol::table>& options)
    {
        bool persistent = true;
        std::string scope;

        if (options)
        {
            if (auto maybePersistent = options->get<sol::optional<bool>>("persistent"))
                persistent = *maybePersistent;
            if (auto maybeScope = options->get<sol::optional<std::string>>("scope"))
                scope = normalizeDynamicRecordScope(*maybeScope);
        }

        if (scope.empty())
        {
            const std::string generatedPrefix = (context ? context->getGeneratedRecordIdPrefix() : std::string("$custom"))
                + "_" + normalizedType + "_";
            scope = recordId.rfind(generatedPrefix, 0) == 0 ? "generated" : "permanent";
        }

        return std::make_pair(scope, persistent);
    };

    auto queueDynamicRecordUpsert = [context, resolveDynamicRecordOptions](
                                        const std::string& recordType, const std::string& recordId,
                                        const sol::table& data, const sol::optional<sol::table>& options) -> bool
    {
        if (!context || recordId.empty())
            return false;

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return false;

        const auto [scope, persistent] = resolveDynamicRecordOptions(normalizedType, recordId, options);
        if (scope.empty())
            return false;

        context->queueUpsertDynamicRecord(normalizedType, recordId, LuaUtil::serialize(data), scope, persistent);
        return true;
    };

    mp.set_function("generateDynamicRecordId", [context](const std::string& recordType, sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return sol::make_object(ts, sol::nil);

        return sol::make_object(ts, context->generateDynamicRecordId(normalizedType));
    });
    mp.set_function("getGeneratedRecordIdPrefix", [context]() -> std::string
    {
        return context ? context->getGeneratedRecordIdPrefix() : std::string("$custom");
    });
    mp.set_function("GenerateDynamicRecordId", [context](const std::string& recordType, sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return sol::make_object(ts, sol::nil);

        return sol::make_object(ts, context->generateDynamicRecordId(normalizedType));
    });

    mp.set_function("upsertDynamicRecord",
        [queueDynamicRecordUpsert](const std::string& recordType, const std::string& recordId, const sol::table& data,
            const sol::optional<sol::table>& options) -> bool
    {
        return queueDynamicRecordUpsert(recordType, recordId, data, options);
    });
    mp.set_function("UpsertDynamicRecord",
        [queueDynamicRecordUpsert](const std::string& recordType, const std::string& recordId, const sol::table& data,
            const sol::optional<sol::table>& options) -> bool
    {
        return queueDynamicRecordUpsert(recordType, recordId, data, options);
    });

    mp.set_function("upsertGeneratedRecord", [context](const std::string& recordType, const sol::table& data,
                                                  const sol::optional<sol::table>& options, sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return sol::make_object(ts, sol::nil);

        const std::string recordId = context->generateDynamicRecordId(normalizedType);
        bool persistent = true;
        if (options)
        {
            if (auto maybePersistent = options->get<sol::optional<bool>>("persistent"))
                persistent = *maybePersistent;
        }

        context->queueUpsertDynamicRecord(normalizedType, recordId, LuaUtil::serialize(data), "generated", persistent);
        return sol::make_object(ts, recordId);
    });
    mp.set_function("UpsertGeneratedRecord", [context](const std::string& recordType, const sol::table& data,
                                                  const sol::optional<sol::table>& options, sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return sol::make_object(ts, sol::nil);

        const std::string recordId = context->generateDynamicRecordId(normalizedType);
        bool persistent = true;
        if (options)
        {
            if (auto maybePersistent = options->get<sol::optional<bool>>("persistent"))
                persistent = *maybePersistent;
        }

        context->queueUpsertDynamicRecord(normalizedType, recordId, LuaUtil::serialize(data), "generated", persistent);
        return sol::make_object(ts, recordId);
    });

    mp.set_function("removeDynamicRecord",
        [context](const std::string& recordType, const std::string& recordId) -> bool
    {
        if (!context || recordId.empty())
            return false;

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return false;

        context->queueRemoveDynamicRecord(normalizedType, recordId);
        return true;
    });
    mp.set_function("setDynamicRecordDependencies",
        [context](const std::string& recordType, const std::string& recordId, const sol::table& dependencyIds) -> bool
    {
        if (!context || recordId.empty())
            return false;

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return false;

        std::vector<std::string> values;
        for (const auto& entry : dependencyIds)
        {
            if (!entry.second.is<std::string>())
                continue;

            const std::string value = entry.second.as<std::string>();
            if (!value.empty())
                values.push_back(value);
        }

        context->queueSetDynamicRecordDependencies(normalizedType, recordId, std::move(values));
        return true;
    });
    mp.set_function("SetDynamicRecordDependencies",
        [context](const std::string& recordType, const std::string& recordId, const sol::table& dependencyIds) -> bool
    {
        if (!context || recordId.empty())
            return false;

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return false;

        std::vector<std::string> values;
        for (const auto& entry : dependencyIds)
        {
            if (!entry.second.is<std::string>())
                continue;

            const std::string value = entry.second.as<std::string>();
            if (!value.empty())
                values.push_back(value);
        }

        context->queueSetDynamicRecordDependencies(normalizedType, recordId, std::move(values));
        return true;
    });

    mp.set_function("listDynamicRecords", [context](sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        sol::state_view lua(ts);
        sol::table list(lua, sol::create);
        int index = 1;
        for (const auto& entry : context->getDynamicRecordCatalog())
            list[index++] = makeDynamicRecordInfoValue(ts, entry);
        return sol::make_object(ts, list);
    });
    mp.set_function("getDynamicRecordInfo", [context](const std::string& recordType, const std::string& recordId,
                                            sol::this_state ts) -> sol::object
    {
        if (!context || recordId.empty())
            return sol::make_object(ts, sol::nil);

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return sol::make_object(ts, sol::nil);

        auto entry = context->getDynamicRecordInfo(normalizedType, recordId);
        if (!entry)
            return sol::make_object(ts, sol::nil);

        return makeDynamicRecordInfoValue(ts, *entry);
    });
    mp.set_function("listDatabaseTables", [context](sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        sol::state_view lua(ts);
        sol::table list(lua, sol::create);
        int index = 1;
        for (const auto& entry : context->listDatabaseTables())
            list[index++] = makeDatabaseTableInfoValue(ts, entry);
        return sol::make_object(ts, LuaUtil::makeReadOnly(list));
    });
    mp.set_function("browseDatabaseTable",
        [context](const std::string& tableName, const sol::optional<int64_t>& offset,
            const sol::optional<int64_t>& limit, sol::this_state ts) -> sol::object
    {
        if (!context || tableName.empty())
            return sol::make_object(ts, sol::nil);

        auto page = context->browseDatabaseTable(tableName, offset.value_or(0), limit.value_or(100));
        if (!page)
            return sol::make_object(ts, sol::nil);

        return makeDatabaseBrowsePageValue(ts, *page);
    });
    mp.set_function("gcDynamicRecords",
        [context](const sol::optional<std::string>& recordType, const sol::object& persistentValue,
            sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);

        std::optional<std::string> normalizedType;
        if (recordType)
        {
            const std::string normalized = normalizeDynamicRecordType(*recordType);
            if (normalized.empty())
                return sol::make_object(ts, sol::nil);
            normalizedType = normalized;
        }

        std::optional<bool> persistent;
        if (persistentValue != sol::nil)
        {
            if (!persistentValue.is<bool>())
                return sol::make_object(ts, sol::nil);
            persistent = persistentValue.as<bool>();
        }

        sol::state_view lua(ts);
        sol::table list(lua, sol::create);
        int index = 1;
        for (const auto& entry : context->queueRemoveUnlinkedGeneratedDynamicRecords(normalizedType, persistent))
            list[index++] = makeDynamicRecordInfoValue(ts, entry);
        return sol::make_object(ts, list);
    });
    mp.set_function("RemoveDynamicRecord",
        [context](const std::string& recordType, const std::string& recordId) -> bool
    {
        if (!context || recordId.empty())
            return false;

        const std::string normalizedType = normalizeDynamicRecordType(recordType);
        if (normalizedType.empty())
            return false;

        context->queueRemoveDynamicRecord(normalizedType, recordId);
        return true;
    });

    mp.set_function("placeObject",
        [context](const std::string& refId, int count, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || refId.empty() || count <= 0 || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        context->queuePlaceObject(refId, count, cellId, *position);
        return true;
    });
    mp.set_function("PlaceObject",
        [context](const std::string& refId, int count, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || refId.empty() || count <= 0 || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        context->queuePlaceObject(refId, count, cellId, *position);
        return true;
    });

    mp.set_function("applyOps", [context](const sol::table& ops) -> std::tuple<bool, std::string>
    {
        if (!context)
            return { false, "context_missing" };

        std::string error;
        const bool ok = context->queueIntentOps(ops, &error);
        return { ok, error };
    });

    // mp.getPlayers() — returns a table of all ScriptPlayer usertypes.
    // Defined in PlayerBindings.cpp after the usertype is registered;
    // we leave a placeholder here that PlayerBindings will overwrite.
    // (PlayerBindings runs second in registerCoreBindings.)

    // ── Server info ──────────────────────────────────────────────────────
    // mp.getUptime() — seconds of real time since server start (float).
    mp.set_function("getUptime", [context]() -> double
    {
        return context ? context->getUptime() : 0.0;
    });

    // ── World time ───────────────────────────────────────────────────────
    // mp.getWorldTime() — current authoritative game hour (0..24, float).
    mp.set_function("getWorldTime", [context]() -> float
    {
        return context ? context->getWorldHour() : 0.f;
    });

    mp.set_function("getTime", [context]() -> float
    {
        return context ? context->getWorldHour() : 0.f;
    });

    // mp.setWorldTime(hour) — override the server clock.
    mp.set_function("setWorldTime", [context](float hour)
    {
        if (context) context->queueSetWorldHour(hour);
    });

    mp.set_function("setTime", [context](float hour)
    {
        if (context) context->queueSetWorldHour(hour);
    });

    // mp.relayChat(guid, text) — relay a player-authored chat message with
    // the server-authoritative display name for the sender.
    mp.set_function("relayChat", [context](uint32_t guid, const std::string& text)
    {
        if (context) context->queueRelayPlayerChat(guid, text);
    });

    mp.set_function("teleportPlayer", [context](uint32_t guid, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || guid == 0 || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        position->isTeleporting = true;
        context->queueTeleportPlayer(guid, cellId, *position);
        return true;
    });
    mp.set_function("TeleportPlayer", [context](uint32_t guid, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || guid == 0 || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        position->isTeleporting = true;
        context->queueTeleportPlayer(guid, cellId, *position);
        return true;
    });

    mp.set_function("getPlayerMarks", [context](uint32_t guid, sol::this_state ts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table marks(lua, sol::create);
        if (!context || guid == 0)
            return marks;

        int index = 1;
        for (const auto& mark : context->getPlayerMarks(guid))
            marks[index++] = makePlayerMarkValue(ts, mark);
        return marks;
    });
    mp.set_function("GetPlayerMarks", [context](uint32_t guid, sol::this_state ts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table marks(lua, sol::create);
        if (!context || guid == 0)
            return marks;

        int index = 1;
        for (const auto& mark : context->getPlayerMarks(guid))
            marks[index++] = makePlayerMarkValue(ts, mark);
        return marks;
    });

    mp.set_function("savePlayerMark",
        [context](uint32_t guid, const std::string& name, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || guid == 0 || name.empty() || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        PlayerMark mark;
        mark.name = name;
        mark.cell = cellId;
        mark.position = *position;
        context->upsertPlayerMark(guid, mark);
        context->queueUpsertPlayerMark(guid, mark);
        return true;
    });
    mp.set_function("SavePlayerMark",
        [context](uint32_t guid, const std::string& name, const std::string& cellId, const sol::table& positionTable) -> bool
    {
        if (!context || guid == 0 || name.empty() || cellId.empty())
            return false;

        auto position = parsePositionTable(positionTable);
        if (!position)
            return false;

        PlayerMark mark;
        mark.name = name;
        mark.cell = cellId;
        mark.position = *position;
        context->upsertPlayerMark(guid, mark);
        context->queueUpsertPlayerMark(guid, mark);
        return true;
    });

    mp.set_function("deletePlayerMark", [context](uint32_t guid, const std::string& name) -> bool
    {
        if (!context || guid == 0 || name.empty())
            return false;

        context->deletePlayerMark(guid, name);
        context->queueDeletePlayerMark(guid, name);
        return true;
    });
    mp.set_function("DeletePlayerMark", [context](uint32_t guid, const std::string& name) -> bool
    {
        if (!context || guid == 0 || name.empty())
            return false;

        context->deletePlayerMark(guid, name);
        context->queueDeletePlayerMark(guid, name);
        return true;
    });

    auto applyCellSurfSettings = [context](const std::string& cellId, const auto& updater) -> bool
    {
        if (!context || cellId.empty())
            return false;

        auto settings = context->getCellSurfPhysicsSettings(cellId);
        settings.cellId = cellId;
        updater(settings);
        context->setCellSurfPhysicsSettings(settings);
        return true;
    };

    auto applyGlobalSurfSettings = [context](const auto& updater) -> bool
    {
        if (!context)
            return false;

        auto settings = context->getGlobalSurfPhysicsSettings();
        updater(settings);
        context->setGlobalSurfPhysicsSettings(settings);
        return true;
    };

    auto mergeSurfPhysicsValues = [](SurfPhysicsSettings& settings, const sol::table& values)
    {
        settings.enabled = values.get_or("enabled", settings.enabled);
        settings.enabled = values.get_or("surfPhysicsEnabled", settings.enabled);
        settings.airAcceleration = values.get_or("airAccel", settings.airAcceleration);
        settings.maxAirSpeed = values.get_or("maxAirSpeed", settings.maxAirSpeed);
        settings.groundFriction = values.get_or("friction", settings.groundFriction);
        settings.groundAcceleration = values.get_or("groundAccel", settings.groundAcceleration);
        settings.jumpSpeed = values.get_or("jumpSpeed", settings.jumpSpeed);
        settings.gravityMultiplier = values.get_or("gravityMult", settings.gravityMultiplier);
        settings.gravityMultiplier = values.get_or("gravity", settings.gravityMultiplier);
        settings.overbounce = values.get_or("overbounce", settings.overbounce);
        settings.rampAngle = values.get_or("rampAngle", settings.rampAngle);
        settings.impactOverbounce = values.get_or("impactOverbounce", settings.impactOverbounce);
        settings.impactVelocityThreshold
            = values.get_or("impactVelocityThreshold", settings.impactVelocityThreshold);
    };

    mp.set_function("getGlobalPhysics", [context](sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getGlobalSurfPhysicsSettings());
    });
    mp.set_function("GetGlobalPhysics", [context](sol::this_state ts) -> sol::object
    {
        if (!context)
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getGlobalSurfPhysicsSettings());
    });

    mp.set_function("setGlobalPhysics", [applyGlobalSurfSettings, mergeSurfPhysicsValues](const sol::table& values) -> bool
    {
        return applyGlobalSurfSettings([&values, mergeSurfPhysicsValues](SurfPhysicsSettings& settings)
        {
            mergeSurfPhysicsValues(settings, values);
        });
    });
    mp.set_function("SetGlobalPhysics", [applyGlobalSurfSettings, mergeSurfPhysicsValues](const sol::table& values) -> bool
    {
        return applyGlobalSurfSettings([&values, mergeSurfPhysicsValues](SurfPhysicsSettings& settings)
        {
            mergeSurfPhysicsValues(settings, values);
        });
    });

    mp.set_function("getCellPhysics", [context](const std::string& cellId, sol::this_state ts) -> sol::object
    {
        if (!context || cellId.empty())
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getCellSurfPhysicsSettings(cellId));
    });
    mp.set_function("GetCellPhysics", [context](const std::string& cellId, sol::this_state ts) -> sol::object
    {
        if (!context || cellId.empty())
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getCellSurfPhysicsSettings(cellId));
    });

    mp.set_function("setCellPhysics",
        [applyCellSurfSettings, mergeSurfPhysicsValues](const std::string& cellId, const sol::table& values) -> bool
    {
        return applyCellSurfSettings(cellId, [&values, mergeSurfPhysicsValues](SurfPhysicsSettings& settings)
        {
            mergeSurfPhysicsValues(settings, values);
        });
    });
    mp.set_function("SetCellPhysics",
        [applyCellSurfSettings, mergeSurfPhysicsValues](const std::string& cellId, const sol::table& values) -> bool
    {
        return applyCellSurfSettings(cellId, [&values, mergeSurfPhysicsValues](SurfPhysicsSettings& settings)
        {
            mergeSurfPhysicsValues(settings, values);
        });
    });
    mp.set_function("clearCellPhysics", [context](const std::string& cellId) -> bool
    {
        if (!context || cellId.empty())
            return false;
        context->clearCellSurfPhysicsSettings(cellId);
        return true;
    });
    mp.set_function("ClearCellPhysics", [context](const std::string& cellId) -> bool
    {
        if (!context || cellId.empty())
            return false;
        context->clearCellSurfPhysicsSettings(cellId);
        return true;
    });

    mp.set_function("getPlayerPhysics", [context](uint32_t guid, sol::this_state ts) -> sol::object
    {
        if (!context || guid == 0)
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getPlayerSurfPhysicsSettings(guid));
    });
    mp.set_function("GetPlayerPhysics", [context](uint32_t guid, sol::this_state ts) -> sol::object
    {
        if (!context || guid == 0)
            return sol::make_object(ts, sol::nil);
        return makeSurfPhysicsValue(ts, context->getPlayerSurfPhysicsSettings(guid));
    });
    mp.set_function("setPlayerPhysics",
        [context, mergeSurfPhysicsValues](uint32_t guid, const sol::table& values) -> bool
    {
        if (!context || guid == 0)
            return false;

        auto settings = context->getPlayerSurfPhysicsSettings(guid);
        mergeSurfPhysicsValues(settings, values);
        context->setPlayerSurfPhysicsSettings(guid, settings);
        return true;
    });
    mp.set_function("SetPlayerPhysics",
        [context, mergeSurfPhysicsValues](uint32_t guid, const sol::table& values) -> bool
    {
        if (!context || guid == 0)
            return false;

        auto settings = context->getPlayerSurfPhysicsSettings(guid);
        mergeSurfPhysicsValues(settings, values);
        context->setPlayerSurfPhysicsSettings(guid, settings);
        return true;
    });
    mp.set_function("clearPlayerPhysics", [context](uint32_t guid) -> bool
    {
        if (!context || guid == 0)
            return false;
        context->clearPlayerSurfPhysicsSettings(guid);
        return true;
    });
    mp.set_function("ClearPlayerPhysics", [context](uint32_t guid) -> bool
    {
        if (!context || guid == 0)
            return false;
        context->clearPlayerSurfPhysicsSettings(guid);
        return true;
    });

    auto sendSettings = [context](const std::string& cellId)
    {
        if (context) context->queueRefreshCellGameSettings(cellId);
    };
    mp.set_function("sendCellPhysics", sendSettings);
    mp.set_function("sendSettings", sendSettings);
    mp.set_function("SendSettings", sendSettings);
    mp.set_function("sendPlayerSettings", [context](uint32_t guid)
    {
        if (context) context->queueRefreshPlayerGameSettings(guid);
    });
    mp.set_function("SendPlayerSettings", [context](uint32_t guid)
    {
        if (context) context->queueRefreshPlayerGameSettings(guid);
    });

    auto getAirAccel = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).airAcceleration : SurfPhysicsSettings{}.airAcceleration;
    };
    auto setAirAccel = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.airAcceleration = static_cast<float>(value);
        });
    };
    mp.set_function("getAirAccel", getAirAccel);
    mp.set_function("GetAirAccel", getAirAccel);
    mp.set_function("setAirAccel", setAirAccel);
    mp.set_function("SetAirAccel", setAirAccel);

    auto getMaxAirSpeed = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).maxAirSpeed : SurfPhysicsSettings{}.maxAirSpeed;
    };
    auto setMaxAirSpeed = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.maxAirSpeed = static_cast<float>(value);
        });
    };
    mp.set_function("getMaxAirSpeed", getMaxAirSpeed);
    mp.set_function("GetMaxAirSpeed", getMaxAirSpeed);
    mp.set_function("setMaxAirSpeed", setMaxAirSpeed);
    mp.set_function("SetMaxAirSpeed", setMaxAirSpeed);

    auto getFriction = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).groundFriction : SurfPhysicsSettings{}.groundFriction;
    };
    auto setFriction = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.groundFriction = static_cast<float>(value);
        });
    };
    mp.set_function("getFriction", getFriction);
    mp.set_function("GetFriction", getFriction);
    mp.set_function("setFriction", setFriction);
    mp.set_function("SetFriction", setFriction);

    auto getGroundAccel = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).groundAcceleration
                       : SurfPhysicsSettings{}.groundAcceleration;
    };
    auto setGroundAccel = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.groundAcceleration = static_cast<float>(value);
        });
    };
    mp.set_function("getGroundAccel", getGroundAccel);
    mp.set_function("GetGroundAccel", getGroundAccel);
    mp.set_function("setGroundAccel", setGroundAccel);
    mp.set_function("SetGroundAccel", setGroundAccel);

    auto getJumpSpeed = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).jumpSpeed : SurfPhysicsSettings{}.jumpSpeed;
    };
    auto setJumpSpeed = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.jumpSpeed = static_cast<float>(value);
        });
    };
    mp.set_function("getJumpSpeed", getJumpSpeed);
    mp.set_function("GetJumpSpeed", getJumpSpeed);
    mp.set_function("setJumpSpeed", setJumpSpeed);
    mp.set_function("SetJumpSpeed", setJumpSpeed);

    auto getGravityMult = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).gravityMultiplier
                       : SurfPhysicsSettings{}.gravityMultiplier;
    };
    auto setGravityMult = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.gravityMultiplier = static_cast<float>(value);
        });
    };
    mp.set_function("getGravityMult", getGravityMult);
    mp.set_function("GetGravityMult", getGravityMult);
    mp.set_function("getGravity", getGravityMult);
    mp.set_function("GetGravity", getGravityMult);
    mp.set_function("setGravityMult", setGravityMult);
    mp.set_function("SetGravityMult", setGravityMult);
    mp.set_function("setGravity", setGravityMult);
    mp.set_function("SetGravity", setGravityMult);

    auto getOverbounce = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).overbounce : SurfPhysicsSettings{}.overbounce;
    };
    auto setOverbounce = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.overbounce = static_cast<float>(value);
        });
    };
    mp.set_function("getOverbounce", getOverbounce);
    mp.set_function("GetOverbounce", getOverbounce);
    mp.set_function("setOverbounce", setOverbounce);
    mp.set_function("SetOverbounce", setOverbounce);

    auto getRampAngle = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).rampAngle : SurfPhysicsSettings{}.rampAngle;
    };
    auto setRampAngle = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.rampAngle = static_cast<float>(value);
        });
    };
    mp.set_function("getRampAngle", getRampAngle);
    mp.set_function("GetRampAngle", getRampAngle);
    mp.set_function("setRampAngle", setRampAngle);
    mp.set_function("SetRampAngle", setRampAngle);

    auto getImpactOverbounce = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).impactOverbounce
                       : SurfPhysicsSettings{}.impactOverbounce;
    };
    auto setImpactOverbounce = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.impactOverbounce = static_cast<float>(value);
        });
    };
    mp.set_function("getImpactOverbounce", getImpactOverbounce);
    mp.set_function("GetImpactOverbounce", getImpactOverbounce);
    mp.set_function("setImpactOverbounce", setImpactOverbounce);
    mp.set_function("SetImpactOverbounce", setImpactOverbounce);

    auto getImpactVelocityThreshold = [context](const std::string& cellId) -> float
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).impactVelocityThreshold
                       : SurfPhysicsSettings{}.impactVelocityThreshold;
    };
    auto setImpactVelocityThreshold = [applyCellSurfSettings](const std::string& cellId, double value) -> bool
    {
        return applyCellSurfSettings(cellId, [value](SurfPhysicsSettings& settings)
        {
            settings.impactVelocityThreshold = static_cast<float>(value);
        });
    };
    mp.set_function("getImpactVelocityThreshold", getImpactVelocityThreshold);
    mp.set_function("GetImpactVelocityThreshold", getImpactVelocityThreshold);
    mp.set_function("setImpactVelocityThreshold", setImpactVelocityThreshold);
    mp.set_function("SetImpactVelocityThreshold", setImpactVelocityThreshold);

    auto getSurfPhysicsEnabled = [context](const std::string& cellId) -> bool
    {
        return context ? context->getCellSurfPhysicsSettings(cellId).enabled : false;
    };
    auto setSurfPhysicsEnabled = [applyCellSurfSettings](const std::string& cellId, bool enabled) -> bool
    {
        return applyCellSurfSettings(cellId, [enabled](SurfPhysicsSettings& settings)
        {
            settings.enabled = enabled;
        });
    };
    mp.set_function("getSurfPhysicsEnabled", getSurfPhysicsEnabled);
    mp.set_function("GetSurfPhysicsEnabled", getSurfPhysicsEnabled);
    mp.set_function("setSurfPhysicsEnabled", setSurfPhysicsEnabled);
    mp.set_function("SetSurfPhysicsEnabled", setSurfPhysicsEnabled);

    if (storage)
        mp["storage"] = LuaUtil::LuaStorage::initGlobalPackage(view, storage);

    return mp;
}

} // namespace mwmp
