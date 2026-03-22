#ifndef OPENMW_MP_BASESTRUCTS_HPP
#define OPENMW_MP_BASESTRUCTS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // Position / rotation (matches OpenMW ESM::Position layout)
    // -----------------------------------------------------------------------
    struct Position
    {
        float pos[3] = { 0.f, 0.f, 0.f };  // x, y, z
        float rot[3] = { 0.f, 0.f, 0.f };  // rx, ry, rz (radians)
    };

    struct Velocity
    {
        float linear[3]  = { 0.f, 0.f, 0.f };
        float angular[3] = { 0.f, 0.f, 0.f };
    };

    // -----------------------------------------------------------------------
    // Cell identifier (mirrors ESM::CellId)
    // -----------------------------------------------------------------------
    struct CellId
    {
        std::string worldspace;      // exterior worldspace name (empty = default)
        bool        isExterior = false;
        int         gridX = 0;
        int         gridY = 0;
        std::string cellName;        // set for interiors; empty for exteriors
    };

    // -----------------------------------------------------------------------
    // Item / inventory entry
    // -----------------------------------------------------------------------
    struct Item
    {
        std::string refId;
        int         count     = 0;
        int         charge    = -1;    // -1 = not applicable
        float       enchantmentCharge = -1.f;
        std::string soul;              // soul gem content
    };

    // -----------------------------------------------------------------------
    // Equipment slot entry
    // -----------------------------------------------------------------------
    struct EquipmentItem
    {
        int  slot  = -1;
        Item item;
    };

    // -----------------------------------------------------------------------
    // Dynamic stats (Health / Magicka / Fatigue — current + base)
    // -----------------------------------------------------------------------
    struct DynamicStat
    {
        float base    = 0.f;
        float current = 0.f;
        float mod     = 0.f;
    };

    struct DynamicStats
    {
        DynamicStat health;
        DynamicStat magicka;
        DynamicStat fatigue;
    };

    // -----------------------------------------------------------------------
    // Attribute
    // -----------------------------------------------------------------------
    struct Attribute
    {
        int   base    = 0;
        float mod     = 0.f;
        float damage  = 0.f;
    };

    // -----------------------------------------------------------------------
    // Skill
    // -----------------------------------------------------------------------
    struct Skill
    {
        float base        = 0.f;
        float mod         = 0.f;
        float damage      = 0.f;
        float progress    = 0.f;
        int   increases   = 0;
    };

    // -----------------------------------------------------------------------
    // Time (in-game calendar)
    // -----------------------------------------------------------------------
    struct Time
    {
        int   day        = 1;
        int   month      = 0;
        int   year       = 427;
        float hour       = 8.f;
        int   dayOfWeek  = 0;
        float gameHour   = 8.f;    // 0..24
    };

    // -----------------------------------------------------------------------
    // Animation flags (matches TES3MP BaseStructs)
    // -----------------------------------------------------------------------
    struct AnimFlags
    {
        uint32_t movementFlags = 0;
        uint32_t actionFlags   = 0;
        int8_t   movementType  = 0;
    };

    // -----------------------------------------------------------------------
    // Attack
    // -----------------------------------------------------------------------
    struct Attack
    {
        std::string target;         // refId of target (empty = none)
        uint32_t    targetMpNum = 0;
        bool        hit     = false;
        bool        block   = false;
        bool        miss    = false;
        bool        pressed = false;
        bool        knocked = false;
        float       strength = 0.f;
        int         type    = 0;    // 0=melee,1=magic,2=bow,3=throw
    };

} // namespace mwmp

#endif // OPENMW_MP_BASESTRUCTS_HPP
