#ifndef OPENMW_MP_PLAYERPACKET_HPP
#define OPENMW_MP_PLAYERPACKET_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Base/BasePlayer.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PlayerPacket — base for all packets that carry a player's state.
    // Concrete subclasses set the player pointer, then encode/decode only
    // the fields relevant to their packet type.
    // -----------------------------------------------------------------------
    class PlayerPacket : public BasePacket
    {
    public:
        explicit PlayerPacket(PacketType type)
            : BasePacket(type), mPlayer(nullptr) {}

        void setPlayer(BasePlayer* player) { mPlayer = player; }
        BasePlayer* getPlayer() const      { return mPlayer; }

    protected:
        // Helpers shared by subclasses
        void packCellId(WriteStream& ws, const CellId& cell)
        {
            ws.writeString(cell.worldspace);
            ws.write(cell.isExterior);
            ws.write(cell.gridX);
            ws.write(cell.gridY);
            ws.writeString(cell.cellName);
        }

        void unpackCellId(ReadStream& rs, CellId& cell)
        {
            cell.worldspace  = rs.readString();
            rs.read(cell.isExterior);
            rs.read(cell.gridX);
            rs.read(cell.gridY);
            cell.cellName    = rs.readString();
        }

        void packPosition(WriteStream& ws, const Position& pos)
        {
            ws.write(pos.pos[0]); ws.write(pos.pos[1]); ws.write(pos.pos[2]);
            ws.write(pos.rot[0]); ws.write(pos.rot[1]); ws.write(pos.rot[2]);
        }

        void unpackPosition(ReadStream& rs, Position& pos)
        {
            rs.read(pos.pos[0]); rs.read(pos.pos[1]); rs.read(pos.pos[2]);
            rs.read(pos.rot[0]); rs.read(pos.rot[1]); rs.read(pos.rot[2]);
        }

        BasePlayer* mPlayer;
    };

} // namespace mwmp

#endif // OPENMW_MP_PLAYERPACKET_HPP
