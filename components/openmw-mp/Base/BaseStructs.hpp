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
    // Animation flags — movement / action state (unreliable, per-frame)
    // -----------------------------------------------------------------------
    struct AnimFlags
    {
        uint32_t movementFlags = 0;  // bitmask: Walk|Run|Sneak|Swim|Jump|Fly|Turn etc.
        uint32_t actionFlags   = 0;  // bitmask: Attack|Cast|Block|WeaponDrawn|SpellReady
        // Body-relative movement axes — projected from world velocity onto body yaw.
        // Range [-1,1]. Replaces the old movementType integer so the receiver sets
        // Movement::mPosition directly, letting the remote CC classify animation
        // groups itself — identical to how a local 1st-person player is handled.
        float    animFwd  = 0.f;  // mPosition[1]: +1=forward, -1=backward, 0=idle
        float    animSide = 0.f;  // mPosition[0]: +1=strafe-right, -1=strafe-left
        float    blockedMoveSpeed = 0.f; // sender-side expected pace for wall-blocked movement

        // movementFlags bit constants — must match the encode side in PlayerSync
        static constexpr uint32_t MF_RUN   = (1u << 0);
        static constexpr uint32_t MF_SNEAK = (1u << 1);
        static constexpr uint32_t MF_SWIM  = (1u << 2);
        static constexpr uint32_t MF_JUMP  = (1u << 3);
        static constexpr uint32_t MF_FLY        = (1u << 4);
        static constexpr uint32_t MF_STRAFING   = (1u << 5);
        // Sustained hit-state flags — mirrored from CharacterController each frame
        // so the receiver's CC can drive the correct knockdown/knockout anim group
        // and looping behaviour without needing a separate reliable packet.
        static constexpr uint32_t MF_KNOCKED_DOWN = (1u << 6);
        static constexpr uint32_t MF_KNOCKED_OUT  = (1u << 7);
        // Sender confirmed that movement is key-held but physically wall-blocked.
        // Receiver uses this to swap in stance-speed cadence only for true
        // blocked-contact frames, avoiding global sneak/walk footstep desync.
        static constexpr uint32_t MF_WALL_BLOCKED = (1u << 8);

        // actionFlags bit constants
        static constexpr uint32_t AF_WEAPON_DRAWN = (1u << 0);
        static constexpr uint32_t AF_SPELL_READY  = (1u << 1);
        static constexpr uint32_t AF_ATTACKING    = (1u << 2);
        static constexpr uint32_t AF_CASTING      = (1u << 3);
        static constexpr uint32_t AF_BLOCKING     = (1u << 4);
    };

    // -----------------------------------------------------------------------
    // One-shot animation play (reliable)
    // Triggers explicit animations on the remote NPC (death, hit, emote, cast).
    // -----------------------------------------------------------------------
    struct AnimPlay
    {
        std::string groupName;
        int         priority  = 0;
        int         loops     = 0;
        std::string startKey  = "start";
        std::string stopKey   = "stop";
    };

    // -----------------------------------------------------------------------
    // Melee / ranged / thrown attack (reliable)
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

    // -----------------------------------------------------------------------
    // Spell cast (reliable)
    // -----------------------------------------------------------------------
    struct CastSpell
    {
        std::string spellId;
        uint32_t    targetGuid  = 0;    // mp guid of target player (0 = world object)
        std::string targetRefId;        // refId of world object target
        bool        success     = false;
    };

} // namespace mwmp

#endif // OPENMW_MP_BASESTRUCTS_HPP
