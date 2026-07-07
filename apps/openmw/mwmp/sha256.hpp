#pragma once

///
/// mwmp/sha256.hpp — self-contained SHA-256 (RFC 6234) + hex encoding.
///
/// Header-only, no external dependencies.
/// Include from any translation unit that needs to hash multiplayer data.
/// Inline definitions make the header safe to include in multiple translation units.
///

#include <array>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <sstream>
#include <string>

namespace mwmp::crypto
{

namespace detail
{
    constexpr std::array<uint32_t, 64> kSHA256 = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    inline uint32_t S0 (uint32_t x) { return rotr(x,  2) ^ rotr(x, 13) ^ rotr(x, 22); }
    inline uint32_t S1 (uint32_t x) { return rotr(x,  6) ^ rotr(x, 11) ^ rotr(x, 25); }
    inline uint32_t G0 (uint32_t x) { return rotr(x,  7) ^ rotr(x, 18) ^ (x >>  3); }
    inline uint32_t G1 (uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    inline void transform(std::array<uint32_t, 8>& h, const uint8_t* block)
    {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16)
                | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i)
            w[i] = G1(w[i - 2]) + w[i - 7] + G0(w[i - 15]) + w[i - 16];

        auto [a, b, c, d, e, f, g, hh] = h;
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + S1(e) + ch(e, f, g) + kSHA256[i] + w[i];
            const uint32_t t2 = S0(a) + maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
} // namespace detail

class Sha256
{
public:
    void update(const uint8_t* data, std::size_t size)
    {
        mTotalBytes += size;
        while (size > 0)
        {
            const std::size_t count = std::min(size, mBlock.size() - mBlockSize);
            std::copy_n(data, count, mBlock.data() + mBlockSize);
            mBlockSize += count;
            data += count;
            size -= count;
            if (mBlockSize == mBlock.size())
            {
                detail::transform(mHash, mBlock.data());
                mBlockSize = 0;
            }
        }
    }

    std::string finish()
    {
        const uint64_t bitLength = mTotalBytes * 8;
        mBlock[mBlockSize++] = 0x80;
        if (mBlockSize > 56)
        {
            std::fill(mBlock.begin() + mBlockSize, mBlock.end(), 0);
            detail::transform(mHash, mBlock.data());
            mBlockSize = 0;
        }
        std::fill(mBlock.begin() + mBlockSize, mBlock.begin() + 56, 0);
        for (int i = 0; i < 8; ++i)
            mBlock[56 + i] = static_cast<uint8_t>(bitLength >> ((7 - i) * 8));
        detail::transform(mHash, mBlock.data());

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint32_t word : mHash)
            out << std::setw(8) << word;
        return out.str();
    }

private:
    std::array<uint32_t, 8> mHash = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::array<uint8_t, 64> mBlock{};
    std::size_t mBlockSize = 0;
    uint64_t mTotalBytes = 0;
};

/// Compute SHA-256(input) and return the lowercase 64-character hex digest.
inline std::string sha256hex(const std::string& input)
{
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    return hash.finish();
}

/// Compute SHA-256 from a binary stream without loading the whole file into memory.
inline std::string sha256hex(std::istream& input)
{
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
            hash.update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<std::size_t>(count));
    }
    return hash.finish();
}

} // namespace mwmp::crypto
