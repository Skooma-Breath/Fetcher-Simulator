#include "TypesStubBindings.hpp"

#include <algorithm>

#include <components/lua/luastate.hpp>
#include <sol/sol.hpp>

#include "PlayerBindings.hpp"

namespace mwmp
{
namespace
{
    sol::table makeDynamicStatTable(sol::state_view lua, const DynamicStat& stat)
    {
        sol::table table(lua, sol::create);
        table["base"] = stat.base;
        table["current"] = stat.current;
        table["modifier"] = stat.mod;
        table["modified"] = stat.base + stat.mod;
        return LuaUtil::makeReadOnly(table);
    }

    sol::table makeSkillTable(sol::state_view lua, const Skill& skill)
    {
        sol::table table(lua, sol::create);
        table["base"] = skill.base;
        table["modifier"] = skill.mod;
        table["damage"] = skill.damage;
        table["modified"] = std::max(0.f, skill.base - skill.damage + skill.mod);
        table["progress"] = skill.progress;
        table["increases"] = skill.increases;
        return LuaUtil::makeReadOnly(table);
    }

    sol::table makeStatsTable(sol::this_state thisState, const ScriptPlayer& player)
    {
        sol::state_view lua(thisState);
        sol::table dynamic(lua, sol::create);
        dynamic["health"] = makeDynamicStatTable(lua, player.data.dynamicStats.health);
        dynamic["magicka"] = makeDynamicStatTable(lua, player.data.dynamicStats.magicka);
        dynamic["fatigue"] = makeDynamicStatTable(lua, player.data.dynamicStats.fatigue);

        sol::table skills(lua, sol::create);
        for (std::size_t i = 0; i < player.data.skills.size(); ++i)
        {
            const sol::table skill = makeSkillTable(lua, player.data.skills[i]);
            skills[static_cast<int>(i + 1)] = skill;
        }

        sol::table stats(lua, sol::create);
        stats["dynamic"] = LuaUtil::makeReadOnly(dynamic);
        stats["skills"] = LuaUtil::makeReadOnly(skills);

        // Convenience aliases for early server scripts; stats.dynamic mirrors
        // the shape used by OpenMW's client-side stats package.
        stats["health"] = dynamic["health"];
        stats["magicka"] = dynamic["magicka"];
        stats["fatigue"] = dynamic["fatigue"];
        return LuaUtil::makeReadOnly(stats);
    }

    sol::table makeInventoryTable(sol::this_state thisState, const ScriptPlayer& player)
    {
        sol::state_view lua(thisState);
        sol::table inventory(lua, sol::create);

        int index = 1;
        for (const Item& item : player.data.inventory)
        {
            sol::table entry(lua, sol::create);
            entry["id"] = item.refId;
            entry["refId"] = item.refId;
            entry["count"] = item.count;
            entry["charge"] = item.charge;
            entry["enchantmentCharge"] = item.enchantmentCharge;
            entry["soul"] = item.soul;
            inventory[index++] = LuaUtil::makeReadOnly(entry);
        }

        return LuaUtil::makeReadOnly(inventory);
    }
}

sol::table initTypesStubPackage(LuaUtil::LuaView& view)
{
    sol::state_view& lua = view.sol();

    sol::table actor(lua, sol::create);
    actor["objectIsInstance"] = [](const ScriptPlayer&) { return true; };
    actor["stats"] = &makeStatsTable;
    actor["inventory"] = &makeInventoryTable;

    sol::table player(lua, sol::create);
    player["baseType"] = LuaUtil::makeReadOnly(actor);
    player["objectIsInstance"] = [](const ScriptPlayer&) { return true; };
    player["stats"] = &makeStatsTable;
    player["inventory"] = &makeInventoryTable;

    sol::table types(lua, sol::create);
    types["Actor"] = player["baseType"];
    types["Player"] = LuaUtil::makeReadOnly(player);

    return types;
}

} // namespace mwmp
