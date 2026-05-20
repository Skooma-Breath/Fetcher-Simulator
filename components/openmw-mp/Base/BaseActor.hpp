#ifndef OPENMW_MP_BASEACTOR_HPP
#define OPENMW_MP_BASEACTOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "BaseStructs.hpp"

namespace mwmp
{
    struct BaseActor
    {
        // ------------------------------------------------------------------
        // Identity
        // ------------------------------------------------------------------
        std::string refId;          // ESM record ID (e.g. "scamp")
        uint32_t    refNum  = 0;    // ESP/ESM ref number
        uint32_t    mpNum   = 0;    // server-assigned multiplayer number
        std::string cellId;         // stringified cell name for easy lookup

        // ------------------------------------------------------------------
        // World state
        // ------------------------------------------------------------------
        Position    position;
        Velocity    velocity;
        bool        isMoving = false;
        bool        hasWeaponDrawn = false;
        bool        hasSpellReadied = false;
        bool        isAttackingOrCasting = false;

        // ------------------------------------------------------------------
        // Combat
        // ------------------------------------------------------------------
        DynamicStats dynamicStats;
        AnimFlags    animFlags;
        AnimPlay     animPlay;
        Attack       attack;
        CastSpell    cast;
        uint8_t      deathState = 0;
        uint32_t     deathEventId = 0;
        bool         isDead = false;
        bool         isInstantDeath = false;
        std::string  deathAnimGroup;         // e.g. "death1"/"death2"/"death_knock_down" synced from authority

        // ------------------------------------------------------------------
        // AI
        // ------------------------------------------------------------------
        struct AIAction
        {
            enum class Type { None=0, Wander, Travel, Follow, Escort, Combat, Pursue };
            Type        type       = Type::None;
            std::string targetId;
            uint32_t    targetMpNum = 0;
            float       duration    = 0.f;
            bool        reset       = false;
        };
        AIAction ai;

        // ------------------------------------------------------------------
        // Equipment (actors can wear items too)
        // ------------------------------------------------------------------
        static constexpr int NUM_EQUIPMENT_SLOTS = 19;
        std::vector<EquipmentItem> equipment;

        bool isFollowerCellChange = false;
    };

    // A batch of actors belonging to a single cell
    struct ActorList
    {
        std::string         cellId;
        std::vector<BaseActor> actors;
        bool                isAuthority = false;  // true = sender is authority for this cell
        uint32_t            authorityGuid = 0;
        uint32_t            victimPlayerGuid = 0;   // non-zero = this request targets a player (NPC->player damage)
        uint32_t            authorityGeneration = 0;
        uint32_t            snapshotSequence = 0;
        uint64_t            serverTimestamp = 0;
    };

} // namespace mwmp

#endif // OPENMW_MP_BASEACTOR_HPP
