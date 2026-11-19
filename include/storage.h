// storage.h — 存储引擎与事务管理器接口 (契约文件)
// 自研页式存储 + WAL。禁止 SQLite/RocksDB。
//
// 设计原则:
//   - 数据/日志统一落在 data_dir (即 /data)。
//   - 支持数据集超内存: 分块/按需读取, 不得全量载入内存。
//   - 事务: 单显式事务; 语句级原子性; 提交前 fsync 保证持久性。
//   - 恢复: 进程强杀后重放 WAL, 已提交不丢, 未提交回滚。
#pragma once
#include "types.h"
#include <string>
#include <functional>

namespace mvdb {

// 一个持久化的表(含 schema + 行集合), 由 StorageEngine 管理。
class Table {
public:
    virtual ~Table() = default;
    virtual const TableSchema& schema() const = 0;
    // 扫描所有事务可见的行; 回调 (primary, const Row&)
    virtual void scan(const std::function<void(int64_t, const Row&)>& fn) = 0;
    // 按主键查
    virtual bool get(int64_t primary, Row& out) = 0;
    // 行数
    virtual size_t size() = 0;
};

// 存储引擎: 管理目录、表、事务、恢复。
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    // 打开/初始化数据目录; 若存在 WAL/数据文件则执行恢复。返回是否完成恢复。
    virtual void open(const std::string& data_dir) = 0;

    // ---- DDL ----
    virtual void create_table(const TableSchema& schema) = 0;

    // ---- 事务控制 ----
    // 未显式 BEGIN 时, 每条修改语句自动提交 (调用方 ensure 自动事务)。
    virtual void begin() = 0;
    virtual void commit() = 0;     // 必须 fsync 后再返回
    virtual void rollback() = 0;
    virtual bool in_txn() = 0;

    // ---- 数据访问 (事务内可见自己的修改) ----
    virtual Table* get_table(const std::string& name) = 0;

    // ---- 数据修改 (作用于当前事务写集, 提交时落盘) ----
    virtual void insert_row(const std::string& table, const Row& row) = 0;
    // 删除满足条件的行, 返回删除数
    virtual size_t delete_rows(const std::string& table,
                               const std::function<bool(const Row&)>& pred) = 0;
    // 更新满足条件的行, setter 返回新 Row; 返回更新数
    virtual size_t update_rows(const std::string& table,
                               const std::function<bool(const Row&)>& pred,
                               const std::function<Row(const Row&)>& setter) = 0;

    // ---- 关闭 ----
    virtual void close() = 0;
};

// 工厂
std::unique_ptr<StorageEngine> make_storage_engine();

} // namespace mvdb
