// types.h — MiniVectorDB 核心公共类型 (契约文件, 所有模块共享)
// 语言: C++17。禁止使用 SQLite/RocksDB/Faiss/VSAG/HNSWlib 等现成引擎。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>
#include <stdexcept>

namespace mvdb {

// ============ 错误语义 (对应官方 .result 中的 error 字符串) ============
enum class ErrCode : int {
    OK = 0,
    SYNTAX,             // -> "syntax_error"
    TABLE_NOT_FOUND,    // -> "table_not_found"
    TABLE_EXISTS,       // -> "table_exists"
    COLUMN_NOT_FOUND,   // -> "column_not_found"
    DUPLICATE_KEY,      // -> "duplicate_key"
    TYPE_MISMATCH,      // -> "type_mismatch"
    DIMENSION_MISMATCH, // -> "dimension_mismatch"
    INVALID_VECTOR,     // -> "invalid_vector"
    VARCHAR_TOO_LONG,   // -> "varchar_too_long"
    UNSUPPORTED,        // -> "unsupported_sql"
    TXN_ERROR,          // -> "transaction_error"
    CONSTRAINT,         // -> "constraint_violation"
    INTERNAL,           // -> "internal_error"
    IO,                 // -> "io_error"
};

// 语义名 -> 官方 error.code 字符串
inline const char* err_code_name(ErrCode c) {
    switch (c) {
        case ErrCode::OK: return "ok";
        case ErrCode::SYNTAX: return "syntax_error";
        case ErrCode::TABLE_NOT_FOUND: return "table_not_found";
        case ErrCode::TABLE_EXISTS: return "table_exists";
        case ErrCode::COLUMN_NOT_FOUND: return "column_not_found";
        case ErrCode::DUPLICATE_KEY: return "duplicate_key";
        case ErrCode::TYPE_MISMATCH: return "type_mismatch";
        case ErrCode::DIMENSION_MISMATCH: return "dimension_mismatch";
        case ErrCode::INVALID_VECTOR: return "invalid_vector";
        case ErrCode::VARCHAR_TOO_LONG: return "varchar_too_long";
        case ErrCode::UNSUPPORTED: return "unsupported_sql";
        case ErrCode::TXN_ERROR: return "transaction_error";
        case ErrCode::CONSTRAINT: return "constraint_violation";
        case ErrCode::INTERNAL: return "internal_error";
        case ErrCode::IO: return "io_error";
    }
    return "internal_error";
}

struct DbError : public std::runtime_error {
    ErrCode code;
    DbError(ErrCode c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

// ============ 值类型 ============
enum class ColType { BIGINT, VARCHAR, VECTOR, DOUBLE };

// 单个向量分量用 float32 (题目要求存储 float32 语义)
struct Vector {
    std::vector<float> v;
    size_t dim() const { return v.size(); }
};

// 一个字段值
struct Value {
    ColType type = ColType::BIGINT;
    int64_t i = 0;              // BIGINT
    std::string s;              // VARCHAR
    Vector vec;                 // VECTOR
    double d = 0.0;             // DOUBLE (仅用于 distance 结果列)
    bool is_null = false;       // 本题不支持 NULL, 保留占位

    static Value of_int(int64_t x) { Value v; v.type = ColType::BIGINT; v.i = x; return v; }
    static Value of_str(std::string x) { Value v; v.type = ColType::VARCHAR; v.s = std::move(x); return v; }
    static Value of_vec(Vector x) { Value v; v.type = ColType::VECTOR; v.vec = std::move(x); return v; }
    static Value of_double(double x) { Value v; v.type = ColType::DOUBLE; v.d = x; return v; }
};

// ============ 表结构 ============
struct ColumnDef {
    std::string name;
    ColType type;
    int varchar_len = 0;   // VARCHAR(n) 的 n (Unicode 字符数)
    int vector_dim = 0;    // VECTOR(n) 的 n
    bool is_primary = false;
};

struct TableSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;   // 列顺序即插入顺序
    int primary_idx = -1;             // 主键列下标
    int vector_idx = -1;              // 向量列下标 (最多一个)
};

// ============ 行 ============
struct Row {
    std::vector<Value> cells;   // 与 schema.columns 一一对应
};

// ============ 常量 ============
constexpr double DIST_EPS = 1e-9;

// ============ 工具函数 ============
size_t utf8_length(const std::string& s);
int utf8_compare(const std::string& a, const std::string& b);

} // namespace mvdb
