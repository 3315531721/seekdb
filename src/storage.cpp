// storage.cpp — MiniVectorDB 存储引擎 + 事务 + WAL 崩溃恢复
// 自研页式存储, 不依赖 SQLite/RocksDB 等现成引擎。
//
// ================= 磁盘布局 (全部在 data_dir, 即 /data) =================
//   catalog.dat   表定义目录: [magic][ver][ntables] + 每表 schema。create_table 后 fsync。
//   <table>.tbl   每表一个页式数据文件:
//                   [文件头: magic(4) + ver(4) + schema_len(4) + schema_bytes]
//                   随后定长页(4096B)。行记录追加在页内: [u32 body_len][i64 pk][列数据...]。
//                 行不跨页, 页满新开一页。
//                 内存仅保留 primary -> RowLoc{page,offset,len} 索引; 整行(含向量)按需 pread。
//   wal.log       预写日志(追加写): [magic(4)][ver(4)] 之后每条记录:
//                   [u32 payload_len][u32 crc32][payload]
//                 payload: [u64 txn_id][u8 op]  +
//                    op=INSERT/UPDATE: [str table][i64 pk][列数据...]  (按该表 schema 编码)
//                    op=DELETE:        [str table][i64 pk]
//                    op=DDL_CREATE:    [schema]
//                    op=COMMIT:        (无更多字段)
//                    op=ABORT:         (无更多字段)
//
// ================= 事务语义 =================
//  * 可见性: 基表(磁盘 .tbl) + 事务写集叠加(inserts/updates/deletes)。
//  * 语句级原子性: 每条修改语句先在「临时结构」构造; 成功才并入事务写集, 失败丢弃。
//  * 自动提交: 未显式 BEGIN 时, 修改语句形成隐式事务, 成功即 COMMIT。
//  * COMMIT: 写集编码成 WAL 记录 + COMMIT -> fsync -> 应用到 .tbl -> fsync -> 返回成功。
//  * ROLLBACK: 丢弃写集。
//  * 恢复: open() 先读 catalog, 再扫描 WAL; 只重放完整且含 COMMIT 的事务; 无 COMMIT 整段丢弃。
//          重放后把数据刷入 .tbl 并 checkpoint 截断 WAL。

#include "storage.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mvdb {
namespace {

// ============================ 常量 ============================
constexpr uint32_t kPageSize      = 131072;   // 128KB: 容纳 VARCHAR(65535) 级别的单行记录
constexpr uint32_t kCatalogMagic  = 0x4D564443u;  // "MVDC"
constexpr uint32_t kTableMagic    = 0x4D564454u;  // "MVDT"
constexpr uint32_t kWalMagic      = 0x4D564457u;  // "MVDW"
constexpr uint32_t kFormatVer     = 1u;
constexpr uint32_t kMaxWalPayload = 64u << 20;

[[noreturn]] void fail(ErrCode c, const std::string& m) { throw DbError(c, m); }

// ============================ CRC32 ============================
uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ============================ IO 工具 ============================
void write_full(int fd, const void* buf, size_t n, const char* what) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; fail(ErrCode::IO, std::string("write ") + what + ": " + std::strerror(errno)); }
        off += static_cast<size_t>(w);
    }
}

void pwrite_full(int fd, const void* buf, size_t n, uint64_t off, const char* what) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::pwrite(fd, p + done, n - done, static_cast<off_t>(off + done));
        if (w < 0) { if (errno == EINTR) continue; fail(ErrCode::IO, std::string("pwrite ") + what + ": " + std::strerror(errno)); }
        done += static_cast<size_t>(w);
    }
}

size_t pread_full(int fd, void* buf, size_t n, uint64_t off) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, p + got, n - got, static_cast<off_t>(off + got));
        if (r < 0) { if (errno == EINTR) continue; fail(ErrCode::IO, std::string("pread: ") + std::strerror(errno)); }
        if (r == 0) break;
        got += static_cast<size_t>(r);
    }
    return got;
}

void fsync_fd(int fd, const char* what) {
    if (::fsync(fd) != 0) fail(ErrCode::IO, std::string("fsync ") + what + ": " + std::strerror(errno));
}

std::string dir_of(const std::string& path) {
    auto s = path.find_last_of('/');
    return (s == std::string::npos) ? std::string(".") : path.substr(0, s);
}

void fsync_dir(const std::string& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
}

// ============================ 序列化 ============================
struct Writer {
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back((v >> (8 * i)) & 0xFF); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back((v >> (8 * i)) & 0xFF); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); u32(u); }
    void bytes(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); buf.insert(buf.end(), b, b + n); }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); bytes(s.data(), s.size()); }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
    void need(size_t k) { if (pos + k > n) fail(ErrCode::INTERNAL, "corrupt record: truncated"); }
    uint8_t u8() { need(1); return p[pos++]; }
    uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[pos++]) << (8 * i); return v; }
    uint64_t u64() { need(8); uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[pos++]) << (8 * i); return v; }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    std::string str() { uint32_t len = u32(); need(len); std::string s(reinterpret_cast<const char*>(p + pos), len); pos += len; return s; }
    bool eof() const { return pos >= n; }
};

void write_schema(Writer& w, const TableSchema& s) {
    w.str(s.table_name);
    w.u32(static_cast<uint32_t>(s.columns.size()));
    for (const auto& c : s.columns) {
        w.str(c.name);
        w.u8(static_cast<uint8_t>(c.type));
        w.u32(static_cast<uint32_t>(c.varchar_len));
        w.u32(static_cast<uint32_t>(c.vector_dim));
        w.u8(c.is_primary ? 1 : 0);
    }
    w.u32(static_cast<uint32_t>(s.primary_idx));
    w.u32(static_cast<uint32_t>(s.vector_idx));
}

TableSchema read_schema(Reader& r) {
    TableSchema s;
    s.table_name = r.str();
    uint32_t nc = r.u32();
    if (nc > 4096) fail(ErrCode::INTERNAL, "corrupt schema: too many columns");
    s.columns.resize(nc);
    for (uint32_t i = 0; i < nc; ++i) {
        ColumnDef& c = s.columns[i];
        c.name = r.str();
        c.type = static_cast<ColType>(r.u8());
        c.varchar_len = static_cast<int>(r.u32());
        c.vector_dim = static_cast<int>(r.u32());
        c.is_primary = r.u8() != 0;
    }
    s.primary_idx = static_cast<int>(r.u32());
    s.vector_idx = static_cast<int>(r.u32());
    return s;
}

void write_row(Writer& w, const TableSchema& s, const Row& row) {
    if (row.cells.size() != s.columns.size())
        fail(ErrCode::TYPE_MISMATCH, "row cell count mismatch for table " + s.table_name);
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const Value& v = row.cells[i];
        switch (s.columns[i].type) {
            case ColType::BIGINT: w.i64(v.i); break;
            case ColType::VARCHAR: w.str(v.s); break;
            case ColType::VECTOR:
                w.u32(static_cast<uint32_t>(v.vec.v.size()));
                for (float f : v.vec.v) w.f32(f);
                break;
            case ColType::DOUBLE: {
                uint64_t u; std::memcpy(&u, &v.d, 8); w.u64(u);
                break;
            }
        }
    }
}

Row decode_row(Reader& r, const TableSchema& s) {
    Row row;
    row.cells.resize(s.columns.size());
    for (size_t i = 0; i < s.columns.size(); ++i) {
        Value& v = row.cells[i];
        switch (s.columns[i].type) {
            case ColType::BIGINT: v.type = ColType::BIGINT; v.i = r.i64(); break;
            case ColType::VARCHAR: v.type = ColType::VARCHAR; v.s = r.str(); break;
            case ColType::VECTOR: {
                v.type = ColType::VECTOR;
                uint32_t dim = r.u32();
                if (dim > (1u << 20)) fail(ErrCode::INTERNAL, "corrupt vector dim");
                v.vec.v.resize(dim);
                for (uint32_t k = 0; k < dim; ++k) v.vec.v[k] = r.f32();
                break;
            }
            case ColType::DOUBLE: {
                v.type = ColType::DOUBLE;
                uint64_t u = r.u64(); std::memcpy(&v.d, &u, 8);
                break;
            }
        }
    }
    return row;
}

int64_t primary_of(const TableSchema& s, const Row& row) {
    if (s.primary_idx < 0 || s.primary_idx >= static_cast<int>(row.cells.size()))
        fail(ErrCode::CONSTRAINT, "table has no valid primary key");
    return row.cells[s.primary_idx].i;
}

// ============================ 页式数据文件 ============================
struct RowLoc { uint32_t page_id; uint32_t offset; uint32_t len; };

// 页内记录: [u32 body_len][i64 pk][列数据...]; body_len 不含自身 4 字节。
class TableDataFile {
public:
    ~TableDataFile() { close_fd(); }

    void create(const std::string& path, const TableSchema& schema) {
        path_ = path;
        schema_ = schema;
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) fail(ErrCode::IO, "create table file " + path + ": " + std::strerror(errno));
        Writer sw; write_schema(sw, schema_);
        Writer w; w.u32(kTableMagic); w.u32(kFormatVer); w.u32(static_cast<uint32_t>(sw.buf.size()));
        w.bytes(sw.buf.data(), sw.buf.size());
        write_full(fd_, w.buf.data(), w.buf.size(), "table header");
        fsync_fd(fd_, "table header");
        header_len_ = w.buf.size();
        file_size_ = header_len_;
        page_offsets_.clear(); index_.clear();
        cur_page_ = 0; cur_used_ = 0; has_page_ = false;
    }

    void open_existing(const std::string& path) {
        path_ = path;
        fd_ = ::open(path.c_str(), O_RDWR);
        if (fd_ < 0) fail(ErrCode::IO, "open table file " + path + ": " + std::strerror(errno));
        struct stat st{};
        if (::fstat(fd_, &st) != 0) fail(ErrCode::IO, "fstat failed");
        file_size_ = static_cast<uint64_t>(st.st_size);
        uint8_t hdr[12];
        if (pread_full(fd_, hdr, 12, 0) != 12) fail(ErrCode::IO, "table header truncated");
        Reader hr(hdr, 12);
        if (hr.u32() != kTableMagic || hr.u32() != kFormatVer) fail(ErrCode::IO, "bad table magic: " + path);
        uint32_t slen = hr.u32();
        std::vector<uint8_t> sbuf(slen);
        if (pread_full(fd_, sbuf.data(), slen, 12) != slen) fail(ErrCode::IO, "schema truncated");
        Reader sr(sbuf.data(), sbuf.size());
        schema_ = read_schema(sr);
        header_len_ = 12 + slen;
        rebuild_index();
    }

    const TableSchema& schema() const { return schema_; }
    bool contains(int64_t pk) const { return index_.find(pk) != index_.end(); }
    size_t count() const { return index_.size(); }

    // 流式扫描 (按需读), pk 升序
    void scan(const std::function<void(int64_t, const Row&)>& cb) {
        std::vector<int64_t> keys; keys.reserve(index_.size());
        for (auto& kv : index_) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        Row tmp;
        for (int64_t pk : keys) {
            auto it = index_.find(pk);
            if (it == index_.end()) continue;
            read_row_at(it->second, tmp);
            cb(pk, tmp);
        }
    }

    bool read_row_by_pk(int64_t pk, Row& out) {
        auto it = index_.find(pk);
        if (it == index_.end()) return false;
        read_row_at(it->second, out);
        return true;
    }

    void read_row_at(const RowLoc& loc, Row& out) {
        uint64_t off = page_offsets_[loc.page_id] + loc.offset;
        std::vector<uint8_t> buf(loc.len);
        if (pread_full(fd_, buf.data(), loc.len, off) != loc.len) fail(ErrCode::IO, "row read truncated");
        Reader r(buf.data(), buf.size());
        uint32_t blen = r.u32();
        // 记录体为 [i64 pk][列数据...]; decode_row 从第一列开始, 须跳过 pk
        Reader br(buf.data() + 4 + 8, blen - 8);
        out = decode_row(br, schema_);
    }

    void append(const Row& row) {
        int64_t pk = primary_of(schema_, row);
        Writer body; body.i64(pk); write_row(body, schema_, row);
        if (4 + body.buf.size() > kPageSize) fail(ErrCode::CONSTRAINT, "row too large for a page");
        uint32_t rec_len = static_cast<uint32_t>(4 + body.buf.size());

        if (!has_page_ || cur_used_ + rec_len > kPageSize) {
            uint64_t base = has_page_ ? (page_offsets_.back() + kPageSize) : header_len_;
            page_offsets_.push_back(base);
            cur_page_ = static_cast<uint32_t>(page_offsets_.size() - 1);
            cur_used_ = 0; has_page_ = true;
        }
        uint64_t write_off = page_offsets_[cur_page_] + cur_used_;
        Writer rec; rec.u32(static_cast<uint32_t>(body.buf.size())); rec.bytes(body.buf.data(), body.buf.size());
        pwrite_full(fd_, rec.buf.data(), rec.buf.size(), write_off, "row");
        index_[pk] = RowLoc{cur_page_, cur_used_, rec_len};
        cur_used_ += rec_len;
        if (write_off + rec_len > file_size_) file_size_ = write_off + rec_len;
    }

    void sync() { fsync_fd(fd_, "table data"); }

    // 压实: 保留 alive 行, override_rows 提供新行
    void compact(const std::unordered_set<int64_t>& alive,
                 const std::unordered_map<int64_t, Row>& override_rows) {
        std::string tmp = path_ + ".tmp";
        int tfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (tfd < 0) fail(ErrCode::IO, "create tmp table file");

        Writer sw; write_schema(sw, schema_);
        Writer w; w.u32(kTableMagic); w.u32(kFormatVer); w.u32(static_cast<uint32_t>(sw.buf.size()));
        w.bytes(sw.buf.data(), sw.buf.size());
        write_full(tfd, w.buf.data(), w.buf.size(), "tmp header");
        uint64_t header_len = w.buf.size();
        uint64_t next_page_off = header_len;

        std::vector<uint64_t> new_pages;
        std::unordered_map<int64_t, RowLoc> new_index;
        std::vector<uint8_t> page_buf;
        uint64_t cur_used = 0; bool has_page = false; uint32_t cur_page = 0;

        std::vector<int64_t> keys(alive.begin(), alive.end());
        std::sort(keys.begin(), keys.end());
        Row tmprow;
        for (int64_t pk : keys) {
            auto ov = override_rows.find(pk);
            const Row* rp;
            if (ov != override_rows.end()) rp = &ov->second;
            else { if (!read_row_by_pk(pk, tmprow)) continue; rp = &tmprow; }

            Writer body; body.i64(pk); write_row(body, schema_, *rp);
            if (4 + body.buf.size() > kPageSize) fail(ErrCode::CONSTRAINT, "row too large");
            uint32_t rec_len = static_cast<uint32_t>(4 + body.buf.size());

            if (!has_page || cur_used + rec_len > kPageSize) {
                if (has_page) pwrite_full(tfd, page_buf.data(), page_buf.size(), new_pages.back(), "tmp page");
                new_pages.push_back(next_page_off);
                cur_page = static_cast<uint32_t>(new_pages.size() - 1);
                cur_used = 0; has_page = true; next_page_off += kPageSize; page_buf.clear();
            }
            uint32_t within = static_cast<uint32_t>(cur_used);
            Writer rec; rec.u32(static_cast<uint32_t>(body.buf.size())); rec.bytes(body.buf.data(), body.buf.size());
            page_buf.insert(page_buf.end(), rec.buf.begin(), rec.buf.end());
            cur_used += rec_len;
            new_index[pk] = RowLoc{cur_page, within, rec_len};
        }
        if (has_page && !page_buf.empty()) pwrite_full(tfd, page_buf.data(), page_buf.size(), new_pages.back(), "tmp page");
        fsync_fd(tfd, "tmp table");
        ::close(tfd);

        if (::rename(tmp.c_str(), path_.c_str()) != 0) {
            ::unlink(tmp.c_str());
            fail(ErrCode::IO, "rename tmp table: " + std::string(std::strerror(errno)));
        }
        fsync_dir(dir_of(path_));

        close_fd();
        fd_ = ::open(path_.c_str(), O_RDWR);
        if (fd_ < 0) fail(ErrCode::IO, "reopen compacted table");
        struct stat st{}; ::fstat(fd_, &st);
        file_size_ = static_cast<uint64_t>(st.st_size);
        index_ = std::move(new_index);
        page_offsets_ = std::move(new_pages);
        header_len_ = header_len;
        has_page_ = !page_offsets_.empty();
        cur_page_ = has_page_ ? static_cast<uint32_t>(page_offsets_.size() - 1) : 0;
        cur_used_ = 0;
        if (has_page_) {
            uint64_t off = page_offsets_.back();
            size_t want = static_cast<size_t>(std::min<uint64_t>(kPageSize, file_size_ - off));
            std::vector<uint8_t> pb(want);
            size_t got = pread_full(fd_, pb.data(), want, off);
            size_t pos = 0;
            while (pos + 4 <= got) { Reader rr(pb.data() + pos, got - pos); uint32_t bl = rr.u32(); if (bl == 0 || bl < 8 || pos + 4 + bl > got) break; pos += 4 + bl; }
            cur_used_ = static_cast<uint32_t>(pos);
        }
    }

    void close_fd() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    void rebuild_index() {
        index_.clear(); page_offsets_.clear();
        uint64_t off = header_len_;
        uint32_t page_id = 0;
        uint64_t last_used = 0;
        std::vector<uint8_t> pbuf(kPageSize);
        while (off < file_size_) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(kPageSize, file_size_ - off));
            size_t got = pread_full(fd_, pbuf.data(), want, off);
            if (got == 0) break;
            page_offsets_.push_back(off);
            size_t pos = 0;
            while (pos + 4 <= got) {
                Reader rr(pbuf.data() + pos, got - pos);
                uint32_t bl = rr.u32();
                if (bl == 0) break;                   // 页尾零填充
                if (bl < 8) break;                    // 非法/损坏
                if (pos + 4 + bl > got) break;        // 不完整
                Reader br(pbuf.data() + pos + 4, bl);
                int64_t pk = br.i64();
                index_[pk] = RowLoc{page_id, static_cast<uint32_t>(pos), static_cast<uint32_t>(4 + bl)};
                pos += 4 + bl;
            }
            last_used = pos;
            off += kPageSize; ++page_id;
        }
        has_page_ = !page_offsets_.empty();
        cur_page_ = has_page_ ? static_cast<uint32_t>(page_offsets_.size() - 1) : 0;
        cur_used_ = static_cast<uint32_t>(last_used);
    }

    int fd_ = -1;
    std::string path_;
    TableSchema schema_;
    uint64_t header_len_ = 0;
    uint64_t file_size_ = 0;
    std::vector<uint64_t> page_offsets_;
    std::unordered_map<int64_t, RowLoc> index_;
    uint32_t cur_page_ = 0;
    uint32_t cur_used_ = 0;
    bool has_page_ = false;
};

enum WalOp : uint8_t {
    WAL_INSERT = 1,
    WAL_UPDATE = 2,
    WAL_DELETE = 3,
    WAL_COMMIT = 4,
    WAL_ABORT  = 5,
    WAL_DDL_CREATE = 6,
};

} // namespace

// ============================================================================
// StorageEngineImpl + TableImpl
// ============================================================================
class StorageEngineImpl;

class TableImpl : public Table {
public:
    TableImpl(TableDataFile* data, const TableSchema* schema, StorageEngineImpl* eng)
        : data_(data), schema_(schema), eng_(eng) {}
    const TableSchema& schema() const override { return *schema_; }
    void scan(const std::function<void(int64_t, const Row&)>& fn) override;
    bool get(int64_t primary, Row& out) override;
    size_t size() override;
    TableDataFile* data() { return data_; }
private:
    TableDataFile* data_;
    const TableSchema* schema_;
    StorageEngineImpl* eng_;
};

class StorageEngineImpl : public StorageEngine {
public:
    ~StorageEngineImpl() override { if (opened_ && !closed_) { try { close(); } catch (...) {} } }

    void open(const std::string& data_dir) override;
    void create_table(const TableSchema& schema) override;
    void begin() override;
    void commit() override;
    void rollback() override;
    bool in_txn() override { return in_txn_; }
    Table* get_table(const std::string& name) override;
    void insert_row(const std::string& table, const Row& row) override;
    size_t delete_rows(const std::string& table, const std::function<bool(const Row&)>& pred) override;
    size_t update_rows(const std::string& table, const std::function<bool(const Row&)>& pred,
                       const std::function<Row(const Row&)>& setter) override;
    void close() override;

    // 供 TableImpl 使用
    bool visible_get(TableDataFile* data, int64_t pk, Row& out);
    void visible_scan(TableDataFile* data, const std::function<void(int64_t, const Row&)>& cb);
    size_t visible_size(TableDataFile* data);

private:
    struct RowWrite { Row row; bool deleted = false; };
    struct TableEntry {
        TableSchema schema;
        std::unique_ptr<TableDataFile> data;
        std::unique_ptr<TableImpl> table;
    };

    TableEntry* find_table(const std::string& name, ErrCode err = ErrCode::TABLE_NOT_FOUND);
    std::string table_name_of(TableDataFile* data);
    void load_catalog();
    void save_catalog();
    void apply_write_set_to_disk();
    void replay_wal();
    void checkpoint_wal();
    uint64_t next_txn_id();

    std::string dir_;
    bool opened_ = false;
    bool closed_ = false;
    std::unordered_map<std::string, std::unique_ptr<TableEntry>> tables_;
    int catalog_fd_ = -1;
    int wal_fd_ = -1;
    uint64_t txn_counter_ = 0;

    bool in_txn_ = false;
    uint64_t cur_txn_id_ = 0;
    std::unordered_map<std::string, std::unordered_map<int64_t, RowWrite>> write_set_;
    std::vector<TableSchema> pending_ddl_;
};

// ---------------------------------------------------------------- TableImpl
void TableImpl::scan(const std::function<void(int64_t, const Row&)>& fn) { eng_->visible_scan(data_, fn); }
bool TableImpl::get(int64_t primary, Row& out) { return eng_->visible_get(data_, primary, out); }
size_t TableImpl::size() { return eng_->visible_size(data_); }

// ---------------------------------------------------------------- 可见性
std::string StorageEngineImpl::table_name_of(TableDataFile* data) {
    for (auto& kv : tables_) if (kv.second->data.get() == data) return kv.first;
    return std::string();
}

bool StorageEngineImpl::visible_get(TableDataFile* data, int64_t pk, Row& out) {
    if (in_txn_) {
        std::string tn = table_name_of(data);
        if (!tn.empty()) {
            auto wit = write_set_.find(tn);
            if (wit != write_set_.end()) {
                auto r = wit->second.find(pk);
                if (r != wit->second.end()) {
                    if (r->second.deleted) return false;
                    out = r->second.row; return true;
                }
            }
        }
    }
    return data->read_row_by_pk(pk, out);
}

void StorageEngineImpl::visible_scan(TableDataFile* data, const std::function<void(int64_t, const Row&)>& cb) {
    std::string tn = table_name_of(data);
    std::unordered_set<int64_t> emitted;
    data->scan([&](int64_t pk, const Row& row) {
        if (in_txn_ && !tn.empty()) {
            auto wit = write_set_.find(tn);
            if (wit != write_set_.end()) {
                auto r = wit->second.find(pk);
                if (r != wit->second.end()) {
                    emitted.insert(pk);
                    if (r->second.deleted) return;
                    cb(pk, r->second.row); return;
                }
            }
        }
        emitted.insert(pk);
        cb(pk, row);
    });
    if (in_txn_ && !tn.empty()) {
        auto wit = write_set_.find(tn);
        if (wit != write_set_.end()) {
            std::vector<int64_t> extra;
            for (auto& kv : wit->second)
                if (!kv.second.deleted && emitted.find(kv.first) == emitted.end()) extra.push_back(kv.first);
            std::sort(extra.begin(), extra.end());
            for (int64_t pk : extra) cb(pk, wit->second[pk].row);
        }
    }
}

size_t StorageEngineImpl::visible_size(TableDataFile* data) {
    size_t n = 0;
    visible_scan(data, [&](int64_t, const Row&) { ++n; });
    return n;
}

// ---------------------------------------------------------------- catalog
StorageEngineImpl::TableEntry* StorageEngineImpl::find_table(const std::string& name, ErrCode err) {
    auto it = tables_.find(name);
    if (it == tables_.end()) fail(err, "table not found: " + name);
    return it->second.get();
}

void StorageEngineImpl::load_catalog() {
    std::string p = dir_ + "/catalog.dat";
    catalog_fd_ = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
    if (catalog_fd_ < 0) fail(ErrCode::IO, "open catalog: " + std::string(std::strerror(errno)));
    struct stat st{};
    ::fstat(catalog_fd_, &st);
    if (st.st_size < 12) return;
    std::vector<uint8_t> buf(static_cast<size_t>(st.st_size));
    if (pread_full(catalog_fd_, buf.data(), buf.size(), 0) != buf.size()) fail(ErrCode::IO, "catalog read");
    Reader r(buf.data(), buf.size());
    if (r.u32() != kCatalogMagic || r.u32() != kFormatVer) fail(ErrCode::IO, "bad catalog magic");
    uint32_t nt = r.u32();
    for (uint32_t i = 0; i < nt; ++i) {
        TableSchema s = read_schema(r);
        auto e = std::make_unique<TableEntry>();
        e->schema = s;
        e->data = std::make_unique<TableDataFile>();
        e->table = std::make_unique<TableImpl>(e->data.get(), &e->schema, this);
        tables_[s.table_name] = std::move(e);
    }
}

void StorageEngineImpl::save_catalog() {
    Writer w;
    w.u32(kCatalogMagic); w.u32(kFormatVer);
    w.u32(static_cast<uint32_t>(tables_.size()));
    for (auto& kv : tables_) write_schema(w, kv.second->schema);
    if (::ftruncate(catalog_fd_, 0) != 0) fail(ErrCode::IO, "truncate catalog");
    pwrite_full(catalog_fd_, w.buf.data(), w.buf.size(), 0, "catalog");
    fsync_fd(catalog_fd_, "catalog");
    fsync_dir(dir_);
}

// ---------------------------------------------------------------- open
void StorageEngineImpl::open(const std::string& data_dir) {
    if (opened_) fail(ErrCode::TXN_ERROR, "already open");
    dir_ = data_dir;
    // 递归创建数据目录 (支持 --data-dir a/b/c)
    if (!dir_.empty()) {
        std::string partial;
        size_t pos = 0;
        while (pos <= dir_.size()) {
            size_t slash = dir_.find('/', pos);
            if (slash == std::string::npos) slash = dir_.size();
            partial = dir_.substr(0, slash);
            if (!partial.empty() && partial != "/") {
                struct stat st{};
                if (::stat(partial.c_str(), &st) != 0) {
                    if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
                        fail(ErrCode::IO, "mkdir data dir: " + std::string(std::strerror(errno)));
                } else if (!S_ISDIR(st.st_mode)) {
                    fail(ErrCode::IO, "data path is not a directory: " + partial);
                }
            }
            if (slash == dir_.size()) break;
            pos = slash + 1;
        }
    }

    load_catalog();
    for (auto& kv : tables_) kv.second->data->open_existing(dir_ + "/" + kv.first + ".tbl");

    std::string wal_path = dir_ + "/wal.log";
    wal_fd_ = ::open(wal_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal_fd_ < 0) fail(ErrCode::IO, "open WAL: " + std::string(std::strerror(errno)));
    {
        struct stat ws{};
        ::fstat(wal_fd_, &ws);
        if (ws.st_size == 0) {
            Writer w; w.u32(kWalMagic); w.u32(kFormatVer);
            write_full(wal_fd_, w.buf.data(), w.buf.size(), "wal header");
            fsync_fd(wal_fd_, "wal header");
        }
    }
    replay_wal();
    checkpoint_wal();
    opened_ = true;
}

// ---------------------------------------------------------------- DDL
void StorageEngineImpl::create_table(const TableSchema& schema) {
    if (!opened_) fail(ErrCode::TXN_ERROR, "engine not open");
    if (schema.table_name.empty()) fail(ErrCode::CONSTRAINT, "empty table name");
    if (tables_.count(schema.table_name)) fail(ErrCode::TABLE_EXISTS, "table exists: " + schema.table_name);
    if (schema.primary_idx < 0 || schema.primary_idx >= static_cast<int>(schema.columns.size()))
        fail(ErrCode::CONSTRAINT, "table must have a primary key column");

    auto e = std::make_unique<TableEntry>();
    e->schema = schema;
    e->data = std::make_unique<TableDataFile>();
    e->table = std::make_unique<TableImpl>(e->data.get(), &e->schema, this);
    std::string p = dir_ + "/" + schema.table_name + ".tbl";
    e->data->create(p, schema);
    tables_[schema.table_name] = std::move(e);

    if (in_txn_) {
        pending_ddl_.push_back(schema);   // catalog 在 commit 时落盘
        return;
    }
    save_catalog();
}

// ---------------------------------------------------------------- 事务
uint64_t StorageEngineImpl::next_txn_id() {
    timespec ts{}; ::clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t base = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    return base ^ (txn_counter_++);
}

void StorageEngineImpl::begin() {
    if (!opened_) fail(ErrCode::TXN_ERROR, "engine not open");
    if (in_txn_) fail(ErrCode::TXN_ERROR, "transaction already active");
    in_txn_ = true;
    cur_txn_id_ = next_txn_id();
    write_set_.clear();
    pending_ddl_.clear();
}

void StorageEngineImpl::rollback() {
    if (!in_txn_) fail(ErrCode::TXN_ERROR, "no active transaction");
    for (const auto& s : pending_ddl_) {
        auto it = tables_.find(s.table_name);
        if (it != tables_.end()) {
            it->second->data->close_fd();
            ::unlink((dir_ + "/" + s.table_name + ".tbl").c_str());
            tables_.erase(it);
        }
    }
    write_set_.clear();
    pending_ddl_.clear();
    in_txn_ = false;
    cur_txn_id_ = 0;
}

void StorageEngineImpl::apply_write_set_to_disk() {
    if (!pending_ddl_.empty()) save_catalog();
    for (auto& tw : write_set_) {
        TableEntry* te = find_table(tw.first);
        bool need_compact = false;
        for (auto& rw : tw.second) {
            if (rw.second.deleted) { need_compact = true; continue; }
            int64_t pk = rw.first;
            if (te->data->contains(pk)) need_compact = true;
            else te->data->append(rw.second.row);
        }
        if (need_compact) {
            std::unordered_set<int64_t> alive;
            te->data->scan([&](int64_t pk, const Row&) {
                auto it = tw.second.find(pk);
                if (it != tw.second.end() && it->second.deleted) return;
                alive.insert(pk);
            });
            std::unordered_map<int64_t, Row> overs;
            for (auto& rw : tw.second)
                if (!rw.second.deleted) { alive.insert(rw.first); overs[rw.first] = rw.second.row; }
            te->data->compact(alive, overs);
        }
        te->data->sync();
    }
}

void StorageEngineImpl::commit() {
    if (!in_txn_) fail(ErrCode::TXN_ERROR, "no active transaction");

    // 1) 构造 WAL 批 (所有 ops + COMMIT), 追加并 fsync
    Writer batch;
    auto emit = [&](Writer& p) {
        uint32_t crc = crc32(p.buf.data(), p.buf.size());
        batch.u32(static_cast<uint32_t>(p.buf.size()));
        batch.u32(crc);
        batch.bytes(p.buf.data(), p.buf.size());
    };
    for (const auto& s : pending_ddl_) {
        Writer p; p.u64(cur_txn_id_); p.u8(WAL_DDL_CREATE); write_schema(p, s); emit(p);
    }
    for (auto& tw : write_set_) {
        TableEntry* te = find_table(tw.first);
        for (auto& rw : tw.second) {
            Writer p; p.u64(cur_txn_id_);
            if (rw.second.deleted) { p.u8(WAL_DELETE); p.str(tw.first); p.i64(rw.first); }
            else { p.u8(WAL_INSERT); p.str(tw.first); p.i64(rw.first); write_row(p, te->schema, rw.second.row); }
            emit(p);
        }
    }
    { Writer p; p.u64(cur_txn_id_); p.u8(WAL_COMMIT); emit(p); }

    write_full(wal_fd_, batch.buf.data(), batch.buf.size(), "wal commit");
    fsync_fd(wal_fd_, "wal commit");   // <-- 提交成功响应前持久化点

    // 2) 应用到数据文件 + catalog
    apply_write_set_to_disk();

    // 3) checkpoint 截断 WAL
    checkpoint_wal();

    // 4) 清理
    write_set_.clear();
    pending_ddl_.clear();
    in_txn_ = false;
    cur_txn_id_ = 0;
}

// ---------------------------------------------------------------- 恢复
void StorageEngineImpl::replay_wal() {
    struct stat st{};
    ::fstat(wal_fd_, &st);
    uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz < 8) return;
    uint8_t hdr[8];
    if (pread_full(wal_fd_, hdr, 8, 0) != 8) return;
    Reader hr(hdr, 8);
    if (hr.u32() != kWalMagic) fail(ErrCode::IO, "bad WAL magic");

    // 解析
    struct DdlRec { TableSchema schema; };
    struct DataRec { std::string table; int64_t pk; std::vector<uint8_t> body; };  // body=列数据原始字节
    struct DelRec  { std::string table; int64_t pk; };
    struct Txn {
        uint64_t id = 0; bool committed = false;
        std::vector<DdlRec> ddls;
        std::vector<DataRec> datas;
        std::vector<DelRec> dels;
    };
    std::unordered_map<uint64_t, size_t> txn_pos;
    std::vector<Txn> txns;

    uint64_t off = 8;
    std::vector<uint8_t> buf;
    while (off + 8 <= fsz) {
        uint8_t rh[8];
        if (pread_full(wal_fd_, rh, 8, off) != 8) break;
        Reader rr(rh, 8);
        uint32_t plen = rr.u32(); uint32_t crc = rr.u32();
        if (plen > kMaxWalPayload) break;
        if (off + 8 + plen > fsz) break;
        buf.resize(plen);
        if (pread_full(wal_fd_, buf.data(), plen, off + 8) != plen) break;
        if (crc32(buf.data(), plen) != crc) break;    // 校验失败 -> 停 (尾部残记录)

        Reader pr(buf.data(), plen);
        uint64_t txn_id = pr.u64();
        uint8_t op = pr.u8();
        size_t idx;
        auto it = txn_pos.find(txn_id);
        if (it == txn_pos.end()) { idx = txns.size(); txn_pos[txn_id] = idx; txns.emplace_back(); txns[idx].id = txn_id; }
        else idx = it->second;

        if (op == WAL_COMMIT) txns[idx].committed = true;
        else if (op == WAL_ABORT) { txns[idx].datas.clear(); txns[idx].dels.clear(); }
        else if (op == WAL_DDL_CREATE) { DdlRec d; d.schema = read_schema(pr); txns[idx].ddls.push_back(std::move(d)); }
        else {
            DataRec d; d.table = pr.str(); d.pk = pr.i64();
            if (op == WAL_INSERT || op == WAL_UPDATE) {
                // 剩余字节 = 行数据 (按表 schema 编码), 延迟解码
                d.body.assign(buf.data() + pr.pos, buf.data() + plen);
                txns[idx].datas.push_back(std::move(d));
            } else if (op == WAL_DELETE) {
                DelRec dr; dr.table = d.table; dr.pk = d.pk;
                txns[idx].dels.push_back(std::move(dr));
            }
        }
        off += 8 + plen;
    }

    bool need_save_catalog = false;
    // 第一遍: 应用所有 committed 事务的 DDL (建表)
    for (auto& t : txns) {
        if (!t.committed) continue;
        for (auto& d : t.ddls) {
            const TableSchema& s = d.schema;
            if (tables_.count(s.table_name)) continue;   // catalog 已含
            auto e = std::make_unique<TableEntry>();
            e->schema = s;
            e->data = std::make_unique<TableDataFile>();
            e->table = std::make_unique<TableImpl>(e->data.get(), &e->schema, this);
            e->data->create(dir_ + "/" + s.table_name + ".tbl", s);
            tables_[s.table_name] = std::move(e);
            need_save_catalog = true;
        }
    }
    // 第二遍: 应用数据 (先解码, 再按表批量落盘)
    for (auto& t : txns) {
        if (!t.committed) continue;
        // 表 -> pk -> 新行; 表 -> 删除 pk
        std::unordered_map<std::string, std::unordered_map<int64_t, Row>> upserts;
        std::unordered_map<std::string, std::unordered_set<int64_t>> deletes;
        for (auto& d : t.datas) {
            TableEntry* te = find_table(d.table);
            Reader br(d.body.data(), d.body.size());
            upserts[d.table][d.pk] = decode_row(br, te->schema);
        }
        for (auto& d : t.dels) { find_table(d.table); deletes[d.table].insert(d.pk); }

        std::unordered_set<std::string> touched;
        for (auto& kv : upserts) touched.insert(kv.first);
        for (auto& kv : deletes) touched.insert(kv.first);

        for (const std::string& tname : touched) {
            TableEntry* te = find_table(tname);
            auto& up = upserts[tname];
            auto& del = deletes[tname];
            bool need_compact = !del.empty();
            for (auto& kv : up) if (te->data->contains(kv.first)) need_compact = true;
            if (!need_compact) {
                for (auto& kv : up) te->data->append(kv.second);
            } else {
                std::unordered_set<int64_t> alive;
                te->data->scan([&](int64_t pk, const Row&) { if (!del.count(pk)) alive.insert(pk); });
                for (auto& kv : up) alive.insert(kv.first);
                te->data->compact(alive, up);
            }
            te->data->sync();
        }
    }
    if (need_save_catalog) save_catalog();
}

void StorageEngineImpl::checkpoint_wal() {
    if (wal_fd_ < 0) return;
    if (::ftruncate(wal_fd_, 8) != 0) fail(ErrCode::IO, "truncate WAL: " + std::string(std::strerror(errno)));
    ::lseek(wal_fd_, 8, SEEK_SET);
    fsync_fd(wal_fd_, "wal checkpoint");
}

// ---------------------------------------------------------------- 数据访问
Table* StorageEngineImpl::get_table(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) fail(ErrCode::TABLE_NOT_FOUND, "table not found: " + name);
    return it->second->table.get();
}

// ---------------------------------------------------------------- 修改语句
void StorageEngineImpl::insert_row(const std::string& table, const Row& row) {
    TableEntry* te = find_table(table);
    if (row.cells.size() != te->schema.columns.size())
        fail(ErrCode::TYPE_MISMATCH, "insert cell count mismatch for " + table);
    int64_t pk = primary_of(te->schema, row);

    bool autocommit = !in_txn_;
    if (autocommit) begin();
    try {
        Row tmp;
        if (visible_get(te->data.get(), pk, tmp))
            fail(ErrCode::DUPLICATE_KEY, "duplicate primary key: " + std::to_string(pk));
        RowWrite w; w.row = row; w.deleted = false;
        write_set_[table][pk] = w;
        if (autocommit) commit();
    } catch (...) {
        if (autocommit) { try { rollback(); } catch (...) {} }
        throw;
    }
}

size_t StorageEngineImpl::delete_rows(const std::string& table, const std::function<bool(const Row&)>& pred) {
    TableEntry* te = find_table(table);
    bool autocommit = !in_txn_;
    if (autocommit) begin();
    std::vector<int64_t> victims;
    try {
        visible_scan(te->data.get(), [&](int64_t pk, const Row& r) { if (pred(r)) victims.push_back(pk); });
        for (int64_t pk : victims) { RowWrite w; w.deleted = true; write_set_[table][pk] = w; }
        if (autocommit) commit();
    } catch (...) {
        if (autocommit) { try { rollback(); } catch (...) {} }
        throw;
    }
    return victims.size();
}

size_t StorageEngineImpl::update_rows(const std::string& table, const std::function<bool(const Row&)>& pred,
                                      const std::function<Row(const Row&)>& setter) {
    TableEntry* te = find_table(table);
    bool autocommit = !in_txn_;
    if (autocommit) begin();
    std::vector<std::pair<int64_t, Row>> updates;
    try {
        visible_scan(te->data.get(), [&](int64_t pk, const Row& r) {
            if (pred(r)) updates.push_back({pk, setter(r)});
        });
        for (auto& u : updates) {
            if (u.second.cells.size() != te->schema.columns.size())
                fail(ErrCode::TYPE_MISMATCH, "update row cell count mismatch");
            if (primary_of(te->schema, u.second) != u.first)
                fail(ErrCode::CONSTRAINT, "primary key update not supported");
        }
        for (auto& u : updates) { RowWrite w; w.row = u.second; w.deleted = false; write_set_[table][u.first] = w; }
        if (autocommit) commit();
    } catch (...) {
        if (autocommit) { try { rollback(); } catch (...) {} }
        throw;
    }
    return updates.size();
}

// ---------------------------------------------------------------- close
void StorageEngineImpl::close() {
    if (closed_) return;
    if (in_txn_) { try { rollback(); } catch (...) {} }
    for (auto& kv : tables_) if (kv.second->data) kv.second->data->sync();
    if (catalog_fd_ >= 0) { ::fsync(catalog_fd_); ::close(catalog_fd_); catalog_fd_ = -1; }
    if (wal_fd_ >= 0) {
        try { if (opened_) checkpoint_wal(); } catch (...) {}
        ::close(wal_fd_); wal_fd_ = -1;
    }
    for (auto& kv : tables_) kv.second->data->close_fd();
    closed_ = true;
}

// ============================================================================
std::unique_ptr<StorageEngine> make_storage_engine() {
    return std::unique_ptr<StorageEngine>(new StorageEngineImpl());
}

} // namespace mvdb
