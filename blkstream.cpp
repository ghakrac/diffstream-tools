// blkstream.cpp - Raw block device to DIFFSTR1 sparse/discard stream tool
// Compile: g++ -O3 -std=c++17 -Wall -Wextra -o blkstream blkstream.cpp
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/fs.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint8_t OP_DATA = 1, OP_HOLE = 2, OP_TRUNCATE = 3, OP_EOF = 4;
constexpr std::array<char, 8> STREAM_MAGIC = {'D', 'I', 'F', 'F', 'S', 'T', 'R', '1'};
constexpr uint64_t READ_CHUNK = 4ULL * 1024 * 1024, SCAN_BLOCK = 64ULL * 1024;
#pragma pack(push, 1)
struct PacketHeader { uint8_t op; uint64_t offset; uint64_t length; };
#pragma pack(pop)
uint64_t parse_size_arg(const std::string& s) {
    if (s.empty()) throw std::runtime_error("empty size");
    char suf = s.back();
    uint64_t mult = 1;
    std::string num = s;
    if (suf == 'K' || suf == 'k') { mult = 1024ULL; num.pop_back(); }
    else if (suf == 'M' || suf == 'm') { mult = 1024ULL * 1024; num.pop_back(); }
    else if (suf == 'G' || suf == 'g') { mult = 1024ULL * 1024 * 1024; num.pop_back(); }
    else if (suf == 'T' || suf == 't') { mult = 1024ULL * 1024 * 1024 * 1024; num.pop_back(); }
    return std::stoull(num) * mult;
}

[[noreturn]] void throw_errno(const std::string& m) { throw std::runtime_error(m + ": " + std::strerror(errno)); }
bool is_all_zero(const uint8_t* b, size_t len) {
    size_t i = 0;
    for (; i + 8 <= len; i += 8) { uint64_t v; std::memcpy(&v, b + i, 8); if (v) return false; }
    for (; i < len; ++i) if (b[i]) return false;
    return true;
}
void write_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::write(fd, p + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; throw_errno("write"); }
        if (n == 0) throw std::runtime_error("unexpected EOF on write");
        off += static_cast<size_t>(n);
    }
}
void write_pkt(int fd, uint8_t op, uint64_t off, uint64_t len) { PacketHeader h{op, htole64(off), htole64(len)}; write_all(fd, &h, sizeof(h)); }
void pread_all(int fd, void* buf, size_t len, off_t pos) {
    auto* p = static_cast<uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::pread(fd, p + off, len - off, pos + static_cast<off_t>(off));
        if (n < 0) { if (errno == EINTR) continue; throw_errno("pread"); }
        if (n == 0) throw std::runtime_error("unexpected EOF on pread");
        off += static_cast<size_t>(n);
    }
}
uint64_t get_size(int fd) {
    struct stat st{}; if (::fstat(fd, &st) < 0) throw_errno("fstat");
    if (S_ISBLK(st.st_mode)) { uint64_t sz = 0; if (::ioctl(fd, BLKGETSIZE64, &sz) < 0) throw_errno("BLKGETSIZE64"); return sz; }
    return static_cast<uint64_t>(st.st_size);
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        bool nocache = false, verbose = false; uint64_t trunc_sz = 0;
        std::string source;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--nocache") nocache = true;
            else if (a == "-v" || a == "--verbose") verbose = true;
            else if (a == "--truncate" && i + 1 < argc) trunc_sz = parse_size_arg(argv[++i]);
            else if ((a == "--source" || a == "--change") && i + 1 < argc) source = argv[++i];
            else { std::cerr << "Usage: " << argv[0] << " --source <dev_or_file> [--nocache] [-v]\n"; return 1; }
        }
        if (source.empty()) { std::cerr << "Usage: " << argv[0] << " --source <dev_or_file> [--nocache] [-v]\n"; return 1; }
        int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw_errno("open source");
        try {
            uint64_t total_size = get_size(fd);
            if (trunc_sz > 0 && total_size > trunc_sz) total_size = trunc_sz;
            if (nocache) ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
            write_all(STDOUT_FILENO, STREAM_MAGIC.data(), STREAM_MAGIC.size());
            write_pkt(STDOUT_FILENO, OP_TRUNCATE, 0, total_size);
            auto t0 = std::chrono::steady_clock::now();
            std::vector<uint8_t> buf(READ_CHUNK);
            uint64_t sent = 0, punched = 0, hole_off = 0, hole_len = 0;
            auto flush_hole = [&]() { if (hole_len > 0) { write_pkt(STDOUT_FILENO, OP_HOLE, hole_off, hole_len); punched += hole_len; hole_len = 0; } };
            for (uint64_t pos = 0; pos < total_size;) {
                uint64_t chunk = std::min(total_size - pos, READ_CHUNK);
                pread_all(fd, buf.data(), chunk, static_cast<off_t>(pos));
                for (uint64_t sub = 0; sub < chunk;) {
                    uint64_t step = std::min(chunk - sub, SCAN_BLOCK);
                    if (is_all_zero(buf.data() + sub, step)) {
                        if (hole_len == 0) hole_off = pos + sub;
                        hole_len += step; sub += step;
                    } else {
                        flush_hole();
                        uint64_t d0 = sub; sub += step;
                        while (sub < chunk && !is_all_zero(buf.data() + sub, std::min(chunk - sub, SCAN_BLOCK))) sub += std::min(chunk - sub, SCAN_BLOCK);
                        write_pkt(STDOUT_FILENO, OP_DATA, pos + d0, sub - d0);
                        write_all(STDOUT_FILENO, buf.data() + d0, sub - d0);
                        sent += (sub - d0);
                    }
                }
                if (nocache) ::posix_fadvise(fd, static_cast<off_t>(pos), static_cast<off_t>(chunk), POSIX_FADV_DONTNEED);
                pos += chunk;
            }
            flush_hole();
            write_pkt(STDOUT_FILENO, OP_EOF, 0, 0);
            if (nocache) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            if (verbose) {
                double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::cerr << std::fixed << std::setprecision(2) << "Done in " << sec << "s ("
                          << (sec > 0.0001 ? (sent / 1048576.0) / sec : 0.0) << " MiB/s): "
                          << sent / 1048576.0 << " MiB written, " << punched / 1048576.0 << " MiB discarded.\n";
            }
        } catch (...) { ::close(fd); throw; }
        ::close(fd);
        return 0;
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }
}
