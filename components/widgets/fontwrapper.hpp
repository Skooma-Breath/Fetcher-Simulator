#ifndef OPENMW_WIDGETS_WRAPPER_H
#define OPENMW_WIDGETS_WRAPPER_H

#include <MyGUI_Prerequest.h>

#include "components/settings/values.hpp"

#include <string>

namespace Gui
{
    /// Wrapper to tell UI element to use font size from settings.cfg
    template <class T>
    class FontWrapper : public T
    {
    public:
#if MYGUI_VERSION >= MYGUI_DEFINE_VERSION(3, 4, 2)
        void setFontName(std::string_view name) override
#else
        void setFontName(const std::string& name) override
#endif
        {
            T::setFontName(name);
            T::setPropertyOverride("FontHeight", std::to_string(Settings::gui().mFontSize));
        }
    };
}

#endif
