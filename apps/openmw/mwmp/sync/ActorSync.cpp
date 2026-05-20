#include "ActorSync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/rng.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorList.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPosition.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPositionV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPresentationV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCombatRequest.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorDeath.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorIdentity.hpp>
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
#include "../../mwmechanics/aiwander.hpp"
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
#include <components/openmw-mp/Packets/Actor/PacketActorAttackV2.hpp>
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

    bool isGeneratedSpawnerRefId(const std::string& refId)
    {
        return refId.rfind("spawner_", 0) == 0;
    }

    bool isUnmanagedSpawnerActor(const mwmp::BaseActor& actor)
    {
        return actor.mpNum == 0 && isGeneratedSpawnerRefId(actor.refId);
    }

    bool applyDefaultServerSpawnedWander(const std::string& cellId, const MWWorld::Ptr& ptr, const mwmp::BaseActor& actor)
    {
        if (actor.mpNum == 0 || actor.isDead || ptr.isEmpty() || !ptr.getClass().isActor())
            return false;

        if (auto* baseNode = ptr.getRefData().getBaseNode())
        {
            bool alreadyApplied = false;
            if (baseNode->getUserValue("mp_default_wander_applied", alreadyApplied) && alreadyApplied)
                return false;
        }

        MWMechanics::CreatureStats& stats = ptr.getClass().getCreatureStats(ptr);
        if (stats.isDead())
            return false;

        MWMechanics::AiSequence& aiSequence = stats.getAiSequence();
        if (!aiSequence.isEmpty())
            return false;

        static const std::vector<unsigned char> idleChances { 30, 20, 10, 5, 0, 0, 0, 0 };
        aiSequence.stack(MWMechanics::AiWander(256, 0, 0, idleChances, true), ptr);

        if (auto* baseNode = ptr.getRefData().getBaseNode())
            baseNode->setUserValue("mp_default_wander_applied", true);

        Log(Debug::Verbose) << "[MP] ActorSync: applied default wander to server-spawned actor"
                            << " cell=" << cellId
                            << " refId=" << actor.refId
                            << " mpNum=" << actor.mpNum;
        return true;
    }

    std::string fallbackDeathAnimGroup(const mwmp::BaseActor& actor)
    {
        const uint32_t seed = actor.mpNum != 0 ? actor.mpNum : actor.refNum;
        return "death" + std::to_string((seed % 5) + 1);
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

    float shortestAngleDelta(float target, float current)
    {
        static constexpr float kPi = 3.14159265358979323846f;
        static constexpr float kTwoPi = kPi * 2.f;

        float delta = std::fmod(target - current + kPi, kTwoPi);
        if (delta < 0.f)
            delta += kTwoPi;
        return delta - kPi;
    }

    float lerpAngle(float current, float target, float alpha)
    {
        return current + shortestAngleDelta(target, current) * alpha;
    }

    bool isNewerEventId(uint32_t incoming, uint32_t previous)
    {
        if (incoming == 0 || previous == 0)
            return true;

        return static_cast<int32_t>(incoming - previous) > 0;
    }

    mwmp::Position interpolatePosition(const mwmp::Position& from, const mwmp::Position& to, float alpha)
    {
        mwmp::Position result = from;
        for (int i = 0; i < 3; ++i)
        {
            result.pos[i] = lerpFloat(from.pos[i], to.pos[i], alpha);
            result.rot[i] = lerpAngle(from.rot[i], to.rot[i], alpha);
        }
        result.isTeleporting = to.isTeleporting;
        return result;
    }

    mwmp::Position extrapolatePosition(const mwmp::Position& from, const mwmp::Velocity& velocity, double milliseconds)
    {
        mwmp::Position result = from;
        const float seconds = static_cast<float>(milliseconds / 1000.0);
        for (int i = 0; i < 3; ++i)
        {
            result.pos[i] += velocity.linear[i] * seconds;
            result.rot[i] += velocity.angular[i] * seconds;
        }
        return result;
    }

    float distanceSquared(const MWWorld::Ptr& lhs, const MWWorld::Ptr& rhs)
    {
        if (lhs.isEmpty() || rhs.isEmpty())
            return -1.f;

        const auto& a = lhs.getRefData().getPosition().pos;
        const auto& b = rhs.getRefData().getPosition().pos;
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return (dx * dx) + (dy * dy) + (dz * dz);
    }

    bool isMeleeAttackType(int attackType)
    {
        return attackType != 1 && attackType != 2 && attackType != 3;
    }

    constexpr float kMaxReplicatedMeleeHitDistance = 512.f;

    bool isHighPriorityPositionActor(const mwmp::BaseActor& actor, bool hasPrevious, float previousHealth,
        bool previousMoving)
    {
        if (actor.isDead)
            return false;
        if (actor.isMoving || actor.isAttackingOrCasting || actor.hasWeaponDrawn || actor.hasSpellReadied)
            return true;
        if (actor.ai.type == mwmp::BaseActor::AIAction::Type::Combat)
            return true;
        if (std::abs(actor.velocity.linear[0]) > 1.f || std::abs(actor.velocity.linear[1]) > 1.f
            || std::abs(actor.velocity.linear[2]) > 1.f)
            return true;
        if (hasPrevious)
        {
            if (previousMoving != actor.isMoving)
                return true;
            const float healthDelta = std::abs(actor.dynamicStats.health.current - previousHealth);
            if (healthDelta > 0.5f)
                return true;
        }
        return false;
    }

    bool isLocomotionAnimGroup(const std::string& group)
    {
        return group.find("walk") != std::string::npos
            || group.find("run") != std::string::npos
            || group.find("swim") != std::string::npos
            || group.find("sneak") != std::string::npos
            || group.find("turn") != std::string::npos;
    }

    bool isHitStateAnimGroup(const std::string& group)
    {
        return group.find("hit") != std::string::npos
            || group.find("knock") != std::string::npos;
    }

    bool isWatchedBorderActor(const mwmp::BaseActor& actor, const std::string& packetCellId);

    bool isWatchedPresentationActor(const mwmp::BaseActor& actor, mwmp::ActorInstanceId actorNetId)
    {
        return actor.refId == "heddvild" || actor.mpNum == 2601
            || actorNetId == mwmp::packActorInstanceKey({ mwmp::ActorKeyKind::SpawnedMpNum, 2601 })
            || isWatchedBorderActor(actor, actor.cellId);
    }

    bool isWatchedBorderCell(const std::string& cellId)
    {
        return cellId == "EXT:0,-7" || cellId == "EXT:-1,-7";
    }

    bool isWatchedBorderActor(const mwmp::BaseActor& actor, const std::string& packetCellId)
    {
        if (actor.refId.find("imperial guard") == std::string::npos)
            return false;
        return isWatchedBorderCell(packetCellId) || isWatchedBorderCell(actor.cellId);
    }

    mwmp::ActorPresentationSnapshot makePresentationSnapshot(const mwmp::BaseActor& actor, mwmp::ActorInstanceId actorNetId)
    {
        const bool axisLocomotion = std::abs(actor.animFlags.animFwd) > 0.1f
            || std::abs(actor.animFlags.animSide) > 0.1f;
        const float velX = actor.velocity.linear[0];
        const float velY = actor.velocity.linear[1];
        const float speedSq = velX * velX + velY * velY;
        const bool velocityLocomotion = speedSq > 20.f * 20.f;
        const bool hasLocomotionInput = !actor.isDead && (axisLocomotion || velocityLocomotion);

        float animFwd = hasLocomotionInput ? actor.animFlags.animFwd : 0.f;
        float animSide = hasLocomotionInput ? actor.animFlags.animSide : 0.f;
        if (hasLocomotionInput && !axisLocomotion && velocityLocomotion)
        {
            // NPC AI often moves the actor by transform velocity without leaving
            // useful movement axes behind.  Treat that as ordinary forward
            // locomotion instead of deriving signed strafe/backpedal axes from
            // world velocity, which proved too noisy near cell borders.
            animFwd = 1.f;
            animSide = 0.f;
        }

        mwmp::ActorPresentationSnapshot snapshot;
        snapshot.actorNetId = actorNetId;
        snapshot.isMoving = hasLocomotionInput;
        snapshot.isAttackingOrCasting = actor.isAttackingOrCasting;
        snapshot.hasWeaponDrawn = actor.hasWeaponDrawn;
        snapshot.hasSpellReadied = actor.hasSpellReadied;
        snapshot.isDead = actor.isDead;
        snapshot.movementFlags = static_cast<uint16_t>(actor.animFlags.movementFlags);
        snapshot.animFwd = hasLocomotionInput ? mwmp::quantizeActorAxis(animFwd) : 0;
        snapshot.animSide = hasLocomotionInput ? mwmp::quantizeActorAxis(animSide) : 0;
        mwmp::BaseActor presentationActor = actor;
        presentationActor.isMoving = hasLocomotionInput;
        snapshot.presentationFlags = mwmp::makeActorPresentationFlags(presentationActor);
        snapshot.currentAnimGroup = actor.animFlags.currentAnimGroup;
        if ((actor.isDead || !hasLocomotionInput) && isLocomotionAnimGroup(snapshot.currentAnimGroup))
            snapshot.currentAnimGroup.clear();
        return snapshot;
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

    bool sameLocalActorObject(const MWWorld::Ptr& lhs, const MWWorld::Ptr& rhs)
    {
        if (lhs.isEmpty() || rhs.isEmpty())
            return false;

        return cellIdForPtr(lhs) == cellIdForPtr(rhs)
            && lhs.getCellRef().getRefId().serializeText() == rhs.getCellRef().getRefId().serializeText()
            && lhs.getCellRef().getRefNum().mIndex == rhs.getCellRef().getRefNum().mIndex;
    }

    std::string exteriorCellIdForPosition(const mwmp::Position& position)
    {
        const float cellSize = static_cast<float>(Constants::CellSizeInUnits);
        const int gridX = static_cast<int>(std::floor(position.pos[0] / cellSize));
        const int gridY = static_cast<int>(std::floor(position.pos[1] / cellSize));
        return std::string("EXT:") + std::to_string(gridX) + "," + std::to_string(gridY);
    }

    float exteriorCellBorderDistance(const mwmp::Position& position)
    {
        const float cellSize = static_cast<float>(Constants::CellSizeInUnits);
        const float gridX = std::floor(position.pos[0] / cellSize);
        const float gridY = std::floor(position.pos[1] / cellSize);
        const float localX = position.pos[0] - gridX * cellSize;
        const float localY = position.pos[1] - gridY * cellSize;
        return std::min(std::min(localX, cellSize - localX), std::min(localY, cellSize - localY));
    }

    uint32_t stampedServerSpawnedMpNum(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return 0;

        if (auto* baseNode = ptr.getRefData().getBaseNode())
        {
            int stampedMpNum = 0;
            if (baseNode->getUserValue("mp_actor_mpnum", stampedMpNum) && stampedMpNum > 0)
                return static_cast<uint32_t>(stampedMpNum);
        }

        return 0;
    }

    bool matchesActor(const MWWorld::Ptr& ptr, const mwmp::BaseActor& actor)
    {
        if (ptr.isEmpty() || ptr.getCell() == nullptr || !ptr.getClass().isActor())
            return false;

        if (ptr.getCellRef().getRefId().serializeText() != actor.refId)
            return false;

        const uint32_t stampedMpNum = stampedServerSpawnedMpNum(ptr);
        if (actor.mpNum == 0 && stampedMpNum != 0)
            return false;
        if (actor.mpNum != 0 && stampedMpNum != 0 && stampedMpNum != actor.mpNum)
            return false;

        if (actor.mpNum == 0 && actor.refNum != 0 && ptr.getCellRef().getRefNum().mIndex != actor.refNum)
            return false;
        return true;
    }

    uint64_t currentClientTimeMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool isExteriorActorCellId(const std::string& cellId)
    {
        return cellId.rfind("EXT:", 0) == 0;
    }

    bool isZeroIdentityPosition(const mwmp::Position& position)
    {
        return std::abs(position.pos[0]) < 0.001f
            && std::abs(position.pos[1]) < 0.001f
            && std::abs(position.pos[2]) < 0.001f;
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

    void ActorSync::resetSessionState()
    {
        // Wipe all per-session state.  Called when the player disconnects so
        // that dangling MWWorld::Ptr references to CellStores from the old
        // game session are never touched again.  The engine destroys all cells
        // and their actors when returning to the main menu, so we must not
        // attempt to call world->deleteObject() here - just drop the maps.
        Log(Debug::Info) << "[MP] ActorSync: resetSessionState - clearing all cell/actor tracking";
        mCells.clear();
        mAuthority.clear();
        mMpNumsByLocalActor.clear();
        mServerSpawnedActorsByMpNum.clear();
        mServerSpawnedActorLastTimestamps.clear();
        mActorsByNetId.clear();
        mCellActorIds.clear();
        mActorNetIdsByKey.clear();
        mActorV2DiagnosticsLastLogMs = 0;
        mActorV2SnapshotsWindow = 0;
        mActorV2InvalidActorIdWindow = 0;
        mActorV2MissingIdentityWindow = 0;
        mActorV2StaleWindow = 0;
        mActorV2DeadLiveSuppressedWindow = 0;
        mActorV2IdentityTransformPreservedWindow = 0;
        mActorV2IdentityZeroTransformSkippedWindow = 0;
        mActorV2PresentationSentWindow = 0;
        mActorV2PresentationAppliedWindow = 0;
        mActorV2PresentationInvalidActorIdWindow = 0;
        mActorV2PresentationMissingIdentityWindow = 0;
        mActorV2PresentationStaleWindow = 0;
        mActorV2PresentationDeadLiveSuppressedWindow = 0;
        mActorV2PresentationStopForcedWindow = 0;
        mActorV2PresentationGroupChangedWindow = 0;
        mActorV2AttackSentWindow = 0;
        mActorV2AttackAppliedWindow = 0;
        mActorV2AttackInvalidActorIdWindow = 0;
        mActorV2AttackMissingIdentityWindow = 0;
        mActorV2AttackDuplicateWindow = 0;
        mActorV2AttackDeadSuppressedWindow = 0;
        mActorV2MissingIdentityByNetIdWindow.clear();
    }

    ActorInstanceId ActorSync::actorNetIdForActorState(const BaseActor& actor) const
    {
        const auto mappedIt = mActorNetIdsByKey.find(makeActorKey(actor));
        if (mappedIt != mActorNetIdsByKey.end())
            return mappedIt->second;

        return actorInstanceIdFromActor(actor);
    }

    void ActorSync::rememberActorNetId(ActorInstanceId actorNetId, const BaseActor& actor)
    {
        if (actorNetId == 0)
            return;

        mActorNetIdsByKey[makeActorKey(actor)] = actorNetId;
    }

    void ActorSync::indexActorNetId(ActorInstanceId actorNetId, const std::string& oldCellId, const std::string& newCellId)
    {
        if (actorNetId == 0)
            return;

        if (!oldCellId.empty() && oldCellId != newCellId)
        {
            auto oldIt = mCellActorIds.find(oldCellId);
            if (oldIt != mCellActorIds.end())
            {
                oldIt->second.erase(actorNetId);
                if (oldIt->second.empty())
                    mCellActorIds.erase(oldIt);
            }
        }

        if (!newCellId.empty())
            mCellActorIds[newCellId].insert(actorNetId);
    }

    ActorSync::ActorRuntime* ActorSync::findPrimaryActorRuntime(const BaseActor& actor)
    {
        const ActorInstanceId actorNetId = actorNetIdForActorState(actor);
        if (actorNetId == 0)
            return nullptr;

        auto runtimeIt = mActorsByNetId.find(actorNetId);
        if (runtimeIt == mActorsByNetId.end())
            return nullptr;

        return &runtimeIt->second;
    }

    ActorSync::ActorRuntime& ActorSync::runtimeForPacketActor(
        const std::string& cellId, CellRuntime& cell, const BaseActor& actor)
    {
        if (ActorRuntime* runtime = findPrimaryActorRuntime(actor))
            return *runtime;

        ActorRuntime& runtime = cell.actors[makeActorKey(actor)];
        runtime.actorNetId = actorNetIdForActorState(actor);
        if (runtime.actorNetId != 0)
            indexActorNetId(runtime.actorNetId, runtime.state.cellId, cellId);
        return runtime;
    }

    MWWorld::Ptr ActorSync::resolvePacketActorBinding(const std::string& packetCellId, CellRuntime& cell,
        const BaseActor& actor, const char* packetName)
    {
        auto findActorInPacketCell = [&]() -> MWWorld::Ptr
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return {};

            MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
            MWWorld::CellStore* packetCell = findActiveCellById(worldImp, packetCellId);
            if (packetCell == nullptr)
                return {};

            MWWorld::Ptr found;
            MWWorld::Ptr looseSpawnedMatch;
            packetCell->forEach([&](MWWorld::Ptr ptr) -> bool
            {
                if (!matchesActor(ptr, actor))
                    return true;

                if (actor.mpNum != 0)
                {
                    const uint32_t stampedMpNum = stampedServerSpawnedMpNum(ptr);
                    const uint32_t mappedMpNum = mappedMpNumForPtr(packetCellId, ptr);
                    if (stampedMpNum == actor.mpNum || mappedMpNum == actor.mpNum)
                    {
                        found = ptr;
                        return false;
                    }

                    if (looseSpawnedMatch.isEmpty())
                        looseSpawnedMatch = ptr;
                    return true;
                }

                found = ptr;
                return false;
            });

            return !found.isEmpty() ? found : looseSpawnedMatch;
        };

        auto attachPacketCellFallback = [&](ActorRuntime& runtime, const MWWorld::Ptr& ptr)
        {
            runtime.boundActor = ptr;
            runtime.bindingLogged = false;

            const std::string resolvedCellId = cellIdForPtr(ptr);
            if (!resolvedCellId.empty() && resolvedCellId != runtime.state.cellId)
            {
                const std::string oldCellId = runtime.state.cellId;
                runtime.state.cellId = resolvedCellId;
                if (runtime.actorNetId != 0)
                    indexActorNetId(runtime.actorNetId, oldCellId, resolvedCellId);
            }
        };

        if (ActorRuntime* primaryRuntime = findPrimaryActorRuntime(actor))
        {
            const std::string targetCellId = primaryRuntime->state.cellId.empty()
                ? packetCellId : primaryRuntime->state.cellId;
            const bool resolved = resolveActorBinding(targetCellId, *primaryRuntime);
            MWWorld::Ptr ptr = resolved ? primaryRuntime->boundActor : findActorInPacketCell();
            if (!resolved && !ptr.isEmpty())
                attachPacketCellFallback(*primaryRuntime, ptr);
            logWatchedBorderActor(packetName, packetCellId, actor, primaryRuntime, ptr,
                resolved ? "primary" : (ptr.isEmpty() ? "primary-unbound" : "packet-cell-fallback"));
            return ptr;
        }

        const std::string actorKey = makeActorKey(actor);
        auto runtimeIt = cell.actors.find(actorKey);
        if (runtimeIt != cell.actors.end())
        {
            const bool resolved = resolveActorBinding(packetCellId, runtimeIt->second);
            MWWorld::Ptr ptr = resolved ? runtimeIt->second.boundActor : findActorInPacketCell();
            if (!resolved && !ptr.isEmpty())
                attachPacketCellFallback(runtimeIt->second, ptr);
            logWatchedBorderActor(packetName, packetCellId, actor, &runtimeIt->second, ptr,
                resolved ? "cell-runtime" : (ptr.isEmpty() ? "cell-runtime-unbound" : "packet-cell-fallback"));
            return ptr;
        }

        MWWorld::Ptr ptr = findActorInPacketCell();
        if (MWBase::World* world = MWBase::Environment::get().getWorld())
        {
            try
            {
                if (ptr.isEmpty())
                    ptr = world->getPtr(ESM::RefId::stringRefId(actor.refId), false);
            }
            catch (...) {}
        }

        logWatchedBorderActor(packetName, packetCellId, actor, nullptr, ptr,
            ptr.isEmpty() ? "missing" : "packet-cell/world-refId");
        return ptr;
    }

    void ActorSync::logWatchedBorderActor(const char* event, const std::string& packetCellId,
        const BaseActor& packetActor, const ActorRuntime* runtime, const MWWorld::Ptr& resolvedPtr,
        const char* source) const
    {
        if (!isWatchedBorderActor(packetActor, packetCellId)
            && (runtime == nullptr || !isWatchedBorderActor(runtime->state, packetCellId)))
            return;

        const ActorInstanceId actorNetId = runtime != nullptr && runtime->actorNetId != 0
            ? runtime->actorNetId : actorNetIdForActorState(packetActor);
        const std::string packetActorCell = packetActor.cellId.empty() ? packetCellId : packetActor.cellId;
        const std::string runtimeCell = runtime != nullptr ? runtime->state.cellId : std::string();
        const std::string boundCell = runtime != nullptr && !runtime->boundActor.isEmpty()
            ? cellIdForPtr(runtime->boundActor) : std::string();
        const std::string resolvedCell = !resolvedPtr.isEmpty() ? cellIdForPtr(resolvedPtr) : std::string();
        const uint64_t latestPositionTimestamp = runtime != nullptr ? runtime->lastServerTimestamp : 0;
        const uint64_t latestPresentationTimestamp = runtime != nullptr ? runtime->lastPresentationServerTimestamp : 0;

        const std::string eventName = event != nullptr ? event : "";
        const std::string sourceName = source != nullptr ? source : "";
        const bool cellMismatch = (!runtimeCell.empty() && !boundCell.empty() && runtimeCell != boundCell)
            || (!runtimeCell.empty() && !resolvedCell.empty() && runtimeCell != resolvedCell);
        const bool important = cellMismatch
            || eventName.find("Attack") != std::string::npos
            || eventName.find("Combat") != std::string::npos
            || eventName.find("migrated") != std::string::npos
            || eventName.find("removed") != std::string::npos
            || sourceName.find("unbound") != std::string::npos
            || sourceName == "missing";

        Log(important ? Debug::Info : Debug::Verbose) << "[MP] ActorSync border watch: " << event
                         << " source=" << source
                         << " actorNetId=" << actorNetId
                         << " refId=" << packetActor.refId
                         << " refNum=" << packetActor.refNum
                         << " mpNum=" << packetActor.mpNum
                         << " packetCell=" << packetCellId
                         << " actorCell=" << packetActorCell
                         << " runtimeCell=" << runtimeCell
                         << " boundCell=" << boundCell
                         << " resolvedCell=" << resolvedCell
                         << " latestPositionTs=" << latestPositionTimestamp
                         << " latestPresentationTs=" << latestPresentationTimestamp
                         << " hasTransform=" << (runtime != nullptr && runtime->hasAuthoritativeTransform)
                         << " boundEmpty=" << (runtime == nullptr || runtime->boundActor.isEmpty())
                         << " resolvedEmpty=" << resolvedPtr.isEmpty();
    }

    void ActorSync::update(float dt)
    {
        std::size_t primaryActors = 0;
        for (auto& [actorNetId, actor] : mActorsByNetId)
        {

            if (actor.state.cellId.empty())
                continue;

            if (hasAuthority(actor.state.cellId))
            {
                const bool needsAuthorityBootstrapBinding = actor.boundActor.isEmpty()
                    && actor.hasAuthoritativeTransform
                    && (actor.state.mpNum != 0 || actor.state.isDead);
                const bool needsAuthorityDeathApply = actor.state.isDead
                    && !actor.boundActor.isEmpty()
                    && (!actor.deathAlreadyApplied || actor.deathFromRealtimePacket);
                if (needsAuthorityBootstrapBinding || needsAuthorityDeathApply)
                {
                    advanceSmoothing(actor, dt);
                    if (resolveActorBinding(actor.state.cellId, actor))
                    {
                        applyBoundActorState(actor);
                        Log(needsAuthorityBootstrapBinding ? Debug::Info : Debug::Verbose)
                            << "[MP] ActorSync v2: authority bootstrap applied actor"
                            << " actorNetId=" << actorNetId
                            << " refId=" << actor.state.refId
                            << " mpNum=" << actor.state.mpNum
                            << " isDead=" << actor.state.isDead
                            << " cell=" << actor.state.cellId;
                    }
                }
                continue;
            }

            if (!actor.hasAuthoritativeTransform)
                continue;

            advanceSmoothing(actor, dt);
            if (resolveActorBinding(actor.state.cellId, actor))
                applyBoundActorState(actor);
            ++primaryActors;
        }

        for (auto& [cellId, cell] : mCells)
        {
            if (hasAuthority(cellId))
            {
                sendAuthoritativeActorUpdates(cellId, cell, dt);
                continue;
            }

            std::size_t cellShadowSuppressedByPrimary = 0;
            for (auto& [actorKey, actor] : cell.actors)
            {
                if (actor.actorNetId != 0)
                {
                    // In v2, mActorsByNetId owns this actor. A cell-owned shadow must not
                    // move/apply the same MWWorld::Ptr even if the primary runtime was not
                    // applied this frame because it is waiting for an authoritative transform.
                    if (mActorsByNetId.find(actor.actorNetId) != mActorsByNetId.end())
                    {
                        ++cellShadowSuppressedByPrimary;
                        continue;
                    }
                }

                if (!actor.hasAuthoritativeTransform)
                    continue;

                advanceSmoothing(actor, dt);
                if (resolveActorBinding(cellId, actor))
                    applyBoundActorState(actor);
            }
            if (cellShadowSuppressedByPrimary > 0)
                Log(Debug::Verbose) << "[MP] ActorSync v2: cellShadowSuppressedByPrimary=" << cellShadowSuppressedByPrimary
                                    << " cell=" << cellId;
        }
    }

    bool ActorSync::shouldAcceptSnapshot(CellRuntime& cell, const ActorList& list, const char* packetName,
        bool isPositionSnapshot)
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

        if (cell.latest.authorityGeneration != 0
            && list.authorityGeneration != 0
            && list.authorityGeneration > cell.latest.authorityGeneration)
        {
            cell.latestPositionSequence = 0;
            cell.latestReliableSequence = 0;
        }

        const uint32_t latestStreamSequence = isPositionSnapshot
            ? cell.latestPositionSequence : cell.latestReliableSequence;
        if (latestStreamSequence != 0
            && list.snapshotSequence != 0
            && list.snapshotSequence < latestStreamSequence)
        {
            Log(Debug::Info) << "[MP] ActorSync: dropped stale " << packetName
                             << " cellId=" << list.cellId
                             << " seq=" << list.snapshotSequence
                             << " latestSeq=" << latestStreamSequence
                             << " gen=" << list.authorityGeneration
                             << " latestGen=" << cell.latest.authorityGeneration;
            return false;
        }

        cell.latest.cellId = list.cellId;
        cell.latest.authorityGuid = list.authorityGuid;
        if (list.authorityGeneration != 0)
            cell.latest.authorityGeneration = list.authorityGeneration;
        if (list.snapshotSequence != 0)
        {
            if (isPositionSnapshot)
                cell.latestPositionSequence = list.snapshotSequence;
            else
                cell.latestReliableSequence = list.snapshotSequence;
            cell.latest.snapshotSequence = std::max(cell.latestPositionSequence, cell.latestReliableSequence);
        }
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
            cell.positionDiagnosticsTimer = 0.f;
            cell.positionSendCursor = 0;
            cell.priorityPositionSendCursor = 0;
            cell.outboundCellId = list.cellId;
            Log(Debug::Info) << "[MP] ActorSync: authority "
                             << (hasLocalAuthority ? "granted" : "revoked")
                             << " for " << list.cellId
                             << " owner=" << list.authorityGuid
                             << " localGuid=" << localGuid;
        }
    }

    void ActorSync::onActorIdentityUpdate(const ActorIdentityList& list)
    {
        if (list.protocolVersion != ActorSyncProtocolVersionV2)
        {
            Log(Debug::Warning) << "[MP] ActorSync: ignored unsupported ActorIdentity protocol="
                                << list.protocolVersion;
            return;
        }

        std::size_t removed = 0;
        std::size_t migrated = 0;
        std::size_t promoted = 0;
        std::size_t baselineApplied = 0;
        std::size_t transformPreserved = 0;
        std::size_t zeroTransformSkipped = 0;
        std::size_t invalidActorNetId = 0;
        std::size_t missingIdentity = 0;
        std::size_t identityMismatch = 0;
        std::size_t ambiguousIdentityNormalized = 0;
        std::size_t unmanagedSpawnerIdentityDropped = 0;
        ActorInstanceId firstInvalidActorNetId = 0;
        ActorInstanceId firstMismatchActorNetId = 0;
        std::vector<ActorInstanceId> ackedActorNetIds;
        ackedActorNetIds.reserve(list.actors.size());
        auto forgetLocalMpNumMappings = [this](uint32_t mpNum)
        {
            if (mpNum == 0)
                return;

            for (auto it = mMpNumsByLocalActor.begin(); it != mMpNumsByLocalActor.end();)
            {
                if (it->second == mpNum)
                    it = mMpNumsByLocalActor.erase(it);
                else
                    ++it;
            }
            mServerSpawnedActorsByMpNum.erase(mpNum);
        };

        for (const ActorIdentityRecord& record : list.actors)
        {
            if (!isValidActorInstanceId(record.actorNetId))
            {
                ++invalidActorNetId;
                if (firstInvalidActorNetId == 0)
                    firstInvalidActorNetId = record.actorNetId;
                continue;
            }

            BaseActor actorState = record.actor;
            if (hasAmbiguousActorInstanceIdentity(actorState))
            {
                ++ambiguousIdentityNormalized;
                if (actorState.mpNum != 0)
                    actorState.refNum = 0;
            }
            if (!record.removed && hasMissingActorInstanceIdentity(actorState))
            {
                ++missingIdentity;
                continue;
            }
            if (!record.removed && isUnmanagedSpawnerActor(actorState))
            {
                ++unmanagedSpawnerIdentityDropped;
                continue;
            }
            if (!record.removed && !actorInstanceIdMatchesActor(record.actorNetId, actorState))
            {
                ++identityMismatch;
                if (firstMismatchActorNetId == 0)
                    firstMismatchActorNetId = record.actorNetId;
                continue;
            }

            if (actorState.cellId.empty())
                actorState.cellId = list.cellId;
            if (record.teleport)
                actorState.position.isTeleporting = true;
            if (!record.removed)
                rememberActorNetId(record.actorNetId, actorState);
            ackedActorNetIds.push_back(record.actorNetId);

            if (record.removed)
            {
                auto runtimeIt = mActorsByNetId.find(record.actorNetId);
                if (runtimeIt != mActorsByNetId.end())
                {
                    ActorRuntime& runtime = runtimeIt->second;
                    const BaseActor previousState = runtime.state;
                    logWatchedBorderActor("identity-removed", list.cellId, actorState, &runtime,
                        runtime.boundActor, "primary");

                    const uint32_t removedMpNum = previousState.mpNum != 0 ? previousState.mpNum : actorState.mpNum;
                    if ((record.serverSpawned || removedMpNum != 0) && !runtime.boundActor.isEmpty())
                    {
                        const std::string boundCellId = cellIdForPtr(runtime.boundActor);
                        const std::string mappingCellId = !boundCellId.empty() ? boundCellId : previousState.cellId;
                        forgetServerSpawnedActor(mappingCellId, runtime.boundActor, removedMpNum);
                        MWBase::Environment::get().getWorld()->deleteObject(runtime.boundActor);
                    }
                    if (removedMpNum != 0)
                        forgetLocalMpNumMappings(removedMpNum);

                    const std::string previousKey = makeActorKey(previousState);
                    const std::string packetKey = makeActorKey(actorState);
                    for (auto& [cellId, cellRuntime] : mCells)
                    {
                        cellRuntime.actors.erase(previousKey);
                        cellRuntime.actors.erase(packetKey);
                    }

                    indexActorNetId(record.actorNetId, previousState.cellId, {});
                    mActorNetIdsByKey.erase(previousKey);
                    mActorNetIdsByKey.erase(packetKey);
                    mActorsByNetId.erase(runtimeIt);
                    ++removed;
                }
                else
                {
                    mActorNetIdsByKey.erase(makeActorKey(actorState));
                    if (actorState.mpNum != 0)
                        forgetLocalMpNumMappings(actorState.mpNum);
                }
                continue;
            }

            ActorRuntime runtime;
            bool hadRuntime = false;
            auto primaryIt = mActorsByNetId.find(record.actorNetId);
            if (primaryIt != mActorsByNetId.end())
            {
                runtime = std::move(primaryIt->second);
                hadRuntime = true;
            }
            else
            {
                const std::string actorKey = makeActorKey(actorState);
                auto cellIt = mCells.find(actorState.cellId);
                if (cellIt != mCells.end())
                {
                    auto cellActorIt = cellIt->second.actors.find(actorKey);
                    if (cellActorIt != cellIt->second.actors.end())
                    {
                        runtime = std::move(cellActorIt->second);
                        cellIt->second.actors.erase(cellActorIt);
                        hadRuntime = true;
                        ++promoted;
                    }
                }

                if (runtime.state.refId.empty() && actorState.mpNum != 0)
                {
                    hadRuntime = takeServerSpawnedRuntimeFromOtherCell(actorState.cellId, actorState,
                        list.serverTimestamp, runtime);
                    if (runtime.boundActor.isEmpty())
                    {
                        auto mappedIt = mServerSpawnedActorsByMpNum.find(actorState.mpNum);
                        if (mappedIt != mServerSpawnedActorsByMpNum.end()
                            && !mappedIt->second.isEmpty()
                            && matchesActor(mappedIt->second, actorState))
                        {
                            runtime.boundActor = mappedIt->second;
                            runtime.bindingLogged = false;
                            hadRuntime = true;
                        }
                    }
                }
            }

            const std::string oldCellId = runtime.state.cellId;
            if (!oldCellId.empty() && oldCellId != actorState.cellId)
                ++migrated;

            runtime.actorNetId = record.actorNetId;
            const bool explicitTransformReset = record.baselineReset || record.teleport;
            const bool zeroIdentityTransform = record.serverSpawned
                && isExteriorActorCellId(actorState.cellId)
                && isZeroIdentityPosition(actorState.position)
                && !explicitTransformReset;
            const bool deathBaseline = actorState.isDead && !zeroIdentityTransform;

            if (hadRuntime && !explicitTransformReset && !deathBaseline)
            {
                runtime.state.refId = actorState.refId;
                runtime.state.refNum = actorState.refNum;
                runtime.state.mpNum = actorState.mpNum;
                runtime.state.cellId = actorState.cellId;
                ++transformPreserved;
            }
            else if (zeroIdentityTransform)
            {
                if (runtime.state.refId.empty())
                {
                    runtime.state.refId = actorState.refId;
                    runtime.state.refNum = actorState.refNum;
                    runtime.state.mpNum = actorState.mpNum;
                    runtime.state.cellId = actorState.cellId;
                    runtime.hasAuthoritativeTransform = false;
                }
                else
                {
                    runtime.state.refId = actorState.refId;
                    runtime.state.refNum = actorState.refNum;
                    runtime.state.mpNum = actorState.mpNum;
                    runtime.state.cellId = actorState.cellId;
                }
                ++zeroTransformSkipped;
                Log(Debug::Warning) << "[MP] ActorSync v2: skipped identity zero transform"
                                    << " actorNetId=" << record.actorNetId
                                    << " refId=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " cell=" << actorState.cellId;
            }
            else
            {
                runtime.state = actorState;
                if (actorState.isDead)
                {
                    runtime.deathFromRealtimePacket = false;
                    runtime.deathAlreadyApplied = false;
                    runtime.deathPacketSent = true;
                }

                ActorList synthetic;
                synthetic.cellId = actorState.cellId;
                synthetic.authorityGuid = list.authorityGuid;
                synthetic.authorityGeneration = list.authorityGeneration;
                synthetic.snapshotSequence = list.sequence;
                synthetic.serverTimestamp = list.serverTimestamp;
                queueSnapshot(runtime, actorState, synthetic);
                ++baselineApplied;
            }
            rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
            if (runtime.state.isDead)
            {
                runtime.deathFromRealtimePacket = false;
                runtime.deathPacketSent = true;
            }
            indexActorNetId(record.actorNetId, oldCellId, actorState.cellId);
            logWatchedBorderActor(!hadRuntime ? "identity-baseline"
                    : (oldCellId != actorState.cellId ? "identity-migrated" : "identity-reused"),
                list.cellId, actorState, &runtime, runtime.boundActor, "identity");
            mActorsByNetId[record.actorNetId] = std::move(runtime);
        }

        if (!ackedActorNetIds.empty())
        {
            ActorIdentityAck ack;
            ack.cellId = list.cellId;
            ack.sequence = list.sequence;
            ack.actorNetIds = std::move(ackedActorNetIds);
            PacketActorIdentityAck ackPacket;
            ackPacket.setAck(&ack);
            mClient.sendReliable(ackPacket.encode());
        }

        mActorV2IdentityTransformPreservedWindow += transformPreserved;
        mActorV2IdentityZeroTransformSkippedWindow += zeroTransformSkipped;
        Log(Debug::Info) << "[MP] ActorSync v2: identity"
                         << " cell=" << list.cellId
                         << " actors=" << list.actors.size()
                         << " primary=" << mActorsByNetId.size()
                         << " promoted=" << promoted
                         << " migrated=" << migrated
                         << " removed=" << removed
                         << " baselineApplied=" << baselineApplied
                         << " transformPreserved=" << transformPreserved
                         << " zeroTransformSkipped=" << zeroTransformSkipped
                         << " invalidActorNetId=" << invalidActorNetId
                         << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                         << " missingIdentity=" << missingIdentity
                         << " identityMismatch=" << identityMismatch
                         << " firstMismatchActorKey=" << describeActorInstanceId(firstMismatchActorNetId)
                         << " ambiguousIdentityNormalized=" << ambiguousIdentityNormalized
                         << " unmanagedSpawnerDropped=" << unmanagedSpawnerIdentityDropped
                         << " seq=" << list.sequence;
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
        std::unordered_set<ActorInstanceId> incomingActorNetIds;
        std::size_t unmanagedSpawnerDropped = 0;

        for (const auto& actorState : list.actors)
        {
            if (isUnmanagedSpawnerActor(actorState))
            {
                ++unmanagedSpawnerDropped;
                continue;
            }

            if (isStaleServerSpawnedActorUpdate(actorState.mpNum, list.serverTimestamp))
            {
                Log(Debug::Verbose) << "[MP] ActorSync: skipped stale ActorList actor"
                                    << " cell=" << list.cellId
                                    << " refId=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " ts=" << list.serverTimestamp;
                continue;
            }

            const std::string actorKey = makeActorKey(actorState);
            const ActorInstanceId knownActorNetId = actorNetIdForActorState(actorState);
            if (knownActorNetId != 0)
                incomingActorNetIds.insert(knownActorNetId);
            ActorRuntime runtime;
            if (actorState.mpNum != 0)
                incomingServerSpawnedMpNums.insert(actorState.mpNum);

            auto existing = cell.actors.find(actorKey);
            if (existing != cell.actors.end())
                runtime = existing->second;
            else if (actorState.mpNum != 0)
            {
                takeServerSpawnedRuntimeFromOtherCell(list.cellId, actorState, list.serverTimestamp, runtime);
                if (runtime.boundActor.isEmpty())
                {
                    auto mappedIt = mServerSpawnedActorsByMpNum.find(actorState.mpNum);
                    if (mappedIt != mServerSpawnedActorsByMpNum.end()
                        && !mappedIt->second.isEmpty()
                        && matchesActor(mappedIt->second, actorState))
                    {
                        runtime.boundActor = mappedIt->second;
                        runtime.bindingLogged = false;
                    }
                }
            }

            const ActorInstanceId actorNetId = actorNetIdForActorState(actorState);
            if (actorNetId != 0 && !hasAuthority(list.cellId))
            {
                auto primaryIt = mActorsByNetId.find(actorNetId);
                const bool hadPrimaryRuntime = primaryIt != mActorsByNetId.end();
                if (hadPrimaryRuntime)
                    runtime = std::move(primaryIt->second);
                const std::string oldCellId = runtime.state.cellId;
                runtime.actorNetId = actorNetId;
                const bool zeroActorListTransform = actorState.mpNum != 0
                    && isExteriorActorCellId(list.cellId)
                    && isZeroIdentityPosition(actorState.position)
                    && !actorState.position.isTeleporting;
                const bool deathBaseline = actorState.isDead && !zeroActorListTransform;
                if (hadPrimaryRuntime && !deathBaseline)
                {
                    runtime.state.refId = actorState.refId;
                    runtime.state.refNum = actorState.refNum;
                    runtime.state.mpNum = actorState.mpNum;
                    runtime.state.cellId = list.cellId;
                    ++mActorV2IdentityTransformPreservedWindow;
                }
                else if (zeroActorListTransform)
                {
                    runtime.state.refId = actorState.refId;
                    runtime.state.refNum = actorState.refNum;
                    runtime.state.mpNum = actorState.mpNum;
                    runtime.state.cellId = list.cellId;
                    runtime.hasAuthoritativeTransform = false;
                    ++mActorV2IdentityZeroTransformSkippedWindow;
                    Log(Debug::Warning) << "[MP] ActorSync v2: skipped ActorList zero transform"
                                        << " actorNetId=" << actorNetId
                                        << " refId=" << actorState.refId
                                        << " mpNum=" << actorState.mpNum
                                        << " cell=" << list.cellId;
                }
                else
                {
                    runtime.state = actorState;
                    if (actorState.isDead)
                    {
                        runtime.deathFromRealtimePacket = false;
                        runtime.deathAlreadyApplied = false;
                        runtime.deathPacketSent = true;
                    }
                    queueSnapshot(runtime, actorState, list);
                }
                rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
                rememberActorNetId(actorNetId, runtime.state);
                indexActorNetId(actorNetId, oldCellId, list.cellId);
                logWatchedBorderActor(hadPrimaryRuntime ? "ActorList-primary-reused" : "ActorList-primary-promoted",
                    list.cellId, actorState, &runtime, runtime.boundActor, "ActorList");
                mActorsByNetId[actorNetId] = std::move(runtime);
                continue;
            }

            runtime.state = actorState;
            // If the actor arrives already dead from a full actor list (not a
            // real-time death packet), mark it so applyBoundActorState() can
            // skip to the final death pose instead of replaying the animation.
            if (actorState.isDead)
            {
                runtime.deathFromRealtimePacket = false;
                runtime.deathAlreadyApplied = false;
                runtime.deathPacketSent = true;
            }
            queueSnapshot(runtime, actorState, list);
            rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
            updatedActors[actorKey] = std::move(runtime);
        }

        for (auto& [actorKey, runtime] : cell.actors)
        {
            if (runtime.state.mpNum == 0 || incomingServerSpawnedMpNums.count(runtime.state.mpNum) != 0)
                continue;

            if (!runtime.boundActor.isEmpty())
            {
                const std::string boundCellId = cellIdForPtr(runtime.boundActor);
                rememberServerSpawnedActor(boundCellId.empty() ? list.cellId : boundCellId,
                    runtime.boundActor, runtime.state.mpNum);
                Log(Debug::Verbose) << "[MP] ActorSync: deferred missing server-spawned actor cleanup"
                                    << " cell=" << list.cellId
                                    << " boundCell=" << boundCellId
                                    << " refId=" << runtime.state.refId
                                    << " mpNum=" << runtime.state.mpNum;
            }
        }

        cell.actors = std::move(updatedActors);

        auto cellIndexIt = mCellActorIds.find(list.cellId);
        if (cellIndexIt != mCellActorIds.end())
        {
            for (auto actorIdIt = cellIndexIt->second.begin(); actorIdIt != cellIndexIt->second.end();)
            {
                if (incomingActorNetIds.count(*actorIdIt) != 0)
                {
                    ++actorIdIt;
                    continue;
                }

                auto runtimeIt = mActorsByNetId.find(*actorIdIt);
                if (runtimeIt != mActorsByNetId.end() && runtimeIt->second.state.cellId == list.cellId)
                {
                    // In v2, primary runtime lifetime is controlled by ActorIdentity (removed flag),
                    // not by absence from a legacy ActorList. Skip the erase so we do not destroy
                    // a live v2 primary runtime that simply wasn't included in this particular list.
                    ++actorIdIt;
                    continue;
                }
                actorIdIt = cellIndexIt->second.erase(actorIdIt);
            }
            if (cellIndexIt->second.empty())
                mCellActorIds.erase(cellIndexIt);
        }

        if (unmanagedSpawnerDropped != 0)
        {
            Log(Debug::Verbose) << "[MP] ActorSync: dropped unmanaged spawner actor(s) from ActorList"
                                << " cell=" << list.cellId
                                << " count=" << unmanagedSpawnerDropped;
        }
    }

    void ActorSync::onActorCellChange(const ActorList& list)
    {
        // list.cellId  = source cell (where the actor was)
        // actor.cellId = destination cell (where the actor is going)
        // Move each actor's runtime from the source cell to the destination cell.
        auto srcIt = mCells.find(list.cellId);

        for (const auto& actorState : list.actors)
        {
            if (isStaleServerSpawnedActorUpdate(actorState.mpNum, list.serverTimestamp))
            {
                Log(Debug::Verbose) << "[MP] ActorSync: skipped stale ActorCellChange actor"
                                    << " from=" << list.cellId
                                    << " to=" << actorState.cellId
                                    << " refId=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " ts=" << list.serverTimestamp;
                continue;
            }

            const std::string actorKey = makeActorKey(actorState);
            const std::string& destCellId = actorState.cellId;
            if (ActorRuntime* primaryRuntime = findPrimaryActorRuntime(actorState))
            {
                const std::string oldCellId = primaryRuntime->state.cellId;
                primaryRuntime->state.cellId = destCellId;
                mergeActorState(*primaryRuntime, actorState, true);
                queueSnapshot(*primaryRuntime, actorState, list);
                rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
                indexActorNetId(primaryRuntime->actorNetId, oldCellId, destCellId);
                Log(Debug::Info) << "[MP] ActorSync v2: cell index change"
                                 << " actorNetId=" << primaryRuntime->actorNetId
                                 << " refId=" << actorState.refId
                                 << " mpNum=" << actorState.mpNum
                                 << " from=" << oldCellId
                                 << " to=" << destCellId;
                continue;
            }

            ActorRuntime runtime;
            if (srcIt != mCells.end())
            {
                auto runtimeIt = srcIt->second.actors.find(actorKey);
                if (runtimeIt != srcIt->second.actors.end())
                {
                    runtime = std::move(runtimeIt->second);
                    srcIt->second.actors.erase(runtimeIt);
                }
            }

            if (runtime.state.refId.empty())
            {
                const bool migrated = takeServerSpawnedRuntimeFromOtherCell(destCellId, actorState,
                    list.serverTimestamp, runtime);
                if (!migrated && actorState.mpNum != 0)
                {
                    auto mappedIt = mServerSpawnedActorsByMpNum.find(actorState.mpNum);
                    if (mappedIt != mServerSpawnedActorsByMpNum.end()
                        && !mappedIt->second.isEmpty()
                        && matchesActor(mappedIt->second, actorState))
                    {
                        runtime.boundActor = mappedIt->second;
                        runtime.bindingLogged = false;
                    }
                }
            }

            auto& destCell = mCells[destCellId];
            runtime.state = actorState;
            queueSnapshot(runtime, actorState, list);
            rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
            destCell.actors[actorKey] = std::move(runtime);

            Log(Debug::Info) << "[MP] ActorSync: cell change " << actorState.refId
                             << " mpNum=" << actorState.mpNum
                             << " from=" << list.cellId
                             << " to=" << destCellId;
        }
    }

    void ActorSync::onActorPositionUpdate(const ActorList& list)
    {
        auto& cell = mCells[list.cellId];
        if (!shouldAcceptSnapshot(cell, list, "ActorPosition", true))
            return;

        cell.latest.cellId = list.cellId;
        cell.latest.authorityGuid = list.authorityGuid;
        cell.latest.authorityGeneration = list.authorityGeneration;
        cell.latest.snapshotSequence = list.snapshotSequence;
        cell.latest.serverTimestamp = list.serverTimestamp;

        for (const auto& actorState : list.actors)
        {
            if (isStaleServerSpawnedActorUpdate(actorState.mpNum, list.serverTimestamp))
            {
                Log(Debug::Verbose) << "[MP] ActorSync: skipped stale ActorPosition actor"
                                    << " cell=" << list.cellId
                                    << " refId=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " ts=" << list.serverTimestamp;
                continue;
            }

            const std::string actorKey = makeActorKey(actorState);
            if (ActorRuntime* primaryRuntime = findPrimaryActorRuntime(actorState))
            {
                mergeActorState(*primaryRuntime, actorState, true);
                queueSnapshot(*primaryRuntime, actorState, list);
                rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
                continue;
            }

            auto runtimeIt = cell.actors.find(actorKey);
            if (runtimeIt == cell.actors.end() && actorState.mpNum != 0)
            {
                ActorRuntime migratedRuntime;
                if (takeServerSpawnedRuntimeFromOtherCell(list.cellId, actorState, list.serverTimestamp, migratedRuntime))
                    runtimeIt = cell.actors.emplace(actorKey, std::move(migratedRuntime)).first;
                else
                {
                    auto mappedIt = mServerSpawnedActorsByMpNum.find(actorState.mpNum);
                    if (mappedIt != mServerSpawnedActorsByMpNum.end()
                        && !mappedIt->second.isEmpty()
                        && matchesActor(mappedIt->second, actorState))
                    {
                        ActorRuntime mappedRuntime;
                        mappedRuntime.boundActor = mappedIt->second;
                        runtimeIt = cell.actors.emplace(actorKey, std::move(mappedRuntime)).first;
                    }
                }
            }
            if (runtimeIt == cell.actors.end())
                runtimeIt = cell.actors.emplace(actorKey, ActorRuntime()).first;

            auto& runtime = runtimeIt->second;
            mergeActorState(runtime, actorState, true);
            queueSnapshot(runtime, actorState, list);
            rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
        }
    }

    void ActorSync::onActorPositionV2Update(const ActorPositionV2List& list)
    {
        if (list.protocolVersion != ActorSyncProtocolVersionV2)
        {
            Log(Debug::Warning) << "[MP] ActorSync: ignored unsupported ActorPositionV2 protocol="
                                << list.protocolVersion;
            return;
        }

        std::size_t accepted = 0;
        std::size_t invalidActorNetId = 0;
        std::size_t missingIdentity = 0;
        std::size_t stale = 0;
        std::size_t deadLiveSuppressed = 0;
        ActorInstanceId firstInvalidActorNetId = 0;
        ActorInstanceId firstMissingActorNetId = 0;
        for (const CompactActorSnapshot& snapshot : list.snapshots)
        {
            if (!isValidActorInstanceId(snapshot.actorNetId))
            {
                ++invalidActorNetId;
                if (firstInvalidActorNetId == 0)
                    firstInvalidActorNetId = snapshot.actorNetId;
                continue;
            }

            auto runtimeIt = mActorsByNetId.find(snapshot.actorNetId);
            if (runtimeIt == mActorsByNetId.end())
            {
                ++missingIdentity;
                if (firstMissingActorNetId == 0)
                    firstMissingActorNetId = snapshot.actorNetId;
                ++mActorV2MissingIdentityByNetIdWindow[snapshot.actorNetId];
                continue;
            }

            ActorRuntime& runtime = runtimeIt->second;
            if (runtime.lastServerTimestamp != 0
                && list.serverTimestamp != 0
                && list.serverTimestamp < runtime.lastServerTimestamp)
            {
                ++stale;
                continue;
            }

            const bool runtimeIsDead = runtime.state.isDead || runtime.deathAlreadyApplied;
            const bool snapshotIsDead = (snapshot.presentationFlags & ActorPresentationDead) != 0;
            if (runtimeIsDead && !snapshotIsDead)
            {
                ++deadLiveSuppressed;
                continue;
            }

            BaseActor actorState = runtime.state;
            actorState.position = snapshot.position;
            actorState.velocity = snapshot.velocity;

            ActorList synthetic;
            synthetic.cellId = actorState.cellId;
            synthetic.authorityGuid = list.authorityGuid;
            synthetic.authorityGeneration = list.authorityGeneration;
            synthetic.snapshotSequence = list.sequence;
            synthetic.serverTimestamp = list.serverTimestamp;
            mergeActorState(runtime, actorState, true);
            queueSnapshot(runtime, actorState, synthetic);
            rememberServerSpawnedActorTimestamp(actorState.mpNum, list.serverTimestamp);
            if (isWatchedBorderActor(runtime.state, runtime.state.cellId))
            {
                Log(Debug::Verbose) << "[MP] ActorSync border watch: ActorPositionV2"
                                    << " source=primary"
                                    << " actorNetId=" << snapshot.actorNetId
                                    << " refId=" << runtime.state.refId
                                    << " refNum=" << runtime.state.refNum
                                    << " mpNum=" << runtime.state.mpNum
                                    << " runtimeCell=" << runtime.state.cellId
                                    << " boundCell=" << cellIdForPtr(runtime.boundActor)
                                    << " latestPositionTs=" << runtime.lastServerTimestamp
                                    << " latestPresentationTs=" << runtime.lastPresentationServerTimestamp
                                    << " pos=(" << runtime.state.position.pos[0]
                                    << "," << runtime.state.position.pos[1]
                                    << "," << runtime.state.position.pos[2] << ")"
                                    << " seq=" << list.sequence;
            }
            ++accepted;
        }

        mActorV2SnapshotsWindow += accepted;
        mActorV2InvalidActorIdWindow += invalidActorNetId;
        mActorV2MissingIdentityWindow += missingIdentity;
        mActorV2StaleWindow += stale;
        mActorV2DeadLiveSuppressedWindow += deadLiveSuppressed;
        const uint64_t now = currentClientTimeMs();
        if (mActorV2DiagnosticsLastLogMs == 0)
            mActorV2DiagnosticsLastLogMs = now;
        else if (now - mActorV2DiagnosticsLastLogMs >= 1000)
        {
            ActorInstanceId noisiestMissingActorNetId = 0;
            std::size_t noisiestMissingCount = 0;
            for (const auto& [actorNetId, count] : mActorV2MissingIdentityByNetIdWindow)
            {
                if (count > noisiestMissingCount)
                {
                    noisiestMissingActorNetId = actorNetId;
                    noisiestMissingCount = count;
                }
            }
            Log(Debug::Info) << "[MP] ActorSync v2: position receive"
                             << " snapshots=" << mActorV2SnapshotsWindow
                             << " invalidActorNetId=" << mActorV2InvalidActorIdWindow
                             << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                             << " missingIdentity=" << mActorV2MissingIdentityWindow
                             << " firstMissingActorNetId=" << firstMissingActorNetId
                             << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
                             << " noisiestMissingActorNetId=" << noisiestMissingActorNetId
                             << " noisiestMissingActorKey=" << describeActorInstanceId(noisiestMissingActorNetId)
                             << " noisiestMissingCount=" << noisiestMissingCount
                             << " stale=" << mActorV2StaleWindow
                             << " deadLiveSuppressed=" << mActorV2DeadLiveSuppressedWindow
                             << " identityTransformPreserved=" << mActorV2IdentityTransformPreservedWindow
                             << " identityZeroTransformSkipped=" << mActorV2IdentityZeroTransformSkippedWindow
                             << " presentationSent=" << mActorV2PresentationSentWindow
                             << " presentationApplied=" << mActorV2PresentationAppliedWindow
                             << " presentationInvalidActorNetId="
                             << mActorV2PresentationInvalidActorIdWindow
                             << " presentationMissingIdentity=" << mActorV2PresentationMissingIdentityWindow
                             << " presentationStale=" << mActorV2PresentationStaleWindow
                             << " presentationDeadLiveSuppressed="
                             << mActorV2PresentationDeadLiveSuppressedWindow
                             << " presentationStopForced=" << mActorV2PresentationStopForcedWindow
                             << " presentationGroupChanged=" << mActorV2PresentationGroupChangedWindow
                             << " attackSent=" << mActorV2AttackSentWindow
                             << " attackApplied=" << mActorV2AttackAppliedWindow
                             << " attackInvalidActorNetId=" << mActorV2AttackInvalidActorIdWindow
                             << " attackMissingIdentity=" << mActorV2AttackMissingIdentityWindow
                             << " attackDuplicate=" << mActorV2AttackDuplicateWindow
                             << " attackDeadSuppressed=" << mActorV2AttackDeadSuppressedWindow
                             << " primaryActors=" << mActorsByNetId.size()
                             << " seq=" << list.sequence;
            mActorV2DiagnosticsLastLogMs = now;
            mActorV2SnapshotsWindow = 0;
            mActorV2InvalidActorIdWindow = 0;
            mActorV2MissingIdentityWindow = 0;
            mActorV2StaleWindow = 0;
            mActorV2DeadLiveSuppressedWindow = 0;
            mActorV2IdentityTransformPreservedWindow = 0;
            mActorV2IdentityZeroTransformSkippedWindow = 0;
            mActorV2PresentationSentWindow = 0;
            mActorV2PresentationAppliedWindow = 0;
            mActorV2PresentationInvalidActorIdWindow = 0;
            mActorV2PresentationMissingIdentityWindow = 0;
            mActorV2PresentationStaleWindow = 0;
            mActorV2PresentationDeadLiveSuppressedWindow = 0;
            mActorV2PresentationStopForcedWindow = 0;
            mActorV2PresentationGroupChangedWindow = 0;
            mActorV2AttackSentWindow = 0;
            mActorV2AttackAppliedWindow = 0;
            mActorV2AttackInvalidActorIdWindow = 0;
            mActorV2AttackMissingIdentityWindow = 0;
            mActorV2AttackDuplicateWindow = 0;
            mActorV2AttackDeadSuppressedWindow = 0;
            mActorV2MissingIdentityByNetIdWindow.clear();
        }
    }

    void ActorSync::onActorPresentationV2Update(const ActorPresentationV2List& list)
    {
        if (list.protocolVersion != ActorSyncProtocolVersionV2)
        {
            Log(Debug::Warning) << "[MP] ActorSync: ignored unsupported ActorPresentationV2 protocol="
                                << list.protocolVersion;
            return;
        }

        std::size_t applied = 0;
        std::size_t invalidActorNetId = 0;
        std::size_t missingIdentity = 0;
        std::size_t stale = 0;
        std::size_t deadLiveSuppressed = 0;
        std::size_t stopForced = 0;
        std::size_t groupChanged = 0;
        ActorInstanceId firstInvalidActorNetId = 0;
        ActorInstanceId firstMissingActorNetId = 0;

        for (const ActorPresentationSnapshot& snapshot : list.snapshots)
        {
            if (!isValidActorInstanceId(snapshot.actorNetId))
            {
                ++invalidActorNetId;
                if (firstInvalidActorNetId == 0)
                    firstInvalidActorNetId = snapshot.actorNetId;
                continue;
            }

            auto runtimeIt = mActorsByNetId.find(snapshot.actorNetId);
            if (runtimeIt == mActorsByNetId.end())
            {
                ++missingIdentity;
                if (firstMissingActorNetId == 0)
                    firstMissingActorNetId = snapshot.actorNetId;
                ++mActorV2MissingIdentityByNetIdWindow[snapshot.actorNetId];
                continue;
            }

            ActorRuntime& runtime = runtimeIt->second;
            if (runtime.lastPresentationServerTimestamp != 0
                && list.serverTimestamp != 0
                && list.serverTimestamp < runtime.lastPresentationServerTimestamp)
            {
                ++stale;
                continue;
            }

            const bool snapshotIsDead = snapshot.isDead || ((snapshot.presentationFlags & ActorPresentationDead) != 0);
            const bool runtimeIsDead = runtime.state.isDead || runtime.deathAlreadyApplied;
            if (runtimeIsDead && !snapshotIsDead)
            {
                ++deadLiveSuppressed;
                continue;
            }

            const bool wasMoving = runtime.state.isMoving
                || std::abs(runtime.state.animFlags.animFwd) > 0.1f
                || std::abs(runtime.state.animFlags.animSide) > 0.1f;
            const bool hadLocomotionGroup = isLocomotionAnimGroup(runtime.lastAppliedAnimGroup)
                || isLocomotionAnimGroup(runtime.state.animFlags.currentAnimGroup);
            const std::string previousGroup = runtime.state.animFlags.currentAnimGroup;

            applyActorPresentationFlags(runtime.state, snapshot.presentationFlags);
            runtime.state.isMoving = snapshot.isMoving;
            runtime.state.isAttackingOrCasting = snapshot.isAttackingOrCasting;
            runtime.state.hasWeaponDrawn = snapshot.hasWeaponDrawn;
            runtime.state.hasSpellReadied = snapshot.hasSpellReadied;
            runtime.state.isDead = snapshotIsDead;
            runtime.state.animFlags.movementFlags = snapshot.movementFlags;
            runtime.state.animFlags.animFwd = dequantizeActorAxis(snapshot.animFwd);
            runtime.state.animFlags.animSide = dequantizeActorAxis(snapshot.animSide);

            if (!snapshot.currentAnimGroup.empty())
                runtime.state.animFlags.currentAnimGroup = snapshot.currentAnimGroup;
            else if (!snapshot.isMoving)
                runtime.state.animFlags.currentAnimGroup.clear();

            if (!snapshot.isMoving)
            {
                runtime.state.animFlags.animFwd = 0.f;
                runtime.state.animFlags.animSide = 0.f;
                runtime.state.isMoving = false;
                if (wasMoving || hadLocomotionGroup)
                    ++stopForced;
            }
            if (snapshotIsDead)
            {
                runtime.state.isMoving = false;
                runtime.state.isAttackingOrCasting = false;
                runtime.state.velocity = Velocity {};
                runtime.state.animFlags.animFwd = 0.f;
                runtime.state.animFlags.animSide = 0.f;
                runtime.state.animFlags.movementFlags = 0;
            }

            if (runtime.state.animFlags.currentAnimGroup != previousGroup)
                ++groupChanged;

            if (list.serverTimestamp != 0)
                runtime.lastPresentationServerTimestamp = list.serverTimestamp;

            if (isWatchedPresentationActor(runtime.state, snapshot.actorNetId))
            {
                Log(Debug::Info) << "[MP] ActorSync v2: presentation apply"
                                 << " actorNetId=" << snapshot.actorNetId
                                 << " refId=" << runtime.state.refId
                                 << " refNum=" << runtime.state.refNum
                                 << " mpNum=" << runtime.state.mpNum
                                 << " cell=" << runtime.state.cellId
                                 << " boundCell=" << cellIdForPtr(runtime.boundActor)
                                 << " moving=" << runtime.state.isMoving
                                 << " fwd=" << runtime.state.animFlags.animFwd
                                 << " side=" << runtime.state.animFlags.animSide
                                 << " movementFlags=" << runtime.state.animFlags.movementFlags
                                 << " group='" << runtime.state.animFlags.currentAnimGroup << "'"
                                 << " flags=" << static_cast<unsigned>(snapshot.presentationFlags)
                                 << " latestPositionTs=" << runtime.lastServerTimestamp
                                 << " ts=" << list.serverTimestamp;
            }

            ++applied;
        }

        mActorV2PresentationAppliedWindow += applied;
        mActorV2PresentationInvalidActorIdWindow += invalidActorNetId;
        mActorV2PresentationMissingIdentityWindow += missingIdentity;
        mActorV2PresentationStaleWindow += stale;
        mActorV2PresentationDeadLiveSuppressedWindow += deadLiveSuppressed;
        mActorV2PresentationStopForcedWindow += stopForced;
        mActorV2PresentationGroupChangedWindow += groupChanged;

        if (invalidActorNetId != 0 || missingIdentity != 0 || stale != 0 || deadLiveSuppressed != 0)
        {
            Log(Debug::Verbose) << "[MP] ActorSync v2: presentation filtered"
                                << " snapshots=" << list.snapshots.size()
                                << " applied=" << applied
                                << " invalidActorNetId=" << invalidActorNetId
                                << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                                << " missingIdentity=" << missingIdentity
                                << " firstMissingActorNetId=" << firstMissingActorNetId
                                << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
                                << " stale=" << stale
                                << " deadLiveSuppressed=" << deadLiveSuppressed
                                << " seq=" << list.sequence;
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
            mergeActorState(runtime, actorState, false);
            logWatchedBorderActor("ActorAttack", list.cellId, actorState, &runtime, runtime.boundActor,
                runtime.actorNetId != 0 ? "primary-or-v2" : "cell-runtime");
            runtime.state.attack = actorState.attack;
            runtime.pendingAttack = true;
            // The authority already does edge detection, so each received packet
            // represents a new attack event. Reset lastAttackPressed so that
            // applyBoundActorState() will see a proper rising edge.
            runtime.lastAttackPressed = false;
        }
    }

    void ActorSync::onActorAttackV2(const ActorAttackV2List& list)
    {
        if (list.protocolVersion != ActorSyncProtocolVersionV2)
        {
            Log(Debug::Warning) << "[MP] ActorSync: ignored unsupported ActorAttackV2 protocol="
                                << list.protocolVersion;
            return;
        }

        std::size_t applied = 0;
        std::size_t invalidActorNetId = 0;
        std::size_t missingIdentity = 0;
        std::size_t duplicate = 0;
        std::size_t deadSuppressed = 0;
        ActorInstanceId firstInvalidActorNetId = 0;
        ActorInstanceId firstMissingActorNetId = 0;

        for (const ActorAttackV2Event& event : list.events)
        {
            if (!isValidActorInstanceId(event.actorNetId))
            {
                ++invalidActorNetId;
                if (firstInvalidActorNetId == 0)
                    firstInvalidActorNetId = event.actorNetId;
                continue;
            }

            auto runtimeIt = mActorsByNetId.find(event.actorNetId);
            if (runtimeIt == mActorsByNetId.end())
            {
                ++missingIdentity;
                if (firstMissingActorNetId == 0)
                    firstMissingActorNetId = event.actorNetId;
                ++mActorV2MissingIdentityByNetIdWindow[event.actorNetId];
                continue;
            }

            ActorRuntime& runtime = runtimeIt->second;
            if (runtime.state.isDead || runtime.deathAlreadyApplied)
            {
                ++deadSuppressed;
                continue;
            }

            if (!isNewerEventId(event.eventId, runtime.lastReceivedAttackEventId))
            {
                ++duplicate;
                continue;
            }

            runtime.state.attack = event.attack;
            runtime.pendingAttack = true;
            runtime.lastAttackPressed = false;
            runtime.lastReceivedAttackEventId = event.eventId;
            if (list.serverTimestamp != 0)
                runtime.lastPresentationServerTimestamp = std::max(runtime.lastPresentationServerTimestamp,
                    list.serverTimestamp);
            logWatchedBorderActor("ActorAttackV2", list.cellId, runtime.state, &runtime, runtime.boundActor,
                "actorNetId");
            ++applied;
        }

        mActorV2AttackAppliedWindow += applied;
        mActorV2AttackInvalidActorIdWindow += invalidActorNetId;
        mActorV2AttackMissingIdentityWindow += missingIdentity;
        mActorV2AttackDuplicateWindow += duplicate;
        mActorV2AttackDeadSuppressedWindow += deadSuppressed;

        if (invalidActorNetId != 0 || missingIdentity != 0 || duplicate != 0 || deadSuppressed != 0 || applied != 0)
        {
            Log((invalidActorNetId != 0 || missingIdentity != 0 || duplicate != 0 || deadSuppressed != 0)
                    ? Debug::Info : Debug::Verbose)
                << "[MP] ActorSync v2: attack receive"
                << " packetCell=" << list.cellId
                << " events=" << list.events.size()
                << " applied=" << applied
                << " invalidActorNetId=" << invalidActorNetId
                << " firstInvalidActorKey=" << describeActorInstanceId(firstInvalidActorNetId)
                << " missingIdentity=" << missingIdentity
                << " firstMissingActorNetId=" << firstMissingActorNetId
                << " firstMissingActorKey=" << describeActorInstanceId(firstMissingActorNetId)
                << " duplicate=" << duplicate
                << " deadSuppressed=" << deadSuppressed
                << " seq=" << list.sequence;
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
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

        std::size_t duplicate = 0;
        for (const auto& actorState : list.actors)
        {
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
            if (actorState.deathEventId != 0
                && runtime.lastReceivedDeathEventId != 0
                && !isNewerEventId(actorState.deathEventId, runtime.lastReceivedDeathEventId))
            {
                ++duplicate;
                continue;
            }

            const bool wasDead = runtime.state.isDead || runtime.deathAlreadyApplied;
            const float previousHealth = runtime.state.dynamicStats.health.current;
            mergeActorState(runtime, actorState, false);
            if (actorState.isDead)
            {
                const std::string deathCellId = actorState.cellId.empty() ? list.cellId : actorState.cellId;
                const std::string oldCellId = runtime.state.cellId;
                runtime.state.cellId = deathCellId;
                if (runtime.actorNetId != 0)
                    indexActorNetId(runtime.actorNetId, oldCellId, deathCellId);
            }
            runtime.state.position = actorState.position;
            runtime.smoothedPosition = actorState.position;
            runtime.hasSmoothedPosition = true;
            runtime.hasAuthoritativeTransform = true;
            if (list.serverTimestamp != 0)
                runtime.lastServerTimestamp = std::max(runtime.lastServerTimestamp, list.serverTimestamp);
            runtime.snapshots.clear();
            runtime.state.deathState = actorState.deathState;
            runtime.state.isDead = actorState.isDead;
            runtime.state.isInstantDeath = actorState.isInstantDeath;
            if (!actorState.deathAnimGroup.empty())
                runtime.state.deathAnimGroup = actorState.deathAnimGroup;
            runtime.state.dynamicStats.health.current = actorState.dynamicStats.health.current;
            runtime.deathFromRealtimePacket = actorState.isDead && !wasDead && !actorState.isInstantDeath;
            if (actorState.deathEventId != 0)
                runtime.lastReceivedDeathEventId = actorState.deathEventId;
            Log(actorState.isDead && !wasDead ? Debug::Info : Debug::Verbose)
                << "[MP] ActorSync: Death received for " << actorState.refId
                << " mpNum=" << actorState.mpNum
                << " eventId=" << actorState.deathEventId
                << " isDead=" << actorState.isDead
                << " prevHp=" << previousHealth
                << " hp=" << actorState.dynamicStats.health.current;
        }

        if (duplicate != 0)
        {
            Log(Debug::Verbose) << "[MP] ActorSync: ignored duplicate ActorDeath"
                                << " cell=" << list.cellId
                                << " count=" << duplicate
                                << " seq=" << list.snapshotSequence;
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
            if ((runtime.state.isDead || runtime.deathAlreadyApplied) && !actorState.isDead)
            {
                Log(Debug::Verbose) << "[MP] ActorSync: ignored live StatsDynamic for dead actor "
                                    << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " hp=" << actorState.dynamicStats.health.current
                                    << " seq=" << list.snapshotSequence;
                continue;
            }

            mergeActorState(runtime, actorState, false);
            runtime.state.dynamicStats = actorState.dynamicStats;
            runtime.state.isDead = actorState.isDead;
            Log(Debug::Verbose) << "[MP] ActorSync: StatsDynamic for " << actorState.refId
                                << " mpNum=" << actorState.mpNum
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
            auto& runtime = runtimeForPacketActor(list.cellId, cell, actorState);
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

                MWWorld::Ptr attacker = resolvePacketActorBinding(list.cellId, cell, actorState,
                    "CombatRequestDamage");

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
                if (attacker.isEmpty() && isMeleeAttackType(actorState.attack.type))
                {
                    Log(Debug::Warning) << "[MP] ActorSync: skipped NPC damage with missing melee attacker"
                                        << " attacker=" << actorState.refId
                                        << " refNum=" << actorState.refNum
                                        << " mpNum=" << actorState.mpNum
                                        << " actorNetId=" << actorNetIdForActorState(actorState)
                                        << " packetCell=" << list.cellId;
                    continue;
                }

                const float attackerDistanceSq = distanceSquared(attacker, player);
                if (isMeleeAttackType(actorState.attack.type)
                    && attackerDistanceSq > kMaxReplicatedMeleeHitDistance * kMaxReplicatedMeleeHitDistance)
                {
                    Log(Debug::Warning) << "[MP] ActorSync: skipped NPC damage outside melee range"
                                        << " attacker=" << actorState.refId
                                        << " refNum=" << actorState.refNum
                                        << " mpNum=" << actorState.mpNum
                                        << " actorNetId=" << actorNetIdForActorState(actorState)
                                        << " packetCell=" << list.cellId
                                        << " attackerCell=" << cellIdForPtr(attacker)
                                        << " distance=" << std::sqrt(attackerDistanceSq)
                                        << " max=" << kMaxReplicatedMeleeHitDistance;
                    continue;
                }

                MWBase::Environment::get().getLuaManager()->onHit(
                    attacker, player, weapon, MWWorld::Ptr(), actorState.attack.type,
                    actorState.attack.strength > 0.f ? actorState.attack.strength : 1.f, actorState.attack.damage,
                    actorState.attack.healthDamage, hitPos, true, sourceType);

                player.getClass().onHit(player, damages, weapon.isEmpty() ? ESM::RefId() : weapon.getCellRef().getRefId(),
                    attacker, true, sourceType);

                Log(Debug::Info) << "[MP] ActorSync: NPC damage applied to local player"
                                 << " damage=" << actorState.attack.damage
                                 << " stat=" << (actorState.attack.healthDamage ? "health" : "fatigue")
                                 << " attacker=" << actorState.refId
                                 << " attackerRefNum=" << actorState.refNum
                                 << " attackerMpNum=" << actorState.mpNum
                                 << " actorNetId=" << actorNetIdForActorState(actorState)
                                 << " packetCell=" << list.cellId
                                 << " attackerCell=" << cellIdForPtr(attacker)
                                 << " distance=" << (attackerDistanceSq >= 0.f ? std::sqrt(attackerDistanceSq) : -1.f);

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
            MWWorld::Ptr victim = resolvePacketActorBinding(list.cellId, cell, actorState,
                "CombatRequestVictim");

            if (victim.isEmpty())
            {
                Log(Debug::Warning) << "[MP] ActorSync: CombatRequest unresolved victim"
                                    << " cell=" << list.cellId
                                    << " victim=" << actorState.refId
                                    << " refNum=" << actorState.refNum
                                    << " mpNum=" << actorState.mpNum
                                    << " attackerGuid=" << list.authorityGuid;
                continue;
            }

            MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
            if (victimStats.isDead())
            {
                Log(Debug::Info) << "[MP] ActorSync: CombatRequest ignored for dead victim="
                                 << actorState.refId << " mpNum=" << actorState.mpNum;
                continue;
            }

            const MWMechanics::DamageSourceType sourceType
                = (actorState.attack.type == 2 || actorState.attack.type == 3)
                ? MWMechanics::DamageSourceType::Ranged
                : (actorState.attack.type == 1 ? MWMechanics::DamageSourceType::Magical
                                               : MWMechanics::DamageSourceType::Melee);
            const float attackerDistanceSq = distanceSquared(attacker, victim);
            if (actorState.attack.damage > 0.f && isMeleeAttackType(actorState.attack.type)
                && attackerDistanceSq > kMaxReplicatedMeleeHitDistance * kMaxReplicatedMeleeHitDistance)
            {
                Log(Debug::Warning) << "[MP] ActorSync: CombatRequest rejected outside melee range"
                                    << " guid=" << list.authorityGuid
                                    << " victim=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " distance=" << std::sqrt(attackerDistanceSq)
                                    << " max=" << kMaxReplicatedMeleeHitDistance;
                continue;
            }

            Log(Debug::Info) << "[MP] ActorSync: CombatRequest from guid=" << list.authorityGuid
                             << " victim=" << actorState.refId
                             << " mpNum=" << actorState.mpNum
                             << " attackerEmpty=" << attacker.isEmpty()
                             << " distance=" << (attackerDistanceSq >= 0.f ? std::sqrt(attackerDistanceSq) : -1.f);

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
                                 << " sourceType=" << static_cast<int>(sourceType)
                                 << " distance=" << (attackerDistanceSq >= 0.f ? std::sqrt(attackerDistanceSq) : -1.f);
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
        const uint32_t mappedMpNum = mappedMpNumForPtr(cellId, npcAttacker);
        if (mappedMpNum != 0)
        {
            npcActor.mpNum = mappedMpNum;
            npcActor.refNum = 0;
            rememberServerSpawnedActor(cellId, npcAttacker, mappedMpNum);
        }

        float victimDistanceSq = -1.f;
        if (Main::isInitialised())
        {
            if (RemotePlayer* victimPlayer = Main::get().getPlayerList().getPlayer(victimGuid))
                victimDistanceSq = distanceSquared(npcAttacker, victimPlayer->getNpcPtr());
        }
        if (isMeleeAttackType(attackType)
            && victimDistanceSq > kMaxReplicatedMeleeHitDistance * kMaxReplicatedMeleeHitDistance)
        {
            Log(Debug::Warning) << "[MP] ActorSync: sendNpcPlayerDamage skipped outside melee range"
                                << " victimGuid=" << victimGuid
                                << " attacker=" << npcActor.refId
                                << " attackerMpNum=" << npcActor.mpNum
                                << " distance=" << std::sqrt(victimDistanceSq)
                                << " max=" << kMaxReplicatedMeleeHitDistance;
            return;
        }

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
                         << " attacker=" << npcActor.refId
                         << " attackerMpNum=" << npcActor.mpNum
                         << " attackerRefNum=" << npcActor.refNum
                         << " cell=" << cellId
                         << " distance=" << (victimDistanceSq >= 0.f ? std::sqrt(victimDistanceSq) : -1.f);
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
        actor.hasAuthoritativeTransform = true;
        BufferedSnapshot snapshot;
        snapshot.position = state.position;
        snapshot.velocity = state.velocity;
        snapshot.sequence = list.snapshotSequence;
        snapshot.serverTimestamp = list.serverTimestamp;
        if (snapshot.serverTimestamp != 0)
            actor.lastServerTimestamp = std::max(actor.lastServerTimestamp, snapshot.serverTimestamp);

        const bool wasEmpty = actor.snapshots.empty();
        if (!actor.snapshots.empty() && actor.snapshots.back().sequence == snapshot.sequence)
            actor.snapshots.back() = snapshot;
        else
            actor.snapshots.push_back(snapshot);

        actor.latestSnapshotAge = 0.f;
        if (snapshot.serverTimestamp != 0
            && (wasEmpty || !actor.hasInterpolationRenderTimestamp))
        {
            actor.interpolationRenderTimestamp = static_cast<double>(snapshot.serverTimestamp);
            actor.hasInterpolationRenderTimestamp = true;
        }

        while (actor.snapshots.size() > 10)
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
        // V2 identity/position own cross-cell placement; reliable events may arrive from the previous cell.
        if (includeTransform || actor.actorNetId == 0 || actor.state.cellId.empty())
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

        actor.latestSnapshotAge += dt;

        if (!actor.hasSmoothedPosition)
        {
            actor.smoothedPosition = actor.snapshots.front().position;
            actor.hasSmoothedPosition = true;
        }

        if (actor.snapshots.back().serverTimestamp == 0)
        {
            while (actor.snapshots.size() > 2)
                actor.snapshots.pop_front();

            const Position& target = (actor.snapshots.size() >= 2)
                ? actor.snapshots[1].position
                : actor.snapshots.front().position;

            const float alpha = std::clamp(dt * 15.f, 0.f, 1.f);
            for (int i = 0; i < 3; ++i)
            {
                actor.smoothedPosition.pos[i] = lerpFloat(actor.smoothedPosition.pos[i], target.pos[i], alpha);
                actor.smoothedPosition.rot[i] = lerpAngle(actor.smoothedPosition.rot[i], target.rot[i], alpha);
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
            return;
        }

        static constexpr double kInterpolationDelayMs = 80.0;
        static constexpr double kMaxExtrapolationMs = 150.0;

        const double latestTimestamp = static_cast<double>(actor.snapshots.back().serverTimestamp);
        const double estimatedServerNow = latestTimestamp + (std::max(0.f, actor.latestSnapshotAge) * 1000.0);
        const double desiredRenderTimestamp = std::min(
            estimatedServerNow - kInterpolationDelayMs,
            latestTimestamp + kMaxExtrapolationMs);
        const double earliestTimestamp = static_cast<double>(actor.snapshots.front().serverTimestamp);
        const double targetRenderTimestamp = std::max(desiredRenderTimestamp, earliestTimestamp);

        if (!actor.hasInterpolationRenderTimestamp
            || actor.interpolationRenderTimestamp < earliestTimestamp
            || actor.interpolationRenderTimestamp > latestTimestamp + kMaxExtrapolationMs)
        {
            actor.interpolationRenderTimestamp = earliestTimestamp;
            actor.hasInterpolationRenderTimestamp = true;
        }
        else if (targetRenderTimestamp > actor.interpolationRenderTimestamp)
        {
            actor.interpolationRenderTimestamp = std::min(
                actor.interpolationRenderTimestamp + (std::max(0.f, dt) * 1000.0),
                targetRenderTimestamp);
        }
        else
            actor.interpolationRenderTimestamp = targetRenderTimestamp;

        const double renderTimestamp = actor.interpolationRenderTimestamp;
        while (actor.snapshots.size() >= 2
            && actor.snapshots[1].serverTimestamp != 0
            && static_cast<double>(actor.snapshots[1].serverTimestamp) <= renderTimestamp)
        {
            actor.snapshots.pop_front();
        }

        Position target = actor.snapshots.front().position;
        if (actor.snapshots.size() >= 2)
        {
            const BufferedSnapshot& from = actor.snapshots[0];
            const BufferedSnapshot& to = actor.snapshots[1];
            const double fromTimestamp = static_cast<double>(from.serverTimestamp);
            const double toTimestamp = static_cast<double>(to.serverTimestamp);
            if (toTimestamp > fromTimestamp && renderTimestamp >= fromTimestamp)
            {
                const float alpha = static_cast<float>(
                    std::clamp((renderTimestamp - fromTimestamp) / (toTimestamp - fromTimestamp), 0.0, 1.0));
                target = interpolatePosition(from.position, to.position, alpha);
            }
        }
        else if (renderTimestamp > latestTimestamp)
        {
            const double extrapolationMs = std::min(renderTimestamp - latestTimestamp, kMaxExtrapolationMs);
            const bool movementInputActive = std::abs(actor.state.animFlags.animFwd) > 0.1f
                || std::abs(actor.state.animFlags.animSide) > 0.1f;
            if (actor.state.isMoving && movementInputActive)
                target = extrapolatePosition(actor.snapshots.back().position, actor.snapshots.back().velocity,
                    extrapolationMs);
            else
                target = actor.snapshots.back().position;
        }

        actor.smoothedPosition = target;
        actor.state.position = actor.smoothedPosition;
    }

    void ActorSync::rememberServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum)
    {
        if (mpNum == 0 || ptr.isEmpty())
            return;

        mMpNumsByLocalActor[makeLocalActorKey(cellId, ptr)] = mpNum;
        mServerSpawnedActorsByMpNum[mpNum] = ptr;
        if (auto* baseNode = ptr.getRefData().getBaseNode())
            baseNode->setUserValue("mp_actor_mpnum", static_cast<int>(mpNum));
    }

    void ActorSync::forgetServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum)
    {
        if (!ptr.isEmpty())
        {
            mMpNumsByLocalActor.erase(makeLocalActorKey(cellId, ptr));

            const std::string currentCellId = cellIdForPtr(ptr);
            if (mpNum != 0 && !currentCellId.empty() && currentCellId != cellId)
            {
                rememberServerSpawnedActor(currentCellId, ptr, mpNum);
                return;
            }
        }

        if (mpNum == 0)
            return;

        auto it = mServerSpawnedActorsByMpNum.find(mpNum);
        if (it != mServerSpawnedActorsByMpNum.end() && (ptr.isEmpty() || it->second == ptr))
            mServerSpawnedActorsByMpNum.erase(it);

        if (!ptr.isEmpty())
        {
            if (auto* baseNode = ptr.getRefData().getBaseNode())
            {
                int stampedMpNum = 0;
                if (baseNode->getUserValue("mp_actor_mpnum", stampedMpNum)
                    && stampedMpNum > 0
                    && static_cast<uint32_t>(stampedMpNum) == mpNum)
                {
                    baseNode->setUserValue("mp_actor_mpnum", 0);
                }
            }
        }
    }

    void ActorSync::forgetServerSpawnedActorPtrMappings(const MWWorld::Ptr& ptr, uint32_t mpNum)
    {
        if (ptr.isEmpty())
            return;

        for (const auto& [knownCellId, cell] : mCells)
            mMpNumsByLocalActor.erase(makeLocalActorKey(knownCellId, ptr));

        const std::string currentCellId = cellIdForPtr(ptr);
        if (!currentCellId.empty())
            mMpNumsByLocalActor.erase(makeLocalActorKey(currentCellId, ptr));

        if (mpNum != 0)
        {
            auto mappedIt = mServerSpawnedActorsByMpNum.find(mpNum);
            if (mappedIt != mServerSpawnedActorsByMpNum.end() && mappedIt->second == ptr)
                mServerSpawnedActorsByMpNum.erase(mappedIt);
        }

        if (auto* baseNode = ptr.getRefData().getBaseNode())
        {
            int stampedMpNum = 0;
            if (baseNode->getUserValue("mp_actor_mpnum", stampedMpNum)
                && (mpNum == 0 || static_cast<uint32_t>(stampedMpNum) == mpNum))
            {
                baseNode->setUserValue("mp_actor_mpnum", 0);
            }
        }
    }

    uint32_t ActorSync::mappedMpNumForPtr(const std::string& cellId, const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return 0;

        auto localIt = mMpNumsByLocalActor.find(makeLocalActorKey(cellId, ptr));
        if (localIt != mMpNumsByLocalActor.end())
            return localIt->second;

        if (const uint32_t stampedMpNum = stampedServerSpawnedMpNum(ptr))
            return stampedMpNum;

        for (const auto& [mpNum, mappedPtr] : mServerSpawnedActorsByMpNum)
        {
            if (!mappedPtr.isEmpty() && mappedPtr == ptr)
                return mpNum;
        }

        return 0;
    }

    bool ActorSync::isStaleServerSpawnedActorUpdate(uint32_t mpNum, uint64_t serverTimestamp) const
    {
        if (mpNum == 0 || serverTimestamp == 0)
            return false;

        auto it = mServerSpawnedActorLastTimestamps.find(mpNum);
        return it != mServerSpawnedActorLastTimestamps.end() && serverTimestamp < it->second;
    }

    void ActorSync::rememberServerSpawnedActorTimestamp(uint32_t mpNum, uint64_t serverTimestamp)
    {
        if (mpNum == 0 || serverTimestamp == 0)
            return;

        auto& latest = mServerSpawnedActorLastTimestamps[mpNum];
        latest = std::max(latest, serverTimestamp);
    }

    bool ActorSync::takeServerSpawnedRuntimeFromOtherCell(const std::string& targetCellId,
        const BaseActor& actorState, uint64_t serverTimestamp, ActorRuntime& runtime)
    {
        if (actorState.mpNum == 0)
            return false;

        const std::string actorKey = makeActorKey(actorState);
        for (auto& [cellId, cell] : mCells)
        {
            if (cellId == targetCellId)
                continue;

            auto runtimeIt = cell.actors.find(actorKey);
            if (runtimeIt == cell.actors.end())
                continue;

            if (serverTimestamp != 0 && runtimeIt->second.lastServerTimestamp != 0
                && serverTimestamp < runtimeIt->second.lastServerTimestamp)
            {
                Log(Debug::Verbose) << "[MP] ActorSync: ignored stale cross-cell runtime update"
                                    << " refId=" << actorState.refId
                                    << " mpNum=" << actorState.mpNum
                                    << " from=" << cellId
                                    << " to=" << targetCellId
                                    << " ts=" << serverTimestamp
                                    << " latestTs=" << runtimeIt->second.lastServerTimestamp;
                return false;
            }

            runtime = std::move(runtimeIt->second);
            cell.actors.erase(runtimeIt);
            runtime.bindingLogged = false;
            Log(Debug::Info) << "[MP] ActorSync: runtime migrated between cells"
                             << " refId=" << actorState.refId
                             << " mpNum=" << actorState.mpNum
                             << " from=" << cellId
                             << " to=" << targetCellId;
            return true;
        }

        return false;
    }

    void ActorSync::applyBootstrapDeathState(ActorRuntime& actor)
    {
        if (!actor.state.isDead || actor.deathFromRealtimePacket || actor.boundActor.isEmpty()
            || !actor.boundActor.getClass().isActor())
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return;

        const bool firstBootstrapApply = !actor.deathAlreadyApplied;
        if (actor.hasAuthoritativeTransform)
        {
            const bool zeroSpawnedExteriorCorpse = actor.state.mpNum != 0
                && isExteriorActorCellId(actor.state.cellId)
                && isZeroIdentityPosition(actor.state.position)
                && !actor.state.position.isTeleporting;

            if (!zeroSpawnedExteriorCorpse)
            {
                const ESM::Position& currentPos = actor.boundActor.getRefData().getPosition();
                const float dx = currentPos.pos[0] - actor.state.position.pos[0];
                const float dy = currentPos.pos[1] - actor.state.position.pos[1];
                const float dz = currentPos.pos[2] - actor.state.position.pos[2];
                const float distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
                const float maxRotDelta = std::max({
                    std::abs(shortestAngleDelta(currentPos.rot[0], actor.state.position.rot[0])),
                    std::abs(shortestAngleDelta(currentPos.rot[1], actor.state.position.rot[1])),
                    std::abs(shortestAngleDelta(currentPos.rot[2], actor.state.position.rot[2])) });

                if (distanceSq > 4.f || maxRotDelta > 0.001f)
                {
                    actor.boundActor = world->moveObject(actor.boundActor,
                        osg::Vec3f(actor.state.position.pos[0], actor.state.position.pos[1],
                            actor.state.position.pos[2]));
                    if (actor.boundActor.isEmpty())
                        return;

                    world->rotateObject(actor.boundActor,
                        osg::Vec3f(actor.state.position.rot[0], actor.state.position.rot[1],
                            actor.state.position.rot[2]));
                    actor.smoothedPosition = actor.state.position;
                    actor.hasSmoothedPosition = true;
                    actor.snapshots.clear();

                    if (firstBootstrapApply || distanceSq > 4096.f)
                    {
                        Log(firstBootstrapApply ? Debug::Info : Debug::Verbose)
                            << "[MP] ActorSync: placed bootstrap corpse"
                            << " refId=" << actor.state.refId
                            << " refNum=" << actor.state.refNum
                            << " mpNum=" << actor.state.mpNum
                            << " actorNetId=" << actor.actorNetId
                            << " cell=" << actor.state.cellId
                            << " distance=" << std::sqrt(distanceSq)
                            << " pos=(" << actor.state.position.pos[0] << ","
                            << actor.state.position.pos[1] << ","
                            << actor.state.position.pos[2] << ")";
                    }
                }
            }
            else if (firstBootstrapApply)
            {
                Log(Debug::Warning) << "[MP] ActorSync: skipped zero-position bootstrap corpse placement"
                                    << " refId=" << actor.state.refId
                                    << " mpNum=" << actor.state.mpNum
                                    << " cell=" << actor.state.cellId;
            }
        }

        MWMechanics::CreatureStats& stats = actor.boundActor.getClass().getCreatureStats(actor.boundActor);

        if (!actor.state.deathAnimGroup.empty())
        {
            if (actor.appliedDeathAnimGroup.empty())
                actor.appliedDeathAnimGroup = actor.state.deathAnimGroup;
            if (auto* baseNode = actor.boundActor.getRefData().getBaseNode())
                baseNode->setUserValue("mp_death_anim_group", actor.appliedDeathAnimGroup);
        }

        const bool wasAlive = !stats.isDead();
        if (wasAlive)
        {
            MWMechanics::DynamicStat<float> deadHealth = stats.getHealth();
            deadHealth.setCurrent(0.f);
            stats.setDeathAnimationFinished(true);
            stats.setHealth(deadHealth);
            Log(Debug::Info) << "[MP] ActorSync: bootstrap dead actor state"
                             << " refId=" << actor.state.refId
                             << " mpNum=" << actor.state.mpNum
                             << " deathAnim='" << actor.appliedDeathAnimGroup << "'";
        }
        else
        {
            stats.setDeathAnimationFinished(true);
        }

        stats.setAttackingOrSpell(false);
        stats.setDrawState(MWMechanics::DrawState::Nothing);
        stats.getAiSequence().clear();
        world->enableActorCollision(actor.boundActor, false);
        if (MWRender::Animation* anim = world->getAnimation(actor.boundActor))
            anim->removeEffects();

        actor.lastAppliedAnimGroup.clear();
        actor.lastAppliedHitFlags = 0;
        actor.lastAttackPressed = false;
        actor.pendingAttack = false;
        actor.animGroupHoldTimer = 0.f;
        actor.attackDrawHoldTimer = 0.f;
        actor.attackLocomotionHoldTimer = 0.f;
        actor.deathAlreadyApplied = true;
        actor.deathFromRealtimePacket = false;
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
            cell.positionDiagnosticsTimer = 0.f;
            cell.positionSendCursor = 0;
            cell.priorityPositionSendCursor = 0;
            cell.actors.clear();
            cell.authorityLoggedMpNums.clear();
            cell.authoritySkippedUnmanagedSpawners.clear();
        }

        MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
        MWWorld::CellStore* targetCell = findActiveCellById(worldImp, cell.outboundCellId);
        if (!targetCell)
            return;

        for (auto& [actorKey, runtime] : cell.actors)
        {
            runtime.timeSinceLastPositionSend += std::max(0.f, dt);
            runtime.presentationSendTimer += std::max(0.f, dt);
        }

        ActorList outgoing;
        outgoing.cellId = cell.outboundCellId;
        outgoing.isAuthority = true;
        outgoing.authorityGuid = Main::get().getPlayerSync().localPlayer().guid;
        struct DuplicateServerSpawnedActor
        {
            MWWorld::Ptr ptr;
            std::string cellId;
            std::string canonicalCellId;
            std::string refId;
            uint32_t localRefNum = 0;
            uint32_t mpNum = 0;
        };
        struct SentServerSpawnedActor : DuplicateServerSpawnedActor
        {
            std::size_t outgoingIndex = 0;
        };
        std::vector<DuplicateServerSpawnedActor> duplicateServerSpawnedActors;
        std::unordered_map<uint32_t, SentServerSpawnedActor> sentServerSpawnedActors;

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

            // As authority, clear any residual mp_remote_actor flag so onHit
            // applies health damage normally.  The flag is stamped by
            // applyBoundActorState() during the brief pre-authority window
            // (between server sending initial actor state and the authority
            // grant packet arriving) and MUST be cleared once we own the cell.
            // Without this, every server-spawned actor in the cell is immune to
            // the authority player's attacks (onHit bails early for puppet NPCs).
            if (auto* baseNode = ptr.getRefData().getBaseNode())
                baseNode->setUserValue("mp_remote_actor", false);

            BaseActor actor;
            actor.refId = ptr.getCellRef().getRefId().serializeText();
            actor.refNum = ptr.getCellRef().getRefNum().mIndex;
            actor.cellId = cell.outboundCellId;
            const auto& pos = ptr.getRefData().getPosition();
            for (int i = 0; i < 3; ++i)
            {
                actor.position.pos[i] = pos.pos[i];
                actor.position.rot[i] = pos.rot[i];
            }

            const uint32_t mappedMpNum = mappedMpNumForPtr(cell.outboundCellId, ptr);
            if (mappedMpNum != 0)
            {
                MWWorld::Ptr canonicalPtr;
                const ActorInstanceId actorNetId = packActorInstanceKey({ ActorKeyKind::SpawnedMpNum, mappedMpNum });
                auto primaryIt = mActorsByNetId.find(actorNetId);
                if (primaryIt != mActorsByNetId.end() && !primaryIt->second.boundActor.isEmpty())
                    canonicalPtr = primaryIt->second.boundActor;
                else
                {
                    auto mappedIt = mServerSpawnedActorsByMpNum.find(mappedMpNum);
                    if (mappedIt != mServerSpawnedActorsByMpNum.end() && !mappedIt->second.isEmpty())
                        canonicalPtr = mappedIt->second;
                }

                if (!canonicalPtr.isEmpty() && !(canonicalPtr == ptr))
                {
                    if (actor.refId == "fargoth")
                    {
                        Log(Debug::Verbose) << "[MP] ActorSync: refreshed stale server-spawned canonical Ptr"
                                            << " cell=" << cell.outboundCellId
                                            << " canonicalCell=" << cellIdForPtr(canonicalPtr)
                                            << " refId=" << actor.refId
                                            << " localRefNum=" << actor.refNum
                                            << " mpNum=" << mappedMpNum;
                    }
                }

                actor.mpNum = mappedMpNum;
                actor.refNum = 0;
                if (cell.authorityLoggedMpNums.insert(mappedMpNum).second)
                {
                    Log(Debug::Info) << "[MP] ActorSync: authority mapped actor"
                                     << " cell=" << cell.outboundCellId
                                     << " refId=" << actor.refId
                                     << " mappedMpNum=" << mappedMpNum
                                     << " pos=(" << actor.position.pos[0] << "," << actor.position.pos[1] << "," << actor.position.pos[2] << ")";
                }
            }
            else if (actor.refId == "fargoth" || actor.refId.find("spawner_") != std::string::npos)
            {
                Log(Debug::Verbose) << "[MP] ActorSync: authority actor has no mapped mpNum"
                                    << " cell=" << cell.outboundCellId
                                    << " refId=" << actor.refId
                                    << " localRefNum=" << actor.refNum
                                    << " pos=(" << actor.position.pos[0] << "," << actor.position.pos[1] << "," << actor.position.pos[2] << ")";
            }

            if (isUnmanagedSpawnerActor(actor))
            {
                const std::string actorKey = makeActorKey(actor);
                if (cell.authoritySkippedUnmanagedSpawners.insert(actorKey).second)
                {
                    Log(Debug::Info) << "[MP] ActorSync: authority skipped unmanaged spawner actor"
                                     << " cell=" << cell.outboundCellId
                                     << " refId=" << actor.refId
                                     << " localRefNum=" << actor.refNum
                                     << " pos=(" << actor.position.pos[0] << "," << actor.position.pos[1] << "," << actor.position.pos[2] << ")";
                }
                return true;
            }

            const std::string actorKey = makeActorKey(actor);
            const auto previousRuntime = cell.actors.find(actorKey);
            if (previousRuntime != cell.actors.end() && dt > 0.0001f)
            {
                const Position& previousPosition = previousRuntime->second.state.position;
                for (int i = 0; i < 3; ++i)
                {
                    actor.velocity.linear[i] = (actor.position.pos[i] - previousPosition.pos[i]) / dt;
                    actor.velocity.angular[i] = shortestAngleDelta(actor.position.rot[i], previousPosition.rot[i]) / dt;
                }
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
            if (!actor.isDead && ptr.getClass().hasInventoryStore(ptr))
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

            if (actor.mpNum != 0)
            {
                const ActorInstanceId actorNetId = packActorInstanceKey({ ActorKeyKind::SpawnedMpNum, actor.mpNum });
                auto primaryIt = mActorsByNetId.find(actorNetId);
                const MWWorld::Ptr primaryPtr = primaryIt != mActorsByNetId.end()
                    ? primaryIt->second.boundActor : MWWorld::Ptr();
                const bool currentIsPrimary = !primaryPtr.isEmpty()
                    && (primaryPtr == ptr || sameLocalActorObject(primaryPtr, ptr));

                SentServerSpawnedActor current;
                current.ptr = ptr;
                current.cellId = cell.outboundCellId;
                current.canonicalCellId = !primaryPtr.isEmpty() ? cellIdForPtr(primaryPtr) : cellIdForPtr(ptr);
                current.refId = actor.refId;
                current.localRefNum = ptr.getCellRef().getRefNum().mIndex;
                current.mpNum = actor.mpNum;
                current.outgoingIndex = outgoing.actors.size();

                auto [sentIt, inserted] = sentServerSpawnedActors.emplace(actor.mpNum, current);
                if (!inserted)
                {
                    SentServerSpawnedActor& previous = sentIt->second;
                    if (currentIsPrimary)
                    {
                        if (!(previous.ptr == ptr) && !sameLocalActorObject(previous.ptr, ptr))
                            duplicateServerSpawnedActors.push_back(previous);

                        current.outgoingIndex = previous.outgoingIndex;
                        outgoing.actors[previous.outgoingIndex] = actor;
                        previous = current;
                        rememberServerSpawnedActor(cell.outboundCellId, ptr, actor.mpNum);
                        if (primaryIt != mActorsByNetId.end())
                            primaryIt->second.boundActor = ptr;
                    }
                    else
                    {
                        duplicateServerSpawnedActors.push_back(current);
                    }
                    return true;
                }

                rememberServerSpawnedActor(cell.outboundCellId, ptr, actor.mpNum);
                if (primaryIt != mActorsByNetId.end() && (primaryPtr.isEmpty() || currentIsPrimary))
                    primaryIt->second.boundActor = ptr;
            }

            outgoing.actors.push_back(std::move(actor));
            return true;
        });

        for (const DuplicateServerSpawnedActor& duplicate : duplicateServerSpawnedActors)
        {
            Log(Debug::Info) << "[MP] ActorSync: removed duplicate server-spawned actor"
                             << " cell=" << duplicate.cellId
                             << " canonicalCell=" << duplicate.canonicalCellId
                             << " refId=" << duplicate.refId
                             << " localRefNum=" << duplicate.localRefNum
                             << " mpNum=" << duplicate.mpNum;
            forgetServerSpawnedActorPtrMappings(duplicate.ptr, duplicate.mpNum);
            world->deleteObject(duplicate.ptr);
        }

        std::sort(outgoing.actors.begin(), outgoing.actors.end(), [](const BaseActor& lhs, const BaseActor& rhs) {
            return makeActorKey(lhs) < makeActorKey(rhs);
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
        cell.positionDiagnosticsTimer += std::max(0.f, dt);
        if (cell.positionSendTimer >= 0.05f)
        {
            cell.positionSendTimer = 0.f;

            // Cap the number of actors per position packet to avoid flooding the
            // server when a cell has many actors (e.g. spawner stress test).
            // The full ActorList (used for keyset changes) is unaffected.
            static constexpr std::size_t kMaxActorsPerPositionPacket = 12;
            static constexpr std::size_t kBaseMaxPositionPacketsPerTick = 2;
            static constexpr std::size_t kPressureMaxPositionPacketsPerTick = 3;
            static constexpr float kActivePositionMaxAge = 0.10f;
            static constexpr float kAbsolutePositionMaxAge = 0.25f;

            auto positionSendAge = [&](const BaseActor& actor) {
                const auto runtimeIt = cell.actors.find(makeActorKey(actor));
                return runtimeIt != cell.actors.end() ? runtimeIt->second.timeSinceLastPositionSend : 0.f;
            };

            float maxPositionSendAge = 0.f;
            bool includesHeddvild = false;
            bool includesMp2601 = false;
            std::string includedHeddvildMpNums;
            std::string sentHeddvildMpNums;

            auto appendHeddvildMpNum = [](std::string& out, const BaseActor& actor) {
                if (actor.refId != "heddvild")
                    return;
                if (!out.empty())
                    out += ",";
                out += std::to_string(actor.mpNum);
            };

            for (const BaseActor& actor : outgoing.actors)
            {
                maxPositionSendAge = std::max(maxPositionSendAge, positionSendAge(actor));
                includesHeddvild = includesHeddvild || actor.refId == "heddvild";
                includesMp2601 = includesMp2601 || actor.mpNum == 2601;
                appendHeddvildMpNum(includedHeddvildMpNums, actor);
            }

            std::size_t priorityCount = 0;
            std::size_t normalCount = 0;
            std::size_t chunksSent = 0;
            std::size_t sentActorCount = 0;
            std::size_t invalidPositionIdentity = 0;
            bool sentHeddvild = false;
            bool sentMp2601 = false;

            auto makePositionChunk = [&]() {
                ActorList chunk;
                chunk.cellId = outgoing.cellId;
                chunk.isAuthority = outgoing.isAuthority;
                chunk.authorityGuid = outgoing.authorityGuid;
                chunk.authorityGeneration = outgoing.authorityGeneration;
                chunk.snapshotSequence = outgoing.snapshotSequence;
                chunk.serverTimestamp = outgoing.serverTimestamp;
                chunk.actors.reserve(kMaxActorsPerPositionPacket);
                return chunk;
            };

            auto sendPositionChunk = [&](ActorList& chunk) {
                ActorPositionV2List compact;
                compact.protocolVersion = ActorSyncProtocolVersionV2;
                compact.authorityGuid = chunk.authorityGuid;
                compact.authorityGeneration = chunk.authorityGeneration;
                compact.sequence = chunk.snapshotSequence;
                compact.serverTimestamp = chunk.serverTimestamp;
                compact.snapshots.reserve(chunk.actors.size());

                for (const BaseActor& actor : chunk.actors)
                {
                    const ActorInstanceId actorNetId = actorNetIdForActorState(actor);
                    if (actorNetId == 0)
                    {
                        ++invalidPositionIdentity;
                        continue;
                    }

                    CompactActorSnapshot snapshot;
                    snapshot.actorNetId = actorNetId;
                    snapshot.position = actor.position;
                    snapshot.velocity = actor.velocity;
                    snapshot.movementFlags = static_cast<uint16_t>(actor.animFlags.movementFlags);
                    snapshot.animFwd = quantizeActorAxis(actor.animFlags.animFwd);
                    snapshot.animSide = quantizeActorAxis(actor.animFlags.animSide);
                    snapshot.presentationFlags = makeActorPresentationFlags(actor);
                    compact.snapshots.push_back(snapshot);
                }

                if (compact.snapshots.empty())
                    return;

                PacketActorPositionV2 positionPacket;
                positionPacket.setPositionList(&compact);
                mClient.sendUnreliable(positionPacket.encode());
                ++chunksSent;
            };

            auto resetPositionSendAge = [&](const BaseActor& actor) {
                const auto runtimeIt = cell.actors.find(makeActorKey(actor));
                if (runtimeIt != cell.actors.end())
                    runtimeIt->second.timeSinceLastPositionSend = 0.f;
            };

            if (outgoing.actors.empty())
            {
                cell.positionSendCursor = 0;
                cell.priorityPositionSendCursor = 0;
            }
            else if (outgoing.actors.size() > kMaxActorsPerPositionPacket)
            {
                const std::size_t actorCount = outgoing.actors.size();
                std::vector<std::size_t> urgentIndices;
                std::vector<std::size_t> priorityIndices;
                std::vector<std::size_t> normalIndices;
                urgentIndices.reserve(actorCount);
                priorityIndices.reserve(actorCount);
                normalIndices.reserve(actorCount);

                for (std::size_t i = 0; i < actorCount; ++i)
                {
                    const BaseActor& actor = outgoing.actors[i];
                    if (actorNetIdForActorState(actor) == 0)
                    {
                        ++invalidPositionIdentity;
                        continue;
                    }
                    const auto previousRuntime = cell.actors.find(makeActorKey(actor));
                    const bool hasPrevious = previousRuntime != cell.actors.end();
                    const float previousHealth = hasPrevious
                        ? previousRuntime->second.state.dynamicStats.health.current
                        : actor.dynamicStats.health.current;
                    const bool previousMoving = hasPrevious && previousRuntime->second.state.isMoving;

                    const bool highPriority = isHighPriorityPositionActor(actor, hasPrevious, previousHealth, previousMoving);
                    const float sendAge = hasPrevious ? previousRuntime->second.timeSinceLastPositionSend : 0.f;
                    if (sendAge >= kAbsolutePositionMaxAge || (highPriority && sendAge >= kActivePositionMaxAge))
                        urgentIndices.push_back(i);
                    else if (highPriority)
                        priorityIndices.push_back(i);
                    else
                        normalIndices.push_back(i);
                }
                priorityCount = urgentIndices.size() + priorityIndices.size();
                normalCount = normalIndices.size();

                std::sort(urgentIndices.begin(), urgentIndices.end(), [&](std::size_t lhs, std::size_t rhs) {
                    const float lhsAge = positionSendAge(outgoing.actors[lhs]);
                    const float rhsAge = positionSendAge(outgoing.actors[rhs]);
                    if (lhsAge != rhsAge)
                        return lhsAge > rhsAge;
                    return makeActorKey(outgoing.actors[lhs]) < makeActorKey(outgoing.actors[rhs]);
                });

                std::vector<char> sent(actorCount, 0);
                auto appendActor = [&](std::size_t actorIndex, ActorList& chunk) {
                    if (actorIndex >= actorCount || sent[actorIndex] || chunk.actors.size() >= kMaxActorsPerPositionPacket)
                        return false;

                    const BaseActor& actor = outgoing.actors[actorIndex];
                    sent[actorIndex] = 1;
                    chunk.actors.push_back(actor);
                    resetPositionSendAge(actor);
                    ++sentActorCount;
                    sentHeddvild = sentHeddvild || actor.refId == "heddvild";
                    sentMp2601 = sentMp2601 || actor.mpNum == 2601;
                    appendHeddvildMpNum(sentHeddvildMpNums, actor);
                    return true;
                };

                std::size_t urgentCursor = 0;
                auto appendLinear = [&](const std::vector<std::size_t>& indices, ActorList& chunk) {
                    while (urgentCursor < indices.size() && chunk.actors.size() < kMaxActorsPerPositionPacket)
                    {
                        appendActor(indices[urgentCursor], chunk);
                        ++urgentCursor;
                    }
                };

                auto appendRoundRobin = [&](const std::vector<std::size_t>& indices, std::size_t& cursor, ActorList& chunk)
                {
                    if (indices.empty())
                    {
                        cursor = 0;
                        return;
                    }
                    if (chunk.actors.size() >= kMaxActorsPerPositionPacket)
                        return;

                    const std::size_t start = cursor % indices.size();
                    std::size_t considered = 0;
                    while (considered < indices.size() && chunk.actors.size() < kMaxActorsPerPositionPacket)
                    {
                        appendActor(indices[(start + considered) % indices.size()], chunk);
                        ++considered;
                    }
                    cursor = (start + considered) % indices.size();
                };

                const bool positionBudgetBehind = actorCount > kMaxActorsPerPositionPacket * kBaseMaxPositionPacketsPerTick
                    && maxPositionSendAge >= kAbsolutePositionMaxAge;
                const std::size_t maxPositionPacketsThisTick = positionBudgetBehind
                    ? kPressureMaxPositionPacketsPerTick
                    : kBaseMaxPositionPacketsPerTick;

                for (std::size_t packetIndex = 0;
                     packetIndex < maxPositionPacketsThisTick && sentActorCount < actorCount;
                     ++packetIndex)
                {
                    ActorList chunk = makePositionChunk();
                    appendLinear(urgentIndices, chunk);
                    appendRoundRobin(priorityIndices, cell.priorityPositionSendCursor, chunk);
                    appendRoundRobin(normalIndices, cell.positionSendCursor, chunk);
                    if (chunk.actors.empty())
                        break;
                    sendPositionChunk(chunk);
                }
            }
            else
            {
                cell.positionSendCursor = 0;
                cell.priorityPositionSendCursor = 0;
                ActorList chunk = makePositionChunk();
                chunk.actors.reserve(outgoing.actors.size());
                for (const BaseActor& actor : outgoing.actors)
                {
                    if (actorNetIdForActorState(actor) == 0)
                    {
                        ++invalidPositionIdentity;
                        continue;
                    }
                    ++priorityCount;
                    ++sentActorCount;
                    chunk.actors.push_back(actor);
                    resetPositionSendAge(actor);
                    sentHeddvild = sentHeddvild || actor.refId == "heddvild";
                    sentMp2601 = sentMp2601 || actor.mpNum == 2601;
                    appendHeddvildMpNum(sentHeddvildMpNums, actor);
                }
                sendPositionChunk(chunk);
            }

            if (cell.positionDiagnosticsTimer >= 1.f)
            {
                cell.positionDiagnosticsTimer = 0.f;
                constexpr float kPositionDiagnosticsInfoAge = 0.25f;
                const bool importantPositionBudget = invalidPositionIdentity != 0
                    || maxPositionSendAge >= kPositionDiagnosticsInfoAge
                    || sentHeddvild
                    || sentMp2601;
                Log(importantPositionBudget ? Debug::Info : Debug::Verbose)
                    << "[MP] ActorSync: position send budget"
                    << " cell=" << outgoing.cellId
                    << " outgoing=" << outgoing.actors.size()
                    << " priority=" << priorityCount
                    << " normal=" << normalCount
                    << " chunks=" << chunksSent
                    << " sent=" << sentActorCount
                    << " invalidIdentity=" << invalidPositionIdentity
                    << " priorityCursor=" << cell.priorityPositionSendCursor
                    << " normalCursor=" << cell.positionSendCursor
                    << " maxAgeMs=" << (maxPositionSendAge * 1000.f)
                    << " includesHeddvild=" << includesHeddvild
                    << " sentHeddvild=" << sentHeddvild
                    << " heddvildMpNums=" << includedHeddvildMpNums
                    << " sentHeddvildMpNums=" << sentHeddvildMpNums
                    << " includesMp2601=" << includesMp2601
                    << " sentMp2601=" << sentMp2601;
            }
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

        ActorAttackV2List attackV2Update;
        attackV2Update.protocolVersion = ActorSyncProtocolVersionV2;
        attackV2Update.cellId = outgoing.cellId;
        attackV2Update.authorityGuid = outgoing.authorityGuid;
        attackV2Update.authorityGeneration = outgoing.authorityGeneration;
        attackV2Update.sequence = outgoing.snapshotSequence;
        attackV2Update.serverTimestamp = outgoing.serverTimestamp;

        ActorList castUpdate;
        castUpdate.cellId = outgoing.cellId;
        castUpdate.isAuthority = true;
        castUpdate.authorityGuid = outgoing.authorityGuid;

        ActorList equipmentUpdate;
        equipmentUpdate.cellId = outgoing.cellId;
        equipmentUpdate.isAuthority = true;
        equipmentUpdate.authorityGuid = outgoing.authorityGuid;

        ActorPresentationV2List presentationUpdate;
        presentationUpdate.protocolVersion = ActorSyncProtocolVersionV2;
        presentationUpdate.authorityGuid = outgoing.authorityGuid;
        presentationUpdate.authorityGeneration = outgoing.authorityGeneration;
        presentationUpdate.sequence = outgoing.snapshotSequence;
        presentationUpdate.serverTimestamp = outgoing.serverTimestamp;

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
                const bool firstSeen = prevIt == cell.actors.end();
                const bool importantHealthChange = !firstSeen && healthDelta > 0.5f
                    && (actor.mpNum != 0 || actor.ai.type == BaseActor::AIAction::Type::Combat
                        || actor.isAttackingOrCasting);
                Log(importantHealthChange ? Debug::Info : Debug::Verbose)
                    << "[MP] ActorSync: health update for " << actor.refId
                    << " mpNum=" << actor.mpNum
                    << " prev=" << prevHealth
                    << " cur=" << actor.dynamicStats.health.current
                    << " dead=" << actor.isDead
                    << " firstSeen=" << firstSeen;
                statsUpdate.actors.push_back(actor);
            }

            // Send a death packet when an actor is dead and we haven't sent one yet.
            // We use deathPacketSent instead of simple edge detection because the
            // MP update runs BEFORE the mechanics update — on the first frame the
            // actor is dead, playRandomDeath() hasn't run yet so mp_death_anim_group
            // is empty. We defer until the anim group is available.
            {
                const bool alreadySent = (prevIt != cell.actors.end())
                    && (prevIt->second.deathPacketSent
                        || (prevIt->second.state.isDead && prevIt->second.deathAlreadyApplied));
                const bool primaryAlreadyAppliedDead = [&]() {
                    if (!actor.isDead)
                        return false;
                    ActorRuntime* primary = findPrimaryActorRuntime(actor);
                    return primary != nullptr
                        && primary->state.isDead
                        && (primary->deathPacketSent || primary->deathAlreadyApplied)
                        && !primary->deathFromRealtimePacket;
                }();
                const bool firstSeenDead = actor.isDead && prevIt == cell.actors.end();
                if (actor.isDead && !firstSeenDead && !alreadySent && !primaryAlreadyAppliedDead)
                {
                    // Read the death anim group from the base node.
                    std::string deathGroup = actor.deathAnimGroup;
                    if (deathGroup.empty() && prevIt != cell.actors.end() && !prevIt->second.boundActor.isEmpty())
                    {
                        if (auto* baseNode = prevIt->second.boundActor.getRefData().getBaseNode())
                            baseNode->getUserValue("mp_death_anim_group", deathGroup);
                    }
                    if (deathGroup.empty())
                    {
                        // Remote CombatRequest damage can kill the authority's
                        // local actor without the CharacterController choosing
                        // a death group before our send window. Pick a stable
                        // group so non-authority clients still get a realtime
                        // ActorDeath packet and can play a death animation.
                        deathGroup = fallbackDeathAnimGroup(actor);
                        if (prevIt != cell.actors.end() && !prevIt->second.boundActor.isEmpty())
                        {
                            if (auto* baseNode = prevIt->second.boundActor.getRefData().getBaseNode())
                                baseNode->setUserValue("mp_death_anim_group", deathGroup);
                        }
                        Log(Debug::Info) << "[MP] ActorSync: synthesized death group for " << actor.refId
                                         << " mpNum=" << actor.mpNum
                                         << " anim='" << deathGroup << "'";
                    }

                    if (!deathGroup.empty())
                    {
                        uint32_t deathEventId = 1;
                        if (prevIt != cell.actors.end())
                        {
                            deathEventId = prevIt->second.nextDeathEventId++;
                            if (deathEventId == 0)
                                deathEventId = prevIt->second.nextDeathEventId++;
                        }

                        Log(Debug::Info) << "[MP] ActorSync: death send for " << actor.refId
                                         << " mpNum=" << actor.mpNum
                                         << " eventId=" << deathEventId
                                         << " hp=" << actor.dynamicStats.health.current
                                         << " anim='" << deathGroup << "'";
                        BaseActor deathActor = actor;
                        deathActor.deathEventId = deathEventId;
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
                // Use the already-resolved boundActor rather than a fresh world
                // lookup — getPtr(refId,false) silently fails for interior cells
                // and multi-instance NPCs; boundActor is always the right ptr.
                if (prevIt != cell.actors.end() && !prevIt->second.boundActor.isEmpty())
                {
                    if (auto* baseNode = prevIt->second.boundActor.getRefData().getBaseNode())
                    {
                        std::string atkType;
                        if (baseNode->getUserValue("mp_attack_type", atkType) && !atkType.empty())
                            atkActor.attack.attackAnimation = atkType;
                    }
                }

                attackUpdate.actors.push_back(atkActor);
                const ActorInstanceId actorNetId = actorNetIdForActorState(actor);
                if (actorNetId != 0)
                {
                    ActorRuntime& runtime = cell.actors[key];
                    runtime.actorNetId = actorNetId;
                    ActorAttackV2Event event;
                    event.actorNetId = actorNetId;
                    event.eventId = runtime.nextAttackEventId++;
                    if (event.eventId == 0)
                        event.eventId = runtime.nextAttackEventId++;
                    event.attack = atkActor.attack;
                    attackV2Update.events.push_back(event);
                    attackUpdate.actors.pop_back();
                }
            }

            // --- Cast start edge detection ---
            // Use boundActor directly instead of a fresh world lookup.
            // getPtr(refId,false) silently fails for interior cells and multi-
            // instance NPCs; boundActor is always the correctly-resolved ptr.
            if (prevIt != cell.actors.end() && !prevIt->second.boundActor.isEmpty())
            {
                if (auto* baseNode = prevIt->second.boundActor.getRefData().getBaseNode())
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

            const ActorInstanceId actorNetId = actorNetIdForActorState(actor);
            if (actorNetId != 0)
            {
                ActorPresentationSnapshot snapshot = makePresentationSnapshot(actor, actorNetId);
                const std::string previousSentAnimGroup = prevIt != cell.actors.end()
                    ? prevIt->second.lastSentAnimGroup
                    : std::string();
                const bool firstPresentation = prevIt == cell.actors.end()
                    || !prevIt->second.lastSentPresentationValid;
                const bool stopEdge = prevIt != cell.actors.end()
                    && prevIt->second.lastSentPresentationValid
                    && prevIt->second.lastSentIsMoving
                    && !snapshot.isMoving;
                const bool presentationChanged = firstPresentation
                    || prevIt->second.lastSentIsMoving != snapshot.isMoving
                    || prevIt->second.lastSentAttackingOrCasting != snapshot.isAttackingOrCasting
                    || prevIt->second.lastSentWeaponDrawn != snapshot.hasWeaponDrawn
                    || prevIt->second.lastSentSpellReadied != snapshot.hasSpellReadied
                    || prevIt->second.lastSentDead != snapshot.isDead
                    || prevIt->second.lastSentMovementFlags != snapshot.movementFlags
                    || prevIt->second.lastSentAnimFwd != snapshot.animFwd
                    || prevIt->second.lastSentAnimSide != snapshot.animSide
                    || prevIt->second.lastSentPresentationFlags != snapshot.presentationFlags
                    || prevIt->second.lastSentAnimGroup != snapshot.currentAnimGroup;
                const bool refreshDue = prevIt != cell.actors.end()
                    && prevIt->second.presentationSendTimer >= 1.f;

                if (presentationChanged || stopEdge || refreshDue)
                {
                    presentationUpdate.snapshots.push_back(snapshot);
                    ActorRuntime& runtime = cell.actors[key];
                    runtime.lastSentPresentationValid = true;
                    runtime.lastSentIsMoving = snapshot.isMoving;
                    runtime.lastSentAttackingOrCasting = snapshot.isAttackingOrCasting;
                    runtime.lastSentWeaponDrawn = snapshot.hasWeaponDrawn;
                    runtime.lastSentSpellReadied = snapshot.hasSpellReadied;
                    runtime.lastSentDead = snapshot.isDead;
                    runtime.lastSentMovementFlags = snapshot.movementFlags;
                    runtime.lastSentAnimFwd = snapshot.animFwd;
                    runtime.lastSentAnimSide = snapshot.animSide;
                    runtime.lastSentPresentationFlags = snapshot.presentationFlags;
                    runtime.lastSentAnimGroup = snapshot.currentAnimGroup;
                    runtime.presentationSendTimer = 0.f;

                    if (stopEdge)
                        ++mActorV2PresentationStopForcedWindow;
                    if (presentationChanged && !firstPresentation && previousSentAnimGroup != snapshot.currentAnimGroup)
                        ++mActorV2PresentationGroupChangedWindow;

                    if (isWatchedPresentationActor(actor, actorNetId))
                    {
                        Log(Debug::Info) << "[MP] ActorSync v2: presentation send"
                                         << " actorNetId=" << actorNetId
                                         << " refId=" << actor.refId
                                         << " mpNum=" << actor.mpNum
                                         << " moving=" << snapshot.isMoving
                                         << " fwd=" << dequantizeActorAxis(snapshot.animFwd)
                                         << " side=" << dequantizeActorAxis(snapshot.animSide)
                                         << " movementFlags=" << snapshot.movementFlags
                                         << " group='" << snapshot.currentAnimGroup << "'"
                                         << " flags=" << static_cast<unsigned>(snapshot.presentationFlags)
                                         << " reason="
                                         << (stopEdge ? "stop" : (presentationChanged ? "change" : "refresh"));
                    }
                }
            }
        }

        if (!presentationUpdate.snapshots.empty())
        {
            PacketActorPresentationV2 presentationPacket;
            presentationPacket.setPresentationList(&presentationUpdate);
            mClient.sendReliable(presentationPacket.encode());
            mActorV2PresentationSentWindow += presentationUpdate.snapshots.size();
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

        if (!attackV2Update.events.empty())
        {
            PacketActorAttackV2 attackPkt;
            attackPkt.setAttackList(&attackV2Update);
            mClient.sendReliable(attackPkt.encode());
            mActorV2AttackSentWindow += attackV2Update.events.size();
            Log(Debug::Info) << "[MP] ActorSync: sent ActorAttackV2 packet with " << attackV2Update.events.size() << " events";
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
                const bool firstSeenInCell = cell.actors.find(key) == cell.actors.end();
                auto& runtime = cell.actors[key];
                const std::string oldRuntimeCellId = runtime.state.cellId;
                runtime.state = actorState;
                runtime.actorNetId = actorNetIdForActorState(actorState);
                if (runtime.actorNetId != 0)
                    indexActorNetId(runtime.actorNetId, oldRuntimeCellId, actorState.cellId);
                runtime.lastAttackingOrCasting = actorState.isAttackingOrCasting;
                // Mark deathPacketSent if we sent a death packet for this actor
                if (deathSentKeys.count(key) || (firstSeenInCell && actorState.isDead))
                {
                    runtime.deathPacketSent = true;
                    runtime.deathAlreadyApplied = true;
                    runtime.deathFromRealtimePacket = false;
                }
                // Reset deathPacketSent if the actor is no longer dead (resurrected)
                if (!actorState.isDead)
                {
                    runtime.deathPacketSent = false;
                    runtime.deathAlreadyApplied = false;
                    runtime.deathFromRealtimePacket = false;
                }
            }
            for (auto it = cell.actors.begin(); it != cell.actors.end(); )
            {
                if (currentKeys.find(it->first) == currentKeys.end())
                {
                    if (it->second.state.mpNum != 0
                        && currentServerSpawnedMpNums.count(it->second.state.mpNum) == 0
                        && !it->second.boundActor.isEmpty())
                    {
                        const std::string boundCellId = cellIdForPtr(it->second.boundActor);
                        rememberServerSpawnedActor(boundCellId.empty() ? cell.outboundCellId : boundCellId,
                            it->second.boundActor, it->second.state.mpNum);
                        if (it->second.state.refId == "heddvild" || isGeneratedSpawnerRefId(it->second.state.refId))
                        {
                            Log(Debug::Info) << "[MP] ActorSync: preserved server-spawned mapping after authority scan omission"
                                             << " cell=" << cell.outboundCellId
                                             << " boundCell=" << boundCellId
                                             << " refId=" << it->second.state.refId
                                             << " mpNum=" << it->second.state.mpNum
                                             << " actorNetId=" << it->second.actorNetId;
                        }
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

        if (actor.actorNetId == 0)
            actor.actorNetId = actorNetIdForActorState(actor.state);

        const bool expectsServerSpawn = actor.state.mpNum != 0;
        const bool expectsV2VanillaCrossCellMove = actor.actorNetId != 0
            && actor.state.mpNum == 0
            && actor.state.refNum != 0;
        const bool canMoveBoundActorAcrossCells = expectsServerSpawn || expectsV2VanillaCrossCellMove;
        const bool logTrackedActor = actor.state.refId == "fargoth"
            || (actor.state.mpNum != 0 && isGeneratedSpawnerRefId(actor.state.refId))
            || isWatchedBorderActor(actor.state, cellId);

        MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
        std::string targetCellId = cellId;
        if (actor.hasAuthoritativeTransform && isExteriorActorCellId(cellId))
        {
            const std::string positionCellId = exteriorCellIdForPosition(actor.state.position);
            if (!positionCellId.empty() && positionCellId != cellId
                && findActiveCellById(worldImp, positionCellId) != nullptr)
            {
                constexpr float kExteriorCellMismatchHysteresis = 64.f;
                const float borderDistance = exteriorCellBorderDistance(actor.state.position);
                const std::string boundCellId = !actor.boundActor.isEmpty()
                    ? cellIdForPtr(actor.boundActor) : std::string();

                if (!boundCellId.empty()
                    && (boundCellId == cellId || boundCellId == positionCellId)
                    && borderDistance <= kExteriorCellMismatchHysteresis)
                {
                    targetCellId = boundCellId;
                }
                else if (borderDistance > kExteriorCellMismatchHysteresis
                    || boundCellId == positionCellId)
                {
                    targetCellId = positionCellId;
                }
            }
        }

        MWWorld::CellStore* targetCell = findActiveCellById(worldImp, targetCellId);
        if (!targetCell)
        {
            actor.boundActor = MWWorld::Ptr();
            return false;
        }

        auto applyDefaultAuthorityAi = [&]()
        {
            if (expectsServerSpawn && hasAuthority(targetCellId))
                applyDefaultServerSpawnedWander(targetCellId, actor.boundActor, actor.state);
        };

        auto moveBoundActorToTargetCell = [&]() -> bool
        {
            if (actor.boundActor.isEmpty())
                return false;

            const std::string currentBoundCellId = cellIdForPtr(actor.boundActor);
            if (!canMoveBoundActorAcrossCells)
                return false;

            if (expectsServerSpawn && actor.actorNetId != 0)
            {
                auto primaryIt = mActorsByNetId.find(actor.actorNetId);
                if (primaryIt != mActorsByNetId.end() && &primaryIt->second != &actor)
                {
                    const ActorRuntime& primary = primaryIt->second;
                    const bool primaryHasComparableTransform = primary.hasAuthoritativeTransform
                        && (actor.lastServerTimestamp == 0
                            || primary.lastServerTimestamp == 0
                            || primary.lastServerTimestamp >= actor.lastServerTimestamp);
                    if (primaryHasComparableTransform)
                    {
                        if (logTrackedActor)
                        {
                            Log(Debug::Verbose) << "[MP] ActorSync: skipped sidecar cross-cell bound actor move"
                                                << " targetCell=" << targetCellId
                                                << " boundCell=" << currentBoundCellId
                                                << " primaryCell=" << primary.state.cellId
                                                << " refId=" << actor.state.refId
                                                << " mpNum=" << actor.state.mpNum
                                                << " actorNetId=" << actor.actorNetId
                                                << " runtimeTs=" << actor.lastServerTimestamp
                                                << " primaryTs=" << primary.lastServerTimestamp;
                        }
                        return false;
                    }
                }
            }

            if (expectsServerSpawn && actor.lastServerTimestamp != 0)
            {
                const auto latestTimestampIt = mServerSpawnedActorLastTimestamps.find(actor.state.mpNum);
                if (latestTimestampIt != mServerSpawnedActorLastTimestamps.end()
                    && latestTimestampIt->second != 0
                    && actor.lastServerTimestamp < latestTimestampIt->second)
                {
                    Log(Debug::Verbose) << "[MP] ActorSync: skipped stale cross-cell bound actor move"
                                        << " targetCell=" << targetCellId
                                        << " boundCell=" << currentBoundCellId
                                        << " refId=" << actor.state.refId
                                        << " mpNum=" << actor.state.mpNum
                                        << " runtimeTs=" << actor.lastServerTimestamp
                                        << " latestTs=" << latestTimestampIt->second;
                    return false;
                }
            }

            const Position* movePos = &actor.state.position;
            if (actor.hasSmoothedPosition)
            {
                bool smoothedMatchesTarget = true;
                bool stateMatchesTarget = true;
                if (isExteriorActorCellId(targetCellId))
                {
                    smoothedMatchesTarget = exteriorCellIdForPosition(actor.smoothedPosition) == targetCellId;
                    stateMatchesTarget = exteriorCellIdForPosition(actor.state.position) == targetCellId;
                }

                if (smoothedMatchesTarget || !stateMatchesTarget)
                    movePos = &actor.smoothedPosition;
                else if (logTrackedActor)
                {
                    Log(Debug::Info) << "[MP] ActorSync: cross-cell move ignored stale smoothed position"
                                     << " targetCell=" << targetCellId
                                     << " smoothedCell=" << exteriorCellIdForPosition(actor.smoothedPosition)
                                     << " stateCell=" << exteriorCellIdForPosition(actor.state.position)
                                     << " refId=" << actor.state.refId
                                     << " mpNum=" << actor.state.mpNum
                                     << " runtimeTs=" << actor.lastServerTimestamp;
                }
            }

            if (!actor.hasAuthoritativeTransform)
            {
                Log(Debug::Verbose) << "[MP] ActorSync: skipped cross-cell move without authoritative transform"
                                    << " targetCell=" << targetCellId
                                    << " refId=" << actor.state.refId
                                    << " mpNum=" << actor.state.mpNum;
                return false;
            }
            if (expectsServerSpawn
                && isExteriorActorCellId(targetCellId)
                && isZeroIdentityPosition(*movePos)
                && !actor.state.position.isTeleporting)
            {
                Log(Debug::Warning) << "[MP] ActorSync: skipped zero-position cross-cell move"
                                    << " targetCell=" << targetCellId
                                    << " refId=" << actor.state.refId
                                    << " mpNum=" << actor.state.mpNum;
                return false;
            }
            actor.boundActor = world->moveObject(actor.boundActor, targetCell,
                osg::Vec3f(movePos->pos[0], movePos->pos[1], movePos->pos[2]));
            if (actor.boundActor.isEmpty())
                return false;

            actor.bindingLogged = false;
            if (expectsServerSpawn)
                rememberServerSpawnedActor(targetCellId, actor.boundActor, actor.state.mpNum);
            applyBootstrapDeathState(actor);
            applyDefaultAuthorityAi();
            Log(Debug::Info) << "[MP] ActorSync: moved bound actor between cells"
                             << " cell=" << targetCellId
                             << " packetCell=" << cellId
                             << " refId=" << actor.state.refId
                             << " refNum=" << actor.state.refNum
                             << " mpNum=" << actor.state.mpNum
                             << " actorNetId=" << actor.actorNetId
                             << " from=" << currentBoundCellId
                             << " runtimeTs=" << actor.lastServerTimestamp
                             << " presentationTs=" << actor.lastPresentationServerTimestamp
                             << " pos=(" << movePos->pos[0] << "," << movePos->pos[1] << "," << movePos->pos[2] << ")";
            return true;
        };

        if (!actor.boundActor.isEmpty() && matchesActor(actor.boundActor, actor.state))
        {
            const std::string boundCellId = cellIdForPtr(actor.boundActor);
            if (canMoveBoundActorAcrossCells && !boundCellId.empty() && boundCellId != targetCellId)
            {
                if (expectsServerSpawn)
                    rememberServerSpawnedActor(boundCellId, actor.boundActor, actor.state.mpNum);
                return moveBoundActorToTargetCell();
            }

            if ((!expectsServerSpawn || mappedMpNumForPtr(targetCellId, actor.boundActor) == actor.state.mpNum)
                && actor.boundActor.getCell() != nullptr)
            {
                if (boundCellId == targetCellId)
                {
                    if (logTrackedActor && !actor.bindingLogged)
                    {
                        Log(Debug::Info) << "[MP] ActorSync: resolveActorBinding reused bound actor"
                                         << " cell=" << targetCellId
                                         << " packetCell=" << cellId
                                         << " refId=" << actor.state.refId
                                         << " mpNum=" << actor.state.mpNum
                                         << " localRefNum=" << actor.boundActor.getCellRef().getRefNum().mIndex;
                        actor.bindingLogged = true;
                    }
                    applyBootstrapDeathState(actor);
                    applyDefaultAuthorityAi();
                    return true;
                }
            }
        }

        if (expectsServerSpawn)
        {
            auto mappedIt = mServerSpawnedActorsByMpNum.find(actor.state.mpNum);
            if (mappedIt != mServerSpawnedActorsByMpNum.end())
            {
                if (!mappedIt->second.isEmpty()
                    && matchesActor(mappedIt->second, actor.state))
                {
                    const std::string mappedCellId = cellIdForPtr(mappedIt->second);
                    if (mappedCellId == targetCellId)
                    {
                        actor.boundActor = mappedIt->second;
                        rememberServerSpawnedActor(targetCellId, actor.boundActor, actor.state.mpNum);
                        if (logTrackedActor && !actor.bindingLogged)
                        {
                            Log(Debug::Info) << "[MP] ActorSync: resolveActorBinding reused mpNum map"
                                             << " cell=" << targetCellId
                                             << " packetCell=" << cellId
                                             << " refId=" << actor.state.refId
                                             << " mpNum=" << actor.state.mpNum
                                             << " localRefNum=" << actor.boundActor.getCellRef().getRefNum().mIndex;
                            actor.bindingLogged = true;
                        }
                        applyBootstrapDeathState(actor);
                        applyDefaultAuthorityAi();
                        return true;
                    }

                    if (!mappedCellId.empty())
                    {
                        actor.boundActor = mappedIt->second;
                        rememberServerSpawnedActor(mappedCellId, actor.boundActor, actor.state.mpNum);
                        return moveBoundActorToTargetCell();
                    }
                }

                if (logTrackedActor)
                {
                    Log(Debug::Info) << "[MP] ActorSync: resolveActorBinding dropped stale mpNum map"
                                     << " cell=" << targetCellId
                                     << " packetCell=" << cellId
                                     << " refId=" << actor.state.refId
                                     << " mpNum=" << actor.state.mpNum;
                }
                mServerSpawnedActorsByMpNum.erase(mappedIt);
            }
        }

        MWWorld::Ptr found;
        targetCell->forEach([&](MWWorld::Ptr ptr) -> bool
        {
            if (!matchesActor(ptr, actor.state))
                return true;
            if (expectsServerSpawn && mappedMpNumForPtr(targetCellId, ptr) != actor.state.mpNum)
                return true;

            found = ptr;
            return false;
        });

        if (found.isEmpty() && expectsV2VanillaCrossCellMove)
        {
            for (const auto& [candidateCellId, candidateCell] : mCells)
            {
                if (candidateCellId == targetCellId)
                    continue;

                MWWorld::CellStore* activeCell = findActiveCellById(worldImp, candidateCellId);
                if (!activeCell)
                    continue;

                activeCell->forEach([&](MWWorld::Ptr ptr) -> bool
                {
                    if (!matchesActor(ptr, actor.state))
                        return true;

                    found = ptr;
                    return false;
                });

                if (!found.isEmpty())
                    break;
            }

            if (!found.isEmpty())
            {
                actor.boundActor = found;
                actor.bindingLogged = false;
                if (logTrackedActor)
                {
                    Log(Debug::Info) << "[MP] ActorSync: recovered vanilla actor from neighboring cell"
                                     << " targetCell=" << targetCellId
                                     << " boundCell=" << cellIdForPtr(found)
                                     << " packetCell=" << cellId
                                     << " refId=" << actor.state.refId
                                     << " refNum=" << actor.state.refNum
                                     << " actorNetId=" << actor.actorNetId;
                }
                return moveBoundActorToTargetCell();
            }
        }

        if (found.isEmpty() && !actor.boundActor.isEmpty()
            && (!expectsServerSpawn || mappedMpNumForPtr(targetCellId, actor.boundActor) == actor.state.mpNum))
        {
            const std::string boundCellId = cellIdForPtr(actor.boundActor);
            if (boundCellId.empty() || boundCellId == targetCellId)
            {
                if (!actor.hasAuthoritativeTransform)
                {
                    Log(Debug::Verbose) << "[MP] ActorSync: skipped same-cell bound actor move without authoritative transform"
                                        << " targetCell=" << targetCellId
                                        << " refId=" << actor.state.refId
                                        << " mpNum=" << actor.state.mpNum;
                    return false;
                }

                if (expectsServerSpawn
                    && isExteriorActorCellId(targetCellId)
                    && isZeroIdentityPosition(actor.state.position)
                    && !actor.state.position.isTeleporting)
                {
                    Log(Debug::Warning) << "[MP] ActorSync: skipped zero-position same-cell bound actor move"
                                        << " targetCell=" << targetCellId
                                        << " refId=" << actor.state.refId
                                        << " mpNum=" << actor.state.mpNum;
                    return false;
                }

                actor.boundActor = world->moveObject(actor.boundActor, targetCell,
                    osg::Vec3f(actor.state.position.pos[0], actor.state.position.pos[1], actor.state.position.pos[2]));
                found = actor.boundActor;
            }
            else
            {
                if (canMoveBoundActorAcrossCells)
                    return moveBoundActorToTargetCell();
                if (expectsServerSpawn)
                    rememberServerSpawnedActor(boundCellId, actor.boundActor, actor.state.mpNum);
                return false;
            }
        }

        if (found.isEmpty() && expectsServerSpawn)
        {
            try
            {
                const float spawnX = actor.state.position.pos[0];
                const float spawnY = actor.state.position.pos[1];
                const float spawnZ = actor.state.position.pos[2];
                const bool suspectPos = isZeroIdentityPosition(actor.state.position);
                if (!actor.hasAuthoritativeTransform)
                {
                    Log(Debug::Verbose) << "[MP] ActorSync: skipped spawned actor placement without authoritative transform"
                                        << " cell=" << targetCellId
                                        << " packetCell=" << cellId
                                        << " refId=" << actor.state.refId
                                        << " mpNum=" << actor.state.mpNum;
                    actor.boundActor = MWWorld::Ptr();
                    return false;
                }
                if (isExteriorActorCellId(targetCellId) && suspectPos && !actor.state.position.isTeleporting)
                {
                    Log(Debug::Warning) << "[MP] ActorSync: skipped zero-position spawned actor placement"
                                        << " cell=" << targetCellId
                                        << " packetCell=" << cellId
                                        << " refId=" << actor.state.refId
                                        << " mpNum=" << actor.state.mpNum;
                    actor.boundActor = MWWorld::Ptr();
                    return false;
                }
                Log(Debug::Info) << "[MP] ActorSync: resolveActorBinding spawning missing actor"
                                 << " cell=" << targetCellId
                                 << " packetCell=" << cellId
                                 << " refId=" << actor.state.refId
                                 << " mpNum=" << actor.state.mpNum
                                 << " pos=(" << spawnX << "," << spawnY << "," << spawnZ << ")"
                                 << (suspectPos ? " WARN:pos-is-zero" : "");
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

                rememberServerSpawnedActor(targetCellId, found, actor.state.mpNum);
                Log(Debug::Info) << "[MP] ActorSync: spawned actor refId=" << actor.state.refId
                                 << " mpNum=" << actor.state.mpNum
                                 << " cell=" << targetCellId
                                 << " packetCell=" << cellId;
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
        actor.bindingLogged = false;  // new binding - allow one fresh "reused" log next call
        if (expectsServerSpawn)
            rememberServerSpawnedActor(targetCellId, actor.boundActor, actor.state.mpNum);
        applyBootstrapDeathState(actor);
        applyDefaultAuthorityAi();
        if (logTrackedActor)
        {
            Log(Debug::Info) << "[MP] ActorSync: resolveActorBinding result"
                             << " cell=" << targetCellId
                             << " packetCell=" << cellId
                             << " refId=" << actor.state.refId
                             << " mpNum=" << actor.state.mpNum
                             << " success=" << !actor.boundActor.isEmpty()
                             << " localRefNum=" << (actor.boundActor.isEmpty() ? 0 : actor.boundActor.getCellRef().getRefNum().mIndex);
        }
        return !actor.boundActor.isEmpty();
    }

    void ActorSync::applyBoundActorState(ActorRuntime& actor)
    {
        if (actor.boundActor.isEmpty())
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return;
        const float frameDuration = MWBase::Environment::get().getFrameDuration();
        actor.animGroupHoldTimer = std::max(0.f, actor.animGroupHoldTimer - frameDuration);
        actor.attackDrawHoldTimer = std::max(0.f, actor.attackDrawHoldTimer - frameDuration);
        actor.attackLocomotionHoldTimer = std::max(0.f, actor.attackLocomotionHoldTimer - frameDuration);

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
        const bool suppressAttackLocomotion = actor.attackLocomotionHoldTimer > 0.f || actor.pendingAttack;
        const bool shouldDriveLocomotion = !actor.state.isDead
            && !suppressAttackLocomotion
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
        const bool forceAttackWeapon = actor.attackDrawHoldTimer > 0.f || actor.pendingAttack;
        MWMechanics::DrawState drawState = MWMechanics::DrawState::Nothing;
        if (actor.state.hasWeaponDrawn || forceAttackWeapon)
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
        if (!actor.state.isDead && !actor.state.equipment.empty()
            && actor.boundActor.getClass().hasInventoryStore(actor.boundActor))
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
            else if (stats.isDeathAnimationFinished())
                world->enableActorCollision(actor.boundActor, false);
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
            actor.attackDrawHoldTimer = 0.f;
            actor.attackLocomotionHoldTimer = 0.f;
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
            actor.attackDrawHoldTimer = 0.f;
            actor.attackLocomotionHoldTimer = 0.f;
        }

        const bool suppressLowerBodyGroupSync = hitFlags != 0 || actor.animGroupHoldTimer > 0.f || actor.pendingAttack || actor.pendingCast;

        if (!actor.state.isDead && !suppressLowerBodyGroupSync)
        {
            const std::string& newGrp = actor.state.animFlags.currentAnimGroup;
            const bool stoppedPresentation = !shouldDriveLocomotion;
            if (stoppedPresentation && !actor.lastAppliedAnimGroup.empty()
                && isLocomotionAnimGroup(actor.lastAppliedAnimGroup))
            {
                if (MWRender::Animation* animObj = world->getAnimation(actor.boundActor))
                    animObj->disable(actor.lastAppliedAnimGroup);
                if (isWatchedPresentationActor(actor.state, actor.actorNetId))
                {
                    Log(Debug::Info) << "[MP] ActorSync v2: cleared stale locomotion"
                                     << " actorNetId=" << actor.actorNetId
                                     << " refId=" << actor.state.refId
                                     << " mpNum=" << actor.state.mpNum
                                     << " oldGroup='" << actor.lastAppliedAnimGroup << "'"
                                     << " newGroup='" << newGrp << "'";
                }
                actor.lastAppliedAnimGroup.clear();
            }

            if (!newGrp.empty() && newGrp != actor.lastAppliedAnimGroup)
            {
                if (MWRender::Animation* animObj = world->getAnimation(actor.boundActor))
                {
                    const std::string& oldGrp = actor.lastAppliedAnimGroup;

                    const bool newIsLoco = isLocomotionAnimGroup(newGrp);
                    const bool oldIsLoco = !oldGrp.empty() && isLocomotionAnimGroup(oldGrp);
                    const bool newIsHitState = isHitStateAnimGroup(newGrp);
                    const bool oldIsHitState = !oldGrp.empty() && isHitStateAnimGroup(oldGrp);

                    if (!oldGrp.empty() && oldGrp != newGrp)
                    {
                        if (oldIsLoco || oldIsHitState || (!oldGrp.empty() && oldGrp != "idle" && !newIsLoco))
                            animObj->disable(oldGrp);
                    }

                    const bool isBaseIdle = (newGrp == "idle" || newGrp == "idleswim" || newGrp == "idlesneak");
                    if (stoppedPresentation && newIsLoco)
                    {
                        Log(Debug::Verbose) << "[MP] ActorSync: skipped stale stopped locomotion group '"
                                            << newGrp << "' actor=" << actor.state.refId;
                    }
                    else if (!isBaseIdle)
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

                        Log(Debug::Verbose) << "[MP] ActorSync: anim group -> '" << newGrp
                            << "' (was '" << oldGrp << "') prio=" << prio
                            << " actor=" << actor.state.refId;
                    }
                    else
                    {
                        Log(Debug::Verbose) << "[MP] ActorSync: anim group -> '" << newGrp
                            << "' (CC-managed) actor=" << actor.state.refId;
                    }
                    if (!(stoppedPresentation && newIsLoco))
                        actor.lastAppliedAnimGroup = newGrp;
                }
            }
        }

        if (actor.pendingBoltTimer >= 0.f)
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
                    actor.attackDrawHoldTimer = std::max(actor.attackDrawHoldTimer, 0.85f);
                    actor.attackLocomotionHoldTimer = std::max(actor.attackLocomotionHoldTimer, 0.35f);
                    if (!actor.lastWeaponVisible)
                    {
                        actor.lastWeaponVisible = true;
                        if (auto* npcAnim = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(actor.boundActor)))
                            npcAnim->equipmentChanged();
                    }

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
                        const std::string hitKey = attackType == "shoot" ? "release" : "hit";
                        std::string startKey = attackType + " max attack";
                        std::string stopKey = attackType == "shoot"
                            ? attackType + " follow stop"
                            : attackType + " large follow stop";
                        auto keyTime = [&](const std::string& key) {
                            return animation->getTextKeyTime(weaponGroup + ": " + key);
                        };
                        float startTime = keyTime(startKey);
                        float stopTime = keyTime(stopKey);
                        if (startTime < 0.f || stopTime <= startTime)
                        {
                            stopKey = attackType + ' ' + hitKey;
                            stopTime = keyTime(stopKey);
                        }
                        if (startTime < 0.f || stopTime <= startTime)
                        {
                            startKey = attackType + " start";
                            stopKey = attackType + " stop";
                        }

                        animation->disable(weaponGroup);
                        animation->play(weaponGroup,
                            MWRender::AnimPriority(static_cast<int>(MWMechanics::Priority_Weapon)),
                            MWRender::BlendMask_All,
                            true,
                            1.f,
                            startKey,
                            stopKey,
                            0.f,
                            0u);

                        Log(Debug::Info) << "[MP] ActorSync: playing attack anim group='" << weaponGroup
                                         << "' type='" << attackType
                                         << "' start='" << startKey
                                         << "' stop='" << stopKey
                                         << "' for " << actor.state.refId;
                    }

                    actor.animGroupHoldTimer = std::max(actor.animGroupHoldTimer, 0.65f);
                }

                stats.setAttackingOrSpell(false);
                actor.pendingAttack = false;
            }
        }

        if (actor.pendingCast)
        {
            const CastSpell& cs = actor.state.cast;
            if (!cs.release && !cs.castAnimation.empty())
            {
                // Suppress lower-body anim group sync so the spellcast animation
                // isn't overwritten by incoming position-packet idle/locomotion groups.
                actor.animGroupHoldTimer = std::max(actor.animGroupHoldTimer, 1.0f);
                actor.castStartReceived = true;
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
                // Also protect the release animation from being stomped by anim group sync.
                actor.animGroupHoldTimer = std::max(actor.animGroupHoldTimer, 0.8f);
                Log(Debug::Info) << "[MP] ActorSync: cast release playback actor=" << actor.state.refId
                                 << " spell='" << cs.spellId << "' castAnim='" << cs.castAnimation
                                 << "' target='" << cs.targetRefId << "'"
                                 << " castStartReceived=" << actor.castStartReceived;
                // Only replay the wind-up animation if we never received the cast-start
                // packet (e.g. the client was loading or the packet was dropped).
                // When the start packet was received the animation is already mid-play at
                // the release point, so restarting it would look wrong and push the bolt
                // timer forward by a full extra wind-up duration.
                if (!cs.castAnimation.empty() && !actor.castStartReceived)
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
                        // If the cast-start packet was received the animation is already
                        // at the release point — the auth sends this packet from the
                        // "release" text-key callback, which is the exact moment the bolt
                        // launches there.  Fire on the next frame (0.0f, enabled by the
                        // >= 0.f guard in the timer countdown).
                        // If the start packet was missed, allow the full wind-up animation
                        // to play before the bolt appears (~0.9s for Morrowind cast anims).
                        actor.pendingBoltTimer = actor.castStartReceived ? 0.0f : 0.9f;
                        actor.pendingBoltSpellId = cs.spellId;
                    }
                }
            }

            // Only clear castStartReceived when the sequence ends (release=true).
            // Clearing it unconditionally here would reset the flag in the same
            // pendingCast cycle it was just set, so the release handler would
            // always see castStartReceived=false and use the wrong bolt timer.
            if (cs.release)
                actor.castStartReceived = false;
            actor.pendingCast = false;
        }
    }
} // namespace mwmp
