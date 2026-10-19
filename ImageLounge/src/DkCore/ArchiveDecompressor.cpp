#include "ArchiveDecompressor.h"

#ifdef WITH_LIBARCHIVE

#include <cstring>
#include <fstream>

// ============================================================
// Compressor 构造 / 析构
// ============================================================
ArchiveCompressor::ArchiveCompressor(const std::string& filepath) {
    readArchive_.reset(archive_read_new());
    archive_read_support_filter_all(readArchive_.get());
    archive_read_support_format_all(readArchive_.get());

    if (archive_read_open_filename(readArchive_.get(), filepath.c_str(), 10240) != ARCHIVE_OK) {
        error_ = archive_error_string(readArchive_.get()) ?: "open failed";
        readArchive_.reset();
    }
}

ArchiveCompressor::ArchiveCompressor(const void* data, size_t size) {
    readArchive_.reset(archive_read_new());
    archive_read_support_filter_all(readArchive_.get());
    archive_read_support_format_all(readArchive_.get());

    if (archive_read_open_memory(readArchive_.get(), data, size) != ARCHIVE_OK) {
        error_ = archive_error_string(readArchive_.get()) ?: "open failed";
        readArchive_.reset();
    }
}

ArchiveCompressor::~ArchiveCompressor() = default;

void ArchiveCompressor::closeRead() {
    if (readArchive_) {
        archive_read_close(readArchive_.get());
        readArchive_.reset();
    }
}

// ============================================================
// 原有的便捷接口（保持不变，内部可用迭代器实现亦可）
// ============================================================
bool ArchiveCompressor::listEntries(std::vector<std::string>& entries) {
    for (auto it = begin(); it != end(); ++it)
        entries.push_back(it->path());
    return error_.empty();
}

bool ArchiveCompressor::extractToMemory(const std::string& entryPath, std::vector<uint8_t>& output) {
    for (auto it = begin(); it != end(); ++it) {
        if (it->path() == entryPath)
            return it->readAll(output);
    }
    error_ = "entry not found: " + entryPath;
    return false;
}

bool ArchiveCompressor::extractToDisk(const std::string& entryPath, const std::string& dir) {
    if (!readArchive_) { error_ = "archive not open"; return false; }

    struct archive_entry* entry = nullptr;
    int r;
    while ((r = archive_read_next_header(readArchive_.get(), &entry)) == ARCHIVE_OK) {
        const char* path = archive_entry_pathname(entry);
        if (!path || entryPath != path) {
            archive_read_data_skip(readArchive_.get());
            continue;
        }
        std::string fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != '/') fullPath += '/';
        fullPath += entryPath;
        archive_entry_set_pathname(entry, fullPath.c_str());
        r = archive_read_extract(readArchive_.get(), entry,
                                 ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
        if (r != ARCHIVE_OK) { error_ = archive_error_string(readArchive_.get()) ?: "extract failed"; return false; }
        return true;
    }
    error_ = "entry not found: " + entryPath;
    return false;
}

bool ArchiveCompressor::createArchive(const std::string& outputPath,
                               const std::vector<std::string>& sourceFiles,
                               const std::string& format) {
    struct archive* a = archive_write_new();
    if (!a) { error_ = "archive_write_new failed"; return false; }

    archive_write_set_format_by_name(a, format.c_str());
    archive_write_add_filter_none(a);

    if (archive_write_open_filename(a, outputPath.c_str()) != ARCHIVE_OK) {
        error_ = archive_error_string(a) ?: "open failed";
        archive_write_free(a);
        return false;
    }

    bool ok = true;
    for (const auto& file : sourceFiles) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { error_ = "cannot open source: " + file; ok = false; break; }

        in.seekg(0, std::ios::end);
        auto fileSize = in.tellg();
        in.seekg(0, std::ios::beg);

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, file.c_str());
        archive_entry_set_size(entry, fileSize);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            error_ = archive_error_string(a) ?: "write header failed";
            archive_entry_free(entry);
            ok = false;
            break;
        }

        std::vector<char> buf(10240);
        while (in) {
            in.read(buf.data(), buf.size());
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            if (archive_write_data(a, buf.data(), static_cast<size_t>(got)) < 0) {
                error_ = archive_error_string(a) ?: "write data failed";
                ok = false;
                break;
            }
        }
        archive_entry_free(entry);
        if (!ok) break;
    }

    archive_write_close(a);
    archive_write_free(a);
    return ok;
}

// ============================================================
// Entry
// ============================================================
ArchiveCompressor::Entry::Entry(struct archive* a, struct archive_entry* e)
    : archive_(a), entry_(e) {
    if (!e) return;

    EntryInfo& i = info_;
    if (const char* p = archive_entry_pathname(e)) i.path = p;
    i.size     = archive_entry_size(e);
    i.filetype = archive_entry_filetype(e);
    i.perm     = archive_entry_perm(e);

    i.mtime     = { archive_entry_mtime(e),       archive_entry_mtime_nsec(e) };
    i.atime     = { archive_entry_atime(e),       archive_entry_atime_nsec(e) };
    i.ctime     = { archive_entry_ctime(e),       archive_entry_ctime_nsec(e) };
    i.birthtime = { archive_entry_birthtime(e),   archive_entry_birthtime_nsec(e) };

    i.uid = archive_entry_uid(e);
    i.gid = archive_entry_gid(e);
    if (const char* u = archive_entry_uname(e)) i.uname = u;
    if (const char* g = archive_entry_gname(e)) i.gname = g;

    if (const char* s = archive_entry_symlink(e)) i.symlink = s;
    if (const char* h = archive_entry_hardlink(e)) i.hardlink = h;

    i.encrypted = archive_entry_is_encrypted(e) != 0;
}

bool ArchiveCompressor::Entry::readAll(std::vector<uint8_t>& out) const {
    if (!archive_) return false;
    if (dataConsumed_) return true;

    // 已知 size 时预留，未知（-1）时不预估
    if (info_.size > 0) out.reserve(out.size() + static_cast<size_t>(info_.size));

    uint8_t buf[64 * 1024];
    while (true) {
        la_ssize_t n = archive_read_data(archive_, buf, sizeof(buf));
        if (n > 0) {
            out.insert(out.end(), buf, buf + n);
        } else if (n == 0) {
            dataConsumed_ = true;
            return true;
        } else {
            const char* err = archive_error_string(archive_);
            error_ = err ? err : "read data failed";
            dataConsumed_ = true;
            return false;
        }
    }
}

la_ssize_t ArchiveCompressor::Entry::readChunk(void* buffer, size_t size) {
    if (!archive_ || dataConsumed_) return 0;
    la_ssize_t n = archive_read_data(archive_, buffer, size);
    if (n == 0) {
        dataConsumed_ = true;
    } else if (n < 0) {
        const char* err = archive_error_string(archive_);
        error_ = err ? err : "read data failed";
        dataConsumed_ = true;
    }
    return n;
}

bool ArchiveCompressor::Entry::skip() {
    if (!archive_ || dataConsumed_) return true;
    int r = archive_read_data_skip(archive_);
    dataConsumed_ = true;
    if (r != ARCHIVE_OK) {
        const char* err = archive_error_string(archive_);
        error_ = err ? err : "skip failed";
        return false;
    }
    return true;
}

// ============================================================
// Iterator
// ============================================================
ArchiveCompressor::Iterator ArchiveCompressor::begin() { return Iterator(this); }
ArchiveCompressor::Iterator ArchiveCompressor::end()   { return Iterator(); }

ArchiveCompressor::Iterator::Iterator(ArchiveCompressor* owner) : owner_(owner) {
    advance();
}

void ArchiveCompressor::Iterator::advance() {
    if (!owner_ || !owner_->readArchive_) {
        done_    = true;
        current_ = Entry();
        return;
    }
    struct archive_entry* entry = nullptr;
    int r = archive_read_next_header(owner_->readArchive_.get(), &entry);
    if (r == ARCHIVE_OK) {
        current_ = Entry(owner_->readArchive_.get(), entry);
        done_    = false;
    } else {
        if (r != ARCHIVE_EOF) {
            const char* err = archive_error_string(owner_->readArchive_.get());
            owner_->error_ = err ? err : "read header failed";
        }
        current_ = Entry();
        done_    = true;
    }
}

ArchiveCompressor::Iterator& ArchiveCompressor::Iterator::operator++() {
    if (done_) return *this;
    // 用户未读完当前 entry → 主动 skip，避免影响下一 header
    if (!current_.dataConsumed()) current_.skip();
    advance();
    return *this;
}

#endif // WITH_LIBARCHIVE