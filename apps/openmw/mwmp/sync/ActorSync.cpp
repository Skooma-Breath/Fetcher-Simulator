#include "ActorSync.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/rng.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorList.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPosition.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCombatRequest.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorDeath.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorStatsDynamic.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/vfs/pathutil.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/luamanager.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/soundmanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwmechanics/aisequence.hpp"
#include "../../mwmechanics/aisetting.hpp"
#include "../../mwmechanics/character.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwmechanics/movement.hpp"
#include "../../mwmechanics/npcstats.hpp"
#include "../../mwrender/animation.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/refdata.hpp"
#include "../../mwworld/scene.hpp"
#include "../../mwworld/worldimp.hpp"

#include "../Main.hpp"
#include "../network/Client.hpp"
#include "PlayerSync.hpp"
#include "RemotePlayer.hpp"
#include <components/openmw-mp/Packets/Actor/PacketActorAttack.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorEquipment.hpp>
#include "../../mwworld/inventorystore.hpp"
#include "../../mwrender/npcanimation.hpp"
#include "../../mwmechanics/spellcasting.hpp"
#include "../../mwmechanics/weapontype.hpp"
#include <components/esm3/loadweap.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadsoun.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCast.hpp>

namespace
{
    std::string makeActorKey(const mwmp::BaseActor& actor)
    {
        if (actor.mpNum != 0)
            return "mp|" + std::to_string(actor.mpNum);
        return actor.refId + "|" + std::to_string(actor.refNum) + "|" + std::to_string(actor.mpNum);
    }

    std::string makeLocalActorKey(const std::string& cellId, const MWWorld::Ptr& ptr)
    {
        return cellId + "|" + ptr.getCellRef().getRefId().serializeText() + "|"
            + std::to_string(ptr.getCellRef().getRefNum().mIndex);
    }

    float lerpFloat(float current, float target, float alpha)
    {
        return current + ((target - current) * alpha);
    }

    MWWorld::CellStore* findActiveCellById(MWWorld::World& world, const std::string& cellId)
    {
        auto& scene = world.getWorldScene();
        for (MWWorld::CellStore* store : scene.getActiveCells())
        {
            if (store == nullptr || store->getCell() == nullptr)
                continue;

            const MWWorld::Cell* cell = store->getCell();
            if (cellId.rfind("EXT:", 0) == 0)
            {
                int gridX = 0;
                int gridY = 0;
                if (std::sscanf(cellId.c_str(), "EXT:%d,%d", &gridX, &gridY) != 2)
                    return nullptr;

                if (cell->isExterior() && cell->getGridX() == gridX && cell->getGridY() == gridY)
                    return store;
            }
            else if (!cell->isExterior() && std::string(cell->getNameId()) == cellId)
                return store;
        }

        return nullptr;
    }

    std::string cellIdForPtr(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || ptr.getCell() == nullptr || ptr.getCell()->getCell() == nullptr)
            return {};

        const MWWorld::Cell* cell = ptr.getCell()->getCell();
        if (cell->isExterior())
            return std::string("EXT:") + std::to_string(cell->getGridX()) + "," + std::to_string(cell->getGridY());
        return std::string(cell->getNameId());
    }

    bool matchesActor(const MWWorld::Ptr& ptr, const mwmp::BaseActor& actor)
    {
        if (ptr.isEmpty() || ptr.getCell() == nullptr || !ptr.getClass().isActor())
            return false;

        if (ptr.getCellRef().getRefId().serializeText() != actor.refId)
            return false;
        if (actor.mpNum == 0 && actor.refNum != 0 && ptr.getCellRef().getRefNum().mIndex != actor.refNum)
            return false;
        return true;
    }

    bool isUnarmedAttack(const MWWorld::Ptr& attacker)
    {
        if (attacker.isEmpty() || !attacker.getClass().hasInventoryStore(attacker))
            return true;

        const MWWorld::InventoryStore& inv = attacker.getClass().getInventoryStore(attacker);
        const MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        return weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId;
    }

    MWWorld::Ptr getEquippedWeapon(const MWWorld::Ptr& attacker)
    {
        if (attacker.isEmpty() || !attacker.getClass().hasInventoryStore(attacker))
            return {};

        MWWorld::InventoryStore& inv = attacker.getClass().getInventoryStore(attacker);
        MWWorld::ContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId)
            return {};

        return *weapon;
    }

    void playReplicatedImpactSound(const MWWorld::Ptr& attacker, const MWWorld::Ptr& target, const mwmp::Attack& atk)
    {
        const MWBase::Environment& env = MWBase::Environment::get();
        MWBase::World* world = env.getWorld();
        MWBase::SoundManager* sound = env.getSoundManager();
        if (!world || !sound || attacker.isEmpty() || target.isEmpty() || !atk.hit)
            return;

        const bool melee = atk.type == 0;
        const bool unarmed = melee && isUnarmedAttack(attacker);
        const bool isWerewolf = unarmed && attacker.getClass().isNpc()
            && attacker.getClass().getNpcStats(attacker).isWerewolf();

        if (isWerewolf)
        {
            const MWWorld::ESMStore& store = world->getStore();
            if (const ESM::Sound* wolfHit = store.get<ESM::Sound>().searchRandom("WolfHit", world->getPrng()))
                sound->playSound3D(target, wolfHit->mId, 1.0f, 1.0f);
        }
        else if (unarmed && !atk.healthDamage)
        {
            static const std::array<ESM::RefId, 2> h2hSounds = {
                ESM::RefId::stringRefId("Hand To Hand Hit"),
                ESM::RefId::stringRefId("Hand To Hand Hit 2"),
            };
            sound->playSound3D(target, h2hSounds[Misc::Rng::rollDice(h2hSounds.size(), world->getPrng())], 1.0f,
                1.0f);
        }
    }

    osg::Vec3f resolveReplicatedHitPos(const MWWorld::Ptr& target, const osg::Vec3f& hitPos)
    {
        if (target.isEmpty())
            return hitPos;

        osg::Vec3f resolvedPos = hitPos;
        if (resolvedPos.length2() < 0.001f)
        {
            resolvedPos = target.getRefData().getPosition().asVec3();
            if (MWBase::World* world = MWBase::Environment::get().getWorld())
                resolvedPos.z() += world->getHalfExtents(target).z() * 1.5f;
        }

        return resolvedPos;
    }

    void spawnReplicatedPlayerBloodEffect(const MWWorld::Ptr& target, const osg::Vec3f& hitPos)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world || target.isEmpty())
            return;

        const osg::Vec3f effectPos = resolveReplicatedHitPos(target, hitPos);

        int bloodType = 0;
        if (target.getType() == ESM::NPC::sRecordId)
            bloodType = target.get<ESM::NPC>()->mBase->mBloodType;
        else if (target.getType() == ESM::Creature::sRecordId)
            bloodType = target.get<ESM::Creature>()->mBase->mBloodType;

        const std::size_t bloodModelIndex = Misc::Rng::rollDice(3, world->getPrng());
        const std::string modelKey = "Blood_Model_" + std::to_string(bloodModelIndex);
        const std::string bloodModel = Misc::ResourceHelpers::correctMeshPath(
            VFS::Path::Normalized(Fallback::Map::getString(modelKey)));

        std::string bloodTexture = std::string(Fallback::Map::getString("Blood_Texture_" + std::to_string(bloodType)));
        if (bloodTexture.empty())
            bloodTexture = std::string(Fallback::Map::getString("Blood_Texture_0"));

        world->spawnEffect(VFS::Path::Normalized(bloodModel), bloodTexture, effectPos, 1.f, false, false);
    }

    bool isNetworkPlayerProxy(const MWWorld::Ptr& ptr)
    {
        if (auto* baseNode = ptr.getRefData().getBaseNode())
        {
            int guid = 0;
            if (baseNode->getUserValue("mp_player_guid", guid) && guid > 0)
                return true;
        }

        return false;
    }

    // Resolve a cast spellId to either a spell or an enchantment.
    // notifyNpcCast sends the item's refId for enchanted items, so a plain
    // Spell lookup fails.  This helper falls back to weapon/armor/clothing/book
    // stores, reads the item's enchantment field, and returns the enchantment.
    // Returns {spell, enchantment} — exactly one will be non-null on success.
    struct ResolvedCastSource
    {
        const ESM::Spell*       spell       = nullptr;
        const ESM::Enchantment* enchantment = nullptr;
    };

    ResolvedCastSource resolveCastSource(const std::string& id)
    {
        const auto& store = *MWBase::Environment::get().getESMStore();
        const ESM::RefId refId = ESM::RefId::stringRefId(id);

        // Try spell first (the common case for non-enchantment casts).
        if (const ESM::Spell* sp = store.get<ESM::Spell>().search(refId))
            return { sp, nullptr };

        // Try each item store and read the enchantment field.
        ESM::RefId enchId;
        if (const ESM::Weapon* w = store.get<ESM::Weapon>().search(refId))
            enchId = w->mEnchant;
        else if (const ESM::Armor* a = store.get<ESM::Armor>().search(refId))
            enchId = a->mEnchant;
        else if (const ESM::Clothing* c = store.get<ESM::Clothing>().search(refId))
            enchId = c->mEnchant;
        else if (const ESM::Book* b = store.get<ESM::Book>().search(refId))
            enchId = b->mEnchant;

        if (!enchId.empty())
        {
            if (const ESM::Enchantment* ench = store.get<ESM::Enchantment>().search(enchId))
                return { nullptr, ench };
        }

        return {};
    }
}

namespace mwmp
{
    ActorSync::ActorSync(NetworkClient& client)
        : mClient(client)
    {
    }

    void ActorSync::update(float dt)
    {
        for (auto& [cellId, cell] : mCells)
        {
            if (hasAuthority(cellId))
            {
                sendAuthoritativeActorUpdates(cellId, cell, dt);
                continue;
            }

            for (auto& [actorKey, actor] : cell.actors)
            {
                advanceSmoothing(actor, dt);
                if (resolveActorBinding(cellId, actor))
                    applyBoundActorState(actor);
            }
        }
    }

    bool ActorSync::shouldAcceptSnapshot(CellRuntime& cell, const ActorList& list, const char* packetName)
    {
        if (cell.latest.authorityGeneration != 0
            && list.authorityGeneration != 0
            && list.authorityGeneration < cell.latest.authorityGeneration)
        {
            Log(Debug::Info) << "[MP] ActorSync: dropped stale " << packetName
                             << " cellId=" << list.cellId
                             << " seq=" << list.snapshotSequence
                             << " latestSeq=" << cell.latest.snapshotSequence
                             << " gen=" << list.authorityGeneration
                             << " latestGen=" << cell.latest.authorityGeneration;
            return false;
        }

        if (cell.latest.snapshotSequence != 0
            && list.snapshotSequence != 0
            && list.snapshotSequence < cell.latest.snapshotSequence)
        {
            Log(Debug::Info) << "[MP] ActorSync: dropped stale " << packetName
                             << " cellId=" << list.cellId
                             << " seq=" << list.snapshotSequence
                             << " latestSeq=" << cell.latest.snapshotSequence
                             << " gen=" << list.authorityGeneration
                             << " latestGen=" << cell.latest.authorityGeneration;
            return false;
        }

        cell.latest.cellId = list.cellId;
        cell.latest.authorityGuid = list.authorityGuid;
        if (list.authorityGeneration != 0)
            cell.latest.authorityGeneration = list.authorityGeneration;
        if (list.snapshotSequence != 0)
            cell.latest.snapshotSequence = list.snapshotSequence;
        if (list.serverTimestamp != 0)
            cell.latest.serverTimestamp = list.serverTimestamp;

        return true;
    }

    void ActorSync::onAuthorityUpdate(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorAuthority"))
            return;

        const uint32_t localGuid = Main::get().getPlayerSync().localPlayer().guid;
        const bool hasLocalAuthority = (localGuid != 0) && (list.authorityGuid == localGuid);
        const bool previous = hasAuthority(list.cellId);
        mAuthority[list.cellId] = hasLocalAuthority;

        cell.latest.cellId = list.cellId;
        cell.latest.isAuthority = list.isAuthority;
        cell.latest.authorityGuid = list.authorityGuid;
        cell.latest.authorityGeneration = list.authorityGeneration;
        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        if (previous != hasLocalAuthority)
        {
            cell.initialListSent = false;
            cell.positionSendTimer = 0.f;
            cell.outboundCellId = list.cellId;
            Log(Debug::Info) << "[MP] ActorSync: authority "
                             << (hasLocalAuthority ? "granted" : "revoked")
                             << " for " << list.cellId
                             << " owner=" << list.authorityGuid
                             << " localGuid=" << localGuid;
        }
    }

    void ActorSync::onActorListUpdate(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorList"))
            return;

        cell.latest = list;

        std::unordered_map<std::string, ActorRuntime> updatedActors;
        updatedActors.reserve(list.actors.size());
        std::unordered_set<uint32_t> incomingServerSpawnedMpNums;

        for (const auto& actorState : list.actors)
        {
            const std::string actorKey = makeActorKey(actorState);
            ActorRuntime runtime;
            if (actorState.mpNum != 0)
                incomingServerSpawnedMpNums.insert(actorState.mpNum);

            auto existing = cell.actors.find(actorKey);
            if (existing != cell.actors.end())
                runtime = existing->second;

            runtime.state = actorState;
            // If the actor arrives already dead from a full actor list (not a
            // real-time death packet), mark it so applyBoundActorState() can
            // skip to the final death pose instead of replaying the animation.
            if (actorState.isDead)
                runtime.deathFromRealtimePacket = false;
            queueSnapshot(runtime, actorState, list);
            updatedActors[actorKey] = std::move(runtime);
        }

        MWBase::World* world = MWBase::Environment::get().getWorld();
        for (auto& [actorKey, runtime] : cell.actors)
        {
            if (runtime.state.mpNum == 0 || incomingServerSpawnedMpNums.count(runtime.state.mpNum) != 0)
                continue;

            if (!runtime.boundActor.isEmpty())
            {
                forgetServerSpawnedActor(list.cellId, runtime.boundActor, runtime.state.mpNum);
                if (world)
                    world->deleteObject(runtime.boundActor);
            }
        }

        cell.actors = std::move(updatedActors);
    }

    void ActorSync::onActorPositionUpdate(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorPosition"))
            return;

        cell.latest.cellId = list.cellId;
        cell.latest.authorityGuid = list.authorityGuid;
        cell.latest.authorityGeneration = list.authorityGeneration;
        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, true);
            queueSnapshot(runtime, actorState, list);
        }
    }

    void ActorSync::onActorAnimFlagsUpdate(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorAnimFlags"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.animFlags = actorState.animFlags;
        }
    }

    void ActorSync::onActorAnimPlay(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorAnimPlay"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.animPlay = actorState.animPlay;
            runtime.pendingAnimPlay = true;
        }
    }

    void ActorSync::onActorAttack(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorAttack"))
            return;

        Log(Debug::Info) << "[MP] ActorSync: onActorAttack received cellId=" << list.cellId
                         << " actors=" << list.actors.size();
        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.attack = actorState.attack;
            runtime.pendingAttack = true;
            // The authority already does edge detection, so each received packet
            // represents a new attack event. Reset lastAttackPressed so that
            // applyBoundActorState() will see a proper rising edge.
            runtime.lastAttackPressed = false;
        }
    }

    void ActorSync::onActorCast(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorCast"))
            return;

        Log(Debug::Info) << "[MP] ActorSync: onActorCast received cellId=" << list.cellId
                         << " actors=" << list.actors.size();
        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.cast = actorState.cast;
            runtime.pendingCast = true;
            Log(Debug::Info) << "[MP] ActorSync: pendingCast set for " << actorState.refId
                             << " spell='" << actorState.cast.spellId
                             << "' castAnim='" << actorState.cast.castAnimation
                             << "' target='" << actorState.cast.targetRefId
                             << "' release=" << actorState.cast.release;
        }
    }

    void ActorSync::onActorDeath(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorDeath"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.deathState = actorState.deathState;
            runtime.state.isDead = actorState.isDead;
            runtime.state.isInstantDeath = actorState.isInstantDeath;
            if (!actorState.deathAnimGroup.empty())
                runtime.state.deathAnimGroup = actorState.deathAnimGroup;
            runtime.state.dynamicStats.health.current = actorState.dynamicStats.health.current;
            runtime.deathFromRealtimePacket = true;
            Log(Debug::Info) << "[MP] ActorSync: Death received for " << actorState.refId
                             << " isDead=" << actorState.isDead
                             << " hp=" << actorState.dynamicStats.health.current;
        }
    }

    void ActorSync::onActorEquipment(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorEquipment"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.equipment = actorState.equipment;
        }
    }

    void ActorSync::onActorStatsDynamic(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorStatsDynamic"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.dynamicStats = actorState.dynamicStats;
            runtime.state.isDead = actorState.isDead;
            Log(Debug::Info) << "[MP] ActorSync: StatsDynamic for " << actorState.refId
                             << " hp=" << actorState.dynamicStats.health.current
                             << " isDead=" << actorState.isDead;
        }
    }

    void ActorSync::onActorAI(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorAI"))
            return;

        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            auto& runtime = cell.actors[makeActorKey(actorState)];
            mergeActorState(runtime, actorState, false);
            runtime.state.ai = actorState.ai;
        }
    }

    void ActorSync::onActorCombatRequest(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorCombatRequest"))
            return;

        const uint32_t localGuid = Main::get().getPlayerSync().localPlayer().guid;

        // Handle NPC->player damage: routed to the victim player (us) by the server.
        if (list.victimPlayerGuid != 0 && list.victimPlayerGuid == localGuid)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (!world) return;

            MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty() || world->getGodModeState())
                return;

            MWMechanics::CreatureStats& pstats = player.getClass().getCreatureStats(player);
            if (pstats.isDead())
                return;

            for (const BaseActor& actorState : list.actors)
            {
                if (actorState.attack.damage <= 0.f)
                    continue;

                MWWorld::Ptr attacker;
                auto cellIt = mCells.find(list.cellId);
                if (cellIt != mCells.end())
                {
                    const std::string actorKey = makeActorKey(actorState);
                    auto runtimeIt = cellIt->second.actors.find(actorKey);
                    if (runtimeIt != cellIt->second.actors.end() && resolveActorBinding(list.cellId, runtimeIt->second))
                        attacker = runtimeIt->second.boundActor;
                }
                if (attacker.isEmpty())
                {
                    try
                    {
                        attacker = world->getPtr(ESM::RefId::stringRefId(actorState.refId), false);
                    }
                    catch (...) {}
                }

                MWWorld::Ptr weapon = getEquippedWeapon(attacker);
                const osg::Vec3f hitPos = resolveReplicatedHitPos(player,
                    osg::Vec3f(actorState.attack.hitPos[0], actorState.attack.hitPos[1], actorState.attack.hitPos[2]));

                std::map<std::string, float> damages;
                damages[actorState.attack.healthDamage ? "health" : "fatigue"] = actorState.attack.damage;

                const MWMechanics::DamageSourceType sourceType
                    = (actorState.attack.type == 2 || actorState.attack.type == 3)
                    ? MWMechanics::DamageSourceType::Ranged
                    : (actorState.attack.type == 1 ? MWMechanics::DamageSourceType::Magical
                                                   : MWMechanics::DamageSourceType::Melee);

                MWBase::Environment::get().getLuaManager()->onHit(
                    attacker, player, weapon, MWWorld::Ptr(), actorState.attack.type,
                    actorState.attack.strength > 0.f ? actorState.attack.strength : 1.f, actorState.attack.damage,
                    actorState.attack.healthDamage, hitPos, true, sourceType);

                player.getClass().onHit(player, damages, weapon.isEmpty() ? ESM::RefId() : weapon.getCellRef().getRefId(),
                    attacker, true, sourceType);

                Log(Debug::Info) << "[MP] ActorSync: NPC damage applied to local player"
                                 << " damage=" << actorState.attack.damage
                                 << " stat=" << (actorState.attack.healthDamage ? "health" : "fatigue")
                                 << " attacker=" << actorState.refId;

                if (actorState.attack.healthDamage && actorState.attack.type != 1)
                    spawnReplicatedPlayerBloodEffect(player, hitPos);
                playReplicatedImpactSound(attacker, player, actorState.attack);

                // Play "when strikes" enchantment VFX on the player from
                // the attacking NPC's weapon.  The authority ran
                // applyOnStrikeEnchantment() locally but the visual effects
                // only fired on the authority's screen. Replay the hit effects
                // on the victim client so the destruction glow, particles and
                // sound are visible to the victim.
                {
                    if (!weapon.isEmpty() && weapon.getType() == ESM::Weapon::sRecordId)
                    {
                        const auto& esmStore = *MWBase::Environment::get().getESMStore();
                        const ESM::Weapon* weapRec = weapon.get<ESM::Weapon>()->mBase;
                        if (weapRec && !weapRec->mEnchant.empty())
                        {
                            const ESM::Enchantment* ench = esmStore.get<ESM::Enchantment>().search(weapRec->mEnchant);
                            if (ench && ench->mData.mType == ESM::Enchantment::WhenStrikes)
                            {
                                const auto& mfxStore = esmStore.get<ESM::MagicEffect>();
                                for (const auto& eff : ench->mEffects.mList)
                                {
                                    const ESM::MagicEffect* me = mfxStore.search(eff.mData.mEffectID);
                                    if (me)
                                        MWMechanics::playEffects(player, *me);
                                }
                                Log(Debug::Info) << "[MP] ActorSync: played enchant VFX on player from "
                                                 << weapon.getCellRef().getRefId().serializeText();
                            }
                        }
                    }
                }
            }
            return;
        }

        if (!hasAuthority(list.cellId))
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
        if (!world || !mechanics)
            return;

        Log(Debug::Info) << "[MP] ActorSync: CombatRequest received cellId=" << list.cellId
                         << " attackerGuid=" << list.authorityGuid
                         << " actorCount=" << list.actors.size();

        MWWorld::Ptr attacker;
        if (list.authorityGuid == localGuid)
            attacker = world->getPlayerPtr();
        else if (Main::isInitialised())
        {
            auto& pl = Main::get().getPlayerList();
            auto* remote = pl.getPlayer(list.authorityGuid);
            if (remote)
                attacker = remote->getNpcPtr();
        }

        if (attacker.isEmpty())
            return;

        for (const BaseActor& actorState : list.actors)
        {
            MWWorld::Ptr victim;
            auto cellIt = mCells.find(list.cellId);
            if (cellIt != mCells.end())
            {
                const std::string actorKey = makeActorKey(actorState);
                auto runtimeIt = cellIt->second.actors.find(actorKey);
                if (runtimeIt != cellIt->second.actors.end() && resolveActorBinding(list.cellId, runtimeIt->second))
                    victim = runtimeIt->second.boundActor;
            }

            if (victim.isEmpty())
            {
                try
                {
                    victim = world->getPtr(ESM::RefId::stringRefId(actorState.refId), false);
                }
                catch (...) {}
            }

            if (victim.isEmpty())
                continue;

            MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
            if (victimStats.isDead())
            {
                Log(Debug::Info) << "[MP] ActorSync: CombatRequest ignored for dead victim="
                                 << actorState.refId << " mpNum=" << actorState.mpNum;
                continue;
            }

            Log(Debug::Info) << "[MP] ActorSync: CombatRequest from guid=" << list.authorityGuid
                             << " victim=" << actorState.refId
                             << " mpNum=" << actorState.mpNum
                             << " attackerEmpty=" << attacker.isEmpty();

            // Use startCombat so the NPC enters combat with the attacker unconditionally.
            // actorAttacked() requires the attacker to already be in combat with the victim,
            // which is never true for a remote-player attack arriving via CombatRequest.
            if (!attacker.isEmpty())
                mechanics->startCombat(victim, attacker, nullptr);

            // Apply the forwarded hit through the normal onHit path so fatigue-only
            // hand-to-hand hits, authoritative knock/recovery, and victim feedback
            // all follow the same code path as a local attack.
            if (actorState.attack.damage > 0.f)
            {
                const MWMechanics::DamageSourceType sourceType
                    = (actorState.attack.type == 2 || actorState.attack.type == 3)
                    ? MWMechanics::DamageSourceType::Ranged
                    : (actorState.attack.type == 1 ? MWMechanics::DamageSourceType::Magical
                                                   : MWMechanics::DamageSourceType::Melee);

                MWWorld::Ptr weapon = getEquippedWeapon(attacker);
                bool healthDamage = actorState.attack.healthDamage;
                if (sourceType == MWMechanics::DamageSourceType::Melee && weapon.isEmpty())
                {
                    // The attacker-side proxy can miss the exact knocked-down edge
                    // while a remote NPC is already on the ground. Re-evaluate the
                    // vanilla unarmed damage split on the authority so follow-up
                    // punches against grounded targets stay as health damage instead
                    // of reapplying fatigue and pinning the KO state.
                    healthDamage = victimStats.isParalyzed() || victimStats.getKnockedDown();
                }

                std::map<std::string, float> damages;
                damages[healthDamage ? "health" : "fatigue"] = actorState.attack.damage;

                victim.getClass().onHit(victim,
                    damages,
                    weapon.isEmpty() ? ESM::RefId() : weapon.getCellRef().getRefId(),
                    attacker,
                    true,
                    sourceType);

                Log(Debug::Info) << "[MP] ActorSync: CombatRequest applied damage=" << actorState.attack.damage
                                 << " stat=" << (healthDamage ? "health" : "fatigue")
                                 << " to " << actorState.refId
                                 << " sourceType=" << static_cast<int>(sourceType);
            }
        }
    }

    bool ActorSync::hasAuthority(const std::string& cellId) const
    {
        auto it = mAuthority.find(cellId);
        return (it != mAuthority.end()) && it->second;
    }

    uint32_t ActorSync::getActorMpNum(const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty() || !ptr.getClass().isActor())
            return 0;

        const std::string cellId = cellIdForPtr(ptr);
        if (cellId.empty())
            return 0;

        return mappedMpNumForPtr(cellId, ptr);
    }

    MWWorld::Ptr ActorSync::getActorByMpNum(uint32_t mpNum) const
    {
        if (mpNum == 0)
            return MWWorld::Ptr();

        auto it = mServerSpawnedActorsByMpNum.find(mpNum);
        return it != mServerSpawnedActorsByMpNum.end() ? it->second : MWWorld::Ptr();
    }

    void ActorSync::sendCombatRequest(const MWWorld::Ptr& victim, float damage, bool healthDamage, bool knocked,
        const osg::Vec3f& hitPos, int attackType, float attackStrength)
    {
        if (victim.isEmpty())
        {
            Log(Debug::Warning) << "[MP] ActorSync: sendCombatRequest skipped — victim is empty";
            return;
        }
        if (victim.getCell() == nullptr)
        {
            Log(Debug::Warning) << "[MP] ActorSync: sendCombatRequest skipped — victim cell is null";
            return;
        }

        const MWWorld::Cell* cell = victim.getCell()->getCell();
        if (!cell)
        {
            Log(Debug::Warning) << "[MP] ActorSync: sendCombatRequest skipped — inner cell is null";
            return;
        }

        const std::string cellId = cell->isExterior()
            ? (std::string("EXT:") + std::to_string(cell->getGridX()) + "," + std::to_string(cell->getGridY()))
            : std::string(cell->getNameId());

        if (hasAuthority(cellId))
        {
            Log(Debug::Info) << "[MP] ActorSync: sendCombatRequest skipped — we have authority for " << cellId;
            return;
        }

        const MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
        if (victimStats.isDead())
        {
            Log(Debug::Info) << "[MP] ActorSync: sendCombatRequest skipped: victim is already dead";
            return;
        }

        const uint32_t victimMpNum = mappedMpNumForPtr(cellId, victim);

        ActorList request;
        request.cellId = cellId;
        request.authorityGuid = Main::get().getPlayerSync().localPlayer().guid;

        BaseActor requestedActor;
        requestedActor.refId = victim.getCellRef().getRefId().serializeText();
        requestedActor.refNum = victimMpNum != 0 ? 0 : victim.getCellRef().getRefNum().mIndex;
        requestedActor.mpNum = victimMpNum;
        requestedActor.cellId = cellId;
        requestedActor.attack.target = victim.getCellRef().getRefId().serializeText();
        requestedActor.attack.targetMpNum = victimMpNum;
        requestedActor.attack.targetKind = mwmp::Attack::TargetActor;
        requestedActor.attack.hit = true;
        requestedActor.attack.block = false;
        requestedActor.attack.miss = false;
        requestedActor.attack.pressed = true;
        requestedActor.attack.knocked = knocked;
        requestedActor.attack.healthDamage = healthDamage;
        requestedActor.attack.damage = damage;
        requestedActor.attack.strength = attackStrength;
        requestedActor.attack.type = attackType;
        requestedActor.attack.hitPos[0] = hitPos.x();
        requestedActor.attack.hitPos[1] = hitPos.y();
        requestedActor.attack.hitPos[2] = hitPos.z();
        request.actors.push_back(requestedActor);

        PacketActorCombatRequest pkt;
        pkt.setActorList(&request);
        mClient.sendReliable(pkt.encode());
        Log(Debug::Info) << "[MP] ActorSync: sent CombatRequest for victim=" << requestedActor.refId
                         << " mpNum=" << requestedActor.mpNum
                         << " damage=" << requestedActor.attack.damage
                         << " healthDamage=" << healthDamage
                         << " cellId=" << cellId
                         << " authorityGuid=" << request.authorityGuid;
    }

    void ActorSync::sendNpcPlayerDamage(uint32_t victimGuid, float damage, bool healthDamage, bool isDead,
        int attackType, const MWWorld::Ptr& npcAttacker)
    {
        if (victimGuid == 0 || damage <= 0.f || npcAttacker.isEmpty())
            return;
        if (npcAttacker.getCell() == nullptr)
            return;

        const MWWorld::Cell* cell = npcAttacker.getCell()->getCell();
        if (!cell) return;

        const std::string cellId = cell->isExterior()
            ? (std::string("EXT:") + std::to_string(cell->getGridX()) + "," + std::to_string(cell->getGridY()))
            : std::string(cell->getNameId());

        ActorList request;
        request.cellId = cellId;
        request.victimPlayerGuid = victimGuid;
        request.authorityGuid = Main::get().getPlayerSync().localPlayer().guid;

        BaseActor npcActor;
        npcActor.refId = npcAttacker.getCellRef().getRefId().serializeText();
        npcActor.refNum = npcAttacker.getCellRef().getRefNum().mIndex;
        npcActor.cellId = cellId;
        npcActor.attack.targetKind = mwmp::Attack::TargetPlayer;
        npcActor.attack.hit = true;
        npcActor.attack.healthDamage = healthDamage;
        npcActor.attack.damage = damage;
        npcActor.attack.type = attackType;
        npcActor.attack.strength = 1.f;
        npcActor.isDead = isDead;
        request.actors.push_back(npcActor);

        PacketActorCombatRequest pkt;
        pkt.setActorList(&request);
        mClient.sendReliable(pkt.encode());

        Log(Debug::Info) << "[MP] ActorSync: sendNpcPlayerDamage victimGuid=" << victimGuid
                         << " damage=" << damage
                         << " healthDamage=" << healthDamage
                         << " attackType=" << attackType
                         << " isDead=" << isDead
                         << " attacker=" << npcActor.refId;
    }

    void ActorSync::notifyNpcCast(const MWWorld::Ptr& npc, const std::string& spellId,
        const std::string& castAnim, const MWWorld::Ptr& target, bool release)
    {
        if (npc.isEmpty() || npc.getCell() == nullptr)
            return;

        const MWWorld::Cell* cell = npc.getCell()->getCell();
        if (!cell) return;

        const std::string cellId = cell->isExterior()
            ? (std::string("EXT:") + std::to_string(cell->getGridX()) + "," + std::to_string(cell->getGridY()))
            : std::string(cell->getNameId());

        if (!hasAuthority(cellId))
            return;

        ActorList castList;
        castList.cellId = cellId;
        castList.isAuthority = true;
        castList.authorityGuid = Main::get().getPlayerSync().localPlayer().guid;

        BaseActor castActor;
        castActor.refId = npc.getCellRef().getRefId().serializeText();
        castActor.refNum = npc.getCellRef().getRefNum().mIndex;
        castActor.cellId = cellId;
        const uint32_t mappedMpNum = mappedMpNumForPtr(cellId, npc);
        if (mappedMpNum != 0)
        {
            castActor.mpNum = mappedMpNum;
            castActor.refNum = 0;
        }
        castActor.cast.spellId = spellId;
        castActor.cast.castAnimation = castAnim;
        castActor.cast.release = release;
        castActor.cast.success = true;
        if (!target.isEmpty())
        {
            castActor.cast.targetRefId = target.getCellRef().getRefId().serializeText();
            if (auto* baseNode = target.getRefData().getBaseNode())
            {
                int tGuid = 0;
                if (baseNode->getUserValue("mp_player_guid", tGuid) && tGuid > 0)
                    castActor.cast.targetGuid = static_cast<uint32_t>(tGuid);
            }
        }
        castList.actors.push_back(castActor);

        PacketActorCast pkt;
        pkt.setActorList(&castList);
        mClient.sendReliable(pkt.encode());

        Log(Debug::Info) << "[MP] ActorSync: notifyNpcCast " << castActor.refId
                         << " spell='" << spellId << "' castAnim='" << castAnim
                         << "' target='" << castActor.cast.targetRefId
                         << "' release=" << release;
    }

    void ActorSync::queueSnapshot(ActorRuntime& actor, const BaseActor& state, const ActorList& list)
    {
        BufferedSnapshot snapshot;
        snapshot.position = state.position;
        snapshot.velocity = state.velocity;
        snapshot.sequence = list.snapshotSequence;
        snapshot.serverTimestamp = list.serverTimestamp;

        if (!actor.snapshots.empty() && actor.snapshots.back().sequence == snapshot.sequence)
            actor.snapshots.back() = snapshot;
        else
            actor.snapshots.push_back(snapshot);

        while (actor.snapshots.size() > 6)
            actor.snapshots.pop_front();

        if (!actor.hasSmoothedPosition)
        {
            actor.smoothedPosition = state.position;
            actor.hasSmoothedPosition = true;
        }
    }

    void ActorSync::mergeActorState(ActorRuntime& actor, const BaseActor& state, bool includeTransform)
    {
        actor.state.refId = state.refId;
        actor.state.refNum = state.refNum;
        actor.state.mpNum = state.mpNum;
        actor.state.cellId = state.cellId;

        if (includeTransform)
        {
            actor.state.position = state.position;
            actor.state.velocity = state.velocity;
            // Update locomotion state from the position packet so the remote CC
            // always has current movement axes without a separate AnimFlags send.
            actor.state.isMoving            = state.isMoving;
            actor.state.hasWeaponDrawn      = state.hasWeaponDrawn;
            actor.state.hasSpellReadied     = state.hasSpellReadied;
            actor.state.isAttackingOrCasting = state.isAttackingOrCasting;
            actor.state.animFlags.animFwd   = state.animFlags.animFwd;
            actor.state.animFlags.animSide  = state.animFlags.animSide;
            actor.state.animFlags.movementFlags = state.animFlags.movementFlags;
            // Propagate the authority's current animation group so non-auth clients
            // can replay the exact same animation (walk, idle variant, etc.).
            actor.state.animFlags.currentAnimGroup = state.animFlags.currentAnimGroup;
        }
    }

    void ActorSync::advanceSmoothing(ActorRuntime& actor, float dt)
    {
        if (actor.snapshots.empty())
            return;

        if (!actor.hasSmoothedPosition)
        {
            actor.smoothedPosition = actor.snapshots.front().position;
            actor.hasSmoothedPosition = true;
        }

        while (actor.snapshots.size() > 2)
            actor.snapshots.pop_front();

        const Position& target = (actor.snapshots.size() >= 2)
            ? actor.snapshots[1].position
            : actor.snapshots.front().position;

        const float alpha = std::clamp(dt * 10.f, 0.f, 1.f);
        for (int i = 0; i < 3; ++i)
        {
            actor.smoothedPosition.pos[i] = lerpFloat(actor.smoothedPosition.pos[i], target.pos[i], alpha);
            actor.smoothedPosition.rot[i] = lerpFloat(actor.smoothedPosition.rot[i], target.rot[i], alpha);
        }
        actor.smoothedPosition.isTeleporting = target.isTeleporting;
        actor.state.position = actor.smoothedPosition;

        if (actor.snapshots.size() >= 2)
        {
            const float dx = actor.smoothedPosition.pos[0] - target.pos[0];
            const float dy = actor.smoothedPosition.pos[1] - target.pos[1];
            const float dz = actor.smoothedPosition.pos[2] - target.pos[2];
            const float distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
            if (distanceSq < 4.f)
                actor.snapshots.pop_front();
        }
    }

    void ActorSync::rememberServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum)
    {
        if (mpNum == 0 || ptr.isEmpty())
            return;

        mMpNumsByLocalActor[makeLocalActorKey(cellId, ptr)] = mpNum;
        mServerSpawnedActorsByMpNum[mpNum] = ptr;
    }

    void ActorSync::forgetServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum)
    {
        if (!ptr.isEmpty())
            mMpNumsByLocalActor.erase(makeLocalActorKey(cellId, ptr));

        if (mpNum == 0)
            return;

        auto it = mServerSpawnedActorsByMpNum.find(mpNum);
        if (it != mServerSpawnedActorsByMpNum.end() && (ptr.isEmpty() || it->second == ptr))
            mServerSpawnedActorsByMpNum.erase(it);
    }

    uint32_t ActorSync::mappedMpNumForPtr(const std::string& cellId, const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return 0;

        auto localIt = mMpNumsByLocalActor.find(makeLocalActorKey(cellId, ptr));
        if (localIt != mMpNumsByLocalActor.end())
            return localIt->second;

        for (const auto& [mpNum, mappedPtr] : mServerSpawnedActorsByMpNum)
        {
            if (!mappedPtr.isEmpty() && mappedPtr == ptr)
                return mpNum;
        }

        return 0;
    }

    void ActorSync::sendAuthoritativeActorUpdates(const std::string& cellId, CellRuntime& cell, float dt)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return;

        if (cell.outboundCellId.empty())
            cell.outboundCellId = cellId;
        else if (cell.outboundCellId != cellId)
        {
            cell.outboundCellId = cellId;
            cell.initialListSent = false;
            cell.positionSendTimer = 0.f;
            cell.actors.clear();
        }

        MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
        MWWorld::CellStore* targetCell = findActiveCellById(worldImp, cell.outboundCellId);
        if (!targetCell)
            return;

        ActorList outgoing;
        outgoing.cellId = cell.outboundCellId;
        outgoing.isAuthority = true;
        outgoing.authorityGuid = Main::get().getPlayerSync().localPlayer().guid;
        std::unordered_set<uint32_t> sentServerSpawnedMpNums;

        for (auto& [actorKey, actor] : cell.actors)
        {
            if (actor.state.mpNum != 0)
                resolveActorBinding(cell.outboundCellId, actor);
        }

        MWWorld::Ptr playerPtr = world->getPlayerPtr();
        targetCell->forEach([&](MWWorld::Ptr ptr) -> bool
        {
            if (ptr.isEmpty() || !ptr.getClass().isActor())
                return true;
            if (ptr == playerPtr)
                return true;
            if (isNetworkPlayerProxy(ptr))
                return true;
            if (!ptr.getRefData().isEnabled())
                return true;

            BaseActor actor;
            actor.refId = ptr.getCellRef().getRefId().serializeText();
            actor.refNum = ptr.getCellRef().getRefNum().mIndex;
            actor.cellId = cell.outboundCellId;
            const uint32_t mappedMpNum = mappedMpNumForPtr(cell.outboundCellId, ptr);
            if (mappedMpNum != 0)
            {
                if (!sentServerSpawnedMpNums.insert(mappedMpNum).second)
                    return true;
                actor.mpNum = mappedMpNum;
                actor.refNum = 0;
            }

            const auto& pos = ptr.getRefData().getPosition();
            for (int i = 0; i < 3; ++i)
            {
                actor.position.pos[i] = pos.pos[i];
                actor.position.rot[i] = pos.rot[i];
            }

            MWMechanics::CreatureStats& stats = ptr.getClass().getCreatureStats(ptr);
            MWMechanics::Movement& movement = ptr.getClass().getMovementSettings(ptr);
            actor.isDead = stats.isDead();
            actor.animFlags.movementFlags = 0;
            if (!actor.isDead && stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Run))
                actor.animFlags.movementFlags |= AnimFlags::MF_RUN;
            if (!actor.isDead && stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Sneak))
                actor.animFlags.movementFlags |= AnimFlags::MF_SNEAK;
            // Capture knockout/knockdown/recovery hit-state so non-authority
            // clients can replicate the correct hit animations, fatigue sign,
            // and damage type (hand-to-hand switches to health damage when
            // the victim is knocked down).
            if (!actor.isDead && stats.getKnockedDown())
                actor.animFlags.movementFlags |= AnimFlags::MF_KNOCKED_DOWN;
            if (!actor.isDead && (stats.getFatigue().getCurrent() < 0.f || stats.getFatigue().getBase() == 0.f))
                actor.animFlags.movementFlags |= AnimFlags::MF_KNOCKED_OUT;
            if (!actor.isDead && stats.getHitRecovery())
                actor.animFlags.movementFlags |= AnimFlags::MF_RECOVERY;
            // Capture body-relative movement axes so the remote CC can classify
            // walk/run/strafe animations without needing a separate AnimFlags packet.
            actor.animFlags.animFwd  = actor.isDead ? 0.f : movement.mPosition[1];
            actor.animFlags.animSide = actor.isDead ? 0.f : movement.mPosition[0];
            actor.ai.type = BaseActor::AIAction::Type::None;
            MWMechanics::AiSequence& aiSequence = stats.getAiSequence();
            if (!actor.isDead && aiSequence.isInCombat())
                actor.ai.type = BaseActor::AIAction::Type::Combat;
            // Use 0.1f threshold to filter out head-tracking micro-inputs that
            // would otherwise trigger locomotion animation on remote clients.
            actor.isMoving = !actor.isDead && (std::abs(movement.mPosition[0]) > 0.1f
                || std::abs(movement.mPosition[1]) > 0.1f
                || std::abs(actor.velocity.linear[0]) > 1.f
                || std::abs(actor.velocity.linear[1]) > 1.f);
            actor.hasWeaponDrawn = !actor.isDead && stats.getDrawState() == MWMechanics::DrawState::Weapon;
            actor.hasSpellReadied = !actor.isDead && stats.getDrawState() == MWMechanics::DrawState::Spell;
            actor.isAttackingOrCasting = !actor.isDead && stats.getAttackingOrSpell();
            actor.dynamicStats.health.base = stats.getHealth().getBase();
            actor.dynamicStats.health.current = stats.getHealth().getCurrent();
            actor.dynamicStats.health.mod = stats.getHealth().getModifier();
            actor.dynamicStats.magicka.base = stats.getMagicka().getBase();
            actor.dynamicStats.magicka.current = stats.getMagicka().getCurrent();
            actor.dynamicStats.magicka.mod = stats.getMagicka().getModifier();
            actor.dynamicStats.fatigue.base = stats.getFatigue().getBase();
            actor.dynamicStats.fatigue.current = stats.getFatigue().getCurrent();
            actor.dynamicStats.fatigue.mod = stats.getFatigue().getModifier();
            // Capture equipped items for visual sync (weapon in hand etc.)
            if (ptr.getClass().hasInventoryStore(ptr))
            {
                const MWWorld::InventoryStore& inv = ptr.getClass().getInventoryStore(ptr);
                actor.equipment.clear();
                for (int eqSlot = 0; eqSlot < MWWorld::InventoryStore::Slots; ++eqSlot)
                {
                    auto eqIt = inv.getSlot(eqSlot);
                    if (eqIt == inv.end())
                        continue;
                    EquipmentItem eq;
                    eq.slot = eqSlot;
                    eq.item.refId = eqIt->getCellRef().getRefId().serializeText();
                    eq.item.count = 1; // equipped items always represent a single instance in the slot
                    eq.item.charge = static_cast<int>(eqIt->getCellRef().getCharge());
                    actor.equipment.push_back(eq);
                }
            }

            // Capture death animation group (for death sync)
            if (actor.isDead)
            {
                if (auto* baseNode = ptr.getRefData().getBaseNode())
                {
                    std::string deathGroup;
                    baseNode->getUserValue("mp_death_anim_group", deathGroup);
                    actor.deathAnimGroup = deathGroup;
                }
            }

            // Capture the current animation group being played on the lower body.
            // Non-authority clients use this to replicate the exact animation (walk,
            // idle variant, etc.) rather than relying on mPosition → CharacterController
            // inference which is unreliable when NPC scripts re-add AI packages each frame.
            if (!actor.isDead)
            {
                if (MWRender::Animation* animObj = world->getAnimation(ptr))
                {
                    std::string_view activeGrp = animObj->getActiveGroup(MWRender::BoneGroup_LowerBody);
                    actor.animFlags.currentAnimGroup = std::string(activeGrp);
                }
            }

            outgoing.actors.push_back(std::move(actor));
            return true;
        });

        std::unordered_set<std::string> outgoingKeys;
        outgoingKeys.reserve(outgoing.actors.size());
        for (const auto& actorState : outgoing.actors)
            outgoingKeys.insert(makeActorKey(actorState));

        bool actorKeySetChanged = !cell.initialListSent || outgoingKeys.size() != cell.actors.size();
        if (!actorKeySetChanged)
        {
            for (const auto& [actorKey, runtime] : cell.actors)
            {
                if (outgoingKeys.find(actorKey) == outgoingKeys.end())
                {
                    actorKeySetChanged = true;
                    break;
                }
            }
        }

        if (actorKeySetChanged)
        {
            PacketActorList listPacket;
            listPacket.setActorList(&outgoing);
            mClient.sendReliable(listPacket.encode());
            const bool wasInitialList = !cell.initialListSent;
            cell.initialListSent = true;
            Log(Debug::Info) << "[MP] ActorSync: sent ActorList for " << cell.outboundCellId
                             << " actors=" << outgoing.actors.size()
                             << " reason=" << (wasInitialList ? "initial" : "keyset");
        }

        cell.positionSendTimer += dt;
        if (cell.positionSendTimer >= 0.05f)
        {
            cell.positionSendTimer = 0.f;

            PacketActorPosition positionPacket;
            positionPacket.setActorList(&outgoing);
            mClient.sendUnreliable(positionPacket.encode());
        }

        // Detect health changes and deaths, send reliable packets when they occur.
        ActorList statsUpdate;
        statsUpdate.cellId = outgoing.cellId;
        statsUpdate.isAuthority = true;
        statsUpdate.authorityGuid = outgoing.authorityGuid;

        ActorList deathUpdate;
        deathUpdate.cellId = outgoing.cellId;
        deathUpdate.isAuthority = true;
        deathUpdate.authorityGuid = outgoing.authorityGuid;

        ActorList attackUpdate;
        attackUpdate.cellId = outgoing.cellId;
        attackUpdate.isAuthority = true;
        attackUpdate.authorityGuid = outgoing.authorityGuid;

        ActorList castUpdate;
        castUpdate.cellId = outgoing.cellId;
        castUpdate.isAuthority = true;
        castUpdate.authorityGuid = outgoing.authorityGuid;

        ActorList equipmentUpdate;
        equipmentUpdate.cellId = outgoing.cellId;
        equipmentUpdate.isAuthority = true;
        equipmentUpdate.authorityGuid = outgoing.authorityGuid;

        for (const auto& actor : outgoing.actors)
        {
            const std::string key = makeActorKey(actor);
            const auto prevIt = cell.actors.find(key);

            const float prevHealth = (prevIt != cell.actors.end())
                ? prevIt->second.state.dynamicStats.health.current : actor.dynamicStats.health.base;
            // Send stats whenever health changes by more than 0.5 HP or on first send.
            const float healthDelta = std::abs(actor.dynamicStats.health.current - prevHealth);
            if (healthDelta > 0.5f || prevIt == cell.actors.end())
            {
                Log(Debug::Info) << "[MP] ActorSync: health update for " << actor.refId
                                 << " prev=" << prevHealth
                                 << " cur=" << actor.dynamicStats.health.current
                                 << " dead=" << actor.isDead;
                statsUpdate.actors.push_back(actor);
            }

            // Send a death packet when an actor is dead and we haven't sent one yet.
            // We use deathPacketSent instead of simple edge detection because the
            // MP update runs BEFORE the mechanics update — on the first frame the
            // actor is dead, playRandomDeath() hasn't run yet so mp_death_anim_group
            // is empty. We defer until the anim group is available.
            {
                const bool alreadySent = (prevIt != cell.actors.end()) && prevIt->second.deathPacketSent;
                if (actor.isDead && !alreadySent)
                {
                    // Read the death anim group from the base node.
                    std::string deathGroup = actor.deathAnimGroup;
                    if (deathGroup.empty() && prevIt != cell.actors.end() && !prevIt->second.boundActor.isEmpty())
                    {
                        if (auto* baseNode = prevIt->second.boundActor.getRefData().getBaseNode())
                            baseNode->getUserValue("mp_death_anim_group", deathGroup);
                    }

                    if (!deathGroup.empty())
                    {
                        Log(Debug::Info) << "[MP] ActorSync: death send for " << actor.refId
                                         << " anim='" << deathGroup << "'";
                        BaseActor deathActor = actor;
                        deathActor.deathAnimGroup = deathGroup;
                        deathUpdate.actors.push_back(std::move(deathActor));
                    }
                    else
                    {
                        Log(Debug::Info) << "[MP] ActorSync: death deferred for " << actor.refId
                                         << " (anim group not yet set)";
                    }
                }
            }

            // --- Attack edge detection ---
            const bool prevAttacking = (prevIt != cell.actors.end())
                && prevIt->second.state.isAttackingOrCasting;
            if (actor.isAttackingOrCasting && !prevAttacking)
            {
                Log(Debug::Info) << "[MP] ActorSync: attack edge detected for " << actor.refId
                                 << " aiType=" << static_cast<int>(actor.ai.type)
                                 << " attackAnim=" << actor.attack.attackAnimation;
                BaseActor atkActor = actor;
                atkActor.attack.pressed = true;
                // Read the actual attack type from the CharacterController's
                // mp_attack_type user value (set during updateWeaponState()).
                atkActor.attack.attackAnimation = "slash"; // fallback
                {
                    // Find the actual actor ptr to read the base node user value
                    MWWorld::Ptr atkPtr;
                    try { atkPtr = world->getPtr(ESM::RefId::stringRefId(actor.refId), false); }
                    catch (...) {}
                    if (!atkPtr.isEmpty())
                    {
                        if (auto* baseNode = atkPtr.getRefData().getBaseNode())
                        {
                            std::string atkType;
                            if (baseNode->getUserValue("mp_attack_type", atkType) && !atkType.empty())
                                atkActor.attack.attackAnimation = atkType;
                        }
                    }
                }

                attackUpdate.actors.push_back(atkActor);
            }

            // --- Cast start edge detection ---
            MWWorld::Ptr castPtr;
            try { castPtr = world->getPtr(ESM::RefId::stringRefId(actor.refId), false); }
            catch (...) {}
            if (!castPtr.isEmpty())
            {
                if (auto* baseNode = castPtr.getRefData().getBaseNode())
                {
                    bool castPending = false;
                    if (baseNode->getUserValue("mp_cast_pending", castPending) && castPending)
                    {
                        baseNode->setUserValue("mp_cast_pending", false);

                        std::string spellId;
                        std::string castAnim;
                        baseNode->getUserValue("mp_cast_spell_id", spellId);
                        baseNode->getUserValue("mp_cast_anim", castAnim);

                        if (!spellId.empty())
                        {
                            BaseActor castActor = actor;
                            castActor.cast.spellId = spellId;
                            castActor.cast.castAnimation = castAnim;
                            castActor.cast.release = false;
                            castActor.cast.success = true;
                            castActor.cast.targetGuid = 0;
                            castActor.cast.targetRefId.clear();
                            castUpdate.actors.push_back(std::move(castActor));

                            Log(Debug::Info) << "[MP] ActorSync: cast start edge detected for " << actor.refId
                                             << " spell='" << spellId
                                             << "' castAnim='" << castAnim << "'";
                        }
                    }
                }
            }

            // --- Equipment change detection ---
            bool equipmentChanged = false;
            if (prevIt == cell.actors.end())
            {
                // First time seeing this actor: always send if it has equipment
                equipmentChanged = !actor.equipment.empty();
            }
            else
            {
                const auto& prevEquip = prevIt->second.state.equipment;
                if (prevEquip.size() != actor.equipment.size())
                    equipmentChanged = true;
                else
                {
                    for (size_t i = 0; i < actor.equipment.size(); ++i)
                    {
                        if (actor.equipment[i].slot != prevEquip[i].slot ||
                            actor.equipment[i].item.refId != prevEquip[i].item.refId)
                        {
                            equipmentChanged = true;
                            break;
                        }
                    }
                }
            }
            if (equipmentChanged)
                equipmentUpdate.actors.push_back(actor);
        }

        if (!statsUpdate.actors.empty())
        {
            PacketActorStatsDynamic statsPacket;
            statsPacket.setActorList(&statsUpdate);
            mClient.sendReliable(statsPacket.encode());
        }

        if (!deathUpdate.actors.empty())
        {
            PacketActorDeath deathPacket;
            deathPacket.setActorList(&deathUpdate);
            mClient.sendReliable(deathPacket.encode());
            Log(Debug::Info) << "[MP] ActorSync: sent ActorDeath packet with " << deathUpdate.actors.size() << " actors";
        }

        if (!attackUpdate.actors.empty())
        {
            PacketActorAttack attackPkt;
            attackPkt.setActorList(&attackUpdate);
            mClient.sendReliable(attackPkt.encode());
            Log(Debug::Info) << "[MP] ActorSync: sent ActorAttack packet with " << attackUpdate.actors.size() << " actors";
        }

        if (!castUpdate.actors.empty())
        {
            PacketActorCast castPkt;
            castPkt.setActorList(&castUpdate);
            mClient.sendReliable(castPkt.encode());
            Log(Debug::Info) << "[MP] ActorSync: sent ActorCast packet with " << castUpdate.actors.size() << " actors";
        }

        if (!equipmentUpdate.actors.empty())
        {
            PacketActorEquipment equipPkt;
            equipPkt.setActorList(&equipmentUpdate);
            mClient.sendReliable(equipPkt.encode());
        }

        // Build a set of actors whose death packets were actually sent this frame.
        std::unordered_set<std::string> deathSentKeys;
        for (const auto& deathActor : deathUpdate.actors)
            deathSentKeys.insert(makeActorKey(deathActor));

        // Update in-place: preserve transient fields (lastAttackingOrCasting etc.)
        // and remove actors no longer present.
        {
            std::unordered_set<std::string> currentKeys;
            std::unordered_set<uint32_t> currentServerSpawnedMpNums;
            for (const auto& actorState : outgoing.actors)
            {
                const std::string key = makeActorKey(actorState);
                currentKeys.insert(key);
                if (actorState.mpNum != 0)
                    currentServerSpawnedMpNums.insert(actorState.mpNum);
                cell.actors[key].state = actorState;
                cell.actors[key].lastAttackingOrCasting = actorState.isAttackingOrCasting;
                // Mark deathPacketSent if we sent a death packet for this actor
                if (deathSentKeys.count(key))
                    cell.actors[key].deathPacketSent = true;
                // Reset deathPacketSent if the actor is no longer dead (resurrected)
                if (!actorState.isDead)
                    cell.actors[key].deathPacketSent = false;
            }
            for (auto it = cell.actors.begin(); it != cell.actors.end(); )
            {
                if (currentKeys.find(it->first) == currentKeys.end())
                {
                    if (it->second.state.mpNum != 0
                        && currentServerSpawnedMpNums.count(it->second.state.mpNum) == 0
                        && !it->second.boundActor.isEmpty())
                    {
                        forgetServerSpawnedActor(cell.outboundCellId, it->second.boundActor, it->second.state.mpNum);
                    }
                    it = cell.actors.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        cell.latest = outgoing;
    }

    bool ActorSync::resolveActorBinding(const std::string& cellId, ActorRuntime& actor)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return false;

        const bool expectsServerSpawn = actor.state.mpNum != 0;

        if (!actor.boundActor.isEmpty() && matchesActor(actor.boundActor, actor.state))
        {
            if ((!expectsServerSpawn || mappedMpNumForPtr(cellId, actor.boundActor) == actor.state.mpNum)
                && actor.boundActor.getCell() != nullptr)
            {
                if (cellIdForPtr(actor.boundActor) == cellId)
                    return true;
            }
        }

        if (expectsServerSpawn)
        {
            auto mappedIt = mServerSpawnedActorsByMpNum.find(actor.state.mpNum);
            if (mappedIt != mServerSpawnedActorsByMpNum.end())
            {
                if (!mappedIt->second.isEmpty()
                    && matchesActor(mappedIt->second, actor.state)
                    && cellIdForPtr(mappedIt->second) == cellId)
                {
                    actor.boundActor = mappedIt->second;
                    rememberServerSpawnedActor(cellId, actor.boundActor, actor.state.mpNum);
                    return true;
                }

                mServerSpawnedActorsByMpNum.erase(mappedIt);
            }
        }

        MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
        MWWorld::CellStore* targetCell = findActiveCellById(worldImp, cellId);
        if (!targetCell)
        {
            actor.boundActor = MWWorld::Ptr();
            return false;
        }

        MWWorld::Ptr found;
        targetCell->forEach([&](MWWorld::Ptr ptr) -> bool
        {
            if (!matchesActor(ptr, actor.state))
                return true;
            if (expectsServerSpawn && mappedMpNumForPtr(cellId, ptr) != actor.state.mpNum)
                return true;

            found = ptr;
            return false;
        });

        if (found.isEmpty() && !actor.boundActor.isEmpty()
            && (!expectsServerSpawn || mappedMpNumForPtr(cellId, actor.boundActor) == actor.state.mpNum))
        {
            actor.boundActor = world->moveObject(actor.boundActor, targetCell,
                osg::Vec3f(actor.state.position.pos[0], actor.state.position.pos[1], actor.state.position.pos[2]));
            found = actor.boundActor;
        }

        if (found.isEmpty() && expectsServerSpawn)
        {
            try
            {
                MWWorld::ManualRef ref(world->getStore(), ESM::RefId::stringRefId(actor.state.refId), 1);
                MWWorld::Ptr actorTemplate = ref.getPtr();
                if (actorTemplate.isEmpty() || !actorTemplate.getClass().isActor())
                {
                    Log(Debug::Warning) << "[MP] ActorSync: cannot spawn non-actor refId=" << actor.state.refId;
                    actor.boundActor = MWWorld::Ptr();
                    return false;
                }

                ESM::Position esmPos;
                for (int i = 0; i < 3; ++i)
                {
                    esmPos.pos[i] = actor.state.position.pos[i];
                    esmPos.rot[i] = actor.state.position.rot[i];
                }

                found = world->placeObject(actorTemplate, targetCell, esmPos);
                if (found.isEmpty())
                {
                    Log(Debug::Warning) << "[MP] ActorSync: placeObject failed for spawned actor refId="
                                        << actor.state.refId << " mpNum=" << actor.state.mpNum;
                    actor.boundActor = MWWorld::Ptr();
                    return false;
                }

                rememberServerSpawnedActor(cellId, found, actor.state.mpNum);
                Log(Debug::Info) << "[MP] ActorSync: spawned actor refId=" << actor.state.refId
                                 << " mpNum=" << actor.state.mpNum
                                 << " cell=" << cellId;
            }
            catch (const std::exception& e)
            {
                Log(Debug::Verbose) << "[MP] ActorSync: delaying actor spawn for refId=" << actor.state.refId
                                    << " mpNum=" << actor.state.mpNum
                                    << " reason=" << e.what();
                actor.boundActor = MWWorld::Ptr();
                return false;
            }
        }

        actor.boundActor = found;
        if (expectsServerSpawn)
            rememberServerSpawnedActor(cellId, actor.boundActor, actor.state.mpNum);
        return !actor.boundActor.isEmpty();
    }

    void ActorSync::applyBoundActorState(ActorRuntime& actor)
    {
        if (actor.boundActor.isEmpty())
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return;
        actor.animGroupHoldTimer = std::max(0.f, actor.animGroupHoldTimer - MWBase::Environment::get().getFrameDuration());

        // Mark this actor as a network-driven puppet so other systems
        // (e.g. updateContinuousVfx) can skip magnitude-based checks
        // that would incorrectly remove persistent spell VFX.
        if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
            baseNode->setUserValue("mp_remote_actor", true);

        const Position& pos = actor.state.position;
        // Pre-compute knock state from incoming flags so we can gate position
        // updates. A prone NPC must not be teleported by the authority's current
        // position (which may already be metres away after recovery) — doing so
        // causes the NPC to visibly slide across the floor while lying down.
        // We use the INCOMING flags (not lastApplied) so the snap back to the
        // authority's real position happens on the very first frame the authority
        // reports the NPC as no longer knocked, which coincides with the start
        // of the get-up animation.
        const uint32_t earlyHitFlags = actor.state.isDead ? 0
            : (actor.state.animFlags.movementFlags
                & (AnimFlags::MF_KNOCKED_DOWN | AnimFlags::MF_KNOCKED_OUT | AnimFlags::MF_RECOVERY));
        const bool skipPositionUpdate = actor.state.isDead
            || (earlyHitFlags & (AnimFlags::MF_KNOCKED_DOWN | AnimFlags::MF_KNOCKED_OUT)) != 0;
        if (!skipPositionUpdate)
        {
            actor.boundActor = world->moveObject(actor.boundActor, osg::Vec3f(pos.pos[0], pos.pos[1], pos.pos[2]));
            world->rotateObject(actor.boundActor, osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]));
        }

        MWMechanics::Movement& movement = actor.boundActor.getClass().getMovementSettings(actor.boundActor);
        // Gate locomotion on actual axis magnitude: head-tracking can set isMoving=true
        // with tiny mPosition values that would produce stepping-in-place on remote.
        // Only drive movement positions when there is a real locomotion input.
        const float fwdMag  = std::abs(actor.state.animFlags.animFwd);
        const float sideMag = std::abs(actor.state.animFlags.animSide);
        const bool shouldDriveLocomotion = !actor.state.isDead
            && actor.state.isMoving
            && (fwdMag > 0.1f || sideMag > 0.1f);
        movement.mPosition[0] = shouldDriveLocomotion ? actor.state.animFlags.animSide : 0.f;
        movement.mPosition[1] = shouldDriveLocomotion ? actor.state.animFlags.animFwd  : 0.f;
        movement.mPosition[2] = 0.f;
        movement.mRotation[0] = 0.f;
        movement.mRotation[1] = 0.f;
        movement.mRotation[2] = 0.f;
        movement.mIsStrafing = shouldDriveLocomotion
            && std::abs(actor.state.animFlags.animSide) > std::abs(actor.state.animFlags.animFwd) * 2.f;

        MWMechanics::CreatureStats& stats = actor.boundActor.getClass().getCreatureStats(actor.boundActor);

        // Freeze all vanilla AI on non-authority clients.
        // Fight=0 prevents spontaneous combat. Hello=0 prevents greeting/idle-walk packages.
        // clear() removes all running AI packages (wander, greet, combat, pursue) every frame
        // so the NPC is a pure puppet driven only by the authority's network state.
        stats.setAiSetting(MWMechanics::AiSetting::Fight, 0);
        stats.setAiSetting(MWMechanics::AiSetting::Hello, 0);
        const bool shouldForceCombatPresentation = actor.state.ai.type == BaseActor::AIAction::Type::Combat;
        stats.getAiSequence().clear();

        Log(Debug::Verbose) << "[MP] ActorSync::applyBound " << actor.state.refId
                            << " combat=" << shouldForceCombatPresentation
                            << " moving=" << actor.state.isMoving
                            << " fwd=" << actor.state.animFlags.animFwd
                            << " side=" << actor.state.animFlags.animSide
                            << " drawn=" << actor.state.hasWeaponDrawn
                            << " dead=" << actor.state.isDead
                            << " hp=" << actor.state.dynamicStats.health.current;

        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run,
            (actor.state.animFlags.movementFlags & AnimFlags::MF_RUN) != 0);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak,
            (actor.state.animFlags.movementFlags & AnimFlags::MF_SNEAK) != 0);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceJump,
            (actor.state.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceMoveJump,
            (actor.state.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0);

        const uint32_t hitFlags = actor.state.isDead ? 0
            : (actor.state.animFlags.movementFlags
                & (AnimFlags::MF_KNOCKED_DOWN | AnimFlags::MF_KNOCKED_OUT | AnimFlags::MF_RECOVERY));
        const bool hitFlagsChanged = (hitFlags != actor.lastAppliedHitFlags);
        const bool isKnockedOut = (hitFlags & AnimFlags::MF_KNOCKED_OUT) != 0;
        const bool isKnockedDown = (hitFlags & AnimFlags::MF_KNOCKED_DOWN) != 0;
        const bool isRecovering = (hitFlags & AnimFlags::MF_RECOVERY) != 0;
        const bool wasKnockedOut = (actor.lastAppliedHitFlags & AnimFlags::MF_KNOCKED_OUT) != 0;
        const bool leftKnockOut = wasKnockedOut && !isKnockedOut;

        if (isKnockedOut || isKnockedDown || isRecovering)
        {
            movement.mPosition[0] = 0.f;
            movement.mPosition[1] = 0.f;
            movement.mIsStrafing = false;

            if (isKnockedOut)
            {
                stats.setKnockedDown(true);
                stats.setHitRecovery(false);
                MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
                if (fatigue.getCurrent() >= 0.f)
                {
                    fatigue.setCurrent(-1.f, true);
                    stats.setFatigue(fatigue);
                }
                if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                    baseNode->setUserValue("mp_knockout_release_pending", false);
            }
            else if (hitFlagsChanged)
            {
                if (leftKnockOut)
                {
                    MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
                    if (fatigue.getCurrent() < 0.f)
                    {
                        fatigue.setCurrent(1.f, true);
                        stats.setFatigue(fatigue);
                    }

                    stats.setKnockedDown(false);
                    stats.setHitRecovery(false);
                    actor.animGroupHoldTimer = std::max(actor.animGroupHoldTimer, 0.6f);
                    if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                    {
                        baseNode->setUserValue("mp_recovery_anim_group", std::string());
                        baseNode->setUserValue("mp_knockout_release_pending", true);
                    }
                }
                else if (isRecovering)
                {
                    stats.setKnockedDown(false);
                    stats.setHitRecovery(true);
                    if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                        baseNode->setUserValue("mp_knockout_release_pending", false);
                }
                else if (isKnockedDown)
                {
                    stats.setKnockedDown(true);
                    stats.setHitRecovery(false);
                    if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                        baseNode->setUserValue("mp_knockout_release_pending", false);
                }
            }
        }
        else if (hitFlagsChanged)
        {
            stats.setKnockedDown(false);
            stats.setHitRecovery(false);
            MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
            if (fatigue.getCurrent() < 0.f)
            {
                fatigue.setCurrent(1.f, true);
                stats.setFatigue(fatigue);
            }
            if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
            {
                baseNode->setUserValue("mp_recovery_anim_group", std::string());
                // If the authority transitioned directly from KO → no flags
                // (skipping the intermediate KD-only "Phase B" that normally
                // carries wasKnockedOut && !isKnockedOut into the inner branch),
                // we still need to fire the get-up release so the knockout
                // animation seeks to "loop stop" immediately instead of waiting
                // for the current loop iteration to reach "stop" naturally.
                baseNode->setUserValue("mp_knockout_release_pending", wasKnockedOut);
            }
        }
        actor.lastAppliedHitFlags = hitFlags;

        // Safety net: even when hitFlagsChanged is false (authority was already
        // sending 0 flags), force-clear any locally-set knockdown/recovery.
        // This catches the case where local onHit() sets these states on a
        // puppet NPC between authority ticks — the authority never sent a
        // transition edge so hitFlagsChanged stays false, but the local state
        // is wrong and must be corrected.
        if (hitFlags == 0)
        {
            if (stats.getKnockedDown() || stats.getHitRecovery())
            {
                stats.setKnockedDown(false);
                stats.setHitRecovery(false);
                MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
                if (fatigue.getCurrent() < 0.f)
                {
                    fatigue.setCurrent(1.f, true);
                    stats.setFatigue(fatigue);
                }
            }
        }

        // Drive weapon/spell draw state directly from the authoritative network state.
        // Do NOT gate on shouldForceCombatPresentation (derived from ai.type) because
        // ai.type is only sent in the initial ActorList and never updated from position
        // packets — an NPC entering combat after the first snapshot would stay invisible.
        // hasWeaponDrawn / hasSpellReadied come from every 20 Hz position packet and
        // directly mirror stats.getDrawState() on the authority.
        MWMechanics::DrawState drawState = MWMechanics::DrawState::Nothing;
        if (actor.state.hasWeaponDrawn)
            drawState = MWMechanics::DrawState::Weapon;
        else if (actor.state.hasSpellReadied)
            drawState = MWMechanics::DrawState::Spell;
        stats.setDrawState(drawState);

        // Re-evaluate NPC equipment mesh whenever weapon visibility changes
        // (draw state Nothing→Weapon or Weapon→Nothing). Without this, the weapon
        // mesh set by equip() during the initial equipment packet is never shown/hidden
        // when the NPC later draws or sheathes the weapon.
        {
            const bool weaponVisible = (drawState == MWMechanics::DrawState::Weapon);
            if (weaponVisible != actor.lastWeaponVisible)
            {
                actor.lastWeaponVisible = weaponVisible;
                if (auto* npcAnim = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(actor.boundActor)))
                    npcAnim->equipmentChanged();
            }
        }

        // Pre-set death animation group on the base node BEFORE any health
        // change that could trigger playRandomDeath().  The StatsDynamic packet
        // can arrive before (or in the same frame as) the ActorDeath packet, and
        // setting health=0 via the stats path immediately triggers
        // kill() → playRandomDeath().  If mp_death_anim_group isn't on the node
        // yet, the CC picks a random animation, causing a mismatch between
        // clients.  By setting it here early, playRandomDeath() always finds the
        // synced group regardless of which code path triggers the death.
        if (actor.state.isDead && !stats.isDead() && !actor.state.deathAnimGroup.empty())
        {
            if (actor.appliedDeathAnimGroup.empty())
                actor.appliedDeathAnimGroup = actor.state.deathAnimGroup;
            if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                baseNode->setUserValue("mp_death_anim_group", actor.appliedDeathAnimGroup);
        }
        const bool waitingForRealtimeDeathPacket = actor.state.isDead
            && !stats.isDead()
            && !actor.deathFromRealtimePacket
            && actor.state.deathAnimGroup.empty();

        // When the actor is transitioning to dead and this is NOT a realtime
        // death event (e.g. ActorList load / late join), mark the death animation
        // as finished so playRandomDeath() jumps to the final corpse pose instead
        // of replaying the full death animation. Realtime deaths wait for the
        // ActorDeath packet so they can start from the synced animation group.
        if (actor.state.isDead && !stats.isDead() && !actor.deathFromRealtimePacket && !waitingForRealtimeDeathPacket)
            stats.setDeathAnimationFinished(true);

        const DynamicStats& dyn = actor.state.dynamicStats;
        const bool hasDynamicStats = actor.state.isDead || dyn.health.base != 0.f || dyn.health.current != 0.f
            || dyn.health.mod != 0.f || dyn.magicka.base != 0.f || dyn.magicka.current != 0.f
            || dyn.magicka.mod != 0.f || dyn.fatigue.base != 0.f || dyn.fatigue.current != 0.f
            || dyn.fatigue.mod != 0.f;

        if (hasDynamicStats)
        {
            MWMechanics::DynamicStat<float> health = stats.getHealth();
            health.setBase(dyn.health.base);
            health.setModifier(dyn.health.mod);
            const float syncedHealthCurrent = waitingForRealtimeDeathPacket
                ? std::max(health.getCurrent(), 1.f)
                : dyn.health.current;
            health.setCurrent(syncedHealthCurrent, true, true);
            stats.setHealth(health);

            MWMechanics::DynamicStat<float> magicka = stats.getMagicka();
            magicka.setBase(dyn.magicka.base);
            magicka.setModifier(dyn.magicka.mod);
            magicka.setCurrent(dyn.magicka.current, true, true);
            stats.setMagicka(magicka);

            MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
            fatigue.setBase(dyn.fatigue.base);
            fatigue.setModifier(dyn.fatigue.mod);
            // Only apply the cached authority fatigue if:
            //  (a) the NPC is not in KO state right now, AND
            //  (b) the cached value is non-negative.
            // Condition (b) is required because dyn.fatigue.current is set from the
            // last StatsDynamic packet, which is only sent on *health* changes.
            // If the NPC was knocked out and damaged in the same window, the cached
            // value is a large negative number from the knockdown period. Without
            // this guard, the hitFlagsChanged branch's fatigue restore (→1) is
            // immediately overwritten with the stale negative value in the same
            // applyBoundActorState call, keeping knockout = (fatigue<0) = true and
            // pinning the CharacterController in the KO animation loop indefinitely.
            if (!isKnockedOut && dyn.fatigue.current >= 0.f)
                fatigue.setCurrent(dyn.fatigue.current, true, true);
            stats.setFatigue(fatigue);
        }

        // Apply equipment from actor state to bound actor's InventoryStore.
        // This makes NPC-held weapons visible on non-authority clients.
        // Only notify the renderer when a slot actually changes to avoid rebuilding
        // the NPC mesh every frame.
        if (!actor.state.equipment.empty() && actor.boundActor.getClass().hasInventoryStore(actor.boundActor))
        {
            MWWorld::InventoryStore& inv = actor.boundActor.getClass().getInventoryStore(actor.boundActor);
            bool anyEquipmentChanged = false;
            for (const auto& eq : actor.state.equipment)
            {
                const int eqSlot = eq.slot;
                const std::string& targetRefId = eq.item.refId;
                if (eqSlot < 0 || eqSlot >= MWWorld::InventoryStore::Slots || targetRefId.empty())
                    continue;

                MWWorld::ContainerStoreIterator current = inv.getSlot(eqSlot);
                if (current != inv.end() &&
                    current->getCellRef().getRefId().serializeText() == targetRefId)
                    continue;

                if (current != inv.end())
                    inv.unequipSlot(eqSlot);

                const ESM::RefId itemId = ESM::RefId::deserializeText(targetRefId);
                MWWorld::ContainerStoreIterator found = inv.end();
                for (auto it = inv.begin(); it != inv.end(); ++it)
                {
                    if (it->getCellRef().getRefId() == itemId)
                    {
                        found = it;
                        break;
                    }
                }
                if (found == inv.end())
                    found = inv.MWWorld::ContainerStore::add(itemId, 1);

                if (found != inv.end())
                {
                    inv.equip(eqSlot, found);
                    anyEquipmentChanged = true;
                }
            }
            if (anyEquipmentChanged)
            {
                if (auto* anim = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(actor.boundActor)))
                    anim->equipmentChanged();
            }
        }

        if (actor.state.isDead && !waitingForRealtimeDeathPacket)
        {
            if (!stats.isDead())
            {
                MWMechanics::DynamicStat<float> deadHealth = stats.getHealth();
                deadHealth.setCurrent(0.f);
                // mp_death_anim_group was already set in the early pre-death block
                // above (before stats application).  Re-set it here for the rare case
                // where only the ActorDeath packet arrived (no prior StatsDynamic).
                if (!actor.state.deathAnimGroup.empty())
                {
                    if (actor.appliedDeathAnimGroup.empty())
                        actor.appliedDeathAnimGroup = actor.state.deathAnimGroup;
                    if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                        baseNode->setUserValue("mp_death_anim_group", actor.appliedDeathAnimGroup);
                }
                if (!actor.deathFromRealtimePacket)
                    stats.setDeathAnimationFinished(true);
                stats.setHealth(deadHealth);
                Log(Debug::Info) << "[MP] ActorSync: applying death for " << actor.state.refId
                                 << " realtime=" << actor.deathFromRealtimePacket
                                 << " deathAnim='" << actor.appliedDeathAnimGroup << "'"
                                 << " alreadyApplied=" << actor.deathAlreadyApplied;
            }

            // Late death anim group arrival: the actor is already dead (kill()
            // was called by the mechanics manager in a previous frame), but the
            // ActorDeath packet with the synced animation group just arrived.
            // Replay the correct death animation so both clients match.
            if (stats.isDead() && actor.deathFromRealtimePacket
                && !actor.state.deathAnimGroup.empty()
                && actor.appliedDeathAnimGroup != actor.state.deathAnimGroup)
            {
                actor.appliedDeathAnimGroup = actor.state.deathAnimGroup;
                if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                    baseNode->setUserValue("mp_death_anim_group", actor.appliedDeathAnimGroup);
                stats.setDeathAnimationFinished(false);

                // Directly play the synced death animation from the start.
                if (MWRender::Animation* animation = world->getAnimation(actor.boundActor))
                {
                    animation->play(actor.appliedDeathAnimGroup,
                        MWRender::AnimPriority(static_cast<int>(MWMechanics::Priority_Death)),
                        MWRender::BlendMask_All,
                        false,  // not looping
                        1.f,
                        "start", "stop",
                        0.f,    // startpoint = beginning
                        0u);    // no loops
                }
                Log(Debug::Info) << "[MP] ActorSync: late death anim replay for " << actor.state.refId
                                 << " anim='" << actor.appliedDeathAnimGroup << "'";
            }

            if (actor.deathFromRealtimePacket)
                stats.setDeathAnimationFinished(false);
            stats.setAttackingOrSpell(false);
            stats.setDrawState(MWMechanics::DrawState::Nothing);
            // Remove persistent spell VFX (Shield bubble, etc.) on death.
            if (MWRender::Animation* anim = world->getAnimation(actor.boundActor))
                anim->removeEffects();
            actor.lastAppliedAnimGroup.clear();
            actor.lastAppliedHitFlags = 0;
            actor.lastAttackPressed = false;
            actor.pendingAttack = false;
            actor.animGroupHoldTimer = 0.f;
            actor.deathAlreadyApplied = true;
            actor.deathFromRealtimePacket = false;
        }
        else if (stats.isDead())
        {
            if (MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager())
                mechanics->resurrect(actor.boundActor);
            actor.lastAppliedAnimGroup.clear();
            actor.lastAppliedHitFlags = 0;
            actor.lastAttackPressed = false;
            actor.appliedDeathAnimGroup.clear();
            actor.pendingAttack = false;
            actor.animGroupHoldTimer = 0.f;
        }

        const bool suppressLowerBodyGroupSync = hitFlags != 0 || actor.animGroupHoldTimer > 0.f || actor.pendingAttack;

        if (!actor.state.isDead && !suppressLowerBodyGroupSync)
        {
            const std::string& newGrp = actor.state.animFlags.currentAnimGroup;
            if (!newGrp.empty() && newGrp != actor.lastAppliedAnimGroup)
            {
                if (MWRender::Animation* animObj = world->getAnimation(actor.boundActor))
                {
                    const std::string& oldGrp = actor.lastAppliedAnimGroup;

                    auto classifyLoco = [](const std::string& g) -> bool {
                        return g.find("walk") != std::string::npos
                            || g.find("run")  != std::string::npos
                            || g.find("swim") != std::string::npos
                            || g.find("sneak") != std::string::npos
                            || g.find("turn") != std::string::npos;
                    };

                    const bool newIsLoco = classifyLoco(newGrp);
                    const bool oldIsLoco = !oldGrp.empty() && classifyLoco(oldGrp);
                    const bool newIsHitState = newGrp.find("hit") != std::string::npos
                        || newGrp.find("knock") != std::string::npos;
                    const bool oldIsHitState = !oldGrp.empty()
                        && (oldGrp.find("hit") != std::string::npos || oldGrp.find("knock") != std::string::npos);

                    if (!oldGrp.empty() && oldGrp != newGrp)
                    {
                        if (oldIsLoco || oldIsHitState || (!oldGrp.empty() && oldGrp != "idle" && !newIsLoco))
                            animObj->disable(oldGrp);
                    }

                    const bool isBaseIdle = (newGrp == "idle" || newGrp == "idleswim" || newGrp == "idlesneak");
                    if (!isBaseIdle)
                    {
                        int prio = static_cast<int>(MWMechanics::Priority_WeaponLowerBody);
                        if (newIsLoco)
                            prio = static_cast<int>(MWMechanics::Priority_Movement);
                        else if (newIsHitState)
                            prio = static_cast<int>(MWMechanics::Priority_Knockdown);

                        animObj->play(newGrp,
                            MWRender::AnimPriority(prio),
                            MWRender::BlendMask_All,
                            false,
                            1.f,
                            "start",
                            "stop",
                            0.f,
                            std::numeric_limits<uint32_t>::max(),
                            true);

                        Log(Debug::Info) << "[MP] ActorSync: anim group -> '" << newGrp
                            << "' (was '" << oldGrp << "') prio=" << prio
                            << " actor=" << actor.state.refId;
                    }
                    else
                    {
                        Log(Debug::Info) << "[MP] ActorSync: anim group -> '" << newGrp
                            << "' (CC-managed) actor=" << actor.state.refId;
                    }
                    actor.lastAppliedAnimGroup = newGrp;
                }
            }
        }

        if (actor.pendingBoltTimer > 0.f)
        {
            actor.pendingBoltTimer -= MWBase::Environment::get().getFrameDuration();
            if (actor.pendingBoltTimer <= 0.f)
            {
                actor.pendingBoltTimer = -1.f;
                if (!actor.pendingBoltSpellId.empty() && !actor.boundActor.isEmpty())
                {
                    const auto& store = *MWBase::Environment::get().getESMStore();
                    const ESM::RefId spellRefId = ESM::RefId::stringRefId(actor.pendingBoltSpellId);
                    const ESM::Spell* spell = store.get<ESM::Spell>().search(spellRefId);
                    if (spell)
                    {
                        osg::Vec3f aimDir(0.f, 1.f, 0.f);
                        const float yaw = actor.boundActor.getRefData().getPosition().rot[2];
                        aimDir = osg::Vec3f(std::sin(-yaw), std::cos(-yaw), 0.f);
                        world->launchMagicBolt(spellRefId, actor.boundActor, aimDir, ESM::RefNum{}, true);
                    }
                }
                actor.pendingBoltSpellId.clear();
            }
        }

        if (actor.pendingAnimPlay && !actor.state.animPlay.groupName.empty())
        {
            if (MWRender::Animation* animation = world->getAnimation(actor.boundActor))
            {
                animation->play(actor.state.animPlay.groupName,
                    MWRender::AnimPriority(actor.state.animPlay.priority),
                    MWRender::BlendMask_All,
                    true,
                    1.f,
                    actor.state.animPlay.startKey,
                    actor.state.animPlay.stopKey,
                    0.f,
                    static_cast<uint32_t>(std::max(0, actor.state.animPlay.loops)));
            }
            actor.pendingAnimPlay = false;
        }

        if (actor.pendingAttack)
        {
            if (hitFlags == 0 && actor.animGroupHoldTimer <= 0.f)
            {
                const std::string& atkType = actor.state.attack.attackAnimation;
                if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                    baseNode->setUserValue("mp_attack_type", atkType);

                const bool pressedChanged = (actor.state.attack.pressed != actor.lastAttackPressed);
                actor.lastAttackPressed = actor.state.attack.pressed;

                if (pressedChanged && actor.state.attack.pressed)
                {
                    if (stats.getDrawState() != MWMechanics::DrawState::Weapon)
                        stats.setDrawState(MWMechanics::DrawState::Weapon);

                    if (MWRender::Animation* animation = world->getAnimation(actor.boundActor))
                    {
                        int weaponType = ESM::Weapon::None;
                        if (actor.boundActor.getClass().hasInventoryStore(actor.boundActor))
                        {
                            MWWorld::InventoryStore& inv = actor.boundActor.getClass().getInventoryStore(actor.boundActor);
                            auto weaponSlot = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                            if (weaponSlot != inv.end() && weaponSlot->getType() == ESM::Weapon::sRecordId)
                            {
                                const ESM::Weapon* weapon = weaponSlot->get<ESM::Weapon>()->mBase;
                                weaponType = weapon->mData.mType;
                            }
                        }

                        std::string weaponGroup = std::string(MWMechanics::getWeaponType(weaponType)->mLongGroup);
                        if (weaponGroup.empty())
                            weaponGroup = "handtohand";

                        std::string attackType = atkType.empty() ? std::string("slash") : std::string(atkType);

                        animation->disable(weaponGroup);
                        animation->play(weaponGroup,
                            MWRender::AnimPriority(static_cast<int>(MWMechanics::Priority_Weapon)),
                            MWRender::BlendMask_All,
                            true,
                            1.f,
                            attackType + " start",
                            attackType + " large follow start",
                            0.f,
                            0u);

                        Log(Debug::Info) << "[MP] ActorSync: playing attack anim group='" << weaponGroup
                                         << "' type='" << attackType << "' for " << actor.state.refId;
                    }

                    actor.animGroupHoldTimer = std::max(actor.animGroupHoldTimer, 0.65f);
                }

                stats.setAttackingOrSpell(actor.state.attack.pressed);
                actor.pendingAttack = false;
            }
        }

        if (actor.pendingCast)
        {
            const CastSpell& cs = actor.state.cast;
            if (!cs.release && !cs.castAnimation.empty())
            {
                Log(Debug::Info) << "[MP] ActorSync: cast start playback actor=" << actor.state.refId
                                 << " spell='" << cs.spellId << "' castAnim='" << cs.castAnimation << "'";
                if (MWRender::Animation* animation = world->getAnimation(actor.boundActor))
                {
                    animation->play("spellcast",
                        MWRender::AnimPriority(7),
                        MWRender::BlendMask_UpperBody,
                        true,
                        1.f,
                        cs.castAnimation + " start",
                        cs.castAnimation + " stop",
                        0.f,
                        0u);
                }

                if (!cs.spellId.empty())
                {
                    auto resolved = resolveCastSource(cs.spellId);
                    if (resolved.spell && world->getAnimation(actor.boundActor))
                    {
                        MWMechanics::CastSpell castHelper(actor.boundActor, actor.boundActor, false);
                        castHelper.playSpellCastingEffects(resolved.spell);
                    }
                    else if (resolved.enchantment && world->getAnimation(actor.boundActor))
                    {
                        MWMechanics::CastSpell castHelper(actor.boundActor, actor.boundActor, false);
                        castHelper.playSpellCastingEffects(resolved.enchantment);
                    }
                }
            }
            else if (cs.release && !cs.spellId.empty())
            {
                Log(Debug::Info) << "[MP] ActorSync: cast release playback actor=" << actor.state.refId
                                 << " spell='" << cs.spellId << "' castAnim='" << cs.castAnimation
                                 << "' target='" << cs.targetRefId << "'";
                if (!cs.castAnimation.empty())
                {
                    if (MWRender::Animation* animation = world->getAnimation(actor.boundActor))
                    {
                        animation->play("spellcast",
                            MWRender::AnimPriority(7),
                            MWRender::BlendMask_UpperBody,
                            true,
                            1.f,
                            cs.castAnimation + " start",
                            cs.castAnimation + " stop",
                            0.f,
                            0u);
                    }
                }

                auto resolved = resolveCastSource(cs.spellId);
                // Use the effect list from whichever source resolved.
                const std::vector<ESM::IndexedENAMstruct>* effectList = nullptr;
                if (resolved.spell)
                    effectList = &resolved.spell->mEffects.mList;
                else if (resolved.enchantment)
                    effectList = &resolved.enchantment->mEffects.mList;

                // Play casting hand-glow
                if (effectList && world->getAnimation(actor.boundActor))
                {
                    Log(Debug::Info) << "[MP] ActorSync: play cast glow actor=" << actor.state.refId
                                     << " spell='" << cs.spellId << "'"
                                     << " resolved=" << (resolved.spell ? "spell" : "enchantment");
                    MWMechanics::CastSpell castHelper(actor.boundActor, actor.boundActor, false);
                    if (resolved.spell)
                        castHelper.playSpellCastingEffects(resolved.spell);
                    else
                        castHelper.playSpellCastingEffects(resolved.enchantment);
                }

                // Play self-range body VFX (restoration glow, etc.)
                if (effectList && !actor.boundActor.isEmpty())
                {
                    const auto& effectStore = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>();
                    for (const auto& effectInfo : *effectList)
                    {
                        if (effectInfo.mData.mRange != ESM::RT_Self)
                            continue;
                        const ESM::MagicEffect* me = effectStore.search(effectInfo.mData.mEffectID);
                        if (me)
                        {
                            Log(Debug::Info) << "[MP] ActorSync: play self VFX actor=" << actor.state.refId
                                             << " spell='" << cs.spellId << "' effect=" << me->mId.getRefIdString();
                            MWMechanics::playEffects(actor.boundActor, *me);
                        }
                    }
                }

                // Play touch-range VFX on the resolved target actor. Player targets
                // can arrive as targetGuid and are not present in mCells[cell].actors.
                if (effectList && (cs.targetGuid != 0 || !cs.targetRefId.empty()))
                {
                    MWWorld::Ptr targetActor;
                    MWBase::World* targetWorld = MWBase::Environment::get().getWorld();
                    if (targetWorld && cs.targetGuid != 0)
                    {
                        if (cs.targetGuid == Main::get().getPlayerSync().localPlayer().guid)
                            targetActor = targetWorld->getPlayerPtr();
                        else if (auto* remotePlayer = Main::get().getPlayerList().getPlayer(cs.targetGuid))
                            targetActor = remotePlayer->getNpcPtr();
                    }

                    if (targetActor.isEmpty() && targetWorld && !cs.targetRefId.empty())
                    {
                        MWWorld::Ptr localPlayer = targetWorld->getPlayerPtr();
                        if (!localPlayer.isEmpty()
                            && localPlayer.getCellRef().getRefId().serializeText() == cs.targetRefId)
                        {
                            targetActor = localPlayer;
                        }
                    }

                    if (targetActor.isEmpty() && !cs.targetRefId.empty())
                    {
                        for (auto& [key, rt] : mCells[actor.state.cellId].actors)
                        {
                            if (rt.state.refId == cs.targetRefId && !rt.boundActor.isEmpty())
                            {
                                targetActor = rt.boundActor;
                                break;
                            }
                        }
                    }

                    if (targetActor.isEmpty() && targetWorld && !cs.targetRefId.empty())
                    {
                        try
                        {
                            targetActor = targetWorld->getPtr(ESM::RefId::stringRefId(cs.targetRefId), false);
                        }
                        catch (...) {}
                    }

                    if (!targetActor.isEmpty())
                    {
                        if (targetWorld && targetActor == targetWorld->getPlayerPtr())
                        {
                            MWMechanics::CastSpell castHelper(actor.boundActor, targetActor, false, false);
                            castHelper.mId = ESM::RefId::stringRefId(cs.spellId);
                            castHelper.mSourceName = resolved.spell ? resolved.spell->mName : cs.spellId;
                            castHelper.mHitPosition = targetActor.getRefData().getPosition().asVec3();
                            castHelper.inflict(targetActor,
                                resolved.spell ? resolved.spell->mEffects : resolved.enchantment->mEffects,
                                ESM::RT_Touch);
                        }
                        else
                        {
                            const auto& effectStore2 = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>();
                            for (const auto& effectInfo : *effectList)
                            {
                                if (effectInfo.mData.mRange != ESM::RT_Touch)
                                    continue;
                                const ESM::MagicEffect* me = effectStore2.search(effectInfo.mData.mEffectID);
                                if (me)
                                    MWMechanics::playEffects(targetActor, *me);
                            }
                        }
                    }
                }

                // Launch magic bolt for non-self-only spells
                if (effectList && !effectList->empty())
                {
                    bool hasNonSelfEffect = false;
                    for (const auto& effect : *effectList)
                    {
                        if (effect.mData.mRange != ESM::RT_Self)
                        {
                            hasNonSelfEffect = true;
                            break;
                        }
                    }

                    if (hasNonSelfEffect)
                    {
                        actor.pendingBoltTimer = 0.6f;
                        actor.pendingBoltSpellId = cs.spellId;
                    }
                }
            }

            actor.pendingCast = false;
        }
    }
} // namespace mwmp
