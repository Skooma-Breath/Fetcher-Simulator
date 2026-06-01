#ifndef OPENMW_MP_ACTORSYNCPROTOCOL_HPP
#define OPENMW_MP_ACTORSYNCPROTOCOL_HPP

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include <components/openmw-mp/Base/BaseActor.hpp>

namespace mwmp
{
    static constexpr uint32_t ActorSyncProtocolVersionV1 = 1;
    // Still named v2 in code because this is the active ActorSync v2 lane, but
    // the wire number is bumped for the deterministic ActorInstanceId key break.
    static constexpr uint32_t ActorSyncProtocolVersionV2 = 5;

    using ActorInstanceId = uint64_t;

    enum class ActorKeyKind : uint8_t
    {
        VanillaRefNum = 0,
        SpawnedMpNum = 1,
        Unknown = 255,
    };

    struct ActorInstanceKey
    {
        ActorKeyKind kind = ActorKeyKind::Unknown;
        uint32_t id = 0;
    };

    inline bool isValidActorInstanceKey(const ActorInstanceKey& key)
    {
        return key.id != 0
            && (key.kind == ActorKeyKind::VanillaRefNum || key.kind == ActorKeyKind::SpawnedMpNum);
    }

    inline bool hasMissingActorInstanceIdentity(const BaseActor& actor)
    {
        return actor.refNum == 0 && actor.mpNum == 0;
    }

    inline bool hasAmbiguousActorInstanceIdentity(const BaseActor& actor)
    {
        return actor.refNum != 0 && actor.mpNum != 0;
    }

    inline ActorInstanceKey makeActorInstanceKey(const BaseActor& actor)
    {
        // Spawned actors can carry a temporary engine refNum locally. The server
        // normalizes this, and deterministic wire identity still belongs to mpNum.
        if (actor.mpNum != 0)
            return { ActorKeyKind::SpawnedMpNum, actor.mpNum };
        if (actor.refNum != 0)
            return { ActorKeyKind::VanillaRefNum, actor.refNum };
        return {};
    }

    inline ActorInstanceId packActorInstanceKey(const ActorInstanceKey& key)
    {
        if (!isValidActorInstanceKey(key))
            return 0;

        return (static_cast<ActorInstanceId>(static_cast<uint8_t>(key.kind)) << 32) | key.id;
    }

    inline ActorInstanceKey unpackActorInstanceId(ActorInstanceId actorInstanceId)
    {
        if (actorInstanceId == 0)
            return {};

        const auto kind = static_cast<ActorKeyKind>((actorInstanceId >> 32) & 0xffu);
        const auto id = static_cast<uint32_t>(actorInstanceId & 0xffffffffu);
        ActorInstanceKey key { kind, id };
        if (!isValidActorInstanceKey(key))
            return {};
        return key;
    }

    inline bool isValidActorInstanceId(ActorInstanceId actorInstanceId)
    {
        return isValidActorInstanceKey(unpackActorInstanceId(actorInstanceId));
    }

    inline ActorInstanceId actorInstanceIdFromActor(const BaseActor& actor)
    {
        return packActorInstanceKey(makeActorInstanceKey(actor));
    }

    inline bool actorInstanceIdMatchesActor(ActorInstanceId actorInstanceId, const BaseActor& actor)
    {
        const ActorInstanceId actorIdentity = actorInstanceIdFromActor(actor);
        return actorInstanceId != 0 && actorIdentity != 0 && actorInstanceId == actorIdentity;
    }

    inline std::string describeActorInstanceId(ActorInstanceId actorInstanceId)
    {
        const ActorInstanceKey key = unpackActorInstanceId(actorInstanceId);
        if (key.kind == ActorKeyKind::VanillaRefNum)
            return "vanilla:" + std::to_string(key.id);
        if (key.kind == ActorKeyKind::SpawnedMpNum)
            return "spawned:" + std::to_string(key.id);
        return "unknown:0";
    }

    inline std::string describeActorInstanceIdentity(const BaseActor& actor)
    {
        return describeActorInstanceId(actorInstanceIdFromActor(actor));
    }

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
        // Transitional field name: this now carries deterministic ActorInstanceId,
        // packed from TES3MP-style refNum/mpNum identity instead of server allocation.
        ActorInstanceId actorNetId = 0;
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
        std::vector<ActorInstanceId> actorNetIds;
    };

    struct CompactActorSnapshot
    {
        ActorInstanceId actorNetId = 0;
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
        ActorInstanceId actorNetId = 0;
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
        // Normalized completion for synced presentation idles. 0 starts at the
        // group's start key and 1 at its stop key. Negative means unavailable.
        float currentAnimCompletion = -1.f;
    };

    struct ActorPresentationV2List
    {
        uint32_t protocolVersion = ActorSyncProtocolVersionV2;
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t sequence = 0;
        uint64_t serverTimestamp = 0;
        // Optional server->authority request: sample and send fresh presentation
        // snapshots for these actors once.  Used to phase-align late-loading
        // clients without periodic idle refresh traffic.
        std::vector<ActorInstanceId> requestActorNetIds;
        std::vector<ActorPresentationSnapshot> snapshots;
    };

    struct ActorAttackV2Event
    {
        ActorInstanceId actorNetId = 0;
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

    inline CompactActorSnapshot makeCompactActorSnapshot(const BaseActor& actor, ActorInstanceId actorNetId)
    {
        const bool axisLocomotion = std::abs(actor.animFlags.animFwd) > 0.1f
            || std::abs(actor.animFlags.animSide) > 0.1f;
        const float speedSq = actor.velocity.linear[0] * actor.velocity.linear[0]
            + actor.velocity.linear[1] * actor.velocity.linear[1]
            + actor.velocity.linear[2] * actor.velocity.linear[2];
        const bool velocityLocomotion = speedSq > 20.f * 20.f;
        const bool hasLocomotionInput = !actor.isDead && (axisLocomotion || velocityLocomotion);

        float animFwd = hasLocomotionInput ? actor.animFlags.animFwd : 0.f;
        float animSide = hasLocomotionInput ? actor.animFlags.animSide : 0.f;
        if (hasLocomotionInput && !axisLocomotion && velocityLocomotion)
        {
            animFwd = 1.f;
            animSide = 0.f;
        }

        BaseActor presentationActor = actor;
        presentationActor.isMoving = hasLocomotionInput;

        CompactActorSnapshot snapshot;
        snapshot.actorNetId = actorNetId;
        snapshot.position = actor.position;
        snapshot.velocity = actor.velocity;
        snapshot.movementFlags = static_cast<uint16_t>(actor.animFlags.movementFlags);
        snapshot.animFwd = hasLocomotionInput ? quantizeActorAxis(animFwd) : 0;
        snapshot.animSide = hasLocomotionInput ? quantizeActorAxis(animSide) : 0;
        snapshot.presentationFlags = makeActorPresentationFlags(presentationActor);
        return snapshot;
    }

    inline void applyCompactActorSnapshotState(
        BaseActor& actor, const CompactActorSnapshot& snapshot, bool includeDeathState = false)
    {
        const bool previousDead = actor.isDead;

        actor.position = snapshot.position;
        actor.velocity = snapshot.velocity;
        applyActorPresentationFlags(actor, snapshot.presentationFlags);
        if (!includeDeathState)
            actor.isDead = previousDead;

        actor.animFlags.movementFlags = snapshot.movementFlags;
        actor.animFlags.animFwd = dequantizeActorAxis(snapshot.animFwd);
        actor.animFlags.animSide = dequantizeActorAxis(snapshot.animSide);

        if (!actor.isMoving)
        {
            actor.animFlags.animFwd = 0.f;
            actor.animFlags.animSide = 0.f;
        }

        if (actor.isDead)
        {
            actor.isMoving = false;
            actor.isAttackingOrCasting = false;
            actor.velocity = Velocity {};
            actor.animFlags.movementFlags = 0;
            actor.animFlags.animFwd = 0.f;
            actor.animFlags.animSide = 0.f;
        }
    }
}

#endif
