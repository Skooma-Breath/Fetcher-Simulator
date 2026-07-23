#include "PlayerSync.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/esm3/journalentry.hpp>
#include <components/misc/rng.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerLoadedCells.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerInventory.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerJournal.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerDeath.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerResurrect.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimPlay.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../network/Client.hpp"
#include "../network/Protocol.hpp"
#include "../Main.hpp"

// OpenMW world/player access
#include "../../mwbase/environment.hpp"
#include "../../mwbase/journal.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwbase/inputmanager.hpp"
#include "../../mwbase/luamanager.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/windowmanager.hpp"
#include "../../mwworld/worldmodel.hpp"
#include "../../mwrender/camera.hpp"
#include "../../mwinput/actions.hpp"
#include "../../mwgui/mode.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/scene.hpp"
#include "../../mwworld/worldimp.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/inventorystore.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwmechanics/npcstats.hpp"
#include "../../mwmechanics/weapontype.hpp"
#include "../../mwworld/player.hpp"
#include "../../mwworld/livecellref.hpp"
#include "../../mwworld/manualref.hpp"
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm3/loadweap.hpp>
#include "../../mwmechanics/movement.hpp"
#include "../../mwrender/npcanimation.hpp"
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimFlags.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAttack.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCast.hpp>

#include "WorldObjectSync.hpp"
#include "InventoryIdentity.hpp"

namespace mwmp
{

namespace
{
    uint64_t steadyTimeUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    std::string journalEntryKey(const ESM::RefId& quest, const ESM::RefId& infoId)
    {
        return quest.serializeText() + '\x1f' + infoId.serializeText();
    }

    bool sameItemIdentity(const Item& left, const Item& right)
    {
        return (left.instanceId == 0 || right.instanceId == 0 || left.instanceId == right.instanceId)
            && left.refId == right.refId
            && left.charge == right.charge
            && std::abs(left.enchantmentCharge - right.enchantmentCharge) < 0.001f
            && left.soul == right.soul;
    }

    bool sameItem(const Item& left, const Item& right)
    {
        return left.count == right.count && sameItemIdentity(left, right);
    }

    bool sameStableItemIdentity(const Item& left, const Item& right)
    {
        if (left.instanceId != 0 && right.instanceId != 0)
            return left.instanceId == right.instanceId && left.refId == right.refId;
        return sameItemIdentity(left, right);
    }

    bool sameEquipment(const EquipmentItem& left, const EquipmentItem& right)
    {
        return left.slot == right.slot && sameItem(left.item, right.item);
    }

    bool sameEquipmentStrict(const EquipmentItem& left, const EquipmentItem& right)
    {
        return left.slot == right.slot
            && left.item.instanceId == right.item.instanceId
            && left.item.count == right.item.count
            && left.item.refId == right.item.refId
            && left.item.charge == right.item.charge
            && std::abs(left.item.enchantmentCharge - right.item.enchantmentCharge) < 0.001f
            && left.item.soul == right.item.soul;
    }

    bool sameEquipmentStateStrict(
        const std::array<EquipmentItem, BasePlayer::NUM_EQUIPMENT_SLOTS>& left,
        const std::array<EquipmentItem, BasePlayer::NUM_EQUIPMENT_SLOTS>& right)
    {
        for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            if (!sameEquipmentStrict(left[slot], right[slot]))
                return false;
        }
        return true;
    }

    std::size_t equippedItemCount(const std::array<EquipmentItem, BasePlayer::NUM_EQUIPMENT_SLOTS>& equipment)
    {
        std::size_t count = 0;
        for (const EquipmentItem& entry : equipment)
        {
            if (!entry.item.refId.empty() && entry.item.count > 0)
                ++count;
        }
        return count;
    }

    std::size_t inventoryStackCount(const std::vector<Item>& items)
    {
        std::size_t count = 0;
        for (const Item& item : items)
        {
            if (!item.refId.empty() && item.count > 0)
                ++count;
        }
        return count;
    }

    bool sameDynamicStat(const DynamicStat& left, const DynamicStat& right)
    {
        constexpr float eps = 0.01f;
        return std::abs(left.base - right.base) <= eps
            && std::abs(left.current - right.current) <= eps
            && std::abs(left.mod - right.mod) <= eps;
    }

    bool sameAttribute(const Attribute& left, const Attribute& right)
    {
        constexpr float eps = 0.01f;
        return left.base == right.base
            && std::abs(left.mod - right.mod) <= eps
            && std::abs(left.damage - right.damage) <= eps;
    }

    bool sameSkill(const Skill& left, const Skill& right)
    {
        constexpr float eps = 0.01f;
        return std::abs(left.base - right.base) <= eps
            && std::abs(left.mod - right.mod) <= eps
            && std::abs(left.damage - right.damage) <= eps
            && std::abs(left.progress - right.progress) <= eps
            && left.increases == right.increases;
    }

    MWMechanics::DynamicStat<float> toMechanicsDynamicStat(const DynamicStat& stat)
    {
        MWMechanics::DynamicStat<float> value;
        value.setBase(stat.base);
        value.setModifier(stat.mod);
        value.setCurrent(stat.current, true, true);
        return value;
    }

    std::string cellIdFromCell(const CellId& cell)
    {
        if (!cell.isExterior)
            return cell.cellName;

        return std::string("EXT:") + std::to_string(cell.gridX) + "," + std::to_string(cell.gridY);
    }

    std::string cellIdForStore(const MWWorld::CellStore* store)
    {
        if (store == nullptr || store->getCell() == nullptr)
            return {};

        const MWWorld::Cell* cell = store->getCell();
        if (cell->isExterior())
            return std::string("EXT:") + std::to_string(cell->getGridX()) + "," + std::to_string(cell->getGridY());

        return std::string(cell->getNameId());
    }

    bool hasRecordId(const MWWorld::ESMStore& store, const std::string& refId)
    {
        return !refId.empty() && store.find(ESM::RefId::stringRefId(refId)) != 0;
    }

    bool inventoryOrder(const Item& left, const Item& right)
    {
        if (left.refId != right.refId) return left.refId < right.refId;
        if (left.charge != right.charge) return left.charge < right.charge;
        if (std::abs(left.enchantmentCharge - right.enchantmentCharge) >= 0.001f)
            return left.enchantmentCharge < right.enchantmentCharge;
        if (left.soul != right.soul) return left.soul < right.soul;
        if (left.count != right.count) return left.count < right.count;
        return left.instanceId < right.instanceId;
    }

    int attackTypeFromWeapon(const MWWorld::Ptr& weapon)
    {
        if (weapon.isEmpty() || weapon.getType() != ESM::Weapon::sRecordId)
            return 0;

        const int weaponTypeId = weapon.get<ESM::Weapon>()->mBase->mData.mType;
        const ESM::WeaponType::Class weaponClass = MWMechanics::getWeaponType(weaponTypeId)->mWeaponClass;
        if (weaponClass == ESM::WeaponType::Thrown)
            return 3;
        if (weaponClass == ESM::WeaponType::Ranged)
            return 2;
        return 0;
    }

    MWWorld::Ptr getEquippedWeapon(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || !actor.getClass().hasInventoryStore(actor))
            return {};

        MWWorld::InventoryStore& inv = actor.getClass().getInventoryStore(actor);
        const MWWorld::ContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId)
            return {};

        return *weapon;
    }

    ESM::Position toEsmPosition(const Position& position)
    {
        ESM::Position esmPos;
        std::memcpy(esmPos.pos, position.pos, sizeof(position.pos));
        std::memcpy(esmPos.rot, position.rot, sizeof(position.rot));
        return esmPos;
    }

    MWWorld::CellStore* resolveTeleportCell(const BasePlayer& authoritative)
    {
        MWWorld::WorldModel* worldModel = MWBase::Environment::get().getWorldModel();
        if (!worldModel)
            return nullptr;

        if (authoritative.cell.isExterior)
        {
            const ESM::RefId worldspace = authoritative.cell.worldspace.empty()
                ? ESM::Cell::sDefaultWorldspaceId
                : ESM::RefId::deserializeText(authoritative.cell.worldspace);
            const auto cellIndex
                = ESM::positionToExteriorCellLocation(
                    authoritative.position.pos[0], authoritative.position.pos[1], worldspace);
            return &worldModel->getExterior(cellIndex);
        }

        if (authoritative.cell.cellName.empty())
            return nullptr;

        return worldModel->findCell(authoritative.cell.cellName);
    }
}

PlayerSync::PlayerSync(NetworkClient& client, Protocol& protocol)
    : mClient(client), mProtocol(protocol)
{
    for (int i = 0; i < BasePlayer::NUM_EQUIPMENT_SLOTS; ++i)
    {
        mLocal.equipment[i].slot = i;
        mLastEquip[i].slot = i;
        mAuthoritativeEquipment[i].slot = i;
    }

    mLocal.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
    mAuthoritativeInventory.action = BasePlayer::InventoryChanges::Action::Set;
}

// ---------------------------------------------------------------------------
void PlayerSync::setPlayer(const MWWorld::Ptr& /*player*/)
{
    // No-op: player readiness is detected each frame via getPlayerPtr()
}

// ---------------------------------------------------------------------------
void PlayerSync::forceFullSync(bool includeInventoryAndEquipment)
{
    if (mLocal.guid == 0) return; // guid not yet assigned; will be called again after handshake
    // Pull latest cell/stats before sending — player ptr may not be set yet
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world)
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!player.isEmpty())
        {
            applyPendingAuthoritativeState(player);

            // Snapshot position
            const auto& refData = player.getRefData();
            const auto& pos     = refData.getPosition();
            mLocal.position.pos[0] = pos.pos[0];
            mLocal.position.pos[1] = pos.pos[1];
            mLocal.position.pos[2] = pos.pos[2];
            mLocal.position.rot[0] = pos.rot[0];
            mLocal.position.rot[1] = pos.rot[1];
            mLocal.position.rot[2] = pos.rot[2];

            // Snapshot velocity (zero for initial full sync)
            mLocal.velocity.linear[0] = 0.f;
            mLocal.velocity.linear[1] = 0.f;
            mLocal.velocity.linear[2] = 0.f;

            // NEW: Record current position as 'last' to prevent massive velocity spike on next update
            mLocal.position.pos[0] = pos.pos[0];
            mLocal.position.pos[1] = pos.pos[1];
            mLocal.position.pos[2] = pos.pos[2];
            mLocal.position.rot[0] = pos.rot[0];
            mLocal.position.rot[1] = pos.rot[1];
            mLocal.position.rot[2] = pos.rot[2];

            // Snapshot appearance — read directly from the player's ESM::NPC record
            // so remote clients can construct a unique-looking NPC for this player.
            if (const auto* npcRef = player.get<ESM::NPC>())
            {
                const ESM::NPC* npcBase = npcRef->mBase;
                mLocal.race     = npcBase->mRace.serializeText();
                mLocal.headMesh = npcBase->mHead.serializeText();
                mLocal.hairMesh = npcBase->mHair.serializeText();
                mLocal.isMale   = npcBase->isMale();
            }

            // Snapshot cell
            if (const MWWorld::CellStore* cs = player.getCell())
            {
                const MWWorld::Cell* ci = cs->getCell();
                mLocal.cell.isExterior = ci->isExterior();
                mLocal.cell.cellName   = std::string(ci->getNameId());
                if (ci->isExterior())
                {
                    mLocal.cell.gridX = ci->getGridX();
                    mLocal.cell.gridY = ci->getGridY();
                }
            }

            captureEquipment(player);
            captureInventory(player);
            capturePersistentStats(player);
        }
    }

    sendBaseInfo();
    sendCellChange();
    sendLoadedActorCells(true);
    if (includeInventoryAndEquipment)
    {
        sendEquipment();
        sendInventory();
    }
    sendDynamicStats();
    mPositionTimer = POSITION_RATE; // force position send next tick
    snapshotPosition();
    snapshotCell();
    snapshotEquipment();
    snapshotInventory();
    snapshotDynamicStats();
    // PlayerAnimFlags is not part of the full-sync packet set.  Arm an
    // immediate baseline for the next update so newly joined observers do not
    // wait for the periodic refresh before receiving an explicit idle/move state.
    mAnimRefreshTimer = ANIM_REFRESH_RATE;
    Log(Debug::Info) << "[MP] PlayerSync: full sync sent (guid=" << mLocal.guid << ")";
}

void PlayerSync::flushPersistentStats()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    MWWorld::Ptr player = world ? world->getPlayerPtr() : MWWorld::Ptr{};
    if (!world || player.isEmpty() || !player.getClass().isNpc())
        return;

    const auto& liveStats = player.getClass().getCreatureStats(player);
    mLocal.dynamicStats.health.base = liveStats.getHealth().getBase();
    mLocal.dynamicStats.health.current = liveStats.getHealth().getCurrent();
    mLocal.dynamicStats.health.mod = liveStats.getHealth().getModifier();
    mLocal.dynamicStats.magicka.base = liveStats.getMagicka().getBase();
    mLocal.dynamicStats.magicka.current = liveStats.getMagicka().getCurrent();
    mLocal.dynamicStats.magicka.mod = liveStats.getMagicka().getModifier();
    mLocal.dynamicStats.fatigue.base = liveStats.getFatigue().getBase();
    mLocal.dynamicStats.fatigue.current = liveStats.getFatigue().getCurrent();
    mLocal.dynamicStats.fatigue.mod = liveStats.getFatigue().getModifier();
    capturePersistentStats(player);

    sendDynamicStats();
    snapshotDynamicStats();
}

// ---------------------------------------------------------------------------
void PlayerSync::applyServerPositionCorrection(const BasePlayer& auth)
{
    const MWBase::Environment& env = MWBase::Environment::get();
    MWBase::World* world = env.getWorld();
    MWWorld::Ptr player = world ? world->getPlayerPtr() : MWWorld::Ptr{};
    if (!world || player.isEmpty())
        return;

    auto& stats = player.getClass().getCreatureStats(player);
    stats.land(true);
    stats.setTeleported(true);
    world->getPlayer().setTeleported(true);

    const osg::Vec3f pos(auth.position.pos[0], auth.position.pos[1], auth.position.pos[2]);
    const osg::Vec3f rot(auth.position.rot[0], auth.position.rot[1], auth.position.rot[2]);
    player = world->moveObject(player, pos);
    world->rotateObject(player, rot);

    if (MWBase::LuaManager* luaManager = env.getLuaManager())
        luaManager->objectTeleported(player);

    MWMechanics::Movement& movement = player.getClass().getMovementSettings(player);
    movement.mPosition[0] = 0.f;
    movement.mPosition[1] = 0.f;
    movement.mPosition[2] = 0.f;
    movement.mRotation[0] = 0.f;
    movement.mRotation[1] = 0.f;
    movement.mRotation[2] = 0.f;
    movement.mIsStrafing = false;

    mLocal.position = auth.position;
    mLocal.position.isTeleporting = false;
    mLocal.velocity = auth.velocity;
    mSmoothedVz = 0.f;
    snapshotPosition();
    // The receiver clears locomotion when it hard-snaps this teleport.  Send a
    // fresh post-teleport baseline on our next update even if our input state did
    // not cross an animation delta threshold.
    mAnimRefreshTimer = ANIM_REFRESH_RATE;

    Log(Debug::Info) << "[MP] Applied server position correction -> ("
                     << auth.position.pos[0] << ", "
                     << auth.position.pos[1] << ", "
                     << auth.position.pos[2] << ")";
}

void PlayerSync::applyServerCellChange(const BasePlayer& auth)
{
    const MWBase::Environment& env = MWBase::Environment::get();
    MWBase::World* world = env.getWorld();
    MWWorld::Ptr player = world ? world->getPlayerPtr() : MWWorld::Ptr{};
    if (!world || player.isEmpty())
        return;

    MWWorld::CellStore* destCell = resolveTeleportCell(auth);
    if (!destCell)
    {
        Log(Debug::Warning) << "[MP] Ignoring server cell correction: destination cell not found";
        return;
    }

    auto& stats = player.getClass().getCreatureStats(player);
    stats.land(true);
    stats.setTeleported(true);
    world->getPlayer().setTeleported(true);

    const osg::Vec3f pos(auth.position.pos[0], auth.position.pos[1], auth.position.pos[2]);
    const osg::Vec3f rot(auth.position.rot[0], auth.position.rot[1], auth.position.rot[2]);
    const bool differentCell = player.getCell() != destCell;

    if (differentCell)
        world->changeToCell(destCell->getCell()->getId(), toEsmPosition(auth.position), false, true);

    player = world->getPlayerPtr();
    player = world->moveObject(player, pos);
    world->rotateObject(player, rot);

    if (MWBase::LuaManager* luaManager = env.getLuaManager())
        luaManager->objectTeleported(player);

    MWMechanics::Movement& movement = player.getClass().getMovementSettings(player);
    movement.mPosition[0] = 0.f;
    movement.mPosition[1] = 0.f;
    movement.mPosition[2] = 0.f;
    movement.mRotation[0] = 0.f;
    movement.mRotation[1] = 0.f;
    movement.mRotation[2] = 0.f;
    movement.mIsStrafing = false;

    mLocal.cell = auth.cell;
    mLocal.position = auth.position;
    mLocal.position.isTeleporting = false;
    mLocal.velocity = auth.velocity;
    mSmoothedVz = 0.f;
    snapshotCell();
    snapshotPosition();
    mAnimRefreshTimer = ANIM_REFRESH_RATE;

    Log(Debug::Info) << "[MP] Applied server cell correction";
}

void PlayerSync::queueAuthoritativeEquipment(const BasePlayer& authoritative)
{
    mAuthoritativeEquipment = authoritative.equipment;
    for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        mAuthoritativeEquipment[slot].slot = slot;

    Log(Debug::Verbose) << "[MP] PlayerSync: queued authoritative equipment"
                        << " guid=" << authoritative.guid
                        << " localGuid=" << mLocal.guid
                        << " equipped=" << equippedItemCount(mAuthoritativeEquipment)
                        << " state=" << static_cast<int>(MWBase::Environment::get().getStateManager()->getState());

    // During MP auto-enter the server can send saved equipment immediately after
    // CharacterData while OpenMW is still in State_NoGame. Do not apply to the
    // pre-game/template player; newGame(true) would recreate the player and wipe
    // the restored equipment. Keep it pending until the real world is running.
    if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
    {
        mPendingEquipmentRestore = true;
        return;
    }

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
    {
        mPendingEquipmentRestore = true;
        return;
    }

    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty())
    {
        mPendingEquipmentRestore = true;
        return;
    }

    // An accepted client equipment change may be echoed by an older server, and
    // login restore acknowledgements can also arrive more than once. Reapplying
    // an identical set calls unequipAll(), restarts equipment animations, and
    // sends another acknowledgement. Treat an exact live match as the completed
    // acknowledgement instead of feeding that loop.
    captureEquipment(player);
    if (sameEquipmentStateStrict(mLocal.equipment, mAuthoritativeEquipment))
    {
        mPendingEquipmentRestore = false;
        snapshotEquipment();
        Log(Debug::Verbose) << "[MP] PlayerSync: authoritative equipment already matches live state";
        return;
    }

    mPendingEquipmentRestore = true;
    applyPendingAuthoritativeState(player);
}

void PlayerSync::queueAuthoritativeInventory(const BasePlayer& authoritative)
{
    mAuthoritativeInventory = authoritative.inventoryChanges;
    mAuthoritativeInventory.action = BasePlayer::InventoryChanges::Action::Set;
    mPendingInventoryRestore = true;

    Log(Debug::Verbose) << "[MP] PlayerSync: queued authoritative inventory"
                        << " guid=" << authoritative.guid
                        << " localGuid=" << mLocal.guid
                        << " stacks=" << inventoryStackCount(mAuthoritativeInventory.items)
                        << " state=" << static_cast<int>(MWBase::Environment::get().getStateManager()->getState());

    // During MP auto-enter the server can send saved inventory immediately after
    // CharacterData while OpenMW is still in State_NoGame. Do not apply to the
    // pre-game/template player; newGame(true) would recreate the player and wipe
    // the restored inventory. Keep it pending until the real world is running.
    if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
        return;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    MWWorld::Ptr player = world->getPlayerPtr();
    if (!player.isEmpty())
        applyPendingAuthoritativeState(player);
}

void PlayerSync::queueAuthoritativeJournal(const BasePlayer& authoritative)
{
    const BasePlayer::JournalChanges& incoming = authoritative.journalChanges;
    if (incoming.action == BasePlayer::JournalChanges::Action::Set || !mPendingJournalRestore)
        mAuthoritativeJournal = incoming;
    else
    {
        mAuthoritativeJournal.items.insert(mAuthoritativeJournal.items.end(),
            incoming.items.begin(), incoming.items.end());
    }
    mPendingJournalRestore = true;

    Log(Debug::Verbose) << "[MP] PlayerSync: queued authoritative journal"
                        << " action=" << static_cast<int>(incoming.action)
                        << " items=" << incoming.items.size();
}

void PlayerSync::queueRestoredStats(const BasePlayer& restored)
{
    mPendingRestoredStats = restored;
    mPendingRestoredStats.hasSavedStats = true;
    mHasPendingRestoredStats = true;

    mLocal.hasSavedStats = true;
    mLocal.dynamicStats = restored.dynamicStats;
    mLocal.attributes = restored.attributes;
    mLocal.skills = restored.skills;
    mLocal.level = restored.level;
    mLocal.levelProgress = restored.levelProgress;

    const Attribute& strength = restored.attributes[0];
    const Skill& blunt = restored.skills[ESM::Skill::refIdToIndex(ESM::Skill::BluntWeapon)];
    if (strength.base > 100 || blunt.base > 100.f)
    {
        Log(Debug::Info) << "[MP] PlayerSync: queued restored persistent stats"
                         << " level=" << restored.level
                         << " strength=" << strength.base
                         << " blunt=" << blunt.base
                         << " hp=" << restored.dynamicStats.health.current
                         << "/" << restored.dynamicStats.health.base;
    }
}

// ---------------------------------------------------------------------------
void PlayerSync::update(float dt)
{
    if (!mClient.isConnected())
        return;

    // A connection can already be live while the character selection menu is
    // still using the unloaded player from the previous world.  Do not inspect
    // or mutate that stale player while waiting for newGame() to rebuild it.
    if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
        return;

    // Pull live state from OpenMW
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    applyPendingAuthoritativeState(player);

    if (mRecentPlayerAttackerTimer > 0.f)
    {
        mRecentPlayerAttackerTimer -= dt;
        if (mRecentPlayerAttackerTimer <= 0.f)
        {
            mRecentPlayerAttackerTimer = 0.f;
            mRecentPlayerAttackerGuid = 0;
        }
    }

    // Don't send anything until we have a valid server-assigned guid.
    // forceFullSync() is called by Main.cpp from the CharacterData handler
    // once mLocal.guid is properly set.
    if (mLocal.guid == 0)
        return;

    const float safeDt = std::max(0.f, dt);
    mPositionDiagTimer += safeDt;
    mPositionDiagFrameDtMax = std::max(mPositionDiagFrameDtMax, safeDt);
    ++mPositionDiagFrames;

    // --- position / rotation / velocity ---
    const auto& refData  = player.getRefData();
    const auto& pos      = refData.getPosition();

    if (dt > 0.0001f)
    {
        mLocal.velocity.linear[0] = (pos.pos[0] - mLocal.position.pos[0]) / dt;
        mLocal.velocity.linear[1] = (pos.pos[1] - mLocal.position.pos[1]) / dt;

        // Z velocity: smooth with EMA to tame stair-geometry spikes.
        const float rawVz = (pos.pos[2] - mLocal.position.pos[2]) / dt;
        const bool airborne = !world->isOnGround(player) && !world->isSwimming(player) && !world->isFlying(player);
        const float vzAlpha = airborne ? 0.75f : VZ_SMOOTH_ALPHA;
        mSmoothedVz = mSmoothedVz + vzAlpha * (rawVz - mSmoothedVz);
        mLocal.velocity.linear[2] = mSmoothedVz;
    }

    mLocal.position.pos[0] = pos.pos[0];
    mLocal.position.pos[1] = pos.pos[1];
    mLocal.position.pos[2] = pos.pos[2];
    mLocal.position.rot[0] = pos.rot[0];
    mLocal.position.rot[1] = pos.rot[1];
    mLocal.position.rot[2] = pos.rot[2];

    // Capture teleport flag from OpenMW engine (coc, scripts, doors)
    // We bitwise-OR it so if a teleport happens between 33ms network ticks, 
    // it's strictly captured for the next outgoing packet.
    if (MWWorld::Player* p = &world->getPlayer())
    {
        mLocal.position.isTeleporting |= p->wasTeleported();
        if (p->wasTeleported())
            p->setTeleported(false); // Consume engine flag
    }

    // --- dynamic stats ---
    const auto& cstats = player.getClass().getCreatureStats(player);
    if (cstats.isDead() && !mLastWasDead)
    {
        sendDeath();
        mRespawnPending = true;
        mRespawnTimer = RESPAWN_DELAY;
    }
    else if (!cstats.isDead() && mLastWasDead)
    {
        mRespawnPending = false;
        mRespawnTimer = 0.f;
    }

    if (cstats.isDead() && mRespawnPending)
    {
        mRespawnTimer -= dt;
        if (mRespawnTimer <= 0.f)
        {
            respawnLocally(player);
            mRespawnPending = false;
            mRespawnTimer = 0.f;
        }
    }

    const auto& liveStats = player.getClass().getCreatureStats(player);
    mLocal.dynamicStats.health.base    = liveStats.getHealth().getBase();
    mLocal.dynamicStats.health.current = liveStats.getHealth().getCurrent();
    mLocal.dynamicStats.health.mod     = liveStats.getHealth().getModifier();
    mLocal.dynamicStats.magicka.base   = liveStats.getMagicka().getBase();
    mLocal.dynamicStats.magicka.current= liveStats.getMagicka().getCurrent();
    mLocal.dynamicStats.magicka.mod    = liveStats.getMagicka().getModifier();
    mLocal.dynamicStats.fatigue.base   = liveStats.getFatigue().getBase();
    mLocal.dynamicStats.fatigue.current= liveStats.getFatigue().getCurrent();
    mLocal.dynamicStats.fatigue.mod    = liveStats.getFatigue().getModifier();
    if (!mHasPendingRestoredStats)
        capturePersistentStats(player);

    if (!liveStats.isDead() && mLastWasDead)
    {
        // Refresh dynamic stats before the respawn full sync so observers don't
        // see a resurrect immediately followed by a stale dead-state snapshot.
        sendResurrect();
        forceFullSync();
    }

    mLastWasDead = liveStats.isDead();

    captureEquipment(player);
    captureInventory(player);

    // --- cell ---
    if (const MWWorld::CellStore* cs = player.getCell())
    {
        const MWWorld::Cell* ci = cs->getCell();
        mLocal.cell.isExterior = ci->isExterior();
        mLocal.cell.cellName   = std::string(ci->getNameId());
        if (ci->isExterior())
        {
            mLocal.cell.gridX = ci->getGridX();
            mLocal.cell.gridY = ci->getGridY();
        }
    }

    tickPosition(dt);
    if (mPositionDiagTimer >= 1.f)
    {
        const float planarSpeed = std::sqrt(mLocal.velocity.linear[0] * mLocal.velocity.linear[0]
            + mLocal.velocity.linear[1] * mLocal.velocity.linear[1]);
        Log(Debug::Info) << "[MPDIAG] PlayerPosition sender"
                         << " frameHz=" << (mPositionDiagFrames / mPositionDiagTimer)
                         << " sendHz=" << (mPositionDiagSends / mPositionDiagTimer)
                         << " opportunities=" << mPositionDiagSendOpportunities
                         << " unchanged=" << mPositionDiagUnchanged
                          << " frameDtMaxMs=" << (mPositionDiagFrameDtMax * 1000.f)
                          << " timerRemainderMs=" << (mPositionTimer * 1000.f)
                          << " planarSpeed=" << planarSpeed
                          << " yaw=" << mLocal.position.rot[2];
        mPositionDiagTimer = std::fmod(mPositionDiagTimer, 1.f);
        mPositionDiagFrameDtMax = 0.f;
        mPositionDiagFrames = 0;
        mPositionDiagSendOpportunities = 0;
        mPositionDiagUnchanged = 0;
        mPositionDiagSends = 0;
    }
    tickDynamicStats(dt);
    tickJournal();
    sendAnimFlags(dt);
    sendAnimPlay();
    sendAttack();
    sendCast();

    // --- on-change checks ---
    if (cellChanged())
    {
        snapshotCell();
        sendCellChange();
        sendLoadedActorCells(true);
    }
    else
        sendLoadedActorCells();
    if (equipmentChanged())
    {
        snapshotEquipment();
        sendEquipment();
    }
    if (inventoryChanged())
    {
        snapshotInventory();
        sendInventory();
    }
}

// ---------------------------------------------------------------------------
// Position — unreliable, rate-limited
void PlayerSync::tickPosition(float dt)
{
    mPositionTimer += dt;
    if (mPositionTimer < POSITION_RATE)
        return;
    // Retain the fractional remainder. Resetting to zero quantises a nominal
    // 30 Hz sender to the render cadence (for example, 20 Hz at 50 FPS).
    mPositionTimer = std::fmod(mPositionTimer, POSITION_RATE);
    ++mPositionDiagSendOpportunities;

    if (!positionChanged())
    {
        ++mPositionDiagUnchanged;
        return;
    }

    // Velocity-Stop edge: if velocity just dropped to zero, send reliably to
    // ensure the "Stop" signal reaches the receiver immediately.
    const bool wasMovingVel = (mLastPos.velocity[0] != 0.f || mLastPos.velocity[1] != 0.f || mLastPos.velocity[2] != 0.f);
    const bool nowMovingVel = (mLocal.velocity.linear[0] != 0.f || mLocal.velocity.linear[1] != 0.f || mLocal.velocity.linear[2] != 0.f);
    const bool reliableStop = wasMovingVel && !nowMovingVel;

    snapshotPosition();
    sendPosition(reliableStop);
}

// ---------------------------------------------------------------------------
// Dynamic stats — reliable, rate-limited
void PlayerSync::tickDynamicStats(float dt)
{
    mStatsTimer += dt;
    if (mStatsTimer < STATS_RATE)
        return;
    mStatsTimer = 0.f;

    if (!dynamicStatsChanged())
        return;

    snapshotDynamicStats();
    sendDynamicStats();
}

void PlayerSync::captureJournalSnapshot()
{
    mLastJournalEntries.clear();
    mLastJournalIndices.clear();

    MWBase::Journal* journal = MWBase::Environment::get().getJournal();
    if (!journal)
        return;

    for (const auto& [questId, quest] : journal->getQuests())
    {
        mLastJournalIndices[questId.serializeText()] = quest.getIndex();
        for (auto it = quest.begin(); it != quest.end(); ++it)
            mLastJournalEntries.insert(journalEntryKey(questId, it->mInfoId));
    }
}

void PlayerSync::tickJournal()
{
    if (!mJournalAuthoritativeInitialized)
        return;

    const MWBase::Environment& environment = MWBase::Environment::get();
    MWBase::Journal* journal = environment.getJournal();
    if (!journal)
        return;

    std::unordered_set<std::string> currentEntries;
    std::unordered_map<std::string, int> currentIndices;
    std::vector<BasePlayer::JournalItem> changes;

    for (const auto& [questId, quest] : journal->getQuests())
    {
        const std::string questText = questId.serializeText();
        currentIndices[questText] = quest.getIndex();

        for (auto it = quest.begin(); it != quest.end(); ++it)
        {
            const std::string key = journalEntryKey(questId, it->mInfoId);
            currentEntries.insert(key);
            if (mLastJournalEntries.contains(key))
                continue;

            BasePlayer::JournalItem item;
            item.type = BasePlayer::JournalItem::Type::Entry;
            item.quest = questText;
            item.infoId = it->mInfoId.serializeText();
            item.text = it->mText;
            item.actorName = it->mActorName;
            item.index = quest.getIndex();

            if (const ESM::Dialogue* dialogue
                = environment.getESMStore()->get<ESM::Dialogue>().search(questId))
            {
                const auto info = std::find_if(dialogue->mInfo.begin(), dialogue->mInfo.end(),
                    [&](const ESM::DialInfo& value) { return value.mId == it->mInfoId; });
                if (info != dialogue->mInfo.end() && info->mData.mJournalIndex >= 0)
                    item.index = info->mData.mJournalIndex;
            }

            const auto stamped = std::find_if(journal->getEntries().begin(), journal->getEntries().end(),
                [&](const MWDialogue::StampedJournalEntry& value) {
                    return value.mTopic == questId && value.mInfoId == it->mInfoId;
                });
            if (stamped != journal->getEntries().end())
            {
                item.hasTimestamp = true;
                item.daysPassed = stamped->mDay;
                item.month = stamped->mMonth;
                item.dayOfMonth = stamped->mDayOfMonth;
            }

            changes.push_back(std::move(item));
        }
    }

    for (const auto& [quest, index] : currentIndices)
    {
        const auto previous = mLastJournalIndices.find(quest);
        if (previous != mLastJournalIndices.end() && previous->second == index)
            continue;

        BasePlayer::JournalItem item;
        item.type = BasePlayer::JournalItem::Type::Index;
        item.quest = quest;
        item.index = index;
        changes.push_back(std::move(item));
    }

    mLastJournalEntries = std::move(currentEntries);
    mLastJournalIndices = std::move(currentIndices);
    if (changes.empty())
        return;

    mLocal.journalChanges.items = std::move(changes);
    sendJournal();
}

// ---------------------------------------------------------------------------
void PlayerSync::sendPosition(bool reliable)
{
    mLocal.positionSampleTimeUs = steadyTimeUs();
    PacketPlayerPosition pkt;
    pkt.setPlayer(&mLocal);
    if (reliable)
        mClient.sendReliable(pkt.encode(mSeqCounter++));
    else
        mClient.sendUnreliable(pkt.encode(mSeqCounter++));
    ++mPositionDiagSends;

    // Reset teleport flag after encoding so it doesn't persist to the next tick.
    mLocal.position.isTeleporting = false;
}

void PlayerSync::sendCellChange()
{
    PacketPlayerCellChange pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
    Log(Debug::Verbose) << "[MP] PlayerSync: cell change sent -> "
                        << mLocal.cell.cellName;
}

std::vector<std::string> PlayerSync::collectLoadedActorCellIds() const
{
    std::vector<std::string> cells;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world)
    {
        MWWorld::World& worldImp = static_cast<MWWorld::World&>(*world);
        for (MWWorld::CellStore* store : worldImp.getWorldScene().getActiveCells())
        {
            const std::string cellId = cellIdForStore(store);
            if (!cellId.empty())
                cells.push_back(cellId);
        }
    }

    const std::string activeCellId = cellIdFromCell(mLocal.cell);
    if (!activeCellId.empty())
        cells.push_back(activeCellId);

    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

void PlayerSync::sendLoadedActorCells(bool force)
{
    const std::vector<std::string> loadedCellIds = collectLoadedActorCellIds();
    if (loadedCellIds.empty())
        return;

    if (!force && loadedCellIds == mLastLoadedActorCells)
        return;

    mLastLoadedActorCells = loadedCellIds;

    PacketPlayerLoadedCells pkt;
    pkt.sequence = ++mLoadedActorCellsSequence;
    pkt.activeCellId = cellIdFromCell(mLocal.cell);
    pkt.loadedCellIds = loadedCellIds;

    mClient.sendReliable(pkt.encode(mSeqCounter++));
    Log(Debug::Verbose) << "[MP] PlayerSync: loaded actor cells sent active="
                        << pkt.activeCellId << " count=" << pkt.loadedCellIds.size()
                        << " seq=" << pkt.sequence;
}

void PlayerSync::sendDynamicStats()
{
    PacketPlayerStatsDynamic pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
    const Attribute& strength = mLocal.attributes[0];
    const Skill& blunt = mLocal.skills[ESM::Skill::refIdToIndex(ESM::Skill::BluntWeapon)];
    if ((strength.base > 100 || blunt.base > 100.f)
        && (strength.base != mLastLoggedPersistentStrength
            || std::abs(blunt.base - mLastLoggedPersistentBlunt) > 0.01f))
    {
        Log(Debug::Info) << "[MP] PlayerSync: sent persistent stats"
                         << " strength=" << strength.base
                         << " blunt=" << blunt.base
                         << " hp=" << mLocal.dynamicStats.health.current << "/" << mLocal.dynamicStats.health.base;
        mLastLoggedPersistentStrength = strength.base;
        mLastLoggedPersistentBlunt = blunt.base;
    }
}

void PlayerSync::sendAnimPlay()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
        return;

    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty())
        return;

    auto* bn = player.getRefData().getBaseNode();
    if (!bn)
        return;

    bool pending = false;
    if (!bn->getUserValue("mp_anim_play_pending", pending) || !pending)
        return;

    bn->setUserValue("mp_anim_play_pending", false);

    std::string groupName;
    bn->getUserValue("mp_anim_play_group", groupName);
    if (groupName.empty())
        return;

    int priority = 0;
    int loops = 0;
    std::string startKey = "start";
    std::string stopKey = "stop";
    bn->getUserValue("mp_anim_play_priority", priority);
    bn->getUserValue("mp_anim_play_loops", loops);
    bn->getUserValue("mp_anim_play_start", startKey);
    bn->getUserValue("mp_anim_play_stop", stopKey);

    mLocal.animPlay.groupName = groupName;
    mLocal.animPlay.priority = priority;
    mLocal.animPlay.loops = loops;
    mLocal.animPlay.startKey = startKey;
    mLocal.animPlay.stopKey = stopKey;

    PacketPlayerAnimPlay pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendBaseInfo()
{
    PacketPlayerBaseInfo pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendEquipment()
{
    // The server accepts normal runtime equipment selection without echoing it
    // back to the sender. Keep the local authoritative fallback current so a
    // later server-driven inventory rebuild restores the player's latest choice
    // instead of an older login snapshot.
    mAuthoritativeEquipment = mLocal.equipment;
    PacketPlayerEquipment pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendInventory()
{
    mLocal.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;
    std::size_t missingInstanceIds = 0;
    const Item* firstMissingInstance = nullptr;
    for (const Item& item : mLocal.inventoryChanges.items)
    {
        if (item.refId.empty() || item.count <= 0 || item.instanceId != 0)
            continue;
        ++missingInstanceIds;
        if (firstMissingInstance == nullptr)
            firstMissingInstance = &item;
    }
    if (firstMissingInstance != nullptr)
    {
        static uint64_t sLastMissingIdentityLogUs = 0;
        const uint64_t nowUs = steadyTimeUs();
        if (nowUs - sLastMissingIdentityLogUs >= 1000000)
        {
            sLastMissingIdentityLogUs = nowUs;
            Log(Debug::Warning) << "[MPDIAG] PlayerInventory outgoing identity gap"
                                << " stacks=" << inventoryStackCount(mLocal.inventoryChanges.items)
                                << " missingInstanceIds=" << missingInstanceIds
                                << " firstRef=" << firstMissingInstance->refId
                                << " firstCount=" << firstMissingInstance->count
                                << " firstCharge=" << firstMissingInstance->charge
                                << " firstEnchant=" << firstMissingInstance->enchantmentCharge;
        }
    }

    PacketPlayerInventory pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendAnimFlags — per-frame, delta-suppressed; edge packets sent reliably.
//
// Normal flow: unreliable send whenever any flag or axis crosses the 0.05
// delta threshold.  Three special cases are ALWAYS sent reliably:
//
//   MF_JUMP rising  edge (0→1): ensures receiver enters JumpState_InAir
//     even if the first in-air unreliable packet is dropped.
//   MF_JUMP falling edge (1→0): the critical one for standstill landings.
//     After a standstill jump the player is perfectly still, so all
//     subsequent packets are identical — delta suppression fires nothing.
//     The receiver stays frozen in JumpState_InAir / ForceJump=true until
//     the 5-second force-refresh fires and the landing anim never plays.
//     Sending the falling edge reliably guarantees the CC makes the
//     JumpState_InAir → JumpState_Landing transition on the receiver.
//   Movement idle↔moving edge (any stance): a quick tap produces exactly
//     1 start and 1 stop packet; UDP loss of either means the remote CC
//     never transitions out of idle — no leg wiggle / walk anim.  Sending
//     both boundaries reliably fixes this for walk, run, and sneak alike.
void PlayerSync::sendAnimFlags(float dt)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    const MWMechanics::CreatureStats& stats
        = player.getClass().getCreatureStats(player);

    AnimFlags f{};

    // animFwd/animSide / MF_STRAFING: classify from world-space velocity projected
    // into body-local axes.
    //
    // WHY NOT mPosition[0/1]:
    //   In 3rd-person mode the engine Lua script (move360.lua) rotates the raw
    //   WASD input by (cameraYaw − bodyYaw) before writing it back into
    //   self.controls.sideMovement / controls.movement.  While the body is
    //   catching up (TurnToMovementDirection), pressing W alone produces a large
    //   sideMovement — we would encode strafe. Always use world velocity instead.
    //
    // WHY world velocity:
    //   mLocal.velocity.linear is computed from successive world-space positions
    //   and is completely camera-mode-agnostic.
    //
    // PROJECTION YAW — always body yaw:
    //   Camera yaw was tried but causes a flip when Tab-orbiting while walking:
    //   the camera rotates 180° while world-space velocity (W still held) stays
    //   forward → camera-relative projection gives fwd=-1 → remote NPC walks
    //   backward while the local player is still moving forward.
    //
    //   Body yaw is now safe for strafe: the isNetworkNpc guard in character.cpp
    //   prevents the CC from overwriting mIsStrafing each frame, so the receiver's
    //   hysteresis (enter=2x, exit=4x) holds through any body-camera lag.  The
    //   initial strafe entry fires cleanly (lag≈0 when A/D is first pressed).
    //
    // WASD GATE:
    //   Tab orbit with no movement keys induces body displacement (~18–35 u/s)
    //   above the velocity gate via move360.lua yawChange.  Zero axes when no
    //   movement key is held so camera orbit never triggers a walk/strafe anim.
    //   For sneak, a short latch extends the gate after key-up so quick taps
    //   (press+release between two sendAnimFlags calls) still encode velocity.
    //
    // VELOCITY GATE:
    //   15 u/s in 3rd-person kills yawChange micro-velocity from camera pans.
    //   5 u/s for 1st-person where the artifact is absent.
    // Detect jump / airborne state up front so movement encoding can preserve
    // mid-air momentum after the key is released. Without this, a running jump
    // that releases W mid-flight sends fwd/side=0 after the 100 ms tap latch
    // expires even while planar speed is still ~200 u/s, making the remote actor
    // freeze in place until the next input packet.
    const bool jumpRequested = world->getPlayer().getJumping();
    const bool physicallyAirborne = !world->isOnGround(player)
        && !world->isSwimming(player)
        && !world->isFlying(player);
    int ccJumpState = -1;
    int ccJumpVisual = 0;
    bool hasCcJumpState = false;
    bool hasCcJumpVisual = false;
    if (auto* baseNode = player.getRefData().getBaseNode())
    {
        hasCcJumpState = baseNode->getUserValue("mp_cc_jump_state", ccJumpState);
        hasCcJumpVisual = baseNode->getUserValue("mp_cc_jump_visual", ccJumpVisual);
    }
    const bool ccJumpInAir = hasCcJumpState && ccJumpState == 1;
    const bool ccJumpVisualActive = hasCcJumpVisual && ccJumpVisual != 0;
    
    // The jump visual latch exists purely to bridge the 1-2 frame "Takeoff gap" where
    // a local player initiates a jump but the physics engine hasn't actually lifted
    // them off the ground yet (delaying ccJumpInAir). 
    // We only charge the latch if ccJumpInAir is false (i.e. we are grounded).
    // If we charge it while already Airborne, the latch stays maxed out during the entire 
    // fall arc, swallowing the true falling edge if a bunny hop lands on the next frame!
    if (ccJumpVisualActive && !ccJumpInAir)
        mCcJumpVisualLatch = CC_JUMP_VISUAL_LATCH_TIME;
    else if (mCcJumpVisualLatch > 0.f)
        mCcJumpVisualLatch = std::max(0.f, mCcJumpVisualLatch - dt);
    const bool ccJumpVisualLatched = mCcJumpVisualLatch > 0.f;
    if (jumpRequested)
        mJumpGraceTimer = JUMP_GRACE_TIME;
    else if (mJumpGraceTimer > 0.f)
        mJumpGraceTimer = std::max(0.f, mJumpGraceTimer - dt);

    float fwd = 0.f, side = 0.f;
    bool wallBlocked = false;
    bool rawHasMovementInput = false;
    float planarSpeed = 0.f;
    float velocityGate = 5.f;
    float blockedMoveSpeed = 0.f;
    {
        // WASD gate — discard projection when no movement key is held.
        // Purpose: prevent camera-orbit (move360.lua yawChange ~18–35 u/s) from
        // triggering walk/strafe anims when the player is only rotating the view.
        bool kFwd = false, kBack = false, kLeft = false, kRight = false;
        bool kJump = false, kSneak = false;
        if (auto inputMgr = MWBase::Environment::get().getInputManager())
        {
            kFwd  = inputMgr->actionIsActive(MWInput::A_MoveForward);
            kBack = inputMgr->actionIsActive(MWInput::A_MoveBackward);
            kLeft = inputMgr->actionIsActive(MWInput::A_MoveLeft);
            kRight = inputMgr->actionIsActive(MWInput::A_MoveRight);
            kJump = inputMgr->actionIsActive(MWInput::A_Jump);
            kSneak = inputMgr->actionIsActive(MWInput::A_Sneak);
            rawHasMovementInput = kFwd || kBack || kLeft || kRight;
        }

        const bool rawHasVerticalInput = kJump || kSneak;

        // Movement tap latch: keep the WASD gate open for a short window after the
        // last real key press (any stance — walk, run, or sneak).
        //
        // Problem: a quick directional tap (press+release between two sendAnimFlags
        // calls, ~16 ms at 60 fps) leaves rawHasMovementInput==false by the time we
        // run.  The WASD gate fires and zeroes fwd/side even though the body still has
        // full momentum.  This applies to all stances, not only sneak — slow walking
        // "inching" and quick non-sneak taps suffer the same blackout.
        //
        // Fix: on any real key press, charge the latch for MOVE_LATCH_TIME.  While the
        // latch is live, keep the projection active so velocity naturally decays to zero
        // rather than being gate-zeroed.  The camera-orbit false-trigger is harmless
        // because the latch only opens after a genuine key press.
        if (rawHasMovementInput)
            mMoveLatch = MOVE_LATCH_TIME;
        else if (mMoveLatch > 0.f)
            mMoveLatch -= dt;

        const float vx  = mLocal.velocity.linear[0];
        const float vy  = mLocal.velocity.linear[1];
        const float spd = std::sqrt(vx * vx + vy * vy);
        planarSpeed = spd;

        const bool is3rdPerson = world->getCamera() && !world->isFirstPerson();
        // velocityGate: still used to filter camera-orbit body displacement when the
        // movement latch is live but no real key is held (Tab-orbit produces ~18-35 u/s).
        velocityGate = is3rdPerson ? 15.0f : 5.0f;

        // Preserve airborne momentum even after key release.
        // Locally, the jump continues to travel forward under inertia; encoding
        // idle mid-flight makes the remote switch to pure fall and then snap to
        // the next non-idle position. Camera-orbit false positives are not a
        // concern here because this path is only active while truly airborne.
        const bool preserveAirMomentum = (hasCcJumpState ? ccJumpInAir : (jumpRequested || physicallyAirborne))
            && spd > velocityGate;
        const bool hasMovementInput = rawHasMovementInput || (mMoveLatch > 0.f) || preserveAirMomentum;

        // Wall-block detection: compare physics speed against the actor's real
        // expected pace for the current movement state, not hardcoded 50/120 values.
        //
        // This fixes the crouch regression from the previous hardcoded threshold:
        // a slow sneaking player might legitimately top out around 25 u/s, so
        // comparing against a fixed 40 u/s tagged ordinary sneak-walk as "blocked"
        // every frame. Using getMaxSpeed(player) keeps the threshold aligned with
        // the sender's actual stats, stance, encumbrance, and settings.
        const float expectedMoveSpeed = player.getClass().getMaxSpeed(player);
        blockedMoveSpeed = expectedMoveSpeed;
        // CharacterController publishes the exact base speed used to advance
        // the local locomotion animation. It includes acceleration, strafing,
        // stance, encumbrance, and low-Speed behavior that cannot be recovered
        // reliably from world displacement on the observer.
        if (auto* baseNode = player.getRefData().getBaseNode())
        {
            float localAnimationSpeed = 0.f;
            if (baseNode->getUserValue("mp_anim_base_speed", localAnimationSpeed)
                && localAnimationSpeed > 0.f)
                blockedMoveSpeed = localAnimationSpeed;
        }
        const float wallSpeedThreshold = std::max(velocityGate, expectedMoveSpeed * 0.8f);
        wallBlocked = rawHasMovementInput
            && expectedMoveSpeed > velocityGate
            && spd < wallSpeedThreshold;

        if (hasMovementInput && spd > velocityGate && !wallBlocked)
        {
            // Always project onto body yaw — immune to camera-orbit direction flips.
            // Divide by spd (not walkSpeed) so the output is a unit-direction vector
            // in body-local space. wallBlocked already catches sub-walkspeed cases
            // (wall contact, accel ramp) so any speed reaching here is >= 80% of walk.
            const float projYaw = mLocal.position.rot[2];
            const float sinYaw = std::sin(projYaw);
            const float cosYaw = std::cos(projYaw);

            float rawSide = (vx * cosYaw - vy * sinYaw);
            float rawFwd  = (vx * sinYaw + vy * cosYaw);

            side = rawSide / spd;  // unit-direction component
            fwd  = rawFwd  / spd;
        }
        else if (rawHasMovementInput)
        {
            // Wall-blocked (or below velocity gate with keys held).
            // DO NOT use mov.mPosition[0/1] here — in 3rd-person move360 mode,
            // mPosition is computed from world-space velocity / camera direction by
            // Lua, so it collapses toward zero whenever the physics collision kills
            // the velocity (e.g. pressing into a wall). The remote NPC would see
            // fwd=0/side=0 and play idle animation despite keys being held.
            //
            // Instead, derive axes directly from which keys are physically pressed.
            // This is completely immune to physics, Lua, and camera-mode artifacts:
            // the sender's intent (key held = moving) is always transmitted correctly.
            fwd = kFwd ? 1.f : (kBack ? -1.f : 0.f);
            side = kRight ? 1.f : (kLeft ? -1.f : 0.f);
        }
        else // No real input, mMoveLatch dead
        {
            fwd = 0.f;  // Snap to idle
            side = 0.f;

            // Zero-Velocity Snap (Ground/XY): forcefully zero transmitted XY velocity
            // to prevent physics "drift tails" from being broadcast after a stop.
            mLocal.velocity.linear[0] = 0.f;
            mLocal.velocity.linear[1] = 0.f;
        }

        // Zero-Velocity Snap (Vertical/Z): if levitating/swimming and no vertical input
        // is active, force Z velocity to zero and reset the EMA to prevent drift tails.
        if (!physicallyAirborne && !rawHasVerticalInput)
        {
            // Reset Z-axis smoothing immediately on levitation stop.
            mSmoothedVz = 0.f;
            mLocal.velocity.linear[2] = 0.f;
        }
        else if (!rawHasVerticalInput && (world->isFlying(player) || world->isSwimming(player)))
        {
            // Force zero vertical drift even if the physics engine has a decay tail.
            mSmoothedVz = 0.f;
            mLocal.velocity.linear[2] = 0.f;
        }
    }

    // Send raw body-relative axes — no classification on the sender.
    // The receiver sets mPosition[0/1] directly so the remote CC runs its own
    // strafe/forward/backward logic, identical to a local 1st-person player.
    // Values are already normalised to [-1,1] from the velocity projection above.
    const float verticalSpeed = std::abs(mLocal.velocity.linear[2]);
    const bool stalledCornerJump = physicallyAirborne
        && rawHasMovementInput
        && wallBlocked
        && planarSpeed < velocityGate
        && verticalSpeed < 10.f;
    if (stalledCornerJump)
        mJumpStallTimer += dt;
    else
        mJumpStallTimer = 0.f;

    const bool suppressJumpForCornerStall = (mJumpGraceTimer <= 0.f)
        && (mJumpStallTimer >= JUMP_STALL_TIME);
    const bool cornerPinnedJump = rawHasMovementInput
        && wallBlocked
        && planarSpeed < velocityGate
        && verticalSpeed < 25.f;
    const bool jumpingFlag = (hasCcJumpState || hasCcJumpVisual)
        ? (ccJumpInAir || ccJumpVisualLatched)
        : ((jumpRequested || physicallyAirborne)
            && !cornerPinnedJump
            && !suppressJumpForCornerStall);

    f.animFwd  = fwd;
    f.animSide = side;
    const bool hasLocomotionAxes = std::abs(fwd) >= 0.1f || std::abs(side) >= 0.1f;
    f.blockedMoveSpeed = (hasLocomotionAxes || wallBlocked) ? blockedMoveSpeed : 0.f;
    // Jump launch velocity: only set on the rising edge so the receiver seeds its
    // parabolic arc from the real vz at the moment the jump impulse fires.
    // For all other frames this stays 0 to keep packet semantics clean.
    const bool wasJumpingPrev = (mLastAnimFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
    f.jumpVz = (!wasJumpingPrev && jumpingFlag) ? mLocal.velocity.linear[2] : 0.f;

    // movementFlags bitmask
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Run))
        f.movementFlags |= AnimFlags::MF_RUN;
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Sneak))
        f.movementFlags |= AnimFlags::MF_SNEAK;
    if (wallBlocked)
        f.movementFlags |= AnimFlags::MF_WALL_BLOCKED;

    {
        const float spd = std::sqrt(mLocal.velocity.linear[0]*mLocal.velocity.linear[0] + mLocal.velocity.linear[1]*mLocal.velocity.linear[1]);
        if (spd > 0.1f)
        {
            Log(Debug::Verbose) << "[MPDBG] Sender Flags: fwd=" << f.animFwd << " side=" << f.animSide
                                << " spd=" << spd << " sneak=" << ((f.movementFlags & AnimFlags::MF_SNEAK) != 0)
                                << " wallBlocked=" << ((f.movementFlags & AnimFlags::MF_WALL_BLOCKED) != 0)
                                << " blockedSpeed=" << f.blockedMoveSpeed
                                << " ccJump=" << ccJumpState
                                << " ccJumpVisual=" << ccJumpVisual
                                << " ccJumpLatch=" << mCcJumpVisualLatch
                                << " vz=" << mLocal.velocity.linear[2]
                                << " mode=" << (int)world->getCamera()->getMode();
        }
    }
    // Use getPlayer().getJumping() — set by PhysicsSystem::handleJump() the same
    // frame the jump impulse fires, before the actor physically leaves the ground.
    // This matches when the local CharacterController starts the jump animation,
    // removing the ~50ms delay that came from waiting for !isOnGround().
    // Fallback: also catch free-falls (walking off a ledge) via !isOnGround(),
    // but exclude swimming and flying which are non-jump airborne states.
    if (jumpingFlag)
        f.movementFlags |= AnimFlags::MF_JUMP;

    // Levitation / flying — keep the remote NPC off the ground while airborne.
    // Without this, the receiver's physics resets onGround=true every frame and
    // the NPC gets shoved into the ground geometry, triggering the engine's
    // "actor stuck" knockout path.
    if (world->isFlying(player))
        f.movementFlags |= AnimFlags::MF_FLY;

    // MF_STRAFING is derived on the receiver from animFwd/animSide directly,
    // mirroring the vanilla CC criterion — no need to encode it from the sender.

    // Mirror a single authoritative hit-state phase per frame.
    //
    // Logs from April 4, 2026 showed mixed combinations like:
    //   0 -> 384  (KNOCKED_OUT | RECOVERY)
    //   384 -> 320 (KNOCKED_DOWN | RECOVERY)
    // during fatigue-punch desync cases. Those overlapping internal CreatureStats
    // flags are valid locally, but they over-describe transient state for remote
    // playback and can synthesize a fall/stand sequence the owning client never
    // visibly enters.
    //
    // Networking only needs the dominant phase:
    //   KO        beats everything
    //   RECOVERY  beats plain KD while standing up
    //   KD        is only sent when neither KO nor recovery is active
    const bool statsKnockedDown = stats.getKnockedDown();
    const bool statsKnockedOut = statsKnockedDown
        && (stats.getFatigue().getCurrent() < 0.f || stats.getFatigue().getBase() == 0.f);
    const bool statsRecovery = stats.getHitRecovery();

    if (statsKnockedOut)
        f.movementFlags |= AnimFlags::MF_KNOCKED_OUT;
    else if (statsRecovery)
        f.movementFlags |= AnimFlags::MF_RECOVERY;
    else if (statsKnockedDown)
        f.movementFlags |= AnimFlags::MF_KNOCKED_DOWN;

    // actionFlags bitmask
    if (stats.getAttackingOrSpell())
        f.actionFlags |= AnimFlags::AF_ATTACKING;
    if (stats.getDrawState() == MWMechanics::DrawState::Weapon)
        f.actionFlags |= AnimFlags::AF_WEAPON_DRAWN;
    if (stats.getDrawState() == MWMechanics::DrawState::Spell)
        f.actionFlags |= AnimFlags::AF_SPELL_READY;

    // ── Jump edge detection ────────────────────────────────────────────────
    // Must run BEFORE the delta-suppression early-return so edges are never
    // silently dropped, even when the only thing that changed is the jump bit.
    const bool wasJumping = (mLastAnimFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
    const bool nowJumping = (f.movementFlags             & AnimFlags::MF_JUMP) != 0;
    const bool jumpEdge   = (wasJumping != nowJumping);
    if (jumpEdge)
        Log(Debug::Verbose) << "[MP] AnimFlags MF_JUMP "
                            << (nowJumping ? "0->1 (rising)" : "1->0 (falling)")
                            << " -- sending reliably";

    // ── Movement edge detection ────────────────────────────────────────────────
    // Any idle↔moving boundary packet is promoted to reliable, regardless of
    // stance.  The same UDP-loss race that was fixed for sneak also affects
    // upright walking and running: a quick tap produces exactly 1 start and 1
    // stop packet; if either is dropped the remote CC never sees the motion.
    //
    // Removing the `isSneak &&` guard makes the fix universal.  In-between
    // packets during sustained movement remain unreliable — no bandwidth
    // regression.  "Moving" is defined as either axis being non-zero after
    // the velocity gate and WASD gate above; exactly 0.f means truly idle.
    const bool wasMoving = (mLastAnimFlags.animFwd != 0.f || mLastAnimFlags.animSide != 0.f);
    const bool nowMoving = (f.animFwd != 0.f || f.animSide != 0.f);
    const bool moveEdge  = (wasMoving != nowMoving);
    if (moveEdge)
        Log(Debug::Verbose) << "[MP] AnimFlags move edge "
                            << (nowMoving ? "idle->moving" : "moving->idle")
                            << " sneak=" << ((f.movementFlags & AnimFlags::MF_SNEAK) != 0)
                            << " fwd=" << f.animFwd << " side=" << f.animSide
                            << " -- sending reliably";

    const uint32_t hitStateMask = AnimFlags::MF_KNOCKED_DOWN | AnimFlags::MF_KNOCKED_OUT | AnimFlags::MF_RECOVERY;
    const bool hitStateEdge = ((mLastAnimFlags.movementFlags ^ f.movementFlags) & hitStateMask) != 0;
    if (hitStateEdge)
        Log(Debug::Info) << "[MP] AnimFlags hit-state edge old=" << (mLastAnimFlags.movementFlags & hitStateMask)
                         << " new=" << (f.movementFlags & hitStateMask)
                         << " -- sending reliably";

    // Periodic refresh: force send every ANIM_REFRESH_RATE seconds even with
    // no delta, so a UDP-lost packet can't permanently strand the receiver.
    mAnimRefreshTimer += dt;
    const bool forceRefresh = (mAnimRefreshTimer >= ANIM_REFRESH_RATE);
    if (forceRefresh)
        mAnimRefreshTimer = 0.f;

    // Float comparison for delta-suppression: treat change < 0.05 as no-change
    // to avoid sending for tiny float noise while still catching real moves.
    // Jump edges and movement edges bypass this guard — they must always be delivered.
    if (!forceRefresh && !jumpEdge && !moveEdge && !hitStateEdge
        && f.movementFlags == mLastAnimFlags.movementFlags
        && f.actionFlags   == mLastAnimFlags.actionFlags
        && std::abs(f.animFwd  - mLastAnimFlags.animFwd)  < 0.05f
        && std::abs(f.animSide - mLastAnimFlags.animSide) < 0.05f
        && std::abs(f.blockedMoveSpeed - mLastAnimFlags.blockedMoveSpeed) < 1.f)
        return;

    mLastAnimFlags   = f;
    mLocal.animFlags = f;

    PacketPlayerAnimFlags pkt;
    pkt.setPlayer(&mLocal);
    // Jump-state transitions and movement edges (idle↔moving) are reliable;
    // all other anim changes are unreliable.
    if (jumpEdge || moveEdge || hitStateEdge)
        mClient.sendReliable(pkt.encode(mSeqCounter++));
    else
        mClient.sendUnreliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendAttack — reliable, edge-triggered on pressed→true transition.
void PlayerSync::sendAttack()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
        return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty())
        return;

    const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
    const bool canAttack
        = !stats.isDead() && !stats.isParalyzed() && !stats.getKnockedDown() && !stats.getHitRecovery();
    const bool pressed = canAttack && stats.getAttackingOrSpell();

    // Only send on edge transitions (press and release both matter: press triggers
    // the wind-up animation; release tells the receiver to end it).
    if (pressed == mLastAttackPressed)
        return;

    // The MP tick runs before mechanics. On the first frame where a ranged input
    // is released, CharacterController has not calculated the launch strength yet.
    // Defer this edge once; the controller normally calls notifyLocalAttackRelease()
    // later in the same frame with the exact value. If that hook never runs (for
    // example because the attack was cancelled before an animation started), the
    // next frame falls through and sends the legacy zero-strength release.
    const bool physicalProjectileRelease = !pressed && mLastAttackPressed && canAttack
        && (mLocal.attack.type == 2 || mLocal.attack.type == 3);
    if (physicalProjectileRelease && !mAwaitingRangedRelease)
    {
        mAwaitingRangedRelease = true;
        return;
    }

    mAwaitingRangedRelease = false;
    mLastAttackPressed = pressed;

    mLocal.attack.hit = false;
    mLocal.attack.block = false;
    mLocal.attack.miss = false;
    mLocal.attack.knocked = false;
    mLocal.attack.healthDamage = false;
    mLocal.attack.strength = 0.f;
    mLocal.attack.damage = 0.f;
    mLocal.attack.onStrikeEnchantment.clear();
    mLocal.attack.target.clear();
    mLocal.attack.targetMpNum = 0;
    mLocal.attack.targetKind = mwmp::Attack::TargetNone;
    mLocal.attack.hitPos[0] = 0.f;
    mLocal.attack.hitPos[1] = 0.f;
    mLocal.attack.hitPos[2] = 0.f;
    mLocal.attack.pressed = pressed;

    // On the rising edge, capture the animation type chosen by CharacterController.
    // The CC writes "mp_attack_type" to the base node just before startKey is built
    // (character.cpp send hook), so it's always fresh when we get here.
    if (pressed)
    {
        mLocal.attack.type = attackTypeFromWeapon(getEquippedWeapon(player));
        std::string atkType;
        if (auto* bn = player.getRefData().getBaseNode();
            bn && bn->getUserValue("mp_attack_type", atkType))
        {
            mLocal.attack.attackAnimation = atkType;
            Log(Debug::Verbose) << "[MP] PlayerSync::sendAttack pressed=true type=" << atkType;
        }
    }

    PacketPlayerAttack pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));

    mLocal.attack.hit = false;
    mLocal.attack.block = false;
    mLocal.attack.miss = false;
}

void PlayerSync::notifyLocalAttackRelease(float attackStrength)
{
    if (mLocal.guid == 0 || !mLastAttackPressed
        || (mLocal.attack.type != 2 && mLocal.attack.type != 3))
    {
        return;
    }

    mAwaitingRangedRelease = false;
    mLastAttackPressed = false;

    mLocal.attack.hit = false;
    mLocal.attack.block = false;
    mLocal.attack.miss = false;
    mLocal.attack.knocked = false;
    mLocal.attack.healthDamage = false;
    mLocal.attack.strength = std::clamp(attackStrength, 0.f, 1.f);
    mLocal.attack.damage = 0.f;
    mLocal.attack.onStrikeEnchantment.clear();
    mLocal.attack.target.clear();
    mLocal.attack.targetMpNum = 0;
    mLocal.attack.targetKind = mwmp::Attack::TargetNone;
    mLocal.attack.hitPos[0] = 0.f;
    mLocal.attack.hitPos[1] = 0.f;
    mLocal.attack.hitPos[2] = 0.f;
    mLocal.attack.pressed = false;

    PacketPlayerAttack pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));

    Log(Debug::Verbose) << "[MP] PlayerSync: ranged release strength=" << mLocal.attack.strength;
}

void PlayerSync::notifyLocalHit(const MWWorld::Ptr& victim, float damage, bool healthDamage, bool knocked,
    const osg::Vec3f& hitPos, int attackType, float attackStrength, const std::string& onStrikeEnchantment)
{
    if (mLocal.guid == 0 || victim.isEmpty())
        return;

    bool attackHeld = false;
    if (MWBase::World* world = MWBase::Environment::get().getWorld())
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!player.isEmpty())
            attackHeld = player.getClass().getCreatureStats(player).getAttackingOrSpell();
    }

    // Preserve the live held state when the hit actually lands. This keeps
    // queued follow-up attacks intact if the player released and re-held attack
    // before the hit event was emitted.
    mLocal.attack.pressed = attackHeld;
    mLocal.attack.hit = true;
    mLocal.attack.block = false;
    mLocal.attack.miss = false;
    mLocal.attack.knocked = knocked;
    mLocal.attack.healthDamage = healthDamage;
    mLocal.attack.type = attackType;
    mLocal.attack.strength = attackStrength;
    mLocal.attack.damage = damage;
    mLocal.attack.onStrikeEnchantment = onStrikeEnchantment;
    mLocal.attack.target = victim.getCellRef().getRefId().serializeText();
    mLocal.attack.targetMpNum = resolveTargetMpNum(victim);
    mLocal.attack.targetKind = mwmp::Attack::TargetNone;
    if (mLocal.attack.targetMpNum != 0)
        mLocal.attack.targetKind = mwmp::Attack::TargetPlayer;
    else if (Main::isInitialised())
    {
        const uint32_t targetActorMpNum = Main::get().getWorldObjectSync().getMpNumForObject(victim);
        if (targetActorMpNum != 0)
        {
            mLocal.attack.targetMpNum = targetActorMpNum;
            mLocal.attack.targetKind = mwmp::Attack::TargetActor;
        }
    }
    mLocal.attack.hitPos[0] = hitPos.x();
    mLocal.attack.hitPos[1] = hitPos.y();
    mLocal.attack.hitPos[2] = hitPos.z();

    if (mLocal.attack.targetKind != mwmp::Attack::TargetPlayer && Main::isInitialised())
        Main::get().sendActorCombatRequest(victim, damage, healthDamage, knocked, hitPos, attackType, attackStrength);

    PacketPlayerAttack pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendJournal()
{
    if (mLocal.journalChanges.items.empty())
        return;

    constexpr std::size_t maxDeltaItems = 256;
    std::vector<BasePlayer::JournalItem> items = std::move(mLocal.journalChanges.items);
    mLocal.journalChanges.action = BasePlayer::JournalChanges::Action::Add;
    for (std::size_t offset = 0; offset < items.size(); offset += maxDeltaItems)
    {
        const std::size_t count = std::min(maxDeltaItems, items.size() - offset);
        mLocal.journalChanges.items.assign(items.begin() + offset, items.begin() + offset + count);
        PacketPlayerJournal pkt;
        pkt.setPlayer(&mLocal);
        mClient.sendReliable(pkt.encode(mSeqCounter++));
    }
    mLocal.journalChanges.items.clear();
}

void PlayerSync::noteRemotePlayerHit(uint32_t attackerGuid)
{
    if (attackerGuid == 0)
        return;

    mRecentPlayerAttackerGuid = attackerGuid;
    mRecentPlayerAttackerTimer = PLAYER_ATTACKER_CONTEXT_SECONDS;
}

void PlayerSync::notifyLocalCastRelease(
    const std::string& spellId, const std::string& castAnimation, const MWWorld::Ptr& target)
{
    sendCastPacket(spellId, castAnimation, true, target);
}

void PlayerSync::sendDeath()
{
    mLocal.isDead = true;
    uint32_t killerGuid = 0;
    std::string killerRefId;
    if (mRecentPlayerAttackerTimer > 0.f)
        killerGuid = mRecentPlayerAttackerGuid;

    mLocal.deathAnimationGroup.clear();
    if (MWBase::World* world = MWBase::Environment::get().getWorld())
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!player.isEmpty())
        {
            if (auto* bn = player.getRefData().getBaseNode())
            {
                bn->getUserValue("mp_death_anim_group", mLocal.deathAnimationGroup);
                bn->setUserValue("mp_anim_play_pending", false);
            }

            const ESM::RefNum hitAttemptActor = player.getClass().getCreatureStats(player).getHitAttemptActor();
            MWWorld::WorldModel* worldModel = MWBase::Environment::get().getWorldModel();
            if (worldModel && hitAttemptActor.isSet())
            {
                MWWorld::Ptr attacker = worldModel->getPtr(hitAttemptActor);
                if (!attacker.isEmpty() && attacker.isInCell())
                {
                    if (killerGuid == 0)
                    {
                        if (auto* attackerNode = attacker.getRefData().getBaseNode())
                        {
                            int guid = 0;
                            if (attackerNode->getUserValue("mp_player_guid", guid) && guid > 0)
                                killerGuid = static_cast<uint32_t>(guid);
                        }
                    }

                    if (killerGuid == 0)
                    {
                        killerRefId = std::string(attacker.getClass().getName(attacker));
                        if (killerRefId.empty())
                            killerRefId = attacker.getCellRef().getRefId().serializeText();
                    }
                }
            }
        }
    }

    PacketPlayerDeath pkt;
    pkt.setPlayer(&mLocal);
    pkt.killerGuid = killerGuid;
    pkt.killerRefId = killerRefId;
    mClient.sendReliable(pkt.encode(mSeqCounter++));
    mRecentPlayerAttackerGuid = 0;
    mRecentPlayerAttackerTimer = 0.f;
}

void PlayerSync::applyServerDeath(const BasePlayer& state)
{
    mLocal.isDead = true;
    mLocal.deathAnimationGroup = state.deathAnimationGroup;
    mLastWasDead = true;
    mRespawnPending = true;
    mRespawnTimer = RESPAWN_DELAY;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
        return;

    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty())
        return;

    if (auto* bn = player.getRefData().getBaseNode())
    {
        if (!mLocal.deathAnimationGroup.empty())
            bn->setUserValue("mp_death_anim_group", mLocal.deathAnimationGroup);
        bn->setUserValue("mp_anim_play_pending", false);
    }

    MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
    MWMechanics::DynamicStat<float> health = stats.getHealth();
    health.setCurrent(0.f);
    stats.setHealth(health);
    stats.setDeathAnimationFinished(false);
    stats.setAttackingOrSpell(false);
    stats.setDrawState(MWMechanics::DrawState::Nothing);

    mLocal.dynamicStats.health.base = stats.getHealth().getBase();
    mLocal.dynamicStats.health.current = stats.getHealth().getCurrent();
    mLocal.dynamicStats.health.mod = stats.getHealth().getModifier();
    snapshotDynamicStats();
}

void PlayerSync::sendResurrect()
{
    mLocal.isDead = false;
    mLocal.deathAnimationGroup.clear();
    if (MWBase::World* world = MWBase::Environment::get().getWorld())
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!player.isEmpty())
        {
            if (const MWWorld::CellStore* cellStore = player.getCell())
            {
                if (const MWWorld::Cell* cell = cellStore->getCell())
                {
                    mLocal.cell.isExterior = cell->isExterior();
                    mLocal.cell.cellName = std::string(cell->getNameId());
                    if (cell->isExterior())
                    {
                        mLocal.cell.gridX = cell->getGridX();
                        mLocal.cell.gridY = cell->getGridY();
                    }
                }
            }
            const auto& position = player.getRefData().getPosition();
            for (int i = 0; i < 3; ++i)
            {
                mLocal.position.pos[i] = position.pos[i];
                mLocal.position.rot[i] = position.rot[i];
            }
            if (auto* bn = player.getRefData().getBaseNode())
                bn->setUserValue("mp_anim_play_pending", false);
        }
    }
    PacketPlayerResurrect pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendCast — reliable, edge-triggered when a spell fires.
void PlayerSync::respawnLocally(const MWWorld::Ptr& player)
{
    const MWBase::Environment& env = MWBase::Environment::get();
    MWBase::World* world = env.getWorld();
    MWBase::MechanicsManager* mechanics = env.getMechanicsManager();
    if (!world || !mechanics || player.isEmpty())
        return;

    const bool useImperialShrine = Misc::Rng::roll0to99(world->getPrng()) >= 50;
    const ESM::RefId respawnMarker
        = ESM::RefId::stringRefId(useImperialShrine ? "divinemarker" : "templemarker");
    world->teleportToClosestMarker(player, respawnMarker);

    if (player.getClass().isNpc() && player.getClass().getNpcStats(player).isWerewolf())
        mechanics->setWerewolf(player, false);

    mechanics->resurrect(player);

    if (MWBase::StateManager* stateManager = env.getStateManager();
        stateManager && stateManager->getState() == MWBase::StateManager::State_Ended)
    {
        stateManager->resumeGame();
    }

    MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
    MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
    fatigue.setCurrent(std::max(1.f, fatigue.getModified()), false, true);
    stats.setFatigue(fatigue);

    stats.setKnockedDown(false);
    stats.setHitRecovery(false);
    stats.setAttackingOrSpell(false);
    stats.setDrawState(MWMechanics::DrawState::Nothing);

    // Clear bounty so guards at the respawn location do not immediately
    // spam "you violated the law" dialogue.  In single-player death simply
    // reloads a save; in MP the player keeps playing, so we forgive crimes
    // on death just like paying at a guard would.
    if (player.getClass().isNpc())
    {
        player.getClass().getNpcStats(player).setBounty(0);
        // Record the crime-id threshold so NPCs restore their disposition
        // (same mechanism as OpPayFineThief / vanilla "pay bounty" path).
        world->getPlayer().recordCrimeId();
        // Stop any NPCs that are still in combat with the player from
        // the pre-death fight — otherwise they resume attacking on sight.
        if (MWBase::MechanicsManager* mm = MWBase::Environment::get().getMechanicsManager())
            mm->stopCombat(player);
    }

    if (player.getClass().isNpc())
        player.getClass().getNpcStats(player).setDrawState(MWMechanics::DrawState::Nothing);

    world->setGlobalInt(MWWorld::Globals::sPCKnownWerewolf, 0);

    if (MWBase::WindowManager* window = env.getWindowManager())
    {
        while (window->containsMode(MWGui::GM_MainMenu))
            window->removeGuiMode(MWGui::GM_MainMenu);
        window->setCursorVisible(false);
        window->setCursorActive(false);
    }
}

void PlayerSync::sendCast()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    // character.cpp writes "mp_cast_pending" = true and "mp_cast_spell_id" to the
    // base node the frame the spellcast animation begins — the same trigger point
    // TES3MP uses (localCast->shouldSend = true inside UpperBodyState::Casting entry).
    // We read-and-clear the flag here so each cast produces exactly one packet.
    auto* bn = player.getRefData().getBaseNode();
    if (!bn) return;

    bool castPending = false;
    if (!bn->getUserValue("mp_cast_pending", castPending) || !castPending)
        return;

    // Clear immediately so we don't double-send if update() runs twice before
    // the base node is refreshed (shouldn't happen, but be defensive).
    bn->setUserValue("mp_cast_pending", false);

    std::string spellId;
    bn->getUserValue("mp_cast_spell_id", spellId);

    std::string castAnim;
    bn->getUserValue("mp_cast_anim", castAnim);
    sendCastPacket(spellId, castAnim, false, MWWorld::Ptr());
}

// ---------------------------------------------------------------------------
// Change detection helpers
bool PlayerSync::positionChanged() const
{
    constexpr float posThreshold = 0.5f;
    constexpr float rotThreshold = 0.02f; // ~1.1 degrees — catches mouse-look turns

    // Force send if a teleport happened, even if delta is technically small.
    if (mLocal.position.isTeleporting)
        return true;

    for (int i = 0; i < 3; ++i)
        if (std::abs(mLocal.position.pos[i] - mLastPos.pos[i]) > posThreshold)
            return true;

    // Rotation: compare with wrap-around so the 0/2π boundary doesn't cause
    // false negatives when the player is facing near north/south.
    constexpr float twoPi = 6.28318530718f;
    constexpr float pi     = 3.14159265359f;
    for (int i = 0; i < 3; ++i)
    {
        float diff = mLocal.position.rot[i] - mLastPos.rot[i];
        // Normalise diff to [-π, π]
        while (diff >  pi) diff -= twoPi;
        while (diff < -pi) diff += twoPi;
        if (std::abs(diff) > rotThreshold)
            return true;
    }

    // Velocity-Delta Sync: force a sync if the velocity dropped to zero, even if
    // the position change is below the threshold. This ensures the "Stop" signal
    // is broadcast immediately, preventing remote NPC overshoot drift.
    const bool wasMovingVel = (mLastPos.velocity[0] != 0.f || mLastPos.velocity[1] != 0.f || mLastPos.velocity[2] != 0.f);
    const bool nowMovingVel = (mLocal.velocity.linear[0] != 0.f || mLocal.velocity.linear[1] != 0.f || mLocal.velocity.linear[2] != 0.f);
    if (wasMovingVel && !nowMovingVel)
        return true;

    return false;
}

bool PlayerSync::cellChanged() const
{
    return mLocal.cell.cellName  != mLastCell.cellName
        || mLocal.cell.isExterior != mLastCell.isExterior
        || mLocal.cell.gridX     != mLastCell.gx
        || mLocal.cell.gridY     != mLastCell.gy;
}

bool PlayerSync::equipmentChanged() const
{
    for (int i = 0; i < BasePlayer::NUM_EQUIPMENT_SLOTS; ++i)
    {
        if (!sameEquipment(mLocal.equipment[i], mLastEquip[i]))
        {
            static uint64_t sLastDiagnosticUs = 0;
            const uint64_t nowUs = steadyTimeUs();
            if (nowUs - sLastDiagnosticUs >= 1000000)
            {
                sLastDiagnosticUs = nowUs;
                const Item& live = mLocal.equipment[i].item;
                const Item& previous = mLastEquip[i].item;
                Log(Debug::Info) << "[MPDIAG] PlayerEquipment delta"
                                 << " slot=" << i
                                 << " liveRef=" << live.refId
                                 << " liveInstance=" << live.instanceId
                                 << " liveCount=" << live.count
                                 << " liveCharge=" << live.charge
                                 << " liveEnchant=" << live.enchantmentCharge
                                 << " previousRef=" << previous.refId
                                 << " previousInstance=" << previous.instanceId
                                 << " previousCount=" << previous.count
                                 << " previousCharge=" << previous.charge
                                 << " previousEnchant=" << previous.enchantmentCharge;
            }
            return true;
        }
    }
    return false;
}

bool PlayerSync::inventoryChanged() const
{
    if (mLocal.inventoryChanges.items.size() != mLastInventory.size())
        return true;

    for (std::size_t i = 0; i < mLocal.inventoryChanges.items.size(); ++i)
    {
        if (!sameItem(mLocal.inventoryChanges.items[i], mLastInventory[i]))
            return true;
    }
    return false;
}

bool PlayerSync::dynamicStatsChanged() const
{
    if (!sameDynamicStat(mLocal.dynamicStats.health, mLastStats.dynamicStats.health)
        || !sameDynamicStat(mLocal.dynamicStats.magicka, mLastStats.dynamicStats.magicka)
        || !sameDynamicStat(mLocal.dynamicStats.fatigue, mLastStats.dynamicStats.fatigue)
        || mLocal.level != mLastStats.level
        || std::abs(mLocal.levelProgress - mLastStats.levelProgress) > 0.01f)
        return true;

    for (std::size_t i = 0; i < mLocal.attributes.size(); ++i)
    {
        if (!sameAttribute(mLocal.attributes[i], mLastStats.attributes[i]))
            return true;
    }
    for (std::size_t i = 0; i < mLocal.skills.size(); ++i)
    {
        if (!sameSkill(mLocal.skills[i], mLastStats.skills[i]))
            return true;
    }
    return false;
}

bool PlayerSync::animFlagsChanged() const
{
    return mLocal.animFlags.movementFlags != mLastAnimFlags.movementFlags
        || mLocal.animFlags.actionFlags   != mLastAnimFlags.actionFlags
        || std::abs(mLocal.animFlags.animFwd  - mLastAnimFlags.animFwd)  >= 0.05f
        || std::abs(mLocal.animFlags.animSide - mLastAnimFlags.animSide) >= 0.05f
        || std::abs(mLocal.animFlags.blockedMoveSpeed - mLastAnimFlags.blockedMoveSpeed) >= 1.f;
}

// ---------------------------------------------------------------------------
void PlayerSync::snapshotPosition()
{
    std::memcpy(mLastPos.pos, mLocal.position.pos, sizeof(mLastPos.pos));
    std::memcpy(mLastPos.rot, mLocal.position.rot, sizeof(mLastPos.rot));
    std::memcpy(mLastPos.velocity, mLocal.velocity.linear, sizeof(mLastPos.velocity));
}
void PlayerSync::snapshotCell()
{
    mLastCell = { mLocal.cell.cellName, mLocal.cell.isExterior,
                  mLocal.cell.gridX, mLocal.cell.gridY };
}
void PlayerSync::snapshotEquipment()
{
    for (int i = 0; i < BasePlayer::NUM_EQUIPMENT_SLOTS; ++i)
        mLastEquip[i] = mLocal.equipment[i];
}

void PlayerSync::snapshotInventory()
{
    mLastInventory = mLocal.inventoryChanges.items;
}
void PlayerSync::snapshotDynamicStats()
{
    mLastStats.dynamicStats = mLocal.dynamicStats;
    mLastStats.attributes = mLocal.attributes;
    mLastStats.skills = mLocal.skills;
    mLastStats.level = mLocal.level;
    mLastStats.levelProgress = mLocal.levelProgress;
}

void PlayerSync::applyRestoredStatsToPlayer()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    MWWorld::Ptr player = world ? world->getPlayerPtr() : MWWorld::Ptr{};
    if (!world || player.isEmpty() || !player.getClass().isNpc())
        return;

    // Returning-player Lua onLoad runs immediately after this method. Install
    // the authoritative inventory/equipment first so scripts see the restored
    // character rather than the newGame template player.
    applyPendingAuthoritativeState(player);

    if (!mHasPendingRestoredStats && !mLocal.hasSavedStats)
        return;

    const BasePlayer& source = mHasPendingRestoredStats ? mPendingRestoredStats : mLocal;
    MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
    for (std::size_t i = 0; i < source.attributes.size(); ++i)
    {
        const Attribute& saved = source.attributes[i];
        MWMechanics::AttributeValue value;
        value.setBase(static_cast<float>(saved.base), true);
        value.setModifier(saved.mod);
        if (saved.damage > 0.f)
            value.damage(saved.damage);
        stats.setAttribute(ESM::Attribute::indexToRefId(static_cast<int>(i)), value);
    }

    for (std::size_t i = 0; i < source.skills.size(); ++i)
    {
        const Skill& saved = source.skills[i];
        MWMechanics::SkillValue value;
        value.setBase(saved.base, true);
        value.setModifier(saved.mod);
        if (saved.damage > 0.f)
            value.damage(saved.damage);
        value.setProgress(saved.progress);
        stats.setSkill(ESM::Skill::indexToRefId(static_cast<int>(i)), value);
    }

    stats.setLevel(std::max(1, source.level));
    stats.setLevelProgress(static_cast<int>(std::max(0.f, source.levelProgress)));
    stats.setHealth(toMechanicsDynamicStat(source.dynamicStats.health));
    stats.setMagicka(toMechanicsDynamicStat(source.dynamicStats.magicka));
    stats.setFatigue(toMechanicsDynamicStat(source.dynamicStats.fatigue));

    mHasPendingRestoredStats = false;
    capturePersistentStats(player);
    const Attribute& strength = mLocal.attributes[0];
    const Skill& blunt = mLocal.skills[ESM::Skill::refIdToIndex(ESM::Skill::BluntWeapon)];
    Log(Debug::Info) << "[MP] PlayerSync: restored persistent player stats"
                     << " level=" << mLocal.level
                     << " strength=" << strength.base
                     << " blunt=" << blunt.base
                     << " hp=" << mLocal.dynamicStats.health.current << "/" << mLocal.dynamicStats.health.base;
}

void PlayerSync::capturePersistentStats(const MWWorld::Ptr& player)
{
    if (player.isEmpty() || !player.getClass().isNpc())
        return;

    MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
    mLocal.level = stats.getLevel();
    mLocal.levelProgress = static_cast<float>(stats.getLevelProgress());
    mLocal.hasSavedStats = true;

    for (std::size_t i = 0; i < mLocal.attributes.size(); ++i)
    {
        const MWMechanics::AttributeValue& value
            = stats.getAttribute(ESM::Attribute::indexToRefId(static_cast<int>(i)));
        Attribute& out = mLocal.attributes[i];
        out.base = static_cast<int>(std::lround(value.getBase()));
        out.mod = value.getModifier();
        out.damage = value.getDamage();
    }

    for (std::size_t i = 0; i < mLocal.skills.size(); ++i)
    {
        const MWMechanics::SkillValue& value = stats.getSkill(ESM::Skill::indexToRefId(static_cast<int>(i)));
        Skill& out = mLocal.skills[i];
        out.base = value.getBase();
        out.mod = value.getModifier();
        out.damage = value.getDamage();
        out.progress = value.getProgress();
        out.increases = 0;
    }
}

void PlayerSync::captureEquipment(const MWWorld::Ptr& player)
{
    if (!player.getClass().hasInventoryStore(player))
        return;

    MWWorld::InventoryStore& invStore = player.getClass().getInventoryStore(player);
    for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
    {
        auto& entry = mLocal.equipment[slot];
        entry.slot = slot;

        MWWorld::ContainerStoreIterator it = invStore.getSlot(slot);
        if (it != invStore.end())
        {
            MWWorld::CellRef& cellRef = it->getCellRef();
            entry.item.refId = cellRef.getRefId().serializeText();
            entry.item.instanceId = inventoryInstanceId(cellRef.getRefNum());
            entry.item.count = cellRef.getCount();
            entry.item.charge = cellRef.getCharge();
            entry.item.enchantmentCharge = cellRef.getEnchantmentCharge();
            entry.item.soul = cellRef.getSoul().serializeText();
        }
        else
        {
            entry.item.refId.clear();
            entry.item.instanceId = 0;
            entry.item.count = 0;
            entry.item.charge = -1;
            entry.item.enchantmentCharge = -1.f;
            entry.item.soul.clear();
        }
    }
}

void PlayerSync::captureInventory(const MWWorld::Ptr& player)
{
    if (!player.getClass().hasInventoryStore(player))
        return;

    MWWorld::InventoryStore& invStore = player.getClass().getInventoryStore(player);
    auto& items = mLocal.inventoryChanges.items;
    items.clear();
    mLocal.inventoryChanges.action = BasePlayer::InventoryChanges::Action::Set;

    for (auto it = invStore.begin(); it != invStore.end(); ++it)
    {
        if (it->getCellRef().getCount() <= 0)
            continue;

        Item item;
        const MWWorld::CellRef& cellRef = it->getCellRef();
        item.refId = cellRef.getRefId().serializeText();
        item.instanceId = inventoryInstanceId(cellRef.getRefNum());
        item.count = cellRef.getCount();
        item.charge = cellRef.getCharge();
        item.enchantmentCharge = cellRef.getEnchantmentCharge();
        item.soul = cellRef.getSoul().serializeText();
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), inventoryOrder);
}

void PlayerSync::applyPendingAuthoritativeState(const MWWorld::Ptr& player)
{
    const MWBase::Environment& environment = MWBase::Environment::get();
    if (environment.getStateManager()->getState() != MWBase::StateManager::State_Running
        || player.isEmpty() || !player.isInCell())
        return;

    if (mPendingJournalRestore)
    {
        MWBase::Journal* journal = environment.getJournal();
        const bool replace = mAuthoritativeJournal.action == BasePlayer::JournalChanges::Action::Set;
        const bool notify = mAuthoritativeJournal.action == BasePlayer::JournalChanges::Action::Add;
        if (replace)
            journal->clearQuestJournal();

        for (const BasePlayer::JournalItem& item : mAuthoritativeJournal.items)
        {
            if (item.quest.empty())
                continue;

            try
            {
                const ESM::RefId questId = ESM::RefId::deserializeText(item.quest);
                if (item.type == BasePlayer::JournalItem::Type::Index)
                {
                    journal->setJournalIndex(questId, item.index);
                    continue;
                }

                if (item.infoId.empty())
                    continue;

                ESM::JournalEntry record{};
                record.mType = ESM::JournalEntry::Type_Journal;
                record.mTopic = questId;
                record.mInfo = ESM::RefId::deserializeText(item.infoId);
                record.mText = item.text;
                record.mActorName = item.actorName;
                record.mDay = item.daysPassed;
                record.mMonth = item.month;
                record.mDayOfMonth = item.dayOfMonth;
                journal->applyJournalEntry(record, item.index, notify);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "[MP] Ignoring invalid authoritative journal item"
                                    << " quest=" << item.quest
                                    << " info=" << item.infoId
                                    << " error=" << e.what();
            }
        }

        mPendingJournalRestore = false;
        mJournalAuthoritativeInitialized = true;
        captureJournalSnapshot();
        Log(Debug::Verbose) << "[MP] PlayerSync: applied authoritative journal"
                            << " replace=" << replace
                            << " items=" << mAuthoritativeJournal.items.size();
    }

    if (!player.getClass().hasInventoryStore(player))
        return;

    MWBase::World* world = environment.getWorld();
    if (!world)
        return;

    MWWorld::InventoryStore& invStore = player.getClass().getInventoryStore(player);
    MWWorld::ContainerStore& containerStore = invStore;
    const MWWorld::ESMStore& store = world->getStore();
    bool applied = false;
    bool inventoryRestoreApplied = false;
    bool equipmentRestoreApplied = false;

    if (mPendingInventoryRestore)
    {
        std::size_t skippedMissingInventory = 0;
        std::string firstMissingInventoryRefId;
        std::optional<Item> selectedEnchantItemBeforeRestore;
        MWWorld::ContainerStoreIterator selectedEnchantIt = containerStore.getSelectedEnchantItem();
        if (selectedEnchantIt != containerStore.end() && selectedEnchantIt->getCellRef().getCount() > 0)
        {
            const MWWorld::CellRef& selectedRef = selectedEnchantIt->getCellRef();
            Item selected;
            selected.refId = selectedRef.getRefId().serializeText();
            selected.instanceId = inventoryInstanceId(selectedRef.getRefNum());
            selected.count = selectedRef.getCount();
            selected.charge = selectedRef.getCharge();
            selected.enchantmentCharge = selectedRef.getEnchantmentCharge();
            selected.soul = selectedRef.getSoul().serializeText();
            selectedEnchantItemBeforeRestore = std::move(selected);
        }

        // ContainerStore::clear() only zeroes stack counts. It does not reset
        // mSelectedEnchantItem, so retaining that iterator across the rebuild
        // leaves scripts and the UI pointing at an obsolete zero-count stack.
        // A later selection/equip action can revive it as an untagged duplicate,
        // feeding another server identity correction and full rebuild.
        containerStore.setSelectedEnchantItem(containerStore.end());
        mLastPendingInventoryMissingRefId.clear();
        invStore.clear();
        for (const Item& item : mAuthoritativeInventory.items)
        {
            if (item.refId.empty() || item.count <= 0)
                continue;

            if (!hasRecordId(store, item.refId))
            {
                if (firstMissingInventoryRefId.empty())
                    firstMissingInventoryRefId = item.refId;
                ++skippedMissingInventory;
                continue;
            }

            MWWorld::ManualRef source(store, ESM::RefId::stringRefId(item.refId), item.count);
            MWWorld::Ptr sourcePtr = source.getPtr();
            MWWorld::CellRef& sourceCellRef = sourcePtr.getCellRef();
            sourceCellRef.setCharge(item.charge);
            sourceCellRef.setEnchantmentCharge(item.enchantmentCharge);
            sourceCellRef.setSoul(item.soul.empty() ? ESM::RefId() : ESM::RefId::deserializeText(item.soul));
            if (item.instanceId != 0)
                sourceCellRef.setRefNum(inventoryInstanceRefNum(item.instanceId));

            MWWorld::ContainerStoreIterator it = containerStore.add(sourcePtr, item.count, false, true, true);
            if (it == invStore.end())
                continue;

            // The preconfigured source RefNum is copied before registration,
            // so every authoritative stack enters the WorldModel under its
            // stable instance identity and cannot merge with an adjacent row.
        }

        bool restoredSelectedEnchantItem = false;
        uint32_t restoredSelectedEnchantInstanceId = 0;
        if (selectedEnchantItemBeforeRestore)
        {
            for (auto it = invStore.begin(); it != invStore.end(); ++it)
            {
                const MWWorld::CellRef& cellRef = it->getCellRef();
                if (cellRef.getCount() <= 0)
                    continue;

                Item candidate;
                candidate.refId = cellRef.getRefId().serializeText();
                candidate.instanceId = inventoryInstanceId(cellRef.getRefNum());
                candidate.count = cellRef.getCount();
                candidate.charge = cellRef.getCharge();
                candidate.enchantmentCharge = cellRef.getEnchantmentCharge();
                candidate.soul = cellRef.getSoul().serializeText();
                if (!sameStableItemIdentity(candidate, *selectedEnchantItemBeforeRestore))
                    continue;

                containerStore.setSelectedEnchantItem(it);
                environment.getWindowManager()->setSelectedEnchantItem(*it);
                restoredSelectedEnchantItem = true;
                restoredSelectedEnchantInstanceId = candidate.instanceId;
                break;
            }

            if (!restoredSelectedEnchantItem)
                environment.getWindowManager()->unsetSelectedSpell();

            static uint64_t sLastSelectedRestoreLogUs = 0;
            const uint64_t nowUs = steadyTimeUs();
            if (nowUs - sLastSelectedRestoreLogUs >= 1000000)
            {
                sLastSelectedRestoreLogUs = nowUs;
                Log(restoredSelectedEnchantItem ? Debug::Info : Debug::Warning)
                    << "[MPDIAG] Selected enchanted item across inventory rebuild"
                    << " ref=" << selectedEnchantItemBeforeRestore->refId
                    << " beforeInstance=" << selectedEnchantItemBeforeRestore->instanceId
                    << " restored=" << restoredSelectedEnchantItem
                    << " restoredInstance=" << restoredSelectedEnchantInstanceId
                    << " requestedStacks=" << inventoryStackCount(mAuthoritativeInventory.items);
            }
        }

        mPendingInventoryRestore = false;
        applied = true;
        inventoryRestoreApplied = true;

        const bool selectedRestoreFailed = selectedEnchantItemBeforeRestore && !restoredSelectedEnchantItem;
        Log(skippedMissingInventory == 0 && !selectedRestoreFailed ? Debug::Verbose : Debug::Info)
            << "[MP] PlayerSync: applied authoritative inventory"
            << " requestedStacks=" << inventoryStackCount(mAuthoritativeInventory.items)
            << " skippedMissing=" << skippedMissingInventory
            << " firstMissing=" << firstMissingInventoryRefId
            << " selectedBefore=" << (selectedEnchantItemBeforeRestore
                    ? selectedEnchantItemBeforeRestore->refId : std::string())
            << " selectedBeforeInstance=" << (selectedEnchantItemBeforeRestore
                    ? selectedEnchantItemBeforeRestore->instanceId : 0)
            << " selectedRestored=" << restoredSelectedEnchantItem;
    }

    if (inventoryRestoreApplied
        && !mPendingEquipmentRestore
        && equippedItemCount(mAuthoritativeEquipment) > 0)
    {
        mPendingEquipmentRestore = true;
        Log(Debug::Verbose) << "[MP] PlayerSync: reapplying authoritative equipment after inventory restore"
                            << " equipped=" << equippedItemCount(mAuthoritativeEquipment);
    }

    if (mPendingEquipmentRestore)
    {
        std::string missingEquipRefId;
        std::size_t recoveredMissingEquippedItems = 0;
        for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            const EquipmentItem& target = mAuthoritativeEquipment[slot];
            if (target.item.refId.empty())
                continue;

            bool found = false;
            for (auto it = invStore.begin(); it != invStore.end(); ++it)
            {
                const MWWorld::CellRef& cellRef = it->getCellRef();
                if (cellRef.getCount() <= 0)
                    continue;

                Item liveItem;
                liveItem.refId = cellRef.getRefId().serializeText();
                liveItem.instanceId = inventoryInstanceId(cellRef.getRefNum());
                liveItem.count = cellRef.getCount();
                liveItem.charge = cellRef.getCharge();
                liveItem.enchantmentCharge = cellRef.getEnchantmentCharge();
                liveItem.soul = cellRef.getSoul().serializeText();

                if (sameStableItemIdentity(liveItem, target.item))
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                if (!hasRecordId(store, target.item.refId))
                {
                    missingEquipRefId = target.item.refId;
                    break;
                }

                const int recoveredCount = std::max(1, target.item.count);
                MWWorld::ManualRef source(
                    store, ESM::RefId::stringRefId(target.item.refId), recoveredCount);
                MWWorld::Ptr sourcePtr = source.getPtr();
                MWWorld::CellRef& sourceCellRef = sourcePtr.getCellRef();
                sourceCellRef.setCharge(target.item.charge);
                sourceCellRef.setEnchantmentCharge(target.item.enchantmentCharge);
                sourceCellRef.setSoul(
                    target.item.soul.empty() ? ESM::RefId() : ESM::RefId::deserializeText(target.item.soul));
                if (target.item.instanceId != 0)
                    sourceCellRef.setRefNum(inventoryInstanceRefNum(target.item.instanceId));

                MWWorld::ContainerStoreIterator added
                    = containerStore.add(sourcePtr, recoveredCount, false, true, true);
                if (added == invStore.end())
                {
                    missingEquipRefId = target.item.refId;
                    break;
                }
                ++recoveredMissingEquippedItems;
            }
        }

        if (!missingEquipRefId.empty())
        {
            if (mLastPendingEquipmentMissingRefId != missingEquipRefId)
            {
                Log(Debug::Verbose) << "[MP] Delaying equipment restore until item is present: " << missingEquipRefId;
                mLastPendingEquipmentMissingRefId = missingEquipRefId;
            }
            return;
        }

        mLastPendingEquipmentMissingRefId.clear();
        invStore.unequipAll();
        for (int slot = 0; slot < BasePlayer::NUM_EQUIPMENT_SLOTS; ++slot)
        {
            const EquipmentItem& target = mAuthoritativeEquipment[slot];
            if (target.item.refId.empty())
                continue;

            for (auto it = invStore.begin(); it != invStore.end(); ++it)
            {
                const MWWorld::CellRef& cellRef = it->getCellRef();
                if (cellRef.getCount() <= 0)
                    continue;

                Item liveItem;
                liveItem.refId = cellRef.getRefId().serializeText();
                liveItem.instanceId = inventoryInstanceId(cellRef.getRefNum());
                liveItem.count = cellRef.getCount();
                liveItem.charge = cellRef.getCharge();
                liveItem.enchantmentCharge = cellRef.getEnchantmentCharge();
                liveItem.soul = cellRef.getSoul().serializeText();

                if (!sameStableItemIdentity(liveItem, target.item))
                    continue;

                invStore.equip(slot, it);
                break;
            }
        }
        mPendingEquipmentRestore = false;
        applied = true;
        equipmentRestoreApplied = true;

        Log(Debug::Verbose) << "[MP] PlayerSync: applied authoritative equipment"
                            << " equipped=" << equippedItemCount(mAuthoritativeEquipment)
                            << " recoveredMissingItems=" << recoveredMissingEquippedItems;
    }

    if (applied)
    {
        if (auto* anim = dynamic_cast<MWRender::NpcAnimation*>(world->getAnimation(player)))
            anim->equipmentChanged();
        MWBase::Environment::get().getWindowManager()->updatePlayer();
        captureInventory(player);
        captureEquipment(player);
        snapshotInventory();
        snapshotEquipment();

        std::size_t missingInstanceIds = 0;
        const Item* firstMissingInstance = nullptr;
        for (const Item& item : mLocal.inventoryChanges.items)
        {
            if (item.refId.empty() || item.count <= 0 || item.instanceId != 0)
                continue;
            ++missingInstanceIds;
            if (firstMissingInstance == nullptr)
                firstMissingInstance = &item;
        }
        const std::size_t requestedStacks = inventoryStackCount(mAuthoritativeInventory.items);
        const std::size_t liveStacks = inventoryStackCount(mLocal.inventoryChanges.items);
        if (inventoryRestoreApplied && (liveStacks != requestedStacks || missingInstanceIds != 0))
        {
            Log(Debug::Warning) << "[MPDIAG] Authoritative inventory rebuild mismatch"
                                << " requestedStacks=" << requestedStacks
                                << " liveStacks=" << liveStacks
                                << " stackDelta=" << static_cast<int64_t>(liveStacks)
                                    - static_cast<int64_t>(requestedStacks)
                                << " missingInstanceIds=" << missingInstanceIds
                                << " firstMissingRef="
                                << (firstMissingInstance ? firstMissingInstance->refId : std::string())
                                << " firstMissingCount="
                                << (firstMissingInstance ? firstMissingInstance->count : 0)
                                << " firstMissingCharge="
                                << (firstMissingInstance ? firstMissingInstance->charge : -1)
                                << " firstMissingEnchant="
                                << (firstMissingInstance ? firstMissingInstance->enchantmentCharge : -1.f);
        }

        // Echo the applied snapshot as an acknowledgement.  The server keeps all
        // equipment slots authoritative during its startup guard until it sees
        // this exact state, preventing a later engine auto-equip pass from being
        // mistaken for the player's intended equipment selection.
        if (equipmentRestoreApplied && mLocal.guid != 0 && mClient.isConnected())
            sendEquipment();

        Log(Debug::Verbose) << "[MP] PlayerSync: local authoritative restore snapshot"
                            << " liveInventoryStacks=" << inventoryStackCount(mLocal.inventoryChanges.items)
                            << " liveEquipped=" << equippedItemCount(mLocal.equipment);
    }
}

uint32_t PlayerSync::resolveTargetMpNum(const MWWorld::Ptr& victim) const
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world && victim == world->getPlayerPtr())
        return mLocal.guid;

    if (auto* baseNode = victim.getRefData().getBaseNode())
    {
        int guid = 0;
        if (baseNode->getUserValue("mp_player_guid", guid) && guid > 0)
            return static_cast<uint32_t>(guid);
    }

    return 0;
}

void PlayerSync::sendCastPacket(
    const std::string& spellId, const std::string& castAnimation, bool release, const MWWorld::Ptr& target)
{
    if (mLocal.guid == 0)
        return;

    mLocal.castSpell.spellId = spellId;
    mLocal.castSpell.success = true; // optimistic; authoritative spell validation is still future work
    mLocal.castSpell.release = release;
    mLocal.castSpell.castAnimation = castAnimation;
    mLocal.castSpell.targetGuid = target.isEmpty() ? 0 : resolveTargetMpNum(target);
    if (!target.isEmpty() && mLocal.castSpell.targetGuid == 0)
        mLocal.castSpell.targetRefId = target.getCellRef().getRefId().serializeText();
    else
        mLocal.castSpell.targetRefId.clear();

    Log(Debug::Info) << "[MP] PlayerSync::sendCast phase=" << (release ? "release" : "start")
                     << " spellId='" << spellId << "'"
                     << " targetGuid=" << mLocal.castSpell.targetGuid
                     << " targetRefId='" << mLocal.castSpell.targetRefId << "'";

    PacketPlayerCast pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

} // namespace mwmp
