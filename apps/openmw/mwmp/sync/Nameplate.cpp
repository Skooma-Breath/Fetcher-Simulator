#include "Nameplate.hpp"

#include <osg/AutoTransform>
#include <osg/Depth>
#include <osg/Group>
#include <osg/StateSet>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osgDB/Registry>
#include <osgText/Font>
#include <osgText/Text>

#include <components/debug/debuglog.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwrender/renderbin.hpp"

namespace mwmp
{
    namespace
    {
        // Height above the NPC's base node origin (feet level) in world units.
        // ~160 world units = typical NPC height; 130 puts the label just
        // above the head without a large gap at close range.
        constexpr float NAMEPLATE_HEIGHT = 145.f;

        // Base character size for fixed-world-size nameplates.
        constexpr float CHAR_SIZE = 16.f;

        // Load the VFS font used for in-game text if available.
        osg::ref_ptr<osgText::Font> loadFont()
        {
            const VFS::Path::Normalized fontPath("Fonts/DejaVuLGCSansMono.ttf");

            if (!osgDB::Registry::instance()->getReaderWriterForExtension("ttf"))
                return nullptr;

            const VFS::Manager* vfs
                = MWBase::Environment::get().getResourceSystem()->getVFS();
            if (!vfs || !vfs->exists(fontPath))
                return nullptr;

            try
            {
                const Files::IStreamPtr stream = vfs->get(fontPath);
                return osgText::readRefFontStream(*stream);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "[MP] Nameplate: font load failed: " << e.what();
                return nullptr;
            }
        }
    }

    // -----------------------------------------------------------------------
    Nameplate::Nameplate(const osg::ref_ptr<osg::Group>& parentNode, const std::string& name)
        : mParentNode(parentNode)
    {
        // --- text node -------------------------------------------------------
        mText = new osgText::Text;
        osg::ref_ptr<osgText::Text> text = mText;

        text->setText(name, osgText::String::ENCODING_UTF8);
        text->setCharacterSize(CHAR_SIZE);
        text->setAlignment(osgText::TextBase::CENTER_BOTTOM);

        // Black outline for readability against any background
        text->setBackdropType(osgText::Text::OUTLINE);
        text->setBackdropColor(osg::Vec4f(0.f, 0.f, 0.f, 0.9f));
        text->setBackdropOffset(0.05f);

        // Slightly warm white — visible against both bright and dark scenes
        text->setColor(osg::Vec4f(1.f, 0.95f, 0.8f, 1.f));

        if (auto font = loadFont())
            text->setFont(font);

        // --- billboard transform ---------------------------------------------
        mLabelNode = new osg::AutoTransform;
        mLabelNode->setName("MPNameplateAutoTransform:" + name);

        // Keep camera-facing rotation, but avoid AutoTransform screen-size scaling.
        // TODO(mp-nameplate): Replace this with a safe distance-based/manual scaling
        // path if fixed world-size nameplates are too hard to read. Do not restore
        // AutoTransform screen auto-scaling without guarding it: it produced
        // repeated NaN cull warnings in interior first-person views when a remote
        // player was offscreen.
        mLabelNode->setAutoRotateMode(osg::AutoTransform::ROTATE_TO_SCREEN);

        // Offset upward so the label sits above the NPC's head
        mLabelNode->setPosition(osg::Vec3f(0.f, 0.f, NAMEPLATE_HEIGHT));
        text->setName("MPNameplateText:" + name);

        // --- render state ----------------------------------------------------
        osg::StateSet* ss = mLabelNode->getOrCreateStateSet();

        // Depth test off: label always visible even through geometry
        ss->setAttributeAndModes(
            new osg::Depth(osg::Depth::ALWAYS, 0.0, 1.0, false),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        // Render after transparent objects and first-person so the label
        // is never occluded by the local player's own weapon/hands.
        ss->setRenderBinDetails(MWRender::RenderBin_SunGlare + 1, "RenderBin");

        // Lighting would tint/darken the text based on cell lighting.
        // Turn it off so the name is always at full brightness.
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        mLabelNode->addChild(text);
        mParentNode->addChild(mLabelNode);

        Log(Debug::Verbose) << "[MP] Nameplate created for '" << name << "'";
    }

    // -----------------------------------------------------------------------
    void Nameplate::updateName(const std::string& name)
    {
        if (mText)
            mText->setText(name, osgText::String::ENCODING_UTF8);
    }

    // -----------------------------------------------------------------------
    Nameplate::~Nameplate()
    {
        if (mParentNode && mLabelNode)
            mParentNode->removeChild(mLabelNode);
    }

} // namespace mwmp
