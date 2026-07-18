#ifndef OPENMW_GAME_MWGUI_MAINMENU_H
#define OPENMW_GAME_MWGUI_MAINMENU_H

#include <memory>
#include <optional>
#include <thread>

#include "savegamedialog.hpp"
#include "windowbase.hpp"

namespace Gui
{
    class ImageButton;
}

namespace VFS
{
    class Manager;
}

#ifdef BUILD_MULTIPLAYER
namespace mwmp
{
    class CharacterSelectDialog;
    class ServerAddressDialog;
    class ServerBrowserDialog;
}
#endif

namespace MWGui
{

    class BackgroundImage;
    class VideoWidget;
    class MenuVideo
    {
        MyGUI::ImageBox* mVideoBackground;
        VideoWidget* mVideo;
        std::thread mThread;
        bool mRunning;

        void run();

    public:
        MenuVideo(const VFS::Manager* vfs);
        void resize(int w, int h);
        void commitFrame();
        ~MenuVideo();
    };

    class MainMenu : public WindowBase
    {
        int mWidth;
        int mHeight;

        bool mHasAnimatedMenu;

    public:
        MainMenu(int w, int h, const VFS::Manager* vfs, const std::string& versionDescription);
        ~MainMenu() override;

        void onResChange(int w, int h) override;
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

        void setVisible(bool visible) override;

        bool exit() override;
        void onFrame(float dt) override;

    private:
        const VFS::Manager* mVFS;

        MyGUI::Widget* mButtonBox;
        MyGUI::TextBox* mVersionText;

        BackgroundImage* mBackground;

        std::optional<MenuVideo> mVideo; // For animated main menus

        std::map<std::string, Gui::ImageButton*, std::less<>> mButtons;

        void onButtonClicked(MyGUI::Widget* sender);
        void onNewGameConfirmed();
#ifdef BUILD_MULTIPLAYER
        void onMainMenuConfirmed();
#endif
        void onExitConfirmed();

        void showBackground(bool show);

        void updateMenu();

        std::unique_ptr<SaveGameDialog> mSaveGameDialog;
#ifdef BUILD_MULTIPLAYER
        std::unique_ptr<mwmp::ServerAddressDialog> mServerAddressDialog;
        std::unique_ptr<mwmp::ServerBrowserDialog> mServerBrowserDialog;
        std::unique_ptr<mwmp::CharacterSelectDialog> mCharSelectDialog;
#endif
    };

}

#endif
