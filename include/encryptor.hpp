#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>   // C++20
#include <stdexcept>

namespace vcp {

// Thrown when AES-GCM authentication tag verification fails during decryption.
struct AuthenticationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown on I/O or OpenSSL errors.
struct EncryptorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Encrypt src → dst using AES-256-GCM.
//
// File format written to dst:
//   [0..3]   Magic  0x56435045 ("VCPE")
//   [4]      Version 0x01
//   [5]      Reserved 0x00
//   [6..7]   Header length = 30 (big-endian uint16)
//   [8..19]  IV / Nonce (12 bytes, random)
//   [20..27] Plaintext length (big-endian uint64)
//   [28..29] Reserved
//   [30..N+30) Ciphertext
//   [N+30..N+46) GCM auth tag (16 bytes)
//
// key must be exactly 32 bytes.
// Throws EncryptorError on failure.
void encrypt_file(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  std::span<const uint8_t, 32> key);

// Decrypt src (written by encrypt_file) → dst.
// Throws AuthenticationError if the GCM tag does not match.
// Throws EncryptorError on other failures.
// On any failure the partial output file is removed before throwing.
void decrypt_file(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  std::span<const uint8_t, 32> key);

} // namespace vcp
