#include "encryptor.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace vcp {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// File format constants
// ---------------------------------------------------------------------------
static constexpr uint32_t MAGIC         = 0x56435045u; // "VCPE"
static constexpr uint8_t  VERSION       = 0x01;
static constexpr uint16_t HEADER_LENGTH = 30;
static constexpr int      IV_SIZE       = 12;
static constexpr int      TAG_SIZE      = 16;
static constexpr int      CHUNK_SIZE    = 4 * 1024 * 1024; // 4 MB streaming chunks

// ---------------------------------------------------------------------------
// Portable big-endian helpers
// ---------------------------------------------------------------------------
static void write_be32(uint8_t* buf, uint32_t v) {
    buf[0] = (v >> 24) & 0xFF;
    buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >>  8) & 0xFF;
    buf[3] = (v >>  0) & 0xFF;
}
static void write_be16(uint8_t* buf, uint16_t v) {
    buf[0] = (v >> 8) & 0xFF;
    buf[1] = (v >> 0) & 0xFF;
}
static void write_be64(uint8_t* buf, uint64_t v) {
    for (int i = 7; i >= 0; --i) { buf[i] = v & 0xFF; v >>= 8; }
}
static uint32_t read_be32(const uint8_t* buf) {
    return (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
           (uint32_t(buf[2]) <<  8) |  uint32_t(buf[3]);
}
static uint16_t read_be16(const uint8_t* buf) {
    return (uint16_t(buf[0]) << 8) | uint16_t(buf[1]);
}
static uint64_t read_be64(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) { v = (v << 8) | buf[i]; }
    return v;
}

// ---------------------------------------------------------------------------
// RAII wrapper for EVP_CIPHER_CTX
// ---------------------------------------------------------------------------
struct CipherCtx {
    EVP_CIPHER_CTX* ctx = nullptr;
    CipherCtx()  { ctx = EVP_CIPHER_CTX_new(); }
    ~CipherCtx() { if (ctx) EVP_CIPHER_CTX_free(ctx); }
    CipherCtx(const CipherCtx&) = delete;
    CipherCtx& operator=(const CipherCtx&) = delete;
};

// ---------------------------------------------------------------------------
// encrypt_file
// ---------------------------------------------------------------------------
void encrypt_file(const fs::path& src,
                  const fs::path& dst,
                  std::span<const uint8_t, 32> key) {
    // Open source
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open())
        throw EncryptorError("encrypt_file: cannot open source: " + src.string());

    // Get plaintext size
    in.seekg(0, std::ios::end);
    const uint64_t plaintext_len = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    // Generate random IV
    uint8_t iv[IV_SIZE];
    if (RAND_bytes(iv, IV_SIZE) != 1)
        throw EncryptorError("encrypt_file: RAND_bytes failed");

    // Build header (30 bytes)
    uint8_t header[HEADER_LENGTH]{};
    write_be32(header + 0,  MAGIC);
    header[4] = VERSION;
    header[5] = 0x00;
    write_be16(header + 6,  HEADER_LENGTH);
    std::memcpy(header + 8, iv, IV_SIZE);   // bytes 8..19
    write_be64(header + 20, plaintext_len); // bytes 20..27
    // bytes 28..29 reserved = 0

    // Open destination
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        throw EncryptorError("encrypt_file: cannot open destination: " + dst.string());

    out.write(reinterpret_cast<const char*>(header), HEADER_LENGTH);

    // Initialize AES-256-GCM encryption
    CipherCtx cctx;
    if (!cctx.ctx) throw EncryptorError("encrypt_file: EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(cctx.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw EncryptorError("encrypt_file: EVP_EncryptInit_ex (alg) failed");
    if (EVP_CIPHER_CTX_ctrl(cctx.ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        throw EncryptorError("encrypt_file: set IV length failed");
    if (EVP_EncryptInit_ex(cctx.ctx, nullptr, nullptr, key.data(), iv) != 1)
        throw EncryptorError("encrypt_file: EVP_EncryptInit_ex (key/iv) failed");

    // Encrypt in chunks
    std::vector<uint8_t> plain_buf(CHUNK_SIZE);
    std::vector<uint8_t> cipher_buf(CHUNK_SIZE + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;

    while (in) {
        in.read(reinterpret_cast<char*>(plain_buf.data()), CHUNK_SIZE);
        const auto bytes_read = static_cast<int>(in.gcount());
        if (bytes_read == 0) break;

        if (EVP_EncryptUpdate(cctx.ctx, cipher_buf.data(), &out_len,
                              plain_buf.data(), bytes_read) != 1)
            throw EncryptorError("encrypt_file: EVP_EncryptUpdate failed");

        out.write(reinterpret_cast<const char*>(cipher_buf.data()), out_len);
    }

    // Finalise (GCM produces 0 output bytes here but updates the tag)
    if (EVP_EncryptFinal_ex(cctx.ctx, cipher_buf.data(), &out_len) != 1)
        throw EncryptorError("encrypt_file: EVP_EncryptFinal_ex failed");
    if (out_len > 0)
        out.write(reinterpret_cast<const char*>(cipher_buf.data()), out_len);

    // Get and append the 16-byte GCM tag
    uint8_t tag[TAG_SIZE];
    if (EVP_CIPHER_CTX_ctrl(cctx.ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) != 1)
        throw EncryptorError("encrypt_file: EVP_CTRL_GCM_GET_TAG failed");
    out.write(reinterpret_cast<const char*>(tag), TAG_SIZE);

    out.flush();
    // fsync for durability
    {
        std::ofstream::pos_type pos = out.tellp();
        (void)pos;
    }
    if (!out.good())
        throw EncryptorError("encrypt_file: write error on destination: " + dst.string());
}

// ---------------------------------------------------------------------------
// decrypt_file
// ---------------------------------------------------------------------------
void decrypt_file(const fs::path& src,
                  const fs::path& dst,
                  std::span<const uint8_t, 32> key) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open())
        throw EncryptorError("decrypt_file: cannot open source: " + src.string());

    // Read and validate header
    uint8_t header[HEADER_LENGTH]{};
    in.read(reinterpret_cast<char*>(header), HEADER_LENGTH);
    if (in.gcount() != HEADER_LENGTH)
        throw EncryptorError("decrypt_file: file too short to contain header");

    const uint32_t magic = read_be32(header + 0);
    if (magic != MAGIC)
        throw EncryptorError("decrypt_file: bad magic bytes — not a .vcpenc file");

    const uint8_t version = header[4];
    if (version != VERSION)
        throw EncryptorError("decrypt_file: unsupported version " + std::to_string(version));

    const uint16_t hdr_len = read_be16(header + 6);
    if (hdr_len != HEADER_LENGTH)
        throw EncryptorError("decrypt_file: unexpected header length");

    uint8_t iv[IV_SIZE];
    std::memcpy(iv, header + 8, IV_SIZE);

    const uint64_t plaintext_len = read_be64(header + 20);

    // Get total file size to locate the tag at the end
    in.seekg(0, std::ios::end);
    const auto total_size = static_cast<uint64_t>(in.tellg());
    if (total_size < HEADER_LENGTH + TAG_SIZE)
        throw EncryptorError("decrypt_file: file too small to contain ciphertext + tag");

    const uint64_t cipher_len = total_size - HEADER_LENGTH - TAG_SIZE;

    // Read the 16-byte tag from the end of the file
    in.seekg(-static_cast<std::streamoff>(TAG_SIZE), std::ios::end);
    uint8_t tag[TAG_SIZE];
    in.read(reinterpret_cast<char*>(tag), TAG_SIZE);
    if (in.gcount() != TAG_SIZE)
        throw EncryptorError("decrypt_file: failed to read GCM tag");

    // Seek back to start of ciphertext
    in.seekg(HEADER_LENGTH, std::ios::beg);

    // Open output file
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        throw EncryptorError("decrypt_file: cannot open destination: " + dst.string());

    // Initialize AES-256-GCM decryption
    CipherCtx cctx;
    if (!cctx.ctx) throw EncryptorError("decrypt_file: EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(cctx.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw EncryptorError("decrypt_file: EVP_DecryptInit_ex (alg) failed");
    if (EVP_CIPHER_CTX_ctrl(cctx.ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, nullptr) != 1)
        throw EncryptorError("decrypt_file: set IV length failed");
    if (EVP_DecryptInit_ex(cctx.ctx, nullptr, nullptr, key.data(), iv) != 1)
        throw EncryptorError("decrypt_file: EVP_DecryptInit_ex (key/iv) failed");

    // Decrypt in chunks (stop before the tag)
    std::vector<uint8_t> cipher_buf(CHUNK_SIZE);
    std::vector<uint8_t> plain_buf(CHUNK_SIZE + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    uint64_t remaining = cipher_len;

    while (remaining > 0) {
        const auto to_read = static_cast<int>(std::min<uint64_t>(remaining, CHUNK_SIZE));
        in.read(reinterpret_cast<char*>(cipher_buf.data()), to_read);
        const auto bytes_read = static_cast<int>(in.gcount());
        if (bytes_read == 0) break;

        if (EVP_DecryptUpdate(cctx.ctx, plain_buf.data(), &out_len,
                              cipher_buf.data(), bytes_read) != 1) {
            fs::remove(dst);
            throw EncryptorError("decrypt_file: EVP_DecryptUpdate failed");
        }
        out.write(reinterpret_cast<const char*>(plain_buf.data()), out_len);
        remaining -= static_cast<uint64_t>(bytes_read);
    }

    // Set expected tag for verification
    if (EVP_CIPHER_CTX_ctrl(cctx.ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                             const_cast<uint8_t*>(tag)) != 1) {
        fs::remove(dst);
        throw EncryptorError("decrypt_file: EVP_CTRL_GCM_SET_TAG failed");
    }

    // EVP_DecryptFinal_ex returns <= 0 if the tag does not match
    const int ret = EVP_DecryptFinal_ex(cctx.ctx, plain_buf.data(), &out_len);
    if (ret <= 0) {
        fs::remove(dst);
        throw AuthenticationError(
            "decrypt_file: authentication failed — file may be corrupted or tampered");
    }
    if (out_len > 0)
        out.write(reinterpret_cast<const char*>(plain_buf.data()), out_len);

    out.flush();
    if (!out.good()) {
        fs::remove(dst);
        throw EncryptorError("decrypt_file: write error on destination: " + dst.string());
    }

    // Sanity check output size
    const auto written = static_cast<uint64_t>(out.tellp());
    if (written != plaintext_len) {
        fs::remove(dst);
        throw EncryptorError("decrypt_file: output size mismatch (expected " +
                             std::to_string(plaintext_len) + ", got " +
                             std::to_string(written) + ")");
    }
}

} // namespace vcp
