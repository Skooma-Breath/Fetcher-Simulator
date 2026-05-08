#ifndef OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP
#define OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerStatsDynamic : public PlayerPacket
    {
    public:
        PacketPlayerStatsDynamic() : PlayerPacket(PacketType::PlayerStatsDynamic) {}

    protected:
        void packStat(WriteStream& ws, const DynamicStat& s)
        {
            ws.write(s.base);
            ws.write(s.current);
            ws.write(s.mod);
        }
        void unpackStat(ReadStream& rs, DynamicStat& s)
        {
            rs.read(s.base);
            rs.read(s.current);
            rs.read(s.mod);
        }

        void packAttribute(WriteStream& ws, const Attribute& s)
        {
            ws.write(s.base);
            ws.write(s.mod);
            ws.write(s.damage);
        }
        void unpackAttribute(ReadStream& rs, Attribute& s)
        {
            rs.read(s.base);
            rs.read(s.mod);
            rs.read(s.damage);
        }

        void packSkill(WriteStream& ws, const Skill& s)
        {
            ws.write(s.base);
            ws.write(s.mod);
            ws.write(s.damage);
            ws.write(s.progress);
            ws.write(s.increases);
        }
        void unpackSkill(ReadStream& rs, Skill& s)
        {
            rs.read(s.base);
            rs.read(s.mod);
            rs.read(s.damage);
            rs.read(s.progress);
            rs.read(s.increases);
        }

        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            packStat(ws, mPlayer->dynamicStats.health);
            packStat(ws, mPlayer->dynamicStats.magicka);
            packStat(ws, mPlayer->dynamicStats.fatigue);
            ws.write(mPlayer->level);
            ws.write(mPlayer->levelProgress);
            for (const auto& attribute : mPlayer->attributes)
                packAttribute(ws, attribute);
            for (const auto& skill : mPlayer->skills)
                packSkill(ws, skill);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            unpackStat(rs, mPlayer->dynamicStats.health);
            unpackStat(rs, mPlayer->dynamicStats.magicka);
            unpackStat(rs, mPlayer->dynamicStats.fatigue);
            rs.read(mPlayer->level);
            rs.read(mPlayer->levelProgress);
            for (auto& attribute : mPlayer->attributes)
                unpackAttribute(rs, attribute);
            for (auto& skill : mPlayer->skills)
                unpackSkill(rs, skill);
            mPlayer->hasSavedStats = true;
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP
