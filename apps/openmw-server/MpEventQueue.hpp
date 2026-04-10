#ifndef OPENMW_SERVER_MPEVENTQUEUE_HPP
#define OPENMW_SERVER_MPEVENTQUEUE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <components/lua/serialization.hpp>

namespace mwmp
{

struct InboundLuaEvent
{
    uint32_t            pid = 0;
    std::string         name;
    LuaUtil::BinaryData data;
};

class MpEventQueue
{
public:
    void push(InboundLuaEvent event)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mBack.push_back(std::move(event));
    }

    std::vector<InboundLuaEvent> takeAll()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFront.clear();
        mFront.swap(mBack);
        return std::move(mFront);
    }

private:
    std::mutex mMutex;
    std::vector<InboundLuaEvent> mFront;
    std::vector<InboundLuaEvent> mBack;
};

} // namespace mwmp

#endif // OPENMW_SERVER_MPEVENTQUEUE_HPP
