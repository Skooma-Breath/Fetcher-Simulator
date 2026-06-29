#pragma once
///
/// Identity — Ed25519 keypair management for OpenMW-MP clients.
///
/// One keypair is stored per (host, port) combination in a directory
/// chosen at startup (default: <exe_dir>/mp-keys/).
///
/// Key format on disk: raw 32-byte private key, base64-encoded, one line.
/// Filename: <host>_<port>.key  (colons in host replaced with underscores)
///
/// Uses the ed25519-donna implementation vendored inside GameNetworkingSockets.
///

#include <cstdint>
#include <filesystem>
#include <string>

namespace mwmp
{
    class Identity
    {
    public:
        /// Set the directory where keypair files are stored.
        /// Call once at startup before any other Identity method.
        static void setKeysDir(const std::filesystem::path& dir);
        static bool hasKeysDir();
        static bool importLegacyKeypair(const std::string& host, uint16_t port,
            const std::filesystem::path& legacyDir, const std::string& expectedUsername);

        /// Returns true if a keypair file already exists for this server.
        static bool hasKeypair(const std::string& host, uint16_t port);

        /// Generate a new keypair for this server and save it to disk.
        /// Also stores the username so keypair-only login doesn't need a login form.
        /// Returns false if the file cannot be written.
        static bool generateKeypair(const std::string& host, uint16_t port,
                                     const std::string& username);

        /// Returns the username stored in the key file, or empty if not found.
        static std::string getStoredUsername(const std::string& host, uint16_t port);

        /// Load the stored public key for this server as a base64 string.
        /// Returns empty string if no keypair exists.
        static std::string getPublicKeyBase64(const std::string& host, uint16_t port);

        /// Sign 32 bytes of challenge data with the stored private key.
        /// Output is placed in signature[64].
        /// Returns false if no keypair exists for this server.
        static bool sign(const std::string& host, uint16_t port,
                         const uint8_t challenge[32],
                         uint8_t       signatureOut[64]);

        /// Delete the keypair file for this server (unlink).
        static bool removeKeypair(const std::string& host, uint16_t port);

    private:
        static std::filesystem::path keyPath(const std::string& host, uint16_t port);

        static std::filesystem::path sKeysDir;
    };

} // namespace mwmp
