// main.cpp — MiniVectorDB 进程入口与装配。
//
// 按官方 2026-competition-round2 的 stdin/stdout JSONL 协议运行：
//   * 程序前台运行，不启动任何网络监听；
//   * 打开 --data-dir（默认 /data），完成 WAL 恢复后由协议层输出
//     {"type":"ready"}；
//   * 协议循环在 src/protocol.cpp 中实现。
//
// 命令行：
//   --data-dir DIR   数据目录，默认 /data
// 未知参数按题面要求忽略。

#include "types.h"
#include "protocol.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

std::string parse_data_dir(int argc, char** argv) {
    std::string data_dir = "/data";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir") {
            if (i + 1 < argc) data_dir = argv[++i];
        } else if (arg.rfind("--data-dir=", 0) == 0) {
            data_dir = arg.substr(std::string("--data-dir=").size());
        }
        // 其它未知参数按题面要求忽略。
    }
    if (data_dir.empty()) data_dir = "/data";
    return data_dir;
}

} // namespace

int main(int argc, char** argv) {
    // 协议层只使用 iostream；关闭与 C stdio 的同步可降低大量小请求的解析开销，
    // 每次响应仍由 protocol.cpp 显式 flush。
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    const std::string data_dir = parse_data_dir(argc, argv);

    try {
        // run_protocol_loop 内部创建 StorageEngine、open(data_dir) 并完成恢复，
        // 恢复结束后才输出 ready，随后处理 stdin 上的 JSONL 请求。
        return mvdb::run_protocol_loop(std::cin, std::cout, std::cerr, data_dir);
    } catch (const mvdb::DbError& e) {
        std::cerr << "[minivectordb] fatal: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[minivectordb] fatal: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "[minivectordb] fatal: unknown exception" << std::endl;
        return 1;
    }
}
