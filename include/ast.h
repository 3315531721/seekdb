// ast.h — SQL 解析结果的抽象语法树 (契约文件)
// 解析器 (parser) 产出 AST; 执行器 (executor) 消费 AST。
#pragma once
#include "types.h"
#include <memory>

namespace mvdb {

// ---- 查询投影项 ----
struct SelectItem {
    std::string col_name;       // 普通列名
    bool is_distance = false;   // 是否为 L2_DISTANCE(col, '[...]')
    std::string alias;          // AS 别名; 空则默认用 col_name / "distance"
    std::vector<float> query_vec; // is_distance 时的查询向量
};

// ---- 比较条件: col <op> literal ----
enum class CmpOp { EQ, LT, LE, GT, GE };

struct Condition {
    std::string col_name;
    CmpOp op;
    Value value;
};

// ---- 语句 ----
enum class StmtKind {
    CREATE_TABLE, INSERT, SELECT, UPDATE, DELETE_STMT,
    CREATE_VECTOR_INDEX,
    BEGIN, COMMIT, ROLLBACK,
    UNKNOWN
};

struct Stmt {
    virtual ~Stmt() = default;
    StmtKind kind = StmtKind::UNKNOWN;
};
using StmtPtr = std::unique_ptr<Stmt>;

// CREATE TABLE
struct CreateTableStmt : Stmt {
    std::string table;
    std::vector<ColumnDef> columns;
    CreateTableStmt() { kind = StmtKind::CREATE_TABLE; }
};

// INSERT INTO t (cols...) VALUES (...), (...)
struct InsertStmt : Stmt {
    std::string table;
    std::vector<std::string> columns;          // 可为空 => 按 schema 顺序
    std::vector<std::vector<Value>> rows;      // 每行值
    InsertStmt() { kind = StmtKind::INSERT; }
};

// SELECT
struct SelectStmt : Stmt {
    std::string table;
    std::vector<SelectItem> projection;   // 空 且 select_star=false => 按 schema 全部列
    bool select_star = false;
    std::vector<Condition> conditions;     // AND 连接
    // 向量检索
    bool has_vector_query = false;         // 投影/ORDER BY 中出现 L2_DISTANCE
    std::vector<float> query_vector;       // 查询向量
    int topk = -1;                          // LIMIT 值 (-1 无)
    bool is_ann = false;                    // ann 模式
    // 排序
    bool has_order_by = false;
    std::string order_by;                  // 列名或别名
    bool order_by_is_distance = false;
    bool asc = true;
    int limit = -1;
    int vector_col_idx = -1;               // 由 executor 结合 schema 解析
    SelectStmt() { kind = StmtKind::SELECT; }
};

// UPDATE t SET col=val,... WHERE ...
struct UpdateStmt : Stmt {
    std::string table;
    std::vector<std::pair<std::string, Value>> sets;
    std::vector<Condition> conditions;
    UpdateStmt() { kind = StmtKind::UPDATE; }
};

// DELETE FROM t WHERE ...
struct DeleteStmt : Stmt {
    std::string table;
    std::vector<Condition> conditions;
    DeleteStmt() { kind = StmtKind::DELETE_STMT; }
};

// CREATE VECTOR INDEX idx ON t(col)
struct CreateVectorIndexStmt : Stmt {
    std::string index_name;
    std::string table;
    std::string column;
    CreateVectorIndexStmt() { kind = StmtKind::CREATE_VECTOR_INDEX; }
};

struct TxnStmt : Stmt {
    // kind 决定 BEGIN/COMMIT/ROLLBACK
};

} // namespace mvdb
