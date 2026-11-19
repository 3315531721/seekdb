// protocol.cpp — 官方 JSONL 协议实现 (STDIO_PROTOCOL.md)
//
// 启动: {"type":"ready"}
// 请求: {"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}
//       {"id":N,"op":"shutdown"}
// 响应: {"id":N,"ok":true,"columns":[...],"rows":[[...]]} / {"id":N,"ok":true,"affected_rows":N}
//       {"id":N,"ok":false,"error":{"code":"...","message":"..."}}
#include "protocol.h"
#include "json.h"
#include "types.h"
#include "ast.h"
#include "storage.h"
#include "executor.h"
#include "parser.h"
#include <iostream>
#include <sstream>
#include <memory>
#include <string>

namespace mvdb {

namespace {

Json value_to_json(const Value& v) {
    switch (v.type) {
        case ColType::BIGINT:  return Json::make_int(v.i);
        case ColType::VARCHAR: return Json::make_str(v.s);
        case ColType::DOUBLE:  return Json::make_num(v.d);
        case ColType::VECTOR: {
            Json a = Json::make_arr();
            for (float f : v.vec.v) a.push(Json::make_num(static_cast<double>(f)));
            return a;
        }
    }
    return Json::make_null();
}

void emit(std::ostream& out, const Json& j) {
    out << j.dump() << '\n';
    out.flush();
}

} // namespace

int run_protocol_loop(std::istream& in, std::ostream& out, std::ostream& err,
                      const std::string& data_dir) {
    std::unique_ptr<StorageEngine> ds;
    try {
        ds = make_storage_engine();
        if (!ds) {
            err << "[fatal] make_storage_engine() returned null" << std::endl;
            return 1;
        }
        ds->open(data_dir);   // 含恢复; 完成后才可报 ready
    } catch (const std::exception& e) {
        err << "[fatal] open failed: " << e.what() << std::endl;
        return 1;
    }

    // 握手: 必须在读取第一条请求之前输出并 flush
    Json ready = Json::make_obj();
    ready.set("type", Json::make_str("ready"));
    emit(out, ready);

    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        Json req;
        try {
            req = Json::parse(line);
        } catch (const std::exception& e) {
            err << "[warn] bad json request: " << e.what() << std::endl;
            // 每个请求都应有一个响应；无法解析时 id 置 null。
            Json bad = Json::make_obj();
            bad.set("id", Json::make_null());
            bad.set("ok", Json::make_bool(false));
            Json eo = Json::make_obj();
            eo.set("code", Json::make_str("invalid_request"));
            eo.set("message", Json::make_str(e.what()));
            bad.set("error", std::move(eo));
            emit(out, bad);
            continue;
        }

        if (req.type != Json::Type::OBJECT) {
            Json bad = Json::make_obj();
            bad.set("id", Json::make_null());
            bad.set("ok", Json::make_bool(false));
            Json eo = Json::make_obj();
            eo.set("code", Json::make_str("invalid_request"));
            eo.set("message", Json::make_str("request must be a JSON object"));
            bad.set("error", std::move(eo));
            emit(out, bad);
            continue;
        }

        const Json* idj = req.get("id");
        Json resp = Json::make_obj();
        resp.set("id", idj ? *idj : Json::make_null());

        const Json* opj = req.get("op");
        std::string op = (opj && opj->type == Json::Type::STRING) ? opj->str : "";

        if (op == "shutdown") {
            try {
                if (ds->in_txn()) ds->rollback();
                ds->close();
            } catch (const std::exception& e) {
                err << "[warn] close: " << e.what() << std::endl;
            }
            resp.set("ok", Json::make_bool(true));
            emit(out, resp);
            return 0;   // 之后 stdout 不得再有输出
        }

        if (op != "execute") {
            resp.set("ok", Json::make_bool(false));
            Json e = Json::make_obj();
            e.set("code", Json::make_str("unsupported_op"));
            e.set("message", Json::make_str("unknown op"));
            resp.set("error", std::move(e));
            emit(out, resp);
            continue;
        }

        const Json* sqlj = req.get("sql");
        std::string sql = (sqlj && sqlj->type == Json::Type::STRING) ? sqlj->str : "";
        bool ann = false;
        const Json* sm = req.get("search_mode");
        if (sm && sm->type == Json::Type::STRING && sm->str == "ann") ann = true;

        try {
            StmtPtr stmt = parse_statement(sql, ann);
            QueryResult r = execute(*ds, *stmt);

            resp.set("ok", Json::make_bool(true));
            if (r.has_resultset) {
                Json cols = Json::make_arr();
                for (auto& c : r.columns) cols.push(Json::make_str(c));
                resp.set("columns", std::move(cols));
                Json rows = Json::make_arr();
                for (auto& row : r.rows) {
                    Json jr = Json::make_arr();
                    for (auto& cell : row) jr.push(value_to_json(cell));
                    rows.push(std::move(jr));
                }
                resp.set("rows", std::move(rows));
            } else if (r.affected_rows >= 0) {
                resp.set("affected_rows", Json::make_int(r.affected_rows));
            }
        } catch (const DbError& e) {
            resp.set("ok", Json::make_bool(false));
            Json eo = Json::make_obj();
            eo.set("code", Json::make_str(err_code_name(e.code)));
            eo.set("message", Json::make_str(e.what()));
            resp.set("error", std::move(eo));
        } catch (const std::exception& e) {
            resp.set("ok", Json::make_bool(false));
            Json eo = Json::make_obj();
            eo.set("code", Json::make_str("internal_error"));
            eo.set("message", Json::make_str(e.what()));
            resp.set("error", std::move(eo));
        }
        emit(out, resp);
    }

    // EOF: 回滚未提交并关闭
    try {
        if (ds->in_txn()) ds->rollback();
        ds->close();
    } catch (...) {}
    return 0;
}

} // namespace mvdb
