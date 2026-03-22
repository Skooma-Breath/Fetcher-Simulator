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

        // ------------------------------------------------------------------
        // Combat
        // ------------------------------------------------------------------
        DynamicStats dynamicStats;
        AnimFlags    animFlags;
        Attack       attack;

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
    };

} // namespace mwmp

#endif // OPENMW_MP_BASEACTOR_HPP
