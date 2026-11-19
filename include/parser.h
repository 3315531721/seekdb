// parser.h — SQL 解析器对外接口
#pragma once
#include "types.h"
#include "ast.h"
#include <memory>
#include <string>

namespace mvdb {

// 解析单条 SQL 语句; 失败抛 DbError(SYNTAX/UNSUPPORTED)。
// ann_mode: 协议层 search_mode == "ann" 时置 true, 用于标记向量检索模式。
std::unique_ptr<Stmt> parse_statement(const std::string& sql, bool ann_mode = false);

} // namespace mvdb
