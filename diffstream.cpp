// diffstream.cpp - Minimal reflink extent diff & apply tool
// Compile: g++ -O3 -std=c++17 -Wall -Wextra -o diffstream diffstream.cpp

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint8_t OP_DATA     = 1;
constexpr uint8_t OP_HOLE     = 2;
constexpr uint8_t OP_TRUNCATE = 3;
constexpr uint8_t OP_EOF      = 4;

constexpr char STREAM_MAGIC[8] = {'D', 'I', 'F', 'F', 'S', 'T', 'R', '1'};
constexpr uint64_t MAX_CHUNK = 4ULL * 1024 * 1024; // 4 MiB

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  op;
    uint64_t offset;
    uint64_t length;
};
#pragma pack(pop)

struct Extent {
    uint64_t logical, physical, length;
    uint32_t flags;
};

struct DiffOp {
    uint8_t  op;
    uint64_t offset, length;
};

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

[[noreturn]] void throw_errno(const std::string& msg) {
    throw std::runtime_error(msg + ": " + std::strerror(errno));
}

void drop_cache(int fd, off_t off, off_t len) {
    if (len > 0) ::posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);
}

bool is_all_zero(const uint8_t* buf, size_t len) {
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t v;
        std::memcpy(&v, buf + i, sizeof(v));
        if (v != 0) return false;
    }
    for (; i < len; ++i) {
        if (buf[i] != 0) return false;
    }
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

void read_all(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::read(fd, p + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; throw_errno("read"); }
        if (n == 0) throw std::runtime_error("unexpected EOF on read");
        off += static_cast<size_t>(n);
    }
}

void write_pkt(int fd, uint8_t op, uint64_t offset, uint64_t length) {
    PacketHeader h{op, htole64(offset), htole64(length)};
    write_all(fd, &h, sizeof(h));
}

PacketHeader read_pkt(int fd) {
    PacketHeader h{};
    read_all(fd, &h, sizeof(h));
    h.offset = le64toh(h.offset);
    h.length = le64toh(h.length);
    return h;
}

void pread_all(int fd, void* buf, size_t len, off_t pos) {
    auto* p = static_cast<uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::pread(fd, p + off, len - off, pos + static_cast<off_t>(off));
        if (n < 0) { if (errno == EINTR) continue; throw_errno("pread"); }
        if (n == 0) throw std::runtime_error("unexpected EOF on pread");
        off += static_cast<size_t>(n);
    }
}
void pwrite_all(int fd, const void* buf, size_t len, off_t pos) {
    const auto* p = static_cast<const uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::pwrite(fd, p + off, len - off, pos + static_cast<off_t>(off));
        if (n < 0) { if (errno == EINTR) continue; throw_errno("pwrite"); }
        if (n == 0) throw std::runtime_error("short pwrite");
        off += static_cast<size_t>(n);
    }
}

std::vector<Extent> get_extents(int fd) {
    std::vector<Extent> out;
    constexpr uint32_t BATCH = 512;
    std::vector<uint8_t> buf(sizeof(fiemap) + BATCH * sizeof(fiemap_extent));
    auto* fm = reinterpret_cast<fiemap*>(buf.data());

    for (uint64_t start = 0;;) {
        std::memset(buf.data(), 0, buf.size());
        fm->fm_start = start;
        fm->fm_length = ~0ULL - start;
        fm->fm_flags = FIEMAP_FLAG_SYNC;
        fm->fm_extent_count = BATCH;

        if (::ioctl(fd, FS_IOC_FIEMAP, fm) < 0) throw_errno("FS_IOC_FIEMAP");
        if (fm->fm_mapped_extents == 0) break;

        bool last = false;
        for (uint32_t i = 0; i < fm->fm_mapped_extents; ++i) {
            const auto& e = fm->fm_extents[i];
            if (e.fe_flags & FIEMAP_EXTENT_LAST) last = true;
            if (!(e.fe_flags & FIEMAP_EXTENT_UNWRITTEN)) {
                out.push_back({e.fe_logical, e.fe_physical, e.fe_length, e.fe_flags});
            }
            start = e.fe_logical + e.fe_length;
        }
        if (last) break;
    }
    return out;
}

std::vector<DiffOp> compute_diff(const std::vector<Extent>& base,
                                 const std::vector<Extent>& chg,
                                 uint64_t chg_size) {
    std::vector<uint64_t> pts = {0, chg_size};
    for (const auto& e : base) {
        pts.push_back(std::min(e.logical, chg_size));
        pts.push_back(std::min(e.logical + e.length, chg_size));
    }
    for (const auto& e : chg) {
        pts.push_back(std::min(e.logical, chg_size));
        pts.push_back(std::min(e.logical + e.length, chg_size));
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

    std::vector<DiffOp> ops;
    size_t bi = 0, ci = 0;
    constexpr uint32_t OPAQUE = FIEMAP_EXTENT_UNKNOWN |
                                FIEMAP_EXTENT_DELALLOC |
                                FIEMAP_EXTENT_DATA_INLINE |
                                FIEMAP_EXTENT_DATA_ENCRYPTED;

    constexpr uint32_t ENCODED = FIEMAP_EXTENT_ENCODED;


    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        uint64_t s = pts[i], len = pts[i + 1] - s;
        if (len == 0) continue;

        while (bi < base.size() && base[bi].logical + base[bi].length <= s) ++bi;
        while (ci < chg.size()  && chg[ci].logical  + chg[ci].length  <= s) ++ci;

        const Extent* b = (bi < base.size() && base[bi].logical <= s) ? &base[bi] : nullptr;
        const Extent* c = (ci < chg.size()  && chg[ci].logical  <= s) ? &chg[ci]  : nullptr;

        if (!c) {
            if (b || base.empty()) ops.push_back({OP_HOLE, s, len});
            continue;
        }
        if (b && !(c->flags & OPAQUE) && !(b->flags & OPAQUE)) {
            if ((c->flags & ENCODED) || (b->flags & ENCODED)) {
                if (c->logical == b->logical &&
                    c->physical == b->physical &&
                    c->length == b->length &&
                    c->flags == b->flags) {
                    continue;
                }
            } else if ((c->physical + (s - c->logical)) == (b->physical + (s - b->logical))) {
                continue;
            }
        }
        ops.push_back({OP_DATA, s, len});
    }
        std::vector<DiffOp> merged;
    for (const auto& op : ops) {
        if (!merged.empty() && merged.back().op == op.op &&
            merged.back().offset + merged.back().length == op.offset) {
            merged.back().length += op.length;
        } else {
            merged.push_back(op);
        }
    }
    return merged;
}

class TargetWriter {
    int fd_;
    bool nocache_, noresize_, taildiscard_, is_blk_ = false;
    uint64_t sector_size_ = 512;
public:
    TargetWriter(const std::string& path, bool nocache, bool noresize = false, bool taildiscard = false)
        : nocache_(nocache), noresize_(noresize), taildiscard_(taildiscard) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) throw_errno("open target");
        struct stat st{};
        if (::fstat(fd_, &st) < 0) throw_errno("fstat target");
        is_blk_ = S_ISBLK(st.st_mode);
        int ssz = 512;
        if (is_blk_ && ::ioctl(fd_, BLKSSZGET, &ssz) == 0 && ssz > 0) sector_size_ = ssz;
    }
    ~TargetWriter() { if (fd_ >= 0) ::close(fd_); }

    void set_size(uint64_t size) {
        if (is_blk_) {
            uint64_t cur = 0;
            if (::ioctl(fd_, BLKGETSIZE64, &cur) < 0) throw_errno("BLKGETSIZE64 target");
            if (cur < size) throw std::runtime_error("target block device smaller than source");
            if (noresize_) { if (taildiscard_ && cur > size) punch_hole(size, cur - size); return; }
            if (cur != size) throw std::runtime_error("cannot resize block device in diffstream (use --noresize)");
        } else if (noresize_) {
            struct stat st{};
            if (::fstat(fd_, &st) < 0) throw_errno("fstat target");
            uint64_t cur = static_cast<uint64_t>(st.st_size);
            if (cur < size) throw std::runtime_error("target file smaller than source");
            if (taildiscard_ && cur > size) punch_hole(size, cur - size);
        } else if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            throw_errno("ftruncate");
        }
    }
    void punch_hole(uint64_t off, uint64_t len) {
        if (len == 0) return;
        if (is_blk_) {
            uint64_t al = (len / sector_size_) * sector_size_;
            if (al > 0) { uint64_t r[] = {off, al}; if (::ioctl(fd_, BLKZEROOUT, r) < 0) throw_errno("BLKZEROOUT"); }
            if (len > al) { std::vector<uint8_t> z(len - al, 0); pwrite_all(fd_, z.data(), len - al, off + al); }
        } else if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               static_cast<off_t>(off), static_cast<off_t>(len)) < 0) {
            throw_errno("fallocate punch hole");
        }
    }
    void write_data(const void* buf, uint64_t off, uint64_t len) {
        pwrite_all(fd_, buf, len, static_cast<off_t>(off));
        if (nocache_) {
            ::sync_file_range(fd_, static_cast<off_t>(off), static_cast<off_t>(len),
                              SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
            drop_cache(fd_, static_cast<off_t>(off), static_cast<off_t>(len));
        }
    }
    void finish() {
        if (::fsync(fd_) < 0) throw_errno("fsync target");
        if (nocache_) drop_cache(fd_, 0, 0);
    }
};

void run_diff(const std::string& base_path, const std::string& chg_path, uint64_t trunc_sz,
              int out_fd, TargetWriter* direct_target, bool nocache, bool verbose) {
    int c_fd = ::open(chg_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (c_fd < 0) throw_errno("open change");

    try {
        struct stat st{};
        if (::fstat(c_fd, &st) < 0) throw_errno("fstat change");
        uint64_t chg_size = static_cast<uint64_t>(st.st_size);
        if (nocache) ::posix_fadvise(c_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        std::vector<Extent> base_extents;
        if (!base_path.empty()) {
            int b_fd = ::open(base_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (b_fd < 0) throw_errno("open base");
            try { base_extents = get_extents(b_fd); } catch (...) { ::close(b_fd); throw; }
            ::close(b_fd);
        }
        if (trunc_sz > 0 && chg_size > trunc_sz) chg_size = trunc_sz;
        auto ops = compute_diff(base_extents, get_extents(c_fd), chg_size);

        if (direct_target) {
            direct_target->set_size(chg_size);
        } else {
            write_all(out_fd, STREAM_MAGIC, sizeof(STREAM_MAGIC));
            write_pkt(out_fd, OP_TRUNCATE, 0, chg_size);
        }

        auto t_start = std::chrono::steady_clock::now();
        std::vector<uint8_t> buf(MAX_CHUNK);
        uint64_t sent = 0, punched = 0;

        for (const auto& op : ops) {
            if (op.op == OP_HOLE) {
                if (direct_target) {
                    direct_target->punch_hole(op.offset, op.length);
                } else {
                    write_pkt(out_fd, OP_HOLE, op.offset, op.length);
                }
                punched += op.length;
                            } else {
                for (uint64_t pos = op.offset, rem = op.length; rem > 0;) {
                    uint64_t n = std::min(rem, MAX_CHUNK);
                    pread_all(c_fd, buf.data(), n, static_cast<off_t>(pos));

                    if (is_all_zero(buf.data(), n)) {
                        if (direct_target) {
                            direct_target->punch_hole(pos, n);
                        } else {
                            write_pkt(out_fd, OP_HOLE, pos, n);
                        }
                        punched += n;
                    } else {
                        if (direct_target) {
                            direct_target->write_data(buf.data(), pos, n);
                        } else {
                            write_pkt(out_fd, OP_DATA, pos, n);
                            write_all(out_fd, buf.data(), n);
                        }
                        sent += n;
                    }

                    if (nocache) drop_cache(c_fd, static_cast<off_t>(pos), static_cast<off_t>(n));
                    pos += n; rem -= n;
                }
            }
        }

        if (direct_target) {
            direct_target->finish();
        } else {
            write_pkt(out_fd, OP_EOF, 0, 0);
        }
        if (nocache) drop_cache(c_fd, 0, 0);

        if (verbose) {
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            double mib = sent / 1048576.0;
            double rate = (sec > 0.0001) ? (mib / sec) : 0.0;
            std::cerr << std::fixed << std::setprecision(2)
                      << "Done in " << sec << "s (" << rate << " MiB/s): "
                      << mib << " MiB written, "
                      << punched / 1048576.0 << " MiB discarded.\n";
        }
    } catch (...) {
        ::close(c_fd);
        throw;
    }
    ::close(c_fd);
}

void run_apply(const std::string& target_path, int in_fd, bool nocache, bool noresize, bool taildiscard) {
    char magic[sizeof(STREAM_MAGIC)]{};
    read_all(in_fd, magic, sizeof(magic));
    if (std::memcmp(magic, STREAM_MAGIC, sizeof(STREAM_MAGIC)) != 0) {
        throw std::runtime_error("invalid stream magic");
    }

    TargetWriter target(target_path, nocache, noresize, taildiscard);
    std::vector<uint8_t> buf(MAX_CHUNK);

    while (true) {
        PacketHeader h = read_pkt(in_fd);
        if (h.op == OP_EOF) break;
        if (h.op == OP_TRUNCATE) {
            target.set_size(h.length);
        } else if (h.op == OP_HOLE) {
            target.punch_hole(h.offset, h.length);
        } else if (h.op == OP_DATA) {
            for (uint64_t pos = h.offset, rem = h.length; rem > 0;) {
                uint64_t n = std::min(rem, MAX_CHUNK);
                read_all(in_fd, buf.data(), n);
                target.write_data(buf.data(), pos, n);
                pos += n; rem -= n;
            }
        } else {
            throw std::runtime_error("unknown opcode");
        }
    }
    target.finish();
}

void usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " --diff [--base <file>] --change <file> [--nocache] [-v]\n"
              << "  " << prog << " --apply --target <file> [--nocache]\n"
              << "  " << prog << " [--base <file>] --change <file> --target <file> [--nocache] [-v]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        bool do_diff = false, do_apply = false, nocache = false, noresize = false, taildiscard = false, verbose = false;
        uint64_t trunc_sz = 0;
        std::string base, change, target;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--diff") do_diff = true;
            else if (a == "--apply") do_apply = true;
            else if (a == "--nocache") nocache = true;
            else if (a == "--noresize") noresize = true;
            else if (a == "--taildiscard") taildiscard = true;
            else if (a == "--truncate" && i + 1 < argc) trunc_sz = parse_size_arg(argv[++i]);
            else if (a == "-v" || a == "--verbose") verbose = true;
            else if (a == "--base"   && i + 1 < argc) base   = argv[++i];
            else if (a == "--change" && i + 1 < argc) change = argv[++i];
            else if (a == "--target" && i + 1 < argc) target = argv[++i];
            else { usage(argv[0]); return 1; }
        }

        if (!do_diff && !do_apply && !change.empty() && !target.empty()) {
            TargetWriter writer(target, nocache, noresize, taildiscard);
            run_diff(base, change, trunc_sz, -1, &writer, nocache, verbose);
            return 0;
        }
        if (do_diff && !do_apply && !change.empty() && target.empty()) {
            run_diff(base, change, trunc_sz, STDOUT_FILENO, nullptr, nocache, verbose);
            return 0;
        }
        if (do_apply && !do_diff && !target.empty() && base.empty() && change.empty()) {
            run_apply(target, STDIN_FILENO, nocache, noresize, taildiscard);
            return 0;
        }

        usage(argv[0]);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
