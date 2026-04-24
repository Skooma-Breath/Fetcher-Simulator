#include "inventoryitemmodel.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "../mwmp/Main.hpp"
#include "../mwmp/sync/WorldObjectSync.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"

namespace
{
    std::string makeCellId(const MWWorld::Ptr& ptr)
    {
        const MWWorld::CellStore* store = ptr.getCell();
        if (!store || !store->getCell())
            return {};

        const MWWorld::Cell* cell = store->getCell();
        if (cell->isExterior())
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "EXT:%d,%d", cell->getGridX(), cell->getGridY());
            return buf;
        }

        return std::string(cell->getNameId());
    }

    void appendOrMerge(std::vector<mwmp::ContainerItem>& items, const mwmp::ContainerItem& item)
    {
        auto it = std::find_if(items.begin(), items.end(),
            [&](const mwmp::ContainerItem& current)
            {
                return current.refId == item.refId && current.charge == item.charge;
            });

        if (it == items.end())
            items.push_back(item);
        else
            it->count += item.count;
    }
}

namespace MWGui
{

    InventoryItemModel::InventoryItemModel(const MWWorld::Ptr& actor)
        : mActor(actor)
    {
        if (!mwmp::Main::isInitialised())
            return;

        if (!actor.getClass().isActor() || !actor.getClass().getCreatureStats(actor).isDead())
            return;

        mSyncInfo.cellId = makeCellId(actor);
        if (mSyncInfo.cellId.empty())
            return;

        mSyncInfo.enabled = true;
        mSyncInfo.refId = actor.getCellRef().getRefId().serializeText();
        mSyncInfo.refNum = actor.getCellRef().getRefNum().mIndex;
        mSyncInfo.mpNum = mwmp::Main::get().getWorldObjectSync().getMpNumForObject(actor);
    }

    ItemStack InventoryItemModel::getItem(ModelIndex index)
    {
        if (index < 0)
            throw std::runtime_error("Invalid index supplied");
        if (mItems.size() <= static_cast<size_t>(index))
            throw std::runtime_error("Item index out of range");
        return mItems[index];
    }

    size_t InventoryItemModel::getItemCount()
    {
        return mItems.size();
    }

    ItemModel::ModelIndex InventoryItemModel::getIndex(const ItemStack& item)
    {
        ModelIndex i = 0;
        for (ItemStack& itemStack : mItems)
        {
            if (itemStack == item)
                return i;
            ++i;
        }
        return -1;
    }

    MWWorld::Ptr InventoryItemModel::addItem(const ItemStack& item, size_t count, bool allowAutoEquip)
    {
        if (item.mBase.getContainerStore() == &mActor.getClass().getContainerStore(mActor))
            throw std::runtime_error("Item to add needs to be from a different container!");
        MWWorld::Ptr added
            = *mActor.getClass().getContainerStore(mActor).add(item.mBase, static_cast<int>(count), allowAutoEquip);
        queueContainerDelta(mwmp::ContainerAction::Add, added, static_cast<int>(count));
        return added;
    }

    MWWorld::Ptr InventoryItemModel::copyItem(const ItemStack& item, size_t count, bool allowAutoEquip)
    {
        if (item.mBase.getContainerStore() == &mActor.getClass().getContainerStore(mActor))
            throw std::runtime_error("Item to copy needs to be from a different container!");

        MWWorld::ManualRef newRef(*MWBase::Environment::get().getESMStore(), item.mBase, static_cast<int>(count));
        MWWorld::Ptr added = *mActor.getClass().getContainerStore(mActor).add(
            newRef.getPtr(), static_cast<int>(count), allowAutoEquip);
        queueContainerDelta(mwmp::ContainerAction::Add, added, static_cast<int>(count));
        return added;
    }

    void InventoryItemModel::removeItem(const ItemStack& item, size_t count)
    {
        int removed = 0;
        // Re-equipping makes sense only if a target has inventory
        if (mActor.getClass().hasInventoryStore(mActor))
        {
            MWWorld::InventoryStore& store = mActor.getClass().getInventoryStore(mActor);
            removed = store.remove(item.mBase, static_cast<int>(count), true);
        }
        else
        {
            MWWorld::ContainerStore& store = mActor.getClass().getContainerStore(mActor);
            removed = store.remove(item.mBase, static_cast<int>(count));
        }

        std::stringstream error;

        if (removed == 0)
        {
            error << "Item '" << item.mBase.getCellRef().getRefId() << "' was not found in container store to remove";
            throw std::runtime_error(error.str());
        }
        else if (removed < static_cast<int>(count))
        {
            error << "Not enough items '" << item.mBase.getCellRef().getRefId() << "' in the stack to remove ("
                  << static_cast<int>(count) << " requested, " << removed << " found)";
            throw std::runtime_error(error.str());
        }

        queueContainerDelta(mwmp::ContainerAction::Remove, item.mBase, static_cast<int>(count));
    }

    MWWorld::Ptr InventoryItemModel::moveItem(
        const ItemStack& item, size_t count, ItemModel* otherModel, bool allowAutoEquip)
    {
        // Can't move conjured items: This is a general fix that also takes care of issues with taking conjured items
        // via the 'Take All' button.
        if (item.mFlags & ItemStack::Flag_Bound)
            return MWWorld::Ptr();

        return ItemModel::moveItem(item, count, otherModel, allowAutoEquip);
    }

    void InventoryItemModel::update()
    {
        if (mActor.isEmpty())
        {
            mItems.clear();
            return;
        }

        MWWorld::ContainerStore& store = mActor.getClass().getContainerStore(mActor);

        mItems.clear();

        for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
        {
            MWWorld::Ptr item = *it;

            if (!item.getClass().showsInInventory(item))
                continue;

            ItemStack newItem(item, this, item.getCellRef().getCount());

            if (mActor.getClass().hasInventoryStore(mActor))
            {
                MWWorld::InventoryStore& invStore = mActor.getClass().getInventoryStore(mActor);
                if (invStore.isEquipped(newItem.mBase))
                    newItem.mType = ItemStack::Type_Equipped;
            }

            mItems.push_back(newItem);
        }
    }

    void InventoryItemModel::beginSyncBatch(mwmp::ContainerAction action)
    {
        if (!mSyncInfo.enabled)
            return;

        mBatchAction = action;
        mBatchItems.clear();
    }

    void InventoryItemModel::endSyncBatch()
    {
        if (!mBatchAction.has_value())
            return;

        flushSyncBatch();
        mBatchAction.reset();
    }

    bool InventoryItemModel::onTakeItem(const MWWorld::Ptr& item, int count)
    {
        // Looting a dead corpse is considered OK
        if (mActor.getClass().isActor() && mActor.getClass().getCreatureStats(mActor).isDead())
            return true;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWBase::Environment::get().getMechanicsManager()->itemTaken(player, item, mActor, count);

        return true;
    }

    bool InventoryItemModel::usesContainer(const MWWorld::Ptr& container)
    {
        return mActor == container;
    }

    void InventoryItemModel::queueContainerDelta(mwmp::ContainerAction action, const MWWorld::Ptr& item, int count)
    {
        if (!mSyncInfo.enabled || count <= 0)
            return;

        mwmp::ContainerItem delta;
        delta.refId = item.getCellRef().getRefId().serializeText();
        delta.count = count;
        delta.charge = static_cast<int>(item.getCellRef().getCharge());

        if (mBatchAction.has_value())
        {
            if (mBatchAction.value() != action)
                flushSyncBatch();
            mBatchAction = action;
            appendOrMerge(mBatchItems, delta);
            return;
        }

        mwmp::Main::get().getWorldObjectSync().onLocalContainerChanged(
            mSyncInfo.cellId, mSyncInfo.refId, mSyncInfo.refNum, mSyncInfo.mpNum, action, {delta});
    }

    void InventoryItemModel::flushSyncBatch()
    {
        if (!mSyncInfo.enabled || !mBatchAction.has_value() || mBatchItems.empty())
            return;

        mwmp::Main::get().getWorldObjectSync().onLocalContainerChanged(
            mSyncInfo.cellId, mSyncInfo.refId, mSyncInfo.refNum, mSyncInfo.mpNum, mBatchAction.value(), mBatchItems);
        mBatchItems.clear();
    }

}
