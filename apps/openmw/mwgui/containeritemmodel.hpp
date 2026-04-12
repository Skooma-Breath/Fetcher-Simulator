#ifndef MWGUI_CONTAINER_ITEM_MODEL_H
#define MWGUI_CONTAINER_ITEM_MODEL_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "itemmodel.hpp"

#include "../mwworld/containerstore.hpp"
#include <components/openmw-mp/Base/BaseObject.hpp>

namespace MWGui
{

    /// @brief The container item model supports multiple item sources, which are needed for
    /// making NPCs sell items from containers owned by them
    class ContainerItemModel : public ItemModel
    {
    public:
        ContainerItemModel(const std::vector<MWWorld::Ptr>& itemSources, const std::vector<MWWorld::Ptr>& worldItems);
        ///< @note The order of elements \a itemSources matters here. The first element has the highest priority for
        ///< removal,
        ///  while the last element will be used to add new items to.

        ContainerItemModel(const MWWorld::Ptr& source);

        bool allowedToUseItems() const override;

        bool onDropItem(const MWWorld::Ptr& item, int count) override;
        bool onTakeItem(const MWWorld::Ptr& item, int count) override;

        ItemStack getItem(ModelIndex index) override;
        ModelIndex getIndex(const ItemStack& item) override;
        size_t getItemCount() override;

        void update() override;
        void beginSyncBatch(mwmp::ContainerAction action);
        void endSyncBatch();

        bool usesContainer(const MWWorld::Ptr& container) override;

    protected:
        MWWorld::Ptr addItem(const ItemStack& item, size_t count, bool allowAutoEquip = true) override;
        MWWorld::Ptr copyItem(const ItemStack& item, size_t count, bool allowAutoEquip = true) override;
        void removeItem(const ItemStack& item, size_t count) override;

    private:
        struct SyncInfo
        {
            bool enabled = false;
            std::string cellId;
            std::string refId;
            uint32_t refNum = 0;
            uint32_t mpNum = 0;
        };

        void queueContainerDelta(mwmp::ContainerAction action, const MWWorld::Ptr& item, int count);
        void flushSyncBatch();

        std::vector<std::pair<MWWorld::Ptr, MWWorld::ResolutionHandle>> mItemSources;
        std::vector<MWWorld::Ptr> mWorldItems;
        const bool mTrading;
        std::vector<ItemStack> mItems;
        SyncInfo mSyncInfo;
        std::optional<mwmp::ContainerAction> mBatchAction;
        std::vector<mwmp::ContainerItem> mBatchItems;
    };

}

#endif
