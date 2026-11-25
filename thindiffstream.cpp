// thindiffstream.cpp - LVM-Thin snapshot diff & apply tool (wire-compatible with diffstream)
// Compile: g++ -O3 -std=c++17 -Wall -Wextra -o thindiffstream thindiffstream.cpp

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <chrono>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <sstream>
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

constexpr char STREAM_MAGIC[8] = {'D', 'I', 'F', 'F', 'S', 'T', 'R', '1'};
constexpr uint64_t MAX_CHUNK = 4ULL * 1024 * 1024; // 4 MiB

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  op;
    uint64_t offset;
    uint64_t length;
};
#pragma pack(pop)

struct DiffOp {
    uint8_t  op;
    uint64_t offset, length;
};

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

std::string dm_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '-') out += "--";
        else out += c;
    }
    return out;
}

void dm_target_message(const std::string& dm_name, const std::string& msg) {
    int fd = ::open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (fd < 0) throw_errno("open /dev/mapper/control");

    constexpr size_t BUF_SIZE = 16384;
    std::vector<uint8_t> buf(BUF_SIZE, 0);
    auto* dmi = reinterpret_cast<struct dm_ioctl*>(buf.data());

    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->version[1] = DM_VERSION_MINOR;
    dmi->version[2] = DM_VERSION_PATCHLEVEL;
    dmi->data_size = BUF_SIZE;
    dmi->data_start = sizeof(struct dm_ioctl);
    std::snprintf(dmi->name, sizeof(dmi->name), "%s", dm_name.c_str());

    auto* tmsg = reinterpret_cast<struct dm_target_msg*>(buf.data() + sizeof(struct dm_ioctl));
    tmsg->sector = 0;
    std::snprintf(reinterpret_cast<char*>(tmsg + 1),
                  BUF_SIZE - sizeof(struct dm_ioctl) - sizeof(struct dm_target_msg),
                  "%s", msg.c_str());

    int rc = ::ioctl(fd, DM_TARGET_MSG, buf.data());
    int saved_err = errno;
    ::close(fd);
    if (rc < 0) {
        errno = saved_err;
        throw_errno("DM_TARGET_MSG (" + msg + ") failed on " + dm_name);
    }
}

struct ThinInfo {
    std::string vg;
    std::string pool;
    uint64_t thin_id = 0;
};

ThinInfo get_thin_info(const std::string& lv_path) {
    std::string out = exec_cmd("lvs --noheadings --separator '|' -o vg_name,pool_lv,thin_id '" + lv_path + "'");
    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };
    trim(out);
    size_t p1 = out.find('|');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : out.find('|', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) {
        throw std::runtime_error("Not a valid thin LV: " + lv_path);
    }
    ThinInfo info;
    info.vg = out.substr(0, p1);
    info.pool = out.substr(p1 + 1, p2 - p1 - 1);
    std::string id_str = out.substr(p2 + 1);
    if (info.pool.empty() || id_str.empty()) {
        throw std::runtime_error("LV is not a thin volume/snapshot: " + lv_path);
    }
    info.thin_id = std::stoull(id_str);
    return info;
}

class MetaSnapGuard {
    std::string dm_name_;
    bool active_ = false;
public:
    MetaSnapGuard(const std::string& vg, const std::string& pool) {
        dm_name_ = dm_escape(vg) + "-" + dm_escape(pool) + "-tpool";
        dm_target_message(dm_name_, "reserve_metadata_snap");
        active_ = true;
    }
    ~MetaSnapGuard() {
        if (active_) {
            try { dm_target_message(dm_name_, "release_metadata_snap"); } catch (...) {}
        }
    }
};
uint64_t parse_xml_attr(const std::string& line, const std::string& key) {
    std::string pat = key + "=\"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) throw std::runtime_error("missing XML attr '" + key + "' in: " + line);
    pos += pat.size();
    size_t end = line.find('"', pos);
    return std::stoull(line.substr(pos, end - pos));
}

std::vector<DiffOp> compute_lvm_diff(const std::string& base_path,
                                     const std::string& chg_path,
                                     uint64_t chg_size) {
    ThinInfo c_info = get_thin_info(chg_path);
    MetaSnapGuard guard(c_info.vg, c_info.pool);
    std::string tmeta = "/dev/mapper/" + dm_escape(c_info.vg) + "-" + dm_escape(c_info.pool) + "_tmeta";

    std::vector<DiffOp> ops;
    uint64_t block_bytes = 0;
    std::string xml;

    if (base_path.empty()) {
        xml = exec_cmd("thin_dump -m --dev-id " + std::to_string(c_info.thin_id) + " '" + tmeta + "'");
    } else {
        ThinInfo b_info = get_thin_info(base_path);
        if (b_info.vg != c_info.vg || b_info.pool != c_info.pool) {
            throw std::runtime_error("base and change snapshots must belong to the same thin pool");
        }
        xml = exec_cmd("thin_delta -m --snap1 " + std::to_string(b_info.thin_id) +
                       " --snap2 " + std::to_string(c_info.thin_id) + " '" + tmeta + "'");
    }

    std::istringstream iss(xml);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("<superblock") != std::string::npos) {
            block_bytes = parse_xml_attr(line, "data_block_size") * 512ULL;
        } else if (base_path.empty() && line.find("<range_mapping ") != std::string::npos) {
            uint64_t off = parse_xml_attr(line, "origin_begin") * block_bytes;
            uint64_t len = parse_xml_attr(line, "length") * block_bytes;
            if (off < chg_size) ops.push_back({OP_DATA, off, std::min(len, chg_size - off)});
        } else if (base_path.empty() && line.find("<single_mapping ") != std::string::npos) {
            uint64_t off = parse_xml_attr(line, "origin_block") * block_bytes;
            if (off < chg_size) ops.push_back({OP_DATA, off, std::min(block_bytes, chg_size - off)});
        } else if (line.find("<different ") != std::string::npos ||
                   line.find("<right_only ") != std::string::npos) {
            uint64_t off = parse_xml_attr(line, "begin") * block_bytes;
            uint64_t len = parse_xml_attr(line, "length") * block_bytes;
            if (off < chg_size) {
                ops.push_back({OP_DATA, off, std::min(len, chg_size - off)});
            }
        } else if (line.find("<left_only ") != std::string::npos) {
            uint64_t off = parse_xml_attr(line, "begin") * block_bytes;
            uint64_t len = parse_xml_attr(line, "length") * block_bytes;
            if (off < chg_size) {
                ops.push_back({OP_HOLE, off, std::min(len, chg_size - off)});
            }
        }
    }

    if (base_path.empty()) {
        std::vector<DiffOp> full_ops;
        uint64_t cur = 0;
        for (const auto& op : ops) {
            if (op.offset > cur) full_ops.push_back({OP_HOLE, cur, op.offset - cur});
            full_ops.push_back(op);
            cur = op.offset + op.length;
        }
        if (cur < chg_size) full_ops.push_back({OP_HOLE, cur, chg_size - cur});
        ops = std::move(full_ops);
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
            if (al > 0) { uint64_t r[] = {off, al}; if (::ioctl(fd_, BLKZEROOUT, r) < 0) throw_errno("BLKZEROOUT"); }
            if (len > al) { std::vector<uint8_t> z(len - al, 0); pwrite_all(fd_, z.data(), len - al, off + al); }
        } else if (::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               static_cast<off_t>(off), static_cast<off_t>(len)) < 0) {
            throw_errno("fallocate punch hole failed on target");
        }
    }

    void set_size(uint64_t size) {
        if (is_blk_) {
            uint64_t cur = 0;
            if (::ioctl(fd_, BLKGETSIZE64, &cur) < 0) throw_errno("BLKGETSIZE64 target");
            if (noresize_) { if (cur < size) throw std::runtime_error("target smaller than source"); if (taildiscard_ && cur > size) punch_hole(size, cur - size); return; }
            if (cur != size) {
                exec_cmd("lvs --noheadings -o lv_name '" + path_ + "' >/dev/null");
                if (size < cur) punch_hole(size, cur - size);
                ::close(fd_);
                fd_ = -1;
                exec_cmd("lvresize -f -L " + std::to_string(size) + "B '" + path_ + "' >/dev/null");
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

void run_diff(const std::string& base_path, const std::string& chg_path, uint64_t trunc_sz,
              int out_fd, TargetWriter* direct_target, bool nocache, bool verbose) {
    int c_fd = ::open(chg_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (c_fd < 0) throw_errno("open change LV");

    try {
        uint64_t chg_size = 0;
        struct stat st{};
        if (::fstat(c_fd, &st) < 0) throw_errno("fstat change");
        if (S_ISBLK(st.st_mode)) {
            if (::ioctl(c_fd, BLKGETSIZE64, &chg_size) < 0) throw_errno("BLKGETSIZE64 change");
        } else {
            chg_size = static_cast<uint64_t>(st.st_size);
        }
        if (nocache) ::posix_fadvise(c_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        if (trunc_sz > 0 && chg_size > trunc_sz) chg_size = trunc_sz;
        auto ops = compute_lvm_diff(base_path, chg_path, chg_size);

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
                if (direct_target) direct_target->punch_hole(op.offset, op.length);
                else { write_pkt(out_fd, OP_HOLE, op.offset, op.length); }
                punched += op.length;
            } else {
                for (uint64_t pos = op.offset, rem = op.length; rem > 0;) {
                    uint64_t n = std::min(rem, MAX_CHUNK);
                    pread_all(c_fd, buf.data(), n, static_cast<off_t>(pos));
                    if (is_all_zero(buf.data(), n)) {
                        if (direct_target) direct_target->punch_hole(pos, n);
                        else { write_pkt(out_fd, OP_HOLE, pos, n); }
                        punched += n;
                    } else {
                        if (direct_target) direct_target->write_data(buf.data(), pos, n);
                        else { write_pkt(out_fd, OP_DATA, pos, n); write_all(out_fd, buf.data(), n); }
                        sent += n;
                    }
                    if (nocache) drop_cache(c_fd, static_cast<off_t>(pos), static_cast<off_t>(n));
                    pos += n; rem -= n;
                }
            }
        }

        if (direct_target) direct_target->finish();
        else { write_pkt(out_fd, OP_EOF, 0, 0); }
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
    } catch (...) { ::close(c_fd); throw; }
    ::close(c_fd);
}

void run_apply(const std::string& target_path, int in_fd, bool nocache, bool noresize, bool taildiscard) {
    char magic[sizeof(STREAM_MAGIC)]{};
    read_all(in_fd, magic, sizeof(magic));
    if (std::memcmp(magic, STREAM_MAGIC, sizeof(STREAM_MAGIC)) != 0) throw std::runtime_error("invalid stream magic");

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
              << "  " << prog << " --diff [--base <lv>] --change <lv> [--nocache] [-v]\n"
              << "  " << prog << " --apply --target <lv_or_file> [--nocache]\n"
              << "  " << prog << " [--base <lv>] --change <lv> --target <lv_or_file> [--nocache] [-v]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        bool do_diff = false, do_apply = false, nocache = false, noresize = false, taildiscard = false, verbose = false; uint64_t trunc_sz = 0;
        std::string base, change, target;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--diff") do_diff = true;
            else if (a == "--apply") do_apply = true;
            else if (a == "--nocache") nocache = true; else if (a == "--noresize") noresize = true; else if (a == "--taildiscard") taildiscard = true;
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
