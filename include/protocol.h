// protocol.h — JSONL stdin/stdout 协议 (官方 STDIO_PROTOCOL.md)
//
// 启动: 输出 {"type":"ready"} 并 flush
// 请求: {"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}
//       {"id":N,"op":"shutdown"}
// 响应: {"id":N,"ok":true,...} / {"id":N,"ok":false,"error":{"code":"...","message":"..."}}
#pragma once
#include <string>
#include <iostream>

namespace mvdb {

// 运行协议主循环。返回进程退出码。
int run_protocol_loop(std::istream& in, std::ostream& out, std::ostream& err,
                      const std::string& data_dir);

} // namespace mvdb
