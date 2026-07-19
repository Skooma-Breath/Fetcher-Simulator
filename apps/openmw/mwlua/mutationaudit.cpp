#include "mutationaudit.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <components/debug/debuglog.hpp>

#include "context.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#ifdef BUILD_MULTIPLAYER
#include "../mwmp/Main.hpp"
#include "../mwmp/sync/InventoryIdentity.hpp"
#include "../mwmp/sync/WorldObjectSync.hpp"
#endif

namespace MWLua
{
    namespace
    {
        struct AuditEntry
        {
            std::chrono::steady_clock::time_point mLast;
            uint32_t mSuppressed = 0;
        };

        std::unordered_map<std::string, AuditEntry> sRecent;

        struct TargetDescription
        {
            std::string mIdentity;
            std::string_view mAuthority;
        };

        TargetDescription describeTarget(const MWWorld::Ptr& ptr)
        {
            if (ptr.isEmpty())
                return { "empty", "none" };

            std::string kind = "world-object";
            std::string_view authority = "server-world";
            if (ptr == MWBase::Environment::get().getWorld()->getPlayerPtr())
            {
                kind = "local-player";
                authority = "local-player";
            }
            else if (ptr.getClass().isActor())
                kind = "actor";
            if (ptr.getContainerStore())
            {
                kind = "inventory-stack";
                authority = "inventory-owner";
            }

            const auto refNum = ptr.getCellRef().getRefNum();
            std::string identity = kind + ":" + ptr.getCellRef().getRefId().serializeText() + "@"
                + std::to_string(refNum.mContentFile) + ":" + std::to_string(refNum.mIndex);
#ifdef BUILD_MULTIPLAYER
            const uint32_t instanceId = mwmp::inventoryInstanceId(refNum);
            if (instanceId != 0)
                identity += ":instanceId=" + std::to_string(instanceId);
            const uint32_t mpNum = mwmp::Main::get().getWorldObjectSync().getMpNumForObject(ptr);
            if (mpNum != 0)
                identity += ":mpNum=" + std::to_string(mpNum);
#endif
            return { std::move(identity), authority };
        }

        void writeAudit(const Context& context, std::string_view operation, std::string target,
            std::string_view authority, std::string_view detail)
        {
#ifdef BUILD_MULTIPLAYER
            if (!mwmp::Main::isConnected())
                return;
#else
            return;
#endif

            std::string_view script = context.mLua->getActiveScriptPath();
            if (script.empty())
                script = "<unknown-script>";

            std::string key;
            key.reserve(script.size() + operation.size() + target.size() + 3);
            key.append(script).push_back('|');
            key.append(operation).push_back('|');
            key.append(target);

            const auto now = std::chrono::steady_clock::now();
            AuditEntry& entry = sRecent[key];
            constexpr auto window = std::chrono::seconds(1);
            if (entry.mLast.time_since_epoch().count() != 0 && now - entry.mLast < window)
            {
                ++entry.mSuppressed;
                return;
            }

            Log(Debug::Warning) << "[MPAUDIT] native Lua mutation script=" << script
                                << " context=" << context.typeName() << " operation=" << operation
                                << " target=" << target << " authority=" << authority
                                << " route=audit-only"
                                << (detail.empty() ? "" : " detail=") << detail
                                << (entry.mSuppressed == 0 ? "" : " repeated_since_last=")
                                << (entry.mSuppressed == 0 ? std::string() : std::to_string(entry.mSuppressed));
            entry.mLast = now;
            entry.mSuppressed = 0;
        }
    }

    void auditNativeMutation(const Context& context, std::string_view operation, const MWWorld::Ptr& target,
        std::string_view detail)
    {
        TargetDescription description = describeTarget(target);
        writeAudit(context, operation, std::move(description.mIdentity), description.mAuthority, detail);
    }

    void auditNativeMutation(const Context& context, std::string_view operation, std::string_view target,
        std::string_view detail)
    {
        writeAudit(context, operation, std::string(target), "server-world", detail);
    }
}
