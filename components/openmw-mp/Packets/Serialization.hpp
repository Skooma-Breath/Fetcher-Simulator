#ifndef OPENMW_MP_SERIALIZATION_HPP
#define OPENMW_MP_SERIALIZATION_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <type_traits>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // WriteStream — append-only binary buffer
    // -----------------------------------------------------------------------
    class WriteStream
    {
    public:
        WriteStream() { mBuffer.reserve(256); }

        // Write a trivially-copyable value (float, int, bool, struct…)
        template<typename T>
        std::enable_if_t<std::is_trivially_copyable_v<T>>
        write(const T& value)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&value);
            mBuffer.insert(mBuffer.end(), p, p + sizeof(T));
        }

        // Write a length-prefixed UTF-8 string (uint16_t length)
        void writeString(const std::string& s)
        {
            auto len = static_cast<uint16_t>(s.size());
            write(len);
            const auto* p = reinterpret_cast<const uint8_t*>(s.data());
            mBuffer.insert(mBuffer.end(), p, p + len);
        }

        // Write a length-prefixed vector of trivially-copyable elements
        template<typename T>
        std::enable_if_t<std::is_trivially_copyable_v<T>>
        writeVector(const std::vector<T>& v)
        {
            auto count = static_cast<uint32_t>(v.size());
            write(count);
            for (const auto& elem : v)
                write(elem);
        }

        // Write a vector of strings
        void writeStringVector(const std::vector<std::string>& v)
        {
            auto count = static_cast<uint32_t>(v.size());
            write(count);
            for (const auto& s : v)
                writeString(s);
        }

        const std::vector<uint8_t>& buffer() const { return mBuffer; }
        std::vector<uint8_t> take() { return std::move(mBuffer); }

    private:
        std::vector<uint8_t> mBuffer;
    };

    // -----------------------------------------------------------------------
    // ReadStream — cursor-based reader over a raw byte span
    // -----------------------------------------------------------------------
    class ReadStream
    {
    public:
        ReadStream(const uint8_t* data, size_t size)
            : mData(data), mSize(size), mPos(0) {}

        explicit ReadStream(const std::vector<uint8_t>& buf)
            : mData(buf.data()), mSize(buf.size()), mPos(0) {}

        template<typename T>
        std::enable_if_t<std::is_trivially_copyable_v<T>>
        read(T& value)
        {
            need(sizeof(T));
            std::memcpy(&value, mData + mPos, sizeof(T));
            mPos += sizeof(T);
        }

        std::string readString()
        {
            uint16_t len = 0;
            read(len);
            need(len);
            std::string s(reinterpret_cast<const char*>(mData + mPos), len);
            mPos += len;
            return s;
        }

        template<typename T>
        std::enable_if_t<std::is_trivially_copyable_v<T>>
        readVector(std::vector<T>& v)
        {
            uint32_t count = 0;
            read(count);
            v.resize(count);
            for (auto& elem : v)
                read(elem);
        }

        void readStringVector(std::vector<std::string>& v)
        {
            uint32_t count = 0;
            read(count);
            v.resize(count);
            for (auto& s : v)
                s = readString();
        }

        bool eof()  const { return mPos >= mSize; }
        size_t pos() const { return mPos; }
        size_t remaining() const { return mSize - mPos; }

    private:
        void need(size_t n) const
        {
            if (mPos + n > mSize)
                throw std::runtime_error("ReadStream: buffer underrun");
        }

        const uint8_t* mData;
        size_t         mSize;
        size_t         mPos;
    };

} // namespace mwmp

#endif // OPENMW_MP_SERIALIZATION_HPP
