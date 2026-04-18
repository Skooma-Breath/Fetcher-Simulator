#include "myguiloglistener.hpp"

#include <iomanip>

#include <components/debug/debuglog.hpp>

namespace MyGUIPlatform
{
    void CustomLogListener::open()
    {
        mStream.open(mFileName, std::ios_base::out);
        if (!mStream.is_open())
            Log(Debug::Error) << "Unable to create MyGUI log with path " << mFileName;
    }

    void CustomLogListener::close()
    {
        if (mStream.is_open())
            mStream.close();
    }

    void CustomLogListener::flush()
    {
        if (mStream.is_open())
            mStream.flush();
    }

#if MYGUI_VERSION >= MYGUI_DEFINE_VERSION(3, 4, 2)
    void CustomLogListener::log(std::string_view section, MyGUI::LogLevel level, const tm* time,
        std::string_view message, std::string_view file, int line)
    {
        if (mStream.is_open())
        {
            std::string_view separator = "  |  ";
            mStream << std::setw(2) << std::setfill('0') << time->tm_hour << ":" << std::setw(2) << std::setfill('0')
                    << time->tm_min << ":" << std::setw(2) << std::setfill('0') << time->tm_sec << separator << section
                    << separator << level.print() << separator << message << separator << file << separator << line
                    << std::endl;
        }
    }
#else
    void CustomLogListener::log(const std::string& section, MyGUI::LogLevel level, const tm* time,
        const std::string& message, const char* file, int line)
    {
        if (mStream.is_open())
        {
            std::string_view separator = "  |  ";
            mStream << std::setw(2) << std::setfill('0') << time->tm_hour << ":" << std::setw(2) << std::setfill('0')
                    << time->tm_min << ":" << std::setw(2) << std::setfill('0') << time->tm_sec << separator << section
                    << separator << level.print() << separator << message << separator << file << separator << line
                    << std::endl;
        }
    }
#endif

    MyGUI::LogLevel LogFacility::getCurrentLogLevel() const
    {
        switch (Log::sMinDebugLevel)
        {
            case Debug::Error:
                return MyGUI::LogLevel::Error;
            case Debug::Warning:
                return MyGUI::LogLevel::Warning;
            default:
                return MyGUI::LogLevel::Info;
        }
    }
}
