#ifndef OPENMW_MP_PACKETACTORPOSITIONV2_HPP
#define OPENMW_MP_PACKETACTORPOSITIONV2_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorPositionV2 : public ActorPacket
    {
    public:
        PacketActorPositionV2()
            : ActorPacket(PacketType::ActorPositionV2)
        {
        }

        void setPositionList(ActorPositionV2List* positionList)
        {
            mPositionList = positionList;
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPositionList->protocolVersion);
            ws.write(mPositionList->authorityGuid);
            ws.write(mPositionList->authorityGeneration);
            ws.write(mPositionList->sequence);
            ws.write(mPositionList->serverTimestamp);

            const auto count = static_cast<uint16_t>(mPositionList->snapshots.size());
            ws.write(count);
            for (const auto& snapshot : mPositionList->snapshots)
                packSnapshot(ws, snapshot);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPositionList->protocolVersion);
            rs.read(mPositionList->authorityGuid);
            rs.read(mPositionList->authorityGeneration);
            rs.read(mPositionList->sequence);
            rs.read(mPositionList->serverTimestamp);

            uint16_t count = 0;
            rs.read(count);
            mPositionList->snapshots.resize(count);
            for (auto& snapshot : mPositionList->snapshots)
                unpackSnapshot(rs, snapshot);
        }

    private:
        static int32_t quantizePosition(float value)
        {
            const double scaled = std::round(static_cast<double>(value) * 100.0);
            const double minValue = static_cast<double>(std::numeric_limits<int32_t>::min());
            const double maxValue = static_cast<double>(std::numeric_limits<int32_t>::max());
            return static_cast<int32_t>(std::clamp(scaled, minValue, maxValue));
        }

        static float dequantizePosition(int32_t value)
        {
            return static_cast<float>(value) / 100.f;
        }

        static int16_t quantizeVelocity(float value)
        {
            const float scaled = std::round(value * 100.f);
            const float minValue = static_cast<float>(std::numeric_limits<int16_t>::min());
            const float maxValue = static_cast<float>(std::numeric_limits<int16_t>::max());
            return static_cast<int16_t>(std::clamp(scaled, minValue, maxValue));
        }

        static float dequantizeVelocity(int16_t value)
        {
            return static_cast<float>(value) / 100.f;
        }

        static int16_t quantizeRotation(float radians)
        {
            static constexpr float kPi = 3.14159265358979323846f;
            static constexpr float kTwoPi = kPi * 2.f;

            float normalized = std::fmod(radians + kPi, kTwoPi);
            if (normalized < 0.f)
                normalized += kTwoPi;
            normalized -= kPi;

            return static_cast<int16_t>(std::round(std::clamp(normalized / kPi, -1.f, 1.f) * 32767.f));
        }

        static float dequantizeRotation(int16_t value)
        {
            static constexpr float kPi = 3.14159265358979323846f;
            return static_cast<float>(value) * kPi / 32767.f;
        }

        void packSnapshot(WriteStream& ws, const CompactActorSnapshot& snapshot)
        {
            ws.write(snapshot.actorNetId);
                ws.write(snapshot.migrationGeneration);
                for (float axis : snapshot.position.pos)
                ws.write(quantizePosition(axis));
            for (float axis : snapshot.position.rot)
                ws.write(quantizeRotation(axis));
            for (float axis : snapshot.velocity.linear)
                ws.write(quantizeVelocity(axis));
            for (float axis : snapshot.velocity.angular)
                ws.write(quantizeVelocity(axis));
            ws.write(snapshot.movementFlags);
            ws.write(snapshot.animFwd);
            ws.write(snapshot.animSide);
            ws.write(snapshot.presentationFlags);
        }

        void unpackSnapshot(ReadStream& rs, CompactActorSnapshot& snapshot)
        {
            rs.read(snapshot.actorNetId);
                rs.read(snapshot.migrationGeneration);

                int32_t positionAxis = 0;
            for (float& axis : snapshot.position.pos)
            {
                rs.read(positionAxis);
                axis = dequantizePosition(positionAxis);
            }

            int16_t rotationAxis = 0;
            for (float& axis : snapshot.position.rot)
            {
                rs.read(rotationAxis);
                axis = dequantizeRotation(rotationAxis);
            }

            int16_t velocityAxis = 0;
            for (float& axis : snapshot.velocity.linear)
            {
                rs.read(velocityAxis);
                axis = dequantizeVelocity(velocityAxis);
            }
            for (float& axis : snapshot.velocity.angular)
            {
                rs.read(velocityAxis);
                axis = dequantizeVelocity(velocityAxis);
            }

            rs.read(snapshot.movementFlags);
            rs.read(snapshot.animFwd);
            rs.read(snapshot.animSide);
            rs.read(snapshot.presentationFlags);
            snapshot.position.isTeleporting = (snapshot.presentationFlags & ActorPresentationTeleporting) != 0;
        }

        ActorPositionV2List* mPositionList = nullptr;
    };
}

#endif
