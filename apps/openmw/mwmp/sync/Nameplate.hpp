#ifndef OPENMW_MWMP_SYNC_NAMEPLATE_HPP
#define OPENMW_MWMP_SYNC_NAMEPLATE_HPP

#include <string>

#include <osg/ref_ptr>
#include <osg/AutoTransform>
#include <osg/Group>
namespace osgText { class Text; }

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
        Nameplate(const osg::ref_ptr<osg::Group>& parentNode, const std::string& name);
        ~Nameplate();

        // Update the displayed text in-place (no node rebuild needed).
        void updateName(const std::string& name);

        // Non-copyable / non-movable
        Nameplate(const Nameplate&)            = delete;
        Nameplate& operator=(const Nameplate&) = delete;

    private:
        osg::ref_ptr<osg::Group>          mParentNode;
        osg::ref_ptr<osg::AutoTransform>  mLabelNode;
        osg::ref_ptr<osgText::Text>       mText;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_NAMEPLATE_HPP
