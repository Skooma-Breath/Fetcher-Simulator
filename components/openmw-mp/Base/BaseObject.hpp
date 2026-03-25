#ifndef OPENMW_MP_BASEOBJECT_HPP
#define OPENMW_MP_BASEOBJECT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "BaseStructs.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PlacedObject — a world object that has been placed by a player.
    // Persists in the server's WorldState map (keyed by cellId).
    // -----------------------------------------------------------------------
    struct PlacedObject
    {
        uint32_t    mpNum   = 0;       // server-assigned unique ID for this instance
        std::string refId;             // ESM record ID (e.g. "iron_sword_001")
        int         count   = 1;       // stack count (for items)
        Position    position;          // world position + rotation
        std::string cellId;            // cell it was placed into
    };

    // -----------------------------------------------------------------------
    // ContainerItem — single slot in a container's inventory record.
    // -----------------------------------------------------------------------
    struct ContainerItem
    {
        std::string refId;
        int         count  = 0;
        int         charge = -1;       // -1 = not a weapon/armor
    };

    // -----------------------------------------------------------------------
    // ContainerRecord — server-authoritative inventory of one container.
    // Keyed in WorldState by "<cellId>|<refId>|<refNum>".
    // -----------------------------------------------------------------------
    struct ContainerRecord
    {
        std::string cellId;
        std::string refId;
        uint32_t    refNum = 0;
        uint32_t    mpNum  = 0;        // may be 0 for static world containers
        std::vector<ContainerItem> items;
        bool        hasAuthority = false;  // true once server has stored contents
    };

    // -----------------------------------------------------------------------
    // Action enum shared by Container packet
    // -----------------------------------------------------------------------
    enum class ContainerAction : uint8_t
    {
        Set    = 0,  // Replace entire contents
        Add    = 1,  // Add items to contents
        Remove = 2,  // Remove items from contents
    };

} // namespace mwmp

#endif // OPENMW_MP_BASEOBJECT_HPP
