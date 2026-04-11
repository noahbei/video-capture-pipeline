// vcpdecrypt — decrypt .vcpenc files produced by vcpcapture
//
// Usage:
//   vcpdecrypt --key <keyfile> --in <file.vcpenc> [--out <file.mp4>]
//   vcpdecrypt --key <keyfile> --dir <directory>  [--out-dir <outdir>]
//
// Exit codes:
//   0  success
//   1  key error (file not found, wrong size)
//   2  authentication failure (GCM tag mismatch — file may be tampered)
//   3  I/O or other error

#include "config.hpp"
#include "encryptor.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --key <keyfile> --in <file.vcpenc> [--out <file>]\n"
        "  %s --key <keyfile> --dir <directory>  [--out-dir <outdir>]\n"
        "\n"
        "Options:\n"
        "  --key <keyfile>   32-byte raw binary AES-256 key file (required)\n"
        "  --in  <file>      Single encrypted input file\n"
        "  --out <file>      Output file (default: strip .vcpenc extension)\n"
        "  --dir <directory> Decrypt all *.vcpenc files in this directory\n"
        "  --out-dir <dir>   Output directory for batch mode (default: same as --dir)\n"
        "\n"
        "Exit codes:\n"
        "  0  success\n"
        "  1  key error\n"
        "  2  authentication failure (file tampered or wrong key)\n"
        "  3  I/O or other error\n",
        argv0, argv0);
}

// Strip ".vcpenc" suffix. If path doesn't end in ".vcpenc", append ".dec".
static fs::path output_path_for(const fs::path& input, const fs::path& out_dir) {
    fs::path base = out_dir.empty() ? input.parent_path() : out_dir;
    std::string fname = input.filename().string();
    if (fname.size() > 7 && fname.substr(fname.size() - 7) == ".vcpenc") {
        fname = fname.substr(0, fname.size() - 7);
    } else {
        fname += ".dec";
    }
    return base / fname;
}

int main(int argc, char* argv[]) {
    std::string key_path;
    std::string in_path;
    std::string out_path;
    std::string dir_path;
    std::string out_dir_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--key" || a == "-k") && i + 1 < argc)         key_path     = argv[++i];
        else if ((a == "--in"  || a == "-i") && i + 1 < argc)    in_path      = argv[++i];
        else if ((a == "--out" || a == "-o") && i + 1 < argc)    out_path     = argv[++i];
        else if ((a == "--dir" || a == "-d") && i + 1 < argc)    dir_path     = argv[++i];
        else if (a == "--out-dir" && i + 1 < argc)                out_dir_path = argv[++i];
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
    }

    if (key_path.empty()) {
        std::fprintf(stderr, "Error: --key is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (in_path.empty() && dir_path.empty()) {
        std::fprintf(stderr, "Error: either --in or --dir is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Load key
    uint8_t key[32]{};
    try {
        vcp::load_key_file(key_path, key);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Key error: %s\n", e.what());
        return 1;
    }

    const std::span<const uint8_t, 32> key_span(key, 32);

    // Collect files to decrypt
    struct Job { fs::path src; fs::path dst; };
    std::vector<Job> jobs;

    if (!in_path.empty()) {
        fs::path src(in_path);
        fs::path dst = out_path.empty() ? output_path_for(src, {}) : fs::path(out_path);
        jobs.push_back({src, dst});
    } else {
        fs::path dir(dir_path);
        fs::path out_dir = out_dir_path.empty() ? dir : fs::path(out_dir_path);

        if (!fs::is_directory(dir)) {
            std::fprintf(stderr, "Error: --dir is not a directory: %s\n", dir_path.c_str());
            return 3;
        }
        if (!out_dir_path.empty()) {
            try {
                fs::create_directories(out_dir);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Error creating output directory: %s\n", e.what());
                return 3;
            }
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".vcpenc") {
                jobs.push_back({entry.path(), output_path_for(entry.path(), out_dir)});
            }
        }
        if (jobs.empty()) {
            std::fprintf(stderr, "No .vcpenc files found in %s\n", dir_path.c_str());
            return 0;
        }
    }

    // Process jobs
    int exit_code = 0;
    for (const auto& job : jobs) {
        std::fprintf(stdout, "Decrypting: %s → %s\n",
                     job.src.c_str(), job.dst.c_str());
        try {
            vcp::decrypt_file(job.src, job.dst, key_span);
            std::fprintf(stdout, "  OK\n");
        } catch (const vcp::AuthenticationError& e) {
            std::fprintf(stderr, "  AUTHENTICATION FAILED: %s\n"
                                 "  File may be corrupted or tampered with.\n", e.what());
            exit_code = 2;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  ERROR: %s\n", e.what());
            if (exit_code == 0) exit_code = 3;
        }
    }

    return exit_code;
}
