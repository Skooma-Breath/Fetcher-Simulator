#ifndef OPENMW_MP_NETWORKMESSAGES_HPP
#define OPENMW_MP_NETWORKMESSAGES_HPP

#include <cstdint>

namespace mwmp
{
    // ---------------------------------------------------------------------------
    // Packet type identifiers (uint16_t).
    // These replace TES3MP's RakNet MessageID enum.
    // Categories mirror TES3MP's ordering channel design:
    //   0-9   System
    //   10-49 Player
    //   50-89 Actor
    //   90-129 Object
    //   130-159 Worldstate
    // ---------------------------------------------------------------------------
    enum class PacketType : uint16_t
    {
        // --- System ---
        Handshake           = 0,
        HandshakeResponse   = 1,
        Disconnect          = 2,
        Loaded              = 3,   // Client finished loading world, ready to play
        GameSettings        = 4,   // Server pushes server-side settings to client
        GUIMessageBox       = 5,   // Server requests a GUI messagebox on the client

        // --- Player ---
        PlayerBaseInfo      = 10,
        PlayerCharGen       = 11,
        PlayerPosition      = 12,
        PlayerMomentum      = 13,
        PlayerCellChange    = 14,
        PlayerCellState     = 15,  // Batch notify of loaded/unloaded cells
        PlayerAnimFlags     = 16,
        PlayerAnimPlay      = 17,
        PlayerAttack        = 18,
        PlayerCast          = 19,
        PlayerAttribute     = 20,
        PlayerSkill         = 21,
        PlayerLevel         = 22,
        PlayerStatsDynamic  = 23,
        PlayerDeath         = 24,
        PlayerResurrect     = 25,
        PlayerEquipment     = 26,
        PlayerInventory     = 27,
        PlayerItemUse       = 28,
        PlayerSpellbook     = 29,
        PlayerSpellsActive  = 30,
        PlayerCooldowns     = 31,
        PlayerQuickKeys     = 32,
        PlayerJournal       = 33,
        PlayerFaction       = 34,
        PlayerTopic         = 35,
        PlayerBook          = 36,
        PlayerBounty        = 37,
        PlayerReputation    = 38,
        PlayerShapeshift    = 39,  // Werewolf / custom race transforms
        PlayerBehavior      = 40,
        PlayerSpeech        = 41,
        PlayerRest          = 42,
        PlayerJail          = 43,
        PlayerAlly          = 44,
        PlayerInput         = 45,
        PlayerMiscellaneous = 46,
        ChatMessage         = 47,

        // --- Actor ---
        ActorList           = 50,
        ActorAuthority      = 51,
        ActorPosition       = 52,
        ActorAnimFlags      = 53,
        ActorAnimPlay       = 54,
        ActorAttack         = 55,
        ActorCast           = 56,
        ActorCellChange     = 57,
        ActorDeath          = 58,
        ActorEquipment      = 59,
        ActorAI             = 60,
        ActorSpeech         = 61,
        ActorSpellsActive   = 62,
        ActorStatsDynamic   = 63,
        ActorTest           = 64,

        // --- Object ---
        ObjectActivate      = 90,
        ObjectAnimPlay      = 91,
        ObjectAttach        = 92,
        ObjectDelete        = 93,
        ObjectDialogueChoice= 94,
        ObjectHit           = 95,
        ObjectLock          = 96,
        ObjectMiscellaneous = 97,
        ObjectMove          = 98,
        ObjectPlace         = 99,
        ObjectRestock       = 100,
        ObjectRotate        = 101,
        ObjectScale         = 102,
        ObjectSound         = 103,
        ObjectSpawn         = 104,
        ObjectState         = 105,
        ObjectTrap          = 106,
        Container           = 107,
        DoorState           = 108,
        DoorDestination     = 109,
        ConsoleCommand      = 110,
        MusicPlay           = 111,
        VideoPlay           = 112,
        ClientScriptLocal   = 113,
        ClientScriptGlobal  = 114,
        ScriptMemberShort   = 115,

        // --- Worldstate ---
        WorldTime           = 130,
        WorldWeather        = 131,
        WorldKillCount      = 132,
        WorldMap            = 133,
        WorldRegionAuthority= 134,
        WorldCollisionOverride = 135,
        WorldDestinationOverride = 136,
        ClientScriptSettings= 137,
        CellReset           = 138,
        RecordDynamic       = 139,

        COUNT               = 140
    };

} // namespace mwmp

#endif // OPENMW_MP_NETWORKMESSAGES_HPP
