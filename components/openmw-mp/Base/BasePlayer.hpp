#ifndef OPENMW_MP_BASEPLAYER_HPP
#define OPENMW_MP_BASEPLAYER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <array>

#include <components/esm/attr.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/npcstats.hpp>

#include "BaseStructs.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // All state that can be synchronised for a player.
    // Both LocalPlayer (client) and ConnectedClient (server) embed this.
    // -----------------------------------------------------------------------
    struct BasePlayer
    {
        // ------------------------------------------------------------------
        // Identity
        // ------------------------------------------------------------------
        uint32_t    guid        = 0;      // server-assigned unique ID
        std::string name;                 // display name / username
        std::string password;             // hashed; only used during handshake

        // ------------------------------------------------------------------
        // World location
        // ------------------------------------------------------------------
        Position    position;
        Velocity    velocity;
        CellId      cell;

        // ------------------------------------------------------------------
        // Character definition
        // ------------------------------------------------------------------
        std::string race;
        std::string headMesh;
        std::string hairMesh;
        bool        isMale     = true;
        bool        isVampire  = false;
        bool        isWerewolf = false;
        float       scale      = 1.f;

        ESM::Class  charClass;
        std::string birthSign;

        // 8 attributes
        static constexpr int NUM_ATTRIBUTES = 8;
        std::array<Attribute, NUM_ATTRIBUTES> attributes;

        // 27 skills
        static constexpr int NUM_SKILLS = 27;
        std::array<Skill, NUM_SKILLS> skills;

        // Dynamic stats
        DynamicStats dynamicStats;

        int   level       = 1;
        float levelProgress = 0.f;
        bool  hasSavedStats = false;

        // ------------------------------------------------------------------
        // Combat / animation
        // ------------------------------------------------------------------
        AnimFlags   animFlags;
        AnimPlay    animPlay;     // most recent one-shot anim (reliable)
        Attack      attack;
        CastSpell   castSpell;

        // ------------------------------------------------------------------
        // Equipment (19 slots matching OpenMW InventoryStore slots)
        // ------------------------------------------------------------------
        static constexpr int NUM_EQUIPMENT_SLOTS = 19;
        std::array<EquipmentItem, NUM_EQUIPMENT_SLOTS> equipment;

        // ------------------------------------------------------------------
        // Inventory sync payload.
        //
        // Clients currently send Set snapshots, but the wire format also allows
        // Add/Remove deltas. The dedicated server folds those updates back into
        // this same vector so it can keep an authoritative full inventory
        // mirror for persistence and Lua snapshot reads without maintaining a
        // second copy.
        // ------------------------------------------------------------------
        struct InventoryChanges
        {
            std::vector<Item> items;
            enum class Action { Set = 0, Add, Remove };
            Action action = Action::Set;
        } inventoryChanges;

        // ------------------------------------------------------------------
        // Spellbook
        // ------------------------------------------------------------------
        struct SpellbookChanges
        {
            std::vector<ESM::Spell> spells;
            enum class Action { Set = 0, Add, Remove };
            Action action = Action::Set;
        } spellbookChanges;

        // ------------------------------------------------------------------
        // Active spells
        // ------------------------------------------------------------------
        std::vector<std::string> activeSpellIds;

        // ------------------------------------------------------------------
        // RPG state
        // ------------------------------------------------------------------
        int   bounty         = 0;
        int   reputation     = 0;
        float charGenStage   = 0.f;
        bool  charGenComplete = false;

        // ------------------------------------------------------------------
        // Cell state (which cells this player currently has loaded)
        // ------------------------------------------------------------------
        struct CellState
        {
            CellId cell;
            enum class Type { Load = 0, Unload };
            Type type = Type::Load;
        };
        std::vector<CellState> cellStates;

        // ------------------------------------------------------------------
        // Journal / topic / book tracking (delta lists)
        // ------------------------------------------------------------------
        struct JournalItem
        {
            std::string quest;
            int         index = 0;
            std::string actorRefId;
            bool        hasTimestamp = false;
            Time        timestamp;
            enum class Type { Entry = 0, Index };
            Type type = Type::Entry;
        };
        std::vector<JournalItem> journalChanges;
        std::vector<std::string> topicChanges;
        std::vector<std::string> bookChanges;

        // ------------------------------------------------------------------
        // Quick keys (1-based slots 1..10)
        // ------------------------------------------------------------------
        struct QuickKey
        {
            uint16_t slot = 0;
            std::string itemId;
            enum class Type { Item = 0, Magic, ItemMagic, Unassigned };
            Type type = Type::Unassigned;
        };
        std::vector<QuickKey> quickKeys;

        // ------------------------------------------------------------------
        // Misc flags
        // ------------------------------------------------------------------
        bool isConnected    = false;
        bool isLoggedIn     = false;
        bool isDead         = false;
        bool isMoving       = false;
        int  deathCount     = 0;
        std::string deathAnimationGroup;
    };

} // namespace mwmp

#endif // OPENMW_MP_BASEPLAYER_HPP
