// zfsdiffstream.cpp - ZFS snapshot diff & apply tool (wire-compatible with diffstream/lvmdiffstream)
// Compile: g++ -O3 -std=c++17 -Wall -Wextra -o zfsdiffstream zfsdiffstream.cpp

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <climits>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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


constexpr uint8_t OP_DATA     = 1;
constexpr uint8_t OP_HOLE     = 2;
constexpr uint8_t OP_TRUNCATE = 3;
constexpr uint8_t OP_EOF      = 4;

constexpr std::array<char, 8> STREAM_MAGIC = {'D', 'I', 'F', 'F', 'S', 'T', 'R', '1'};
constexpr uint64_t MAX_CHUNK = 4ULL * 1024 * 1024; // 4 MiB

// OpenZFS DMU Replay Record constants (platform-independent layout)
constexpr uint64_t DMU_BACKUP_MAGIC = 0x2F5bacbacULL;
constexpr uint32_t DRR_BEGIN        = 0;
constexpr uint32_t DRR_OBJECT       = 1;
constexpr uint32_t DRR_FREEOBJECTS  = 2;
constexpr uint32_t DRR_WRITE        = 3;
constexpr uint32_t DRR_FREE         = 4;
constexpr uint32_t DRR_END          = 5;
constexpr uint32_t DRR_WRITE_BYREF  = 6;
constexpr uint32_t DRR_SPILL        = 7;
constexpr uint32_t DRR_WRITE_EMBEDDED = 8;

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  op;
    uint64_t offset;
    uint64_t length;
};

struct drr_begin_t {
    uint64_t drr_magic;
    uint64_t drr_versioninfo;
    uint64_t drr_creation_time;
    uint32_t drr_type;
    uint32_t drr_flags;
    uint64_t drr_toguid;
    uint64_t drr_fromguid;
    char     drr_toname[224];
};

struct drr_object_t {
    uint64_t drr_object;
    uint32_t drr_type;
    uint32_t drr_bonustype;
    uint32_t drr_blksz;
    uint32_t drr_bonuslen;
    uint8_t  drr_checksumtype;
    uint8_t  drr_compress;
    uint8_t  drr_dn_slots;
    uint8_t  drr_flags;
    uint32_t drr_raw_bonuslen;
    uint64_t drr_toguid;
    uint8_t  drr_indblkshift;
    uint8_t  drr_nlevels;
    uint8_t  drr_nblkptr;
    uint8_t  drr_pad;
    uint64_t drr_maxblkid;
};

struct drr_write_t {
    uint64_t drr_object;
    uint32_t drr_type;
    uint32_t drr_pad;
    uint64_t drr_offset;
    uint64_t drr_logical_size;
    uint64_t drr_toguid;
    uint8_t  drr_checksumtype;
    uint8_t  drr_flags;
    uint8_t  drr_compressiontype;
    uint8_t  drr_pad2;
    uint64_t drr_key_ddobj;
    uint64_t drr_key_propobj;
    uint64_t drr_key_gen;
    uint64_t drr_compressed_size;
};
struct drr_free_t {
    uint64_t drr_object;
    uint64_t drr_offset;
    uint64_t drr_length;
    uint64_t drr_toguid;
};

struct dmu_replay_record_t {
    uint32_t drr_type;
    uint32_t drr_payloadlen;
    union {
        drr_begin_t  drr_begin;
        drr_object_t drr_object;
        drr_write_t  drr_write;
        drr_free_t   drr_free;
        uint8_t      drr_pad[272];
    } drr_u;
    uint64_t drr_checksum[4]; // zio_cksum_t (4 * uint64_t)
};
#pragma pack(pop)

static_assert(sizeof(dmu_replay_record_t) == 312, "dmu_replay_record_t must be 312 bytes");

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

void pwrite_all(int fd, const void* buf, size_t len, off_t pos) {
    const auto* p = static_cast<const uint8_t*>(buf);
    for (size_t off = 0; off < len;) {
        ssize_t n = ::pwrite(fd, p + off, len - off, pos + static_cast<off_t>(off));
        if (n < 0) { if (errno == EINTR) continue; throw_errno("pwrite"); }
        if (n == 0) throw std::runtime_error("short pwrite");
        off += static_cast<size_t>(n);
    }
}

std::string exec_cmd(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) throw_errno("popen failed: " + cmd);
    while (size_t n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        out.append(buf.data(), n);
    }
    int rc = ::pclose(pipe);
    if (rc != 0) throw std::runtime_error("command failed (" + std::to_string(rc) + "): " + cmd);
    return out;
}
class TargetWriter {
    std::string path_;
    int fd_ = -1;
    bool nocache_, noresize_, taildiscard_; uint64_t sector_size_ = 512;
    bool is_blk_ = false;

    void open_target() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) throw_errno("open target");
        struct stat st{};
        if (::fstat(fd_, &st) < 0) throw_errno("fstat target");
        is_blk_ = S_ISBLK(st.st_mode); int ssz = 512; if (is_blk_ && ::ioctl(fd_, BLKSSZGET, &ssz) == 0 && ssz > 0) sector_size_ = ssz;
    }
public:
    TargetWriter(const std::string& path, bool nocache, bool noresize = false, bool taildiscard = false) : path_(path), nocache_(nocache), noresize_(noresize), taildiscard_(taildiscard) {
        open_target();
    }
    ~TargetWriter() { if (fd_ >= 0) ::close(fd_); }

    void punch_hole(uint64_t off, uint64_t len) {
        if (len == 0) return;
        if (is_blk_) {
            uint64_t al = (len / sector_size_) * sector_size_;
            if (al > 0) { std::array<uint64_t, 2> r = {off, al}; if (::ioctl(fd_, BLKZEROOUT, r.data()) < 0) throw_errno("BLKZEROOUT"); }
            if (len > al) { std::vector<uint8_t> z(len - al, 0); pwrite_all(fd_, z.data(), len - al, off + al); }
        } else if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               static_cast<off_t>(off), static_cast<off_t>(len)) < 0) {
            throw_errno("fallocate punch hole failed");
        }
    }
    void set_size(uint64_t size) {
        if (is_blk_) {
            uint64_t cur = 0;
            if (::ioctl(fd_, BLKGETSIZE64, &cur) < 0) throw_errno("BLKGETSIZE64 target");
            if (noresize_) { if (cur < size) throw std::runtime_error("target smaller than source"); if (taildiscard_ && cur > size) punch_hole(size, cur - size); return; }
            if (cur != size) {
                std::array<char, PATH_MAX> real_buf{};
                if (!(::realpath(path_.c_str(), real_buf.data()) != nullptr && std::strncmp(real_buf.data(), "/dev/zd", 7) == 0 && path_.find("/dev/zvol/") == 0))
                    throw std::runtime_error("cannot resize non-ZVOL block device without --noresize: " + path_);
                if (size < cur) punch_hole(size, cur - size);
                ::close(fd_); fd_ = -1;
                exec_cmd("zfs set volsize=" + std::to_string(size) + " '" + path_.substr(10) + "'");
                open_target();
            }
        } else if (noresize_) { struct stat st{}; ::fstat(fd_, &st); if ((uint64_t)st.st_size < size) throw std::runtime_error("target smaller than source"); if (taildiscard_ && (uint64_t)st.st_size > size) punch_hole(size, (uint64_t)st.st_size - size); } else if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            throw_errno("ftruncate");
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

uint64_t resolve_target_size(const std::string& chg_snap, const std::string& rel_file, uint64_t& target_obj) {
    size_t at = chg_snap.find('@');
    if (at == std::string::npos) throw std::runtime_error("snapshot must be in pool/dataset@snap format");
    std::string ds = chg_snap.substr(0, at);
    std::string snap = chg_snap.substr(at + 1);

    std::string type = exec_cmd("zfs get -H -o value type '" + ds + "'");
    while (!type.empty() && (type.back() == '\n' || type.back() == '\r')) type.pop_back();

    if (type == "volume") {
        target_obj = 1; // ZVOL data object is always object 1
        std::string vs = exec_cmd("zfs get -Hp -o value volsize '" + chg_snap + "'");
        return std::stoull(vs);
    }
    if (rel_file.empty()) {
        throw std::runtime_error("--file <relative_path> is required when source is a ZFS filesystem dataset");
    }
    std::string mp = exec_cmd("zfs get -H -o value mountpoint '" + ds + "'");
    while (!mp.empty() && (mp.back() == '\n' || mp.back() == '\r')) mp.pop_back();
    std::string snap_file = mp + "/.zfs/snapshot/" + snap + "/" + rel_file;

    struct stat st{};
    if (::stat(snap_file.c_str(), &st) < 0) throw_errno("stat snapshot file: " + snap_file);
    target_obj = static_cast<uint64_t>(st.st_ino); // ZFS object ID == inode number
    return static_cast<uint64_t>(st.st_size);
}
void run_diff(const std::string& base_snap, const std::string& chg_snap, const std::string& rel_file, uint64_t trunc_sz,
              int out_fd, TargetWriter* direct_target, bool nocache, bool verbose) {
    (void)nocache;
    auto t_start = std::chrono::steady_clock::now();
    uint64_t target_obj = 0;
    uint64_t chg_size = resolve_target_size(chg_snap, rel_file, target_obj);
    if (trunc_sz > 0 && chg_size > trunc_sz) chg_size = trunc_sz;

    std::string send_cmd = base_snap.empty()
        ? "zfs send '" + chg_snap + "'"
        : "zfs send -i '" + base_snap + "' '" + chg_snap + "'";

    FILE* zpipe = ::popen(send_cmd.c_str(), "r");
    if (!zpipe) throw_errno("popen zfs send");
    int zfd = ::fileno(zpipe);

    try {
        if (direct_target) {
            direct_target->set_size(chg_size);
        } else {
            write_all(out_fd, STREAM_MAGIC.data(), STREAM_MAGIC.size());
            write_pkt(out_fd, OP_TRUNCATE, 0, chg_size);
        }

        std::vector<uint8_t> buf(MAX_CHUNK);
        uint64_t sent = 0, punched = 0, full_cursor = 0;

        while (true) {
            dmu_replay_record_t drr{};
            read_all(zfd, &drr, sizeof(drr));
            if (drr.drr_type == DRR_END) break;

            if (drr.drr_type == DRR_BEGIN) {
                if (drr.drr_u.drr_begin.drr_magic != DMU_BACKUP_MAGIC) {
                    throw std::runtime_error("invalid ZFS send stream magic (check endianness)");
                }
            } else if (drr.drr_type == DRR_WRITE && drr.drr_u.drr_write.drr_object == target_obj) {
                uint64_t off = drr.drr_u.drr_write.drr_offset;
                uint64_t len = drr.drr_u.drr_write.drr_logical_size;
                if (base_snap.empty() && off > full_cursor && full_cursor < chg_size) {
                    uint64_t hlen = std::min(off, chg_size) - full_cursor;
                    if (direct_target) direct_target->punch_hole(full_cursor, hlen);
                    else write_pkt(out_fd, OP_HOLE, full_cursor, hlen);
                    punched += hlen;
                }
                if (base_snap.empty()) full_cursor = std::max(full_cursor, std::min(off + len, chg_size));
                if (drr.drr_payloadlen != 0 && drr.drr_payloadlen < len) {
                    throw std::runtime_error("compressed/raw ZFS send stream not supported; do not pass -c");
                }
                uint64_t pad_bytes = (drr.drr_payloadlen > len) ? (drr.drr_payloadlen - len) : 0;
                for (uint64_t pos = off, rem = len; rem > 0;) {
                    uint64_t n = std::min(rem, MAX_CHUNK);
                    read_all(zfd, buf.data(), n);
                    if (pos < chg_size) {
                        uint64_t valid = std::min(n, chg_size - pos);
                        if (is_all_zero(buf.data(), valid)) {
                            if (direct_target) direct_target->punch_hole(pos, valid);
                            else { write_pkt(out_fd, OP_HOLE, pos, valid); }
                            punched += valid;
                        } else {
                            if (direct_target) direct_target->write_data(buf.data(), pos, valid);
                            else { write_pkt(out_fd, OP_DATA, pos, valid); write_all(out_fd, buf.data(), valid); }
                            sent += valid;
                        }
                    }
                    pos += n; rem -= n;
                }
                for (uint64_t rem = pad_bytes; rem > 0;) {
                    uint64_t n = std::min(rem, MAX_CHUNK);
                    read_all(zfd, buf.data(), n);
                    rem -= n;
                }
                continue;
            } else if (drr.drr_type == DRR_FREE && drr.drr_u.drr_free.drr_object == target_obj) {
                uint64_t off = drr.drr_u.drr_free.drr_offset;
                uint64_t len = drr.drr_u.drr_free.drr_length;
                if (off < chg_size) {
                    uint64_t valid = std::min(len, chg_size - off);
                    if (direct_target) direct_target->punch_hole(off, valid);
                    else { write_pkt(out_fd, OP_HOLE, off, valid); }
                    punched += valid;
                }
            }

            // Skip payload for non-matching objects or metadata records (ZFS 0.8 & 2.x compatible)
            uint64_t skip_len = drr.drr_payloadlen;
            if (skip_len == 0) {
                if (drr.drr_type == DRR_OBJECT) {
                    skip_len = (static_cast<uint64_t>(drr.drr_u.drr_object.drr_bonuslen) + 7ULL) & ~7ULL;
                } else if (drr.drr_type == DRR_WRITE) {
                    skip_len = drr.drr_u.drr_write.drr_logical_size;
                }
            }
            for (uint64_t rem = skip_len; rem > 0;) {
                uint64_t n = std::min(rem, MAX_CHUNK);
                read_all(zfd, buf.data(), n);
                rem -= n;
            }
        }

        if (base_snap.empty() && full_cursor < chg_size) {
            uint64_t hlen = chg_size - full_cursor;
            if (direct_target) direct_target->punch_hole(full_cursor, hlen);
            else write_pkt(out_fd, OP_HOLE, full_cursor, hlen);
            punched += hlen;
        }
        if (direct_target) direct_target->finish();
        else { write_pkt(out_fd, OP_EOF, 0, 0); }

        if (verbose) {
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            double mib = sent / 1048576.0;
            std::cerr << std::fixed << std::setprecision(2)
                      << "Done in " << sec << "s (" << (sec > 0.0001 ? mib / sec : 0.0) << " MiB/s): "
                      << mib << " MiB written, " << punched / 1048576.0 << " MiB discarded.\n";
        }
    } catch (...) { ::pclose(zpipe); throw; }
    if (::pclose(zpipe) != 0) throw std::runtime_error("zfs send exited with error");
}

void run_apply(const std::string& target_path, int in_fd, bool nocache, bool noresize, bool taildiscard) {
    std::array<char, sizeof(STREAM_MAGIC)> magic{};
    read_all(in_fd, magic.data(), magic.size());
    if (std::memcmp(magic.data(), STREAM_MAGIC.data(), STREAM_MAGIC.size()) != 0) throw std::runtime_error("invalid stream magic");
    TargetWriter target(target_path, nocache, noresize, taildiscard);
    std::vector<uint8_t> buf(MAX_CHUNK);
    while (true) {
        PacketHeader h = read_pkt(in_fd);
        if (h.op == OP_EOF) break;
        if (h.op == OP_TRUNCATE) target.set_size(h.length);
        else if (h.op == OP_HOLE) target.punch_hole(h.offset, h.length);
        else if (h.op == OP_DATA) {
            for (uint64_t pos = h.offset, rem = h.length; rem > 0;) {
                uint64_t n = std::min(rem, MAX_CHUNK);
                read_all(in_fd, buf.data(), n);
                target.write_data(buf.data(), pos, n);
                pos += n; rem -= n;
            }
        } else throw std::runtime_error("unknown opcode");
    }
    target.finish();
}

void usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " --diff [--base <snap>] --change <snap> [--file <relpath>] [--nocache] [-v]\n"
              << "  " << prog << " --apply --target <file_or_dev> [--nocache]\n"
              << "  " << prog << " [--base <snap>] --change <snap> [--file <relpath>] --target <file_or_dev> [--nocache] [-v]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        bool do_diff = false, do_apply = false, nocache = false, noresize = false, taildiscard = false, verbose = false; uint64_t trunc_sz = 0;
        std::string base, change, file, target;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--diff") do_diff = true;
            else if (a == "--apply") do_apply = true;
            else if (a == "--nocache") nocache = true; else if (a == "--noresize") noresize = true; else if (a == "--taildiscard") taildiscard = true;
            else if (a == "--truncate" && i + 1 < argc) trunc_sz = parse_size_arg(argv[++i]);
            else if (a == "-v" || a == "--verbose") verbose = true;
            else if (a == "--base"   && i + 1 < argc) base   = argv[++i];
            else if (a == "--change" && i + 1 < argc) change = argv[++i];
            else if (a == "--file"   && i + 1 < argc) file   = argv[++i];
            else if (a == "--target" && i + 1 < argc) target = argv[++i];
            else { usage(argv[0]); return 1; }
        }
        if (!do_diff && !do_apply && !change.empty() && !target.empty()) {
            TargetWriter writer(target, nocache, noresize, taildiscard);
            run_diff(base, change, file, trunc_sz, -1, &writer, nocache, verbose);
            return 0;
        }
        if (do_diff && !do_apply && !change.empty() && target.empty()) {
            run_diff(base, change, file, trunc_sz, STDOUT_FILENO, nullptr, nocache, verbose);
            return 0;
        }
        if (do_apply && !do_diff && !target.empty() && change.empty()) {
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
