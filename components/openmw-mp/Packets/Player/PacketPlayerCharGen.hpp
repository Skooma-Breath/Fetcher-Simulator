#ifndef OPENMW_MP_PACKETPLAYERCHARGEN_HPP
#define OPENMW_MP_PACKETPLAYERCHARGEN_HPP

#include "PlayerPacket.hpp"
#include <components/esm/refid.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerCharGen — client → server notification that the player
    // has completed character creation (Review Dialog "Done" clicked and
    // the Morrowind script set sCharGenState = -1).
    //
    // Zero payload: the server only needs to know it happened so it can
    // mark the DB record is_new=0 and optionally trigger a spawn teleport.
    // -----------------------------------------------------------------------
    class PacketPlayerCharGen : public PlayerPacket
    {
    public:
        PacketPlayerCharGen() : PlayerPacket(PacketType::PlayerCharGen) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->race);
            ws.writeString(mPlayer->headMesh);
            ws.writeString(mPlayer->hairMesh);
            ws.write(mPlayer->isMale);
            ws.writeString(mPlayer->charClass.mId.serializeText());
            ws.writeString(mPlayer->charClass.mName);
            ws.writeString(mPlayer->birthSign);
            // Full CLDTstruct so server can persist and restore the class
            ws.write(mPlayer->charClass.mData.mSpecialization);
            for (auto v : mPlayer->charClass.mData.mAttribute) ws.write(v);
            for (auto& row : mPlayer->charClass.mData.mSkills)
                for (auto v : row) ws.write(v);
            ws.write(mPlayer->charClass.mData.mIsPlayable);
            ws.write(mPlayer->charClass.mData.mServices);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->race      = rs.readString();
            mPlayer->headMesh  = rs.readString();
            mPlayer->hairMesh  = rs.readString();
            rs.read(mPlayer->isMale);
            mPlayer->charClass.mId   = ESM::RefId::deserializeText(rs.readString());
            mPlayer->charClass.mName = rs.readString();
            mPlayer->birthSign = rs.readString();
            rs.read(mPlayer->charClass.mData.mSpecialization);
            for (auto& v : mPlayer->charClass.mData.mAttribute) rs.read(v);
            for (auto& row : mPlayer->charClass.mData.mSkills)
                for (auto& v : row) rs.read(v);
            rs.read(mPlayer->charClass.mData.mIsPlayable);
            rs.read(mPlayer->charClass.mData.mServices);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERCHARGEN_HPP
