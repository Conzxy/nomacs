#pragma once

#ifdef WITH_LIBARCHIVE

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

class ArchiveCompressor {
public:
    struct TimeInfo {
        std::time_t sec  = 0;
        long        nsec = 0;

        // 字段缺失时 libarchive 返回 0；用 sec/nsec 同时为 0 判定"未设置"
        bool isSet() const { return sec != 0 || nsec != 0; }

        std::chrono::system_clock::time_point toTimePoint() const {
            using namespace std::chrono;
            auto d = seconds(sec) + nanoseconds(nsec);
            return system_clock::time_point(
                duration_cast<system_clock::duration>(d));
        }
    };

    // 纯元数据快照：值语义，可在 Entry 失效后继续持有
    struct EntryInfo {
        std::string path;
        int64_t     size     = 0;
        mode_t      filetype = 0;   // AE_IFREG / AE_IFDIR / AE_IFLNK ...
        mode_t      perm     = 0;   // 权限位
        TimeInfo    mtime, atime, ctime, birthtime;
        int64_t     uid = 0, gid = 0;
        std::string uname, gname;
        std::string symlink, hardlink;
        bool        encrypted = false;
    };

    class Entry;
    class Iterator;

    explicit ArchiveCompressor(const std::string& filepath);
    explicit ArchiveCompressor(const void* data, size_t size);
    ~ArchiveCompressor();

    ArchiveCompressor(const ArchiveCompressor&) = delete;
    ArchiveCompressor& operator=(const ArchiveCompressor&) = delete;
    ArchiveCompressor(ArchiveCompressor&&) noexcept = default;
    ArchiveCompressor& operator=(ArchiveCompressor&&) noexcept = default;

    bool listEntries(std::vector<std::string>& entries);
    bool extractToMemory(const std::string& entryPath, std::vector<uint8_t>& output);
    bool extractToDisk(const std::string& entryPath, const std::string& dir = ".");
    bool createArchive(const std::string& outputPath,
                       const std::vector<std::string>& sourceFiles,
                       const std::string& format = "zip");
    void closeRead();

    const std::string& lastError() const { return error_; }

    Iterator begin();
    Iterator end();

    // ============================================================
    // Entry：元数据 + 数据读取
    // ============================================================
    class Entry {
    public:
        Entry() = default;
        Entry(struct archive* a, struct archive_entry* e);

        // 完整元数据快照（可安全拷贝保存）
        const EntryInfo& info() const { return info_; }

        // 常用字段转发，避免 info().xxx 的噪音
        const std::string& path()       const { return info_.path; }
        int64_t            size()       const { return info_.size; }
        bool isDirectory()              const { return info_.filetype == AE_IFDIR; }
        bool isRegularFile()            const { return info_.filetype == AE_IFREG; }
        bool isSymlink()                const { return info_.filetype == AE_IFLNK; }
        mode_t filetype()               const { return info_.filetype; }
        mode_t perm()                   const { return info_.perm; }

        const TimeInfo& mtime()         const { return info_.mtime; }
        const TimeInfo& atime()         const { return info_.atime; }
        const TimeInfo& ctime()         const { return info_.ctime; }
        const TimeInfo& birthtime()     const { return info_.birthtime; }

        int64_t uid()                   const { return info_.uid; }
        int64_t gid()                   const { return info_.gid; }
        const std::string& uname()      const { return info_.uname; }
        const std::string& gname()      const { return info_.gname; }

        const std::string& symlink()    const { return info_.symlink; }
        const std::string& hardlink()   const { return info_.hardlink; }
        bool isEncrypted()              const { return info_.encrypted; }

        // 数据读取
        bool readAll(std::vector<uint8_t>& out) const;
        la_ssize_t readChunk(void* buffer, size_t size);
        bool skip();
        bool dataConsumed() const { return dataConsumed_; }
        const std::string& lastError() const { return error_; }

    private:
        friend class Iterator;
        struct archive*       archive_      = nullptr;
        struct archive_entry* entry_        = nullptr;
        EntryInfo             info_;
        mutable bool                  dataConsumed_ = false;
        mutable std::string           error_;
    };

    // ============================================================
    // Iterator：input_iterator，single-pass
    // ============================================================
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = Entry;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Entry*;
        using reference         = const Entry&;

        Iterator() = default;
        explicit Iterator(ArchiveCompressor* owner);

        reference operator*()  const { return current_; }
        pointer   operator->() const { return &current_; }

        Iterator& operator++();
        Iterator  operator++(int) { Iterator t = *this; ++(*this); return t; }

        bool operator==(const Iterator& other) const { return done_ && other.done_; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }

    private:
        void advance();
        ArchiveCompressor* owner_ = nullptr;
        Entry       current_;
        bool        done_  = true;
    };

private:
    struct ArchiveDeleter {
        void operator()(struct archive* a) const { if (a) archive_read_free(a); }
    };
    using ArchivePtr = std::unique_ptr<struct archive, ArchiveDeleter>;

    ArchivePtr  readArchive_;
    std::string error_;
};

#endif // WITH_LIBARCHIVE