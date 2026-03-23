#include "Identity.hpp"

#include <components/debug/debuglog.hpp>

// GNS C++ crypto API — gives us CECSigningPrivateKey / PublicKey and
// CCrypto::GenerateSigningKeyPair without reaching into donna internals.
// Include paths supplied via CMakeLists:
//   extern/GameNetworkingSockets/src/common  (crypto_25519.h, keypair.h)
//   extern/GameNetworkingSockets/src/public  (tier0/*, minbase/*)
#include <crypto_25519.h>

#include <cstring>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Minimal base64 (RFC 4648, no line breaks)
// ---------------------------------------------------------------------------
namespace
{
    static const char kB64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string b64Encode(const unsigned char* in, size_t len)
    {
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        for (size_t i = 0; i < len; i += 3)
        {
            unsigned int v = static_cast<unsigned int>(in[i]) << 16;
            if (i + 1 < len) v |= static_cast<unsigned int>(in[i+1]) << 8;
            if (i + 2 < len) v |= static_cast<unsigned int>(in[i+2]);
            out += kB64Chars[(v >> 18) & 63];
            out += kB64Chars[(v >> 12) & 63];
            out += (i + 1 < len) ? kB64Chars[(v >> 6) & 63] : '=';
            out += (i + 2 < len) ? kB64Chars[v & 63]        : '=';
        }
        return out;
    }

    static int b64Val(char c)
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    // Decode up to `maxOut` bytes. Returns number of bytes written, -1 on error.
    int b64Decode(const std::string& in, unsigned char* out, size_t maxOut)
    {
        size_t n = 0;
        for (size_t i = 0; i + 3 < in.size(); i += 4)
        {
            int a = b64Val(in[i]),   b = b64Val(in[i+1]),
                c = b64Val(in[i+2]), d = b64Val(in[i+3]);
            if (a < 0 || b < 0) return -1;
            if (n < maxOut) out[n++] = static_cast<unsigned char>((a << 2) | (b >> 4));
            if (in[i+2] != '=' && c >= 0 && n < maxOut)
                out[n++] = static_cast<unsigned char>(((b & 15) << 4) | (c >> 2));
            if (in[i+3] != '=' && d >= 0 && n < maxOut)
                out[n++] = static_cast<unsigned char>(((c & 3) << 6) | d);
        }
        return static_cast<int>(n);
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Identity implementation
// ---------------------------------------------------------------------------
namespace mwmp
{

std::filesystem::path Identity::sKeysDir;

void Identity::setKeysDir(const std::filesystem::path& dir)
{
    sKeysDir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        Log(Debug::Warning) << "[Identity] Could not create keys dir: " << ec.message();
}

std::filesystem::path Identity::keyPath(const std::string& host, uint16_t port)
{
    // Sanitise host: replace characters that are unsafe in filenames.
    std::string safe = host;
    for (char& c : safe)
        if (c == ':' || c == '/' || c == '\\' || c == ' ')
            c = '_';
    return sKeysDir / (safe + "_" + std::to_string(port) + ".key");
}

bool Identity::hasKeypair(const std::string& host, uint16_t port)
{
    return std::filesystem::exists(keyPath(host, port));
}

bool Identity::generateKeypair(const std::string& host, uint16_t port,
                                const std::string& username)
{
    // CCrypto::GenerateSigningKeyPair calls the correct backend
    // (donna / BCrypt / OpenSSL / libsodium) and uses the platform CSPRNG
    // internally — no manual random generation needed here.
    CECSigningPrivateKey privKey;
    CECSigningPublicKey  pubKey;
    CCrypto::GenerateSigningKeyPair(&pubKey, &privKey);

    if (!privKey.IsValid() || !pubKey.IsValid())
    {
        Log(Debug::Error) << "[Identity] generateKeypair: GNS key generation failed";
        return false;
    }

    // Extract raw 32-byte key material
    unsigned char sk[32], pk[32];
    privKey.GetRawData(sk);
    pubKey.GetRawData(pk);

    // File format:  Line 1 = base64(sk), Line 2 = base64(pk), Line 3 = username
    const auto path = keyPath(host, port);
    std::ofstream f(path);
    if (!f)
    {
        Log(Debug::Error) << "[Identity] Cannot write key file: " << path;
        std::memset(sk, 0, 32);
        privKey.Wipe();
        return false;
    }
    f << b64Encode(sk, 32) << "\n" << b64Encode(pk, 32) << "\n" << username << "\n";
    Log(Debug::Info) << "[Identity] Keypair generated for " << host << ":" << port
                     << " user=" << username;
    std::memset(sk, 0, 32);
    privKey.Wipe();
    return true;
}

std::string Identity::getStoredUsername(const std::string& host, uint16_t port)
{
    const auto path = keyPath(host, port);
    std::ifstream f(path);
    if (!f) return {};
    std::string skLine, pkLine, username;
    std::getline(f, skLine);
    std::getline(f, pkLine);
    std::getline(f, username);
    return username;
}

std::string Identity::getPublicKeyBase64(const std::string& host, uint16_t port)
{
    const auto path = keyPath(host, port);
    std::ifstream f(path);
    if (!f) return {};
    std::string skLine, pkLine;
    std::getline(f, skLine);
    std::getline(f, pkLine);
    return pkLine;
}

bool Identity::sign(const std::string& host, uint16_t port,
                    const uint8_t challenge[32],
                    uint8_t       signatureOut[64])
{
    const auto path = keyPath(host, port);
    std::ifstream f(path);
    if (!f)
    {
        Log(Debug::Error) << "[Identity] sign: key file not found: " << path.string();
        return false;
    }

    std::string skLine, pkLine;
    std::getline(f, skLine);
    std::getline(f, pkLine);

    unsigned char sk[32];
    if (b64Decode(skLine, sk, 32) != 32)
    {
        Log(Debug::Error) << "[Identity] Corrupt key file for " << host << ":" << port;
        std::memset(sk, 0, 32);
        return false;
    }

    // Load into a GNS key object.  SetRawDataAndWipeInput zeroes our sk[] buffer
    // as a side-effect — exactly what we want (no separate memset needed).
    CECSigningPrivateKey privKey;
    if (!privKey.SetRawDataAndWipeInput(sk, 32) || !privKey.IsValid())
    {
        Log(Debug::Error) << "[Identity] Failed to load private key for "
                          << host << ":" << port;
        privKey.Wipe();
        return false;
    }

    CryptoSignature_t sig;
    privKey.GenerateSignature(challenge, 32, &sig);
    std::memcpy(signatureOut, sig, 64);

    privKey.Wipe();
    return true;
}

bool Identity::removeKeypair(const std::string& host, uint16_t port)
{
    const auto path = keyPath(host, port);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec)
    {
        Log(Debug::Warning) << "[Identity] removeKeypair: " << ec.message();
        return false;
    }
    Log(Debug::Info) << "[Identity] Keypair removed for " << host << ":" << port;
    return true;
}

} // namespace mwmp
