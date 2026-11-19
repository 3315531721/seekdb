// executor.h — 查询执行引擎接口 (契约文件)
#pragma once
#include "types.h"
#include "ast.h"
#include "storage.h"

namespace mvdb {

struct QueryResult {
    std::vector<std::string> columns;        // 结果集列名 (仅 has_resultset)
    std::vector<std::vector<Value>> rows;     // 结果行 (仅 has_resultset)
    bool has_resultset = false;
    int64_t affected_rows = -1;               // >=0 表示 DML 受影响行数
};

// 执行一条已解析语句。失败抛 DbError。
QueryResult execute(StorageEngine& ds, const Stmt& stmt);

// L2 欧氏距离 (按 float32 存储值计算)
double l2_distance(const Vector& a, const Vector& b);

} // namespace mvdb
