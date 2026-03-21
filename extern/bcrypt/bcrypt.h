/**
 * bcrypt.h — Portable bcrypt password hashing wrapper
 *
 * Provides bcrypt_hash() and bcrypt_verify() using the best available
 * backend for each platform:
 *
 *   Linux/Mac : libcrypt / libxcrypt (crypt_r, always supports $2b$)
 *   Windows   : OpenSSL EVP + a bundled Blowfish bcrypt implementation
 *
 * Usage:
 *   std::string hash = Bcrypt::hash("mypassword");        // cost 12
 *   bool ok          = Bcrypt::verify("mypassword", hash);
 *
 * Public domain — incorporates the Solar Designer / OpenWall crypt_blowfish
 * algorithm on Windows. On Linux, delegates to the system libcrypt which
 * has supported $2b$ since glibc 2.28 / libxcrypt 4.1.
 *
 * The default cost factor is 12, producing ~250ms hashes on modern hardware.
 * This is the recommended setting for interactive login.
 */

#pragma once

#include <string>
#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Platform detection
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)
#  define BCRYPT_USE_OPENSSL 1
#else
#  define BCRYPT_USE_CRYPT 1
#endif

// ─────────────────────────────────────────────────────────────────────────────
// POSIX / Linux path — delegates to libcrypt
// ─────────────────────────────────────────────────────────────────────────────

#if defined(BCRYPT_USE_CRYPT)

#include <crypt.h>

namespace Bcrypt
{
    namespace detail
    {
        // Generate a bcrypt salt string: "$2b$NN$<22 base64 chars>"
        // Uses /dev/urandom for randomness.
        inline std::string generateSalt(int cost = 12)
        {
            if (cost < 4 || cost > 31)
                throw std::invalid_argument("bcrypt cost must be 4-31");

            // Read 16 random bytes
            unsigned char raw[16];
            FILE* urandom = fopen("/dev/urandom", "rb");
            if (!urandom) throw std::runtime_error("Cannot open /dev/urandom");
            if (fread(raw, 1, 16, urandom) != 16)
            {
                fclose(urandom);
                throw std::runtime_error("Cannot read /dev/urandom");
            }
            fclose(urandom);

            // bcrypt base64 alphabet (not standard base64)
            static const char b64[] =
                "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

            // Encode 16 bytes → 22 bcrypt-base64 characters
            // (we encode 128 bits, bcrypt salt uses 128 bits = 22 chars in its alphabet)
            char saltChars[23] = {};
            const unsigned char* p = raw;
            int idx = 0;
            // Process in 3-byte groups (bcrypt base64 packs 4 chars per 3 bytes)
            auto enc3 = [&](unsigned c1, unsigned c2, unsigned c3) {
                unsigned v = (c1 << 16) | (c2 << 8) | c3;
                saltChars[idx++] = b64[(v >> 18) & 0x3f];
                saltChars[idx++] = b64[(v >> 12) & 0x3f];
                saltChars[idx++] = b64[(v >> 6)  & 0x3f];
                saltChars[idx++] = b64[(v)        & 0x3f];
            };
            enc3(p[0],  p[1],  p[2]);
            enc3(p[3],  p[4],  p[5]);
            enc3(p[6],  p[7],  p[8]);
            enc3(p[9],  p[10], p[11]);
            enc3(p[12], p[13], p[14]);
            // Final byte pair (only 2 chars needed, but bcrypt uses 22 total)
            unsigned v = (p[15] << 16);
            saltChars[idx++] = b64[(v >> 18) & 0x3f];
            saltChars[idx++] = b64[(v >> 12) & 0x3f];
            // Total: 20 chars from 5 full groups + 2 from final partial = 22 — trim to 22
            saltChars[22] = '\0';

            char out[32];
            snprintf(out, sizeof(out), "$2b$%02d$%s", cost, saltChars);
            return std::string(out, out + 29); // "$2b$12$" (7) + 22 salt chars = 29
        }
    }

    /**
     * Hash a plaintext password with bcrypt.
     * Returns the full hash string (60 chars), suitable for storing in the DB.
     */
    inline std::string hash(const std::string& password, int cost = 12)
    {
        std::string salt = detail::generateSalt(cost);
        struct crypt_data cd;
        memset(&cd, 0, sizeof(cd));
        const char* result = crypt_r(password.c_str(), salt.c_str(), &cd);
        if (!result || result[0] != '$')
            throw std::runtime_error("bcrypt hash failed");
        return std::string(result);
    }

    /**
     * Verify a plaintext password against a stored bcrypt hash.
     * Uses a constant-time comparison to prevent timing attacks.
     */
    inline bool verify(const std::string& password, const std::string& stored)
    {
        if (stored.empty() || stored[0] != '$')
            return false;
        struct crypt_data cd;
        memset(&cd, 0, sizeof(cd));
        const char* result = crypt_r(password.c_str(), stored.c_str(), &cd);
        if (!result) return false;

        // Constant-time string comparison
        const char* a = result;
        const char* b = stored.c_str();
        size_t lenA = strlen(a);
        size_t lenB = stored.size();
        unsigned char diff = (unsigned char)(lenA ^ lenB);
        size_t n = lenA < lenB ? lenA : lenB;
        for (size_t i = 0; i < n; ++i)
            diff |= (unsigned char)(a[i] ^ b[i]);
        return diff == 0;
    }
} // namespace Bcrypt

#endif // BCRYPT_USE_CRYPT

// ─────────────────────────────────────────────────────────────────────────────
// Windows path — OpenSSL + Blowfish
// ─────────────────────────────────────────────────────────────────────────────

#if defined(BCRYPT_USE_OPENSSL)

#include <openssl/rand.h>
#include <openssl/evp.h>

// Forward declarations for the portable Blowfish/bcrypt core (bcrypt_impl.cpp)
// On Windows we compile bcrypt_impl.cpp alongside any translation unit that
// includes this header. The implementation is in extern/bcrypt/bcrypt_impl.cpp.
extern "C" {
    int bcrypt_hashpw(const char* key, const char* setting, char* output);
    char* bcrypt_gensalt(int log_rounds, uint8_t* input, char* output);
}

namespace Bcrypt
{
    namespace detail
    {
        inline std::string generateSalt(int cost = 12)
        {
            if (cost < 4 || cost > 31)
                throw std::invalid_argument("bcrypt cost must be 4-31");
            uint8_t random[16];
            if (RAND_bytes(random, sizeof(random)) != 1)
                throw std::runtime_error("RAND_bytes failed");
            char saltOut[64] = {};
            bcrypt_gensalt(cost, random, saltOut);
            return std::string(saltOut);
        }
    }

    inline std::string hash(const std::string& password, int cost = 12)
    {
        std::string salt = detail::generateSalt(cost);
        char output[64] = {};
        if (bcrypt_hashpw(password.c_str(), salt.c_str(), output) != 0)
            throw std::runtime_error("bcrypt_hashpw failed");
        return std::string(output);
    }

    inline bool verify(const std::string& password, const std::string& stored)
    {
        if (stored.empty() || stored[0] != '$') return false;
        char output[64] = {};
        if (bcrypt_hashpw(password.c_str(), stored.c_str(), output) != 0)
            return false;
        // Constant-time compare
        const char* a = output;
        const char* b = stored.c_str();
        size_t lenA = strlen(a);
        size_t lenB = stored.size();
        unsigned char diff = (unsigned char)(lenA ^ lenB);
        size_t n = lenA < lenB ? lenA : lenB;
        for (size_t i = 0; i < n; ++i)
            diff |= (unsigned char)(a[i] ^ b[i]);
        return diff == 0;
    }
} // namespace Bcrypt

#endif // BCRYPT_USE_OPENSSL
