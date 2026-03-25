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
    //   150-199 Lua bridge (reserved)
    //   200+  Auth extensions
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
        CharacterList       = 6,   // Server → client: list of characters for this account
        CharacterSelect     = 7,   // Client → server: which character to play (or "" = new)
        CharacterData       = 8,   // Server → client: full chargen/position for selected char
        CharacterSelectError= 9,   // Server → client: character select rejected (reason string)
        Challenge           = 200, // Server → client: 32-byte nonce for Ed25519 challenge-response
        ChallengeResponse   = 201, // Client → server: 64-byte Ed25519 signature of challenge nonce
        LinkKeyRequest      = 202, // Client → server: register a public key to this account
        UnlinkKeyRequest    = 203, // Client → server: remove a registered public key
        DeleteCharRequest   = 204, // Client → server: delete a character slot
        DeleteCharResponse  = 205, // Server → client: deletion result (ok / error)

        // --- Player ---
        PlayerBaseInfo      = 10,
        PlayerCharGen       = 11,
        PlayerPosition      = 12,
        PlayerMomentum      = 13,
        PlayerCellChange    = 14,
        PlayerCellState     = 15,  // Batch notify of loaded/unloaded cells
        PlayerAnimFlags     = 16,  // Unreliable, per-frame movement/action state
        PlayerAnimPlay      = 17,  // Reliable one-shot animation trigger
        PlayerAttack        = 18,  // Reliable attack event
        PlayerCast          = 19,  // Reliable spell cast event
        PlayerAttribute     = 20,
        PlayerSkill         = 21,
        PlayerLevel         = 22,
        PlayerStatsDynamic  = 23,
        PlayerDeath         = 24,
        PlayerResurrect     = 25,
        PlayerEquipment     = 26,
        PlayerInventory     = 27,  // Inventory delta (Add/Remove/Set)
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
        ObjectDelete        = 93,  // Remove a placed object by mpNum
        ObjectDialogueChoice= 94,
        ObjectHit           = 95,
        ObjectLock          = 96,
        ObjectMiscellaneous = 97,
        ObjectMove          = 98,  // Move/rotate a placed object
        ObjectPlace         = 99,  // Place a new world object (server assigns mpNum)
        ObjectRestock       = 100,
        ObjectRotate        = 101,
        ObjectScale         = 102,
        ObjectSound         = 103,
        ObjectSpawn         = 104,
        ObjectState         = 105,
        ObjectTrap          = 106,
        Container           = 107, // Container inventory sync (Set/Add/Remove)
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

        // --- Lua bridge (7C+) ---
        // IDs 150-199 reserved; exact assignments TBD in Phase 7C/7D.
        PacketLuaEvent      = 150, // Bidirectional: named event with BinaryData payload
        PacketLuaStorage    = 151, // Server→client global storage snapshot/delta
    };

} // namespace mwmp

#endif // OPENMW_MP_NETWORKMESSAGES_HPP
