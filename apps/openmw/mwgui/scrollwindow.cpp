#include "scrollwindow.hpp"

#include <MyGUI_ImageBox.h>
#include <MyGUI_ScrollView.h>

#include <algorithm>
#include <cctype>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/widgets/imagebutton.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/actiontake.hpp"
#include "../mwworld/class.hpp"

#include "formatting.hpp"

namespace
{
    std::string scrollLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool scrollContainsCaseInsensitive(const std::string& value, const std::string& needle)
    {
        return scrollLowerCopy(value).find(needle) != std::string::npos;
    }

    bool isStarwindDatapadScroll(const MWWorld::Ptr& scroll)
    {
        if (scroll.isEmpty() || scroll.getType() != ESM::REC_BOOK)
            return false;

        const ESM::Book* record = scroll.get<ESM::Book>()->mBase;
        if (!record)
            return false;

        const std::string id = scrollLowerCopy(record->mId.serializeText());
        if (id.rfind("sw_", 0) != 0)
            return false;

        return scrollContainsCaseInsensitive(record->mModel, "datapad.nif")
            || scrollContainsCaseInsensitive(record->mIcon, "datapad")
            || scrollContainsCaseInsensitive(record->mName, "datapad");
    }
}

namespace MWGui
{

    ScrollWindow::ScrollWindow()
        : BookWindowBase("openmw_scroll.layout")
        , mTakeButtonShow(true)
        , mTakeButtonAllowed(true)
    {
        getWidget(mTextView, "TextView");
        getWidget(mScrollImage, "ScrollImage");

        getWidget(mCloseButton, "CloseButton");
        mCloseButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onCloseButtonClicked);

        getWidget(mTakeButton, "TakeButton");
        mTakeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onTakeButtonClicked);

        adjustButton("CloseButton");
        adjustButton("TakeButton");

        mCloseButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);
        mTakeButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);

        mControllerScrollWidget = mTextView;
        mControllerButtons.mB = "#{Interface:Close}";
        mControllerButtons.mDpad = "#{Interface:ScrollDown}";

        center();
    }

    void ScrollWindow::setPtr(const MWWorld::Ptr& scroll)
    {
        if (scroll.isEmpty() || (scroll.getType() != ESM::REC_BOOK && scroll.getType() != ESM::REC_BOOK4))
            throw std::runtime_error("Invalid argument in ScrollWindow::setPtr");
        mScroll = scroll;

        const bool starwindDatapad = isStarwindDatapadScroll(scroll);
        const char* scrollTexture = starwindDatapad ? "textures\\starwind_compat\\tablet_reader.dds" : "textures\\scroll.dds";
        if (scroll.getType() == ESM::REC_BOOK)
        {
            const ESM::Book* debugRecord = scroll.get<ESM::Book>()->mBase;
            Log(Debug::Info) << "[StarwindDatapad] ScrollWindow::setPtr id=" << debugRecord->mId.serializeText()
                             << " name=" << debugRecord->mName
                             << " model=" << debugRecord->mModel
                             << " icon=" << debugRecord->mIcon
                             << " isDatapad=" << (starwindDatapad ? "true" : "false")
                             << " texture=" << scrollTexture;
        }
        mScrollImage->setImageTexture(scrollTexture);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        bool showTakeButton = scroll.getContainerStore() != &player.getClass().getContainerStore(player);

        const std::string* text;
        if (scroll.getType() == ESM::REC_BOOK)
            text = &scroll.get<ESM::Book>()->mBase->mText;
        else
            text = &scroll.get<ESM4::Book>()->mBase->mText;
        bool shrinkTextAtLastTag = scroll.getType() == ESM::REC_BOOK;

        Formatting::BookFormatter formatter;
        formatter.markupToWidget(mTextView, *text, 390, mTextView->getHeight(), shrinkTextAtLastTag);
        MyGUI::IntSize size = mTextView->getChildAt(0)->getSize();

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mTextView->setVisibleVScroll(false);
        if (size.height > mTextView->getSize().height)
            mTextView->setCanvasSize(mTextView->getWidth(), size.height);
        else
            mTextView->setCanvasSize(mTextView->getWidth(), mTextView->getSize().height);
        mTextView->setVisibleVScroll(true);

        mTextView->setViewOffset(MyGUI::IntPoint(0, 0));

        setTakeButtonShow(showTakeButton);

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mCloseButton);
    }

    void ScrollWindow::onKeyButtonPressed(MyGUI::Widget* /*sender*/, MyGUI::KeyCode key, MyGUI::Char character)
    {
        int scroll = 0;
        if (key == MyGUI::KeyCode::ArrowUp)
            scroll = 40;
        else if (key == MyGUI::KeyCode::ArrowDown)
            scroll = -40;

        if (scroll != 0)
            mTextView->setViewOffset(mTextView->getViewOffset() + MyGUI::IntPoint(0, scroll));
    }

    void ScrollWindow::setTakeButtonShow(bool show)
    {
        mTakeButtonShow = show;
        mTakeButton->setVisible(mTakeButtonShow && mTakeButtonAllowed);
    }

    void ScrollWindow::setInventoryAllowed(bool allowed)
    {
        mTakeButtonAllowed = allowed;
        mTakeButton->setVisible(mTakeButtonShow && mTakeButtonAllowed);
    }

    void ScrollWindow::onCloseButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onTakeButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->playSound(mScroll.getClass().getUpSoundId(mScroll));

        MWWorld::ActionTake take(mScroll);
        take.execute(MWMechanics::getPlayer());

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onClose()
    {
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(true);
        BookWindowBase::onClose();
    }

    ControllerButtons* ScrollWindow::getControllerButtons()
    {
        if (mTakeButton->getVisible())
            mControllerButtons.mA = "#{Interface:Take}";
        else
            mControllerButtons.mA.clear();
        return &mControllerButtons;
    }

    bool ScrollWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mTakeButton->getVisible())
                onTakeButtonClicked(mTakeButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCloseButtonClicked(mCloseButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            return false; // Fall through to keyboard

        return true;
    }
}
