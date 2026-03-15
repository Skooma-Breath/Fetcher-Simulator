#ifndef OPENMW_MWMP_SYNC_NAMEPLATE_HPP
#define OPENMW_MWMP_SYNC_NAMEPLATE_HPP

#include <string>

#include <osg/ref_ptr>
#include <osg/AutoTransform>
#include <osg/Group>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // Nameplate — a world-space billboard label that floats above a remote
    // player's NPC, always facing the camera at a fixed screen size.
    //
    // Usage:
    //   mNameplate = std::make_unique<Nameplate>(mNpcPtr.getRefData().getBaseNode(), mName);
    //   mNameplate.reset(); // detaches cleanly
    //
    // The nameplate attaches itself to the given parent node on construction
    // and removes itself on destruction — no manual cleanup needed.
    // -----------------------------------------------------------------------
    class Nameplate
    {
    public:
        // Attach a label showing 'name' to parentNode, offset above the NPC.
        // parentNode must remain valid for the lifetime of this object.
        Nameplate(osg::Group* parentNode, const std::string& name);
        ~Nameplate();

        // Non-copyable / non-movable
        Nameplate(const Nameplate&)            = delete;
        Nameplate& operator=(const Nameplate&) = delete;

    private:
        osg::Group*                       mParentNode;
        osg::ref_ptr<osg::AutoTransform>  mLabelNode;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_NAMEPLATE_HPP
