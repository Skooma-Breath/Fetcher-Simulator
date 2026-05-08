#ifndef OPENMW_MP_ACTORSYNCPROTOCOL_HPP
#define OPENMW_MP_ACTORSYNCPROTOCOL_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <components/openmw-mp/Base/BaseActor.hpp>

namespace mwmp
{
    static constexpr uint32_t ActorSyncProtocolVersionV1 = 1;
    static constexpr uint32_t ActorSyncProtocolVersionV2 = 2;
    static constexpr uint32_t FirstServerAssignedActorNetId = 1000000000u;

    enum ActorPresentationFlags : uint8_t
    {
        ActorPresentationMoving = 1u << 0,
        ActorPresentationWeaponDrawn = 1u << 1,
        ActorPresentationSpellReadied = 1u << 2,
        ActorPresentationAttackingOrCasting = 1u << 3,
        ActorPresentationTeleporting = 1u << 4,
        ActorPresentationDead = 1u << 5,
    };

    inline uint8_t makeActorPresentationFlags(const BaseActor& actor)
    {
        uint8_t flags = 0;
        if (actor.isMoving)
            flags |= ActorPresentationMoving;
        if (actor.hasWeaponDrawn)
            flags |= ActorPresentationWeaponDrawn;
        if (actor.hasSpellReadied)
            flags |= ActorPresentationSpellReadied;
        if (actor.isAttackingOrCasting)
            flags |= ActorPresentationAttackingOrCasting;
        if (actor.position.isTeleporting)
            flags |= ActorPresentationTeleporting;
        if (actor.isDead)
            flags |= ActorPresentationDead;
        return flags;
    }

    inline void applyActorPresentationFlags(BaseActor& actor, uint8_t flags)
    {
        actor.isMoving = (flags & ActorPresentationMoving) != 0;
        actor.hasWeaponDrawn = (flags & ActorPresentationWeaponDrawn) != 0;
        actor.hasSpellReadied = (flags & ActorPresentationSpellReadied) != 0;
        actor.isAttackingOrCasting = (flags & ActorPresentationAttackingOrCasting) != 0;
        actor.position.isTeleporting = (flags & ActorPresentationTeleporting) != 0;
        actor.isDead = (flags & ActorPresentationDead) != 0;
    }

    struct ActorIdentityRecord
    {
        uint32_t actorNetId = 0;
        bool persistent = false;
        bool serverSpawned = false;
        bool removed = false;
        bool baselineReset = false;
        bool teleport = false;
        BaseActor actor;
    };

    struct ActorIdentityList
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        std::string cellId;
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t sequence = 0;
        uint64_t serverTimestamp = 0;
        std::vector<ActorIdentityRecord> actors;
    };

    struct ActorIdentityAck
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        std::string cellId;
        uint32_t sequence = 0;
        std::vector<uint32_t> actorNetIds;
    };

    struct CompactActorSnapshot
    {
        uint32_t actorNetId = 0;
        Position position;
        Velocity velocity;
        uint16_t movementFlags = 0;
        int8_t animFwd = 0;
        int8_t animSide = 0;
        uint8_t presentationFlags = 0;
    };

    struct ActorPositionV2List
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t sequence = 0;
        uint64_t serverTimestamp = 0;
        std::vector<CompactActorSnapshot> snapshots;
    };

    struct ActorPresentationSnapshot
    {
        uint32_t actorNetId = 0;
        bool isMoving = false;
        bool isAttackingOrCasting = false;
        bool hasWeaponDrawn = false;
        bool hasSpellReadied = false;
        bool isDead = false;
        uint16_t movementFlags = 0;
        int8_t animFwd = 0;
        int8_t animSide = 0;
        uint8_t presentationFlags = 0;
        // Temporary v2 bridge. Replace with a small animation group id/string table
        // before this becomes part of a long-lived high-churn protocol surface.
        std::string currentAnimGroup;
    };

    struct ActorPresentationV2List
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t sequence = 0;
        uint64_t serverTimestamp = 0;
        std::vector<ActorPresentationSnapshot> snapshots;
    };

    struct ActorAttackV2Event
    {
        uint32_t actorNetId = 0;
        uint32_t eventId = 0;
        Attack attack;
    };

    struct ActorAttackV2List
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        std::string cellId;
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t sequence = 0;
        uint64_t serverTimestamp = 0;
        std::vector<ActorAttackV2Event> events;
    };

    inline int8_t quantizeActorAxis(float value)
    {
        value = std::clamp(value, -1.f, 1.f);
        return static_cast<int8_t>(value * 127.f);
    }

    inline float dequantizeActorAxis(int8_t value)
    {
        return static_cast<float>(value) / 127.f;
    }
}

#endif
