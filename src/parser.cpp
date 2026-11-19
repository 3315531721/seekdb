// parser.cpp — MiniVectorDB SQL 解析器实现
// 手写词法器 + 递归下降。只依赖 types.h / ast.h。
//
// 支持的 SQL 子集:
//   CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(64), emb VECTOR(128));
//   INSERT INTO t (c1,c2,...) VALUES (v1,v2,...),(...);   (列名可省略)
//   SELECT a,b / * FROM t [WHERE cond AND ...]
//          [ORDER BY col [ASC|DESC]] [LIMIT n];
//     其中投影/WHERE/ORDER BY 可含 L2_DISTANCE(vector_col, '[1,2,3]')
//   UPDATE t SET col=val,... [WHERE ...];
//   DELETE FROM t [WHERE ...];
//   CREATE VECTOR INDEX idx ON t(col);
//   BEGIN; / COMMIT; / ROLLBACK;   (大小写不敏感)
//
// 不支持的语法(JOIN/聚合/NULL/子查询/ALTER 等)抛 UNSUPPORTED。
#include "parser.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace mvdb {
namespace {

// ======================= 词法 =======================

enum class Tok {
    IDENT,    // 标识符 / 关键字
    NUMBER,   // 整数字面量
    STRING,   // 单引号字符串 (已处理 '' 转义)
    SYM,      // 标点/运算: [ ] ( ) , = < <= > >=
    END       // 词法结束
};

struct Token {
    Tok kind;
    std::string text;   // IDENT 原样文本 / STRING 已解码内容 / NUMBER 原样 / SYM 符号
    size_t pos;         // 起始位置 (用于报错)
};

// 判断是否合法标识符起始/组成部分 (字母/下划线/数字)
static bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
}
static bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

// 词法分析: 把 sql 切成 token 流
static std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> out;
    const size_t n = sql.size();
    size_t i = 0;
    while (i < n) {
        char c = sql[i];
        // 空白
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        size_t start = i;

        // 单引号字符串, 支持 '' 转义为单个单引号
        if (c == '\'') {
            ++i;
            std::string s;
            bool closed = false;
            while (i < n) {
                char d = sql[i];
                if (d == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        s.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    closed = true;
                    break;
                }
                s.push_back(d);
                ++i;
            }
            if (!closed)
                throw DbError(ErrCode::SYNTAX, "字符串未闭合 (位置 " + std::to_string(start) + ")");
            out.push_back({Tok::STRING, std::move(s), start});
            continue;
        }

        // 数字: 仅非负整数 (负数由符号处理); 支持 0x? 不需要, 仅十进制
        if (std::isdigit((unsigned char)c)) {
            std::string num;
            while (i < n && std::isdigit((unsigned char)sql[i])) {
                num.push_back(sql[i]);
                ++i;
            }
            out.push_back({Tok::NUMBER, std::move(num), start});
            continue;
        }

        // 标识符/关键字
        if (is_ident_start(c)) {
            std::string id;
            while (i < n && is_ident_char(sql[i])) {
                id.push_back(sql[i]);
                ++i;
            }
            out.push_back({Tok::IDENT, std::move(id), start});
            continue;
        }

        // 符号 (含双字符比较运算符)
        if (c == '<') {
            if (i + 1 < n && sql[i + 1] == '=') { out.push_back({Tok::SYM, "<=", start}); i += 2; }
            else { out.push_back({Tok::SYM, "<", start}); ++i; }
            continue;
        }
        if (c == '>') {
            if (i + 1 < n && sql[i + 1] == '=') { out.push_back({Tok::SYM, ">=", start}); i += 2; }
            else { out.push_back({Tok::SYM, ">", start}); ++i; }
            continue;
        }
        if (c == '=' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == ',' || c == '*' || c == ';' || c == '-') {
            out.push_back({Tok::SYM, std::string(1, c), start});
            ++i;
            continue;
        }

        // 其它字符一律语法错误
        throw DbError(ErrCode::SYNTAX,
                      std::string("无法识别的字符 '") + c + "' (位置 " + std::to_string(start) + ")");
    }
    out.push_back({Tok::END, "", n});
    return out;
}

// ======================= 数值/向量解析工具 =======================

// 解析整数文本 -> int64, 溢出抛 TYPE_MISMATCH
static int64_t parse_int64(const std::string& s) {
    errno = 0;
    char* endp = nullptr;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (errno == ERANGE || endp == s.c_str() || *endp != '\0')
        throw DbError(ErrCode::TYPE_MISMATCH, "整数字面量溢出或非法: " + s);
    return (int64_t)v;
}

// 解析单个 float 分量, 拒绝 NaN/Infinity/溢出 (必须是有限 float32)
static float parse_float_component(const std::string& s) {
    const char* cstr = s.c_str();
    errno = 0;
    char* endp = nullptr;
    double d = std::strtod(cstr, &endp);
    if (endp == cstr || *endp != '\0')
        throw DbError(ErrCode::INVALID_VECTOR, "向量分量非法: " + s);
    if (!std::isfinite(d))
        throw DbError(ErrCode::INVALID_VECTOR, "向量分量必须有限 (拒绝 NaN/Infinity): " + s);
    float f = static_cast<float>(d);
    // 检查转 float32 后是否溢出为 inf (double 有限但超 float 范围)
    if (!std::isfinite(f))
        throw DbError(ErrCode::INVALID_VECTOR, "向量分量超出 float32 范围: " + s);
    return f;
}

// 从字符串形式 "[1.0, 2.0, 3.0]" 解析出向量分量
static Vector parse_vector_string(const std::string& raw) {
    // 允许前后空白; 必须以 [ 开头 ] 结尾
    size_t b = 0, e = raw.size();
    while (b < e && std::isspace((unsigned char)raw[b])) ++b;
    while (e > b && std::isspace((unsigned char)raw[e - 1])) --e;
    if (b >= e || raw[b] != '[' || raw[e - 1] != ']')
        throw DbError(ErrCode::SYNTAX, "向量字面量需形如 '[v1,v2,...]': " + raw);

    std::string body = raw.substr(b + 1, e - b - 2);
    Vector vec;
    // 空向量 "[ ]" 视为维度 0 (维度校验留给执行器)
    size_t i = 0, m = body.size();
    bool saw_any = false;
    while (i < m) {
        while (i < m && std::isspace((unsigned char)body[i])) ++i;
        if (i >= m) break;
        size_t s = i;
        while (i < m && body[i] != ',') ++i;
        std::string comp = body.substr(s, i - s);
        // 去掉分量两端空白
        size_t cb = 0, ce = comp.size();
        while (cb < ce && std::isspace((unsigned char)comp[cb])) ++cb;
        while (ce > cb && std::isspace((unsigned char)comp[ce - 1])) --ce;
        if (ce == cb)
            throw DbError(ErrCode::SYNTAX, "向量存在空分量: " + raw);
        vec.v.push_back(parse_float_component(comp.substr(cb, ce - cb)));
        saw_any = true;
        if (i < m && body[i] == ',') ++i;
    }
    (void)saw_any;
    return vec;
}

// ======================= 语法解析器 =======================

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::unique_ptr<Stmt> parse() {
        std::unique_ptr<Stmt> stmt;
        // 空语句直接报错
        if (peek().kind == Tok::END)
            throw DbError(ErrCode::SYNTAX, "空 SQL 语句");

        if (is_kw("CREATE")) {
            stmt = parse_create();
        } else if (is_kw("INSERT")) {
            stmt = parse_insert();
        } else if (is_kw("SELECT")) {
            stmt = parse_select();
        } else if (is_kw("UPDATE")) {
            stmt = parse_update();
        } else if (is_kw("DELETE")) {
            stmt = parse_delete();
        } else if (is_kw("BEGIN")) {
            advance();
            stmt = std::make_unique<TxnStmt>();
            stmt->kind = StmtKind::BEGIN;
        } else if (is_kw("COMMIT")) {
            advance();
            stmt = std::make_unique<TxnStmt>();
            stmt->kind = StmtKind::COMMIT;
        } else if (is_kw("ROLLBACK")) {
            advance();
            stmt = std::make_unique<TxnStmt>();
            stmt->kind = StmtKind::ROLLBACK;
        } else {
            throw DbError(ErrCode::UNSUPPORTED,
                          "不支持的语句起始关键字: " + peek().text);
        }

        // 可选结尾分号 (允许多个连续分号)
        while (peek().kind == Tok::SYM && peek().text == ";") advance();
        if (peek().kind != Tok::END)
            throw DbError(ErrCode::SYNTAX,
                          "语句结束后存在多余内容: " + peek().text);
        return stmt;
    }

private:
    std::vector<Token> toks_;
    size_t i_ = 0;

    const Token& peek(size_t ahead = 0) const {
        size_t idx = i_ + ahead;
        if (idx >= toks_.size()) return toks_.back(); // END 兜底
        return toks_[idx];
    }
    const Token& advance() {
        const Token& t = toks_[i_];
        if (i_ + 1 < toks_.size()) ++i_;
        return t;
    }

    // 当前 token 是否为指定关键字 (大小写不敏感, 且必须是 IDENT)
    bool is_kw(const char* kw) const {
        if (peek().kind != Tok::IDENT) return false;
        return ieq(peek().text, kw);
    }

    static bool ieq(const std::string& a, const char* b) {
        size_t n = std::strlen(b);
        if (a.size() != n) return false;
        for (size_t k = 0; k < n; ++k)
            if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
                return false;
        return true;
    }

    // 期望关键字并前进
    void expect_kw(const char* kw) {
        if (!is_kw(kw))
            throw DbError(ErrCode::SYNTAX,
                          std::string("期望关键字 ") + kw + ", 实际: " + peek().text);
        advance();
    }

    // 期望符号
    void expect_sym(const char* sym) {
        if (!(peek().kind == Tok::SYM && peek().text == sym))
            throw DbError(ErrCode::SYNTAX,
                          std::string("期望符号 '") + sym + "', 实际: " + peek().text);
        advance();
    }

    bool is_sym(const char* sym) const {
        return peek().kind == Tok::SYM && peek().text == sym;
    }

    // 接受一个标识符 (表名/列名/索引名)
    std::string parse_ident() {
        if (peek().kind != Tok::IDENT)
            throw DbError(ErrCode::SYNTAX, "期望标识符, 实际: " + peek().text);
        return advance().text;
    }

    // ---------- 值解析 (INSERT / UPDATE / 条件中的字面量) ----------
    // 支持: 可选负号整数、整数字面量、字符串、向量字符串 '[..]'
    Value parse_value() {
        // 负数: 符号 '-' 后接数字 (词法把 '-' 视为非法字符, 此处单独处理)
        if (peek().kind == Tok::SYM && peek().text == "-") {
            // 注意: 词法器不会产出 '-' 作为 SYM, 因此实际不会走到这; 保留防御
            advance();
            if (peek().kind != Tok::NUMBER)
                throw DbError(ErrCode::SYNTAX, "负号后期望数字");
            int64_t v = parse_int64(peek().text);
            advance();
            return Value::of_int(-v);
        }
        if (peek().kind == Tok::NUMBER) {
            int64_t v = parse_int64(peek().text);
            advance();
            return Value::of_int(v);
        }
        if (peek().kind == Tok::STRING) {
            std::string s = peek().text;
            advance();
            // 字符串内容若是向量形式 '[..]' 也按字符串处理? 依题意:
            // 插入向量列使用字符串形式的向量 => 在此解析为 Vector。
            // 为区分, 若字符串明显是向量字面量(去空白后以[开头])则解析成向量。
            std::string trimmed = ltrim(s);
            if (!trimmed.empty() && trimmed[0] == '[') {
                Vector vec = parse_vector_string(s);
                return Value::of_vec(std::move(vec));
            }
            return Value::of_str(std::move(s));
        }
        throw DbError(ErrCode::SYNTAX, "期望字面量值, 实际: " + peek().text);
    }

    static std::string ltrim(const std::string& s) {
        size_t b = 0;
        while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
        return s.substr(b);
    }

    // ---------- 解析 L2_DISTANCE(col, '[..]') -> (col, Vector) ----------
    // 前置: 当前 token 为 L2_DISTANCE 关键字
    void parse_l2_distance(std::string& out_col, Vector& out_vec) {
        expect_kw("L2_DISTANCE");
        expect_sym("(");
        out_col = parse_ident();
        expect_sym(",");
        if (peek().kind != Tok::STRING)
            throw DbError(ErrCode::SYNTAX, "L2_DISTANCE 第二参数需为向量字符串");
        out_vec = parse_vector_string(peek().text);
        advance();
        expect_sym(")");
    }

    // ---------- CREATE ... ----------
    std::unique_ptr<Stmt> parse_create() {
        expect_kw("CREATE");
        if (is_kw("VECTOR")) {
            return parse_create_vector_index();
        }
        if (!is_kw("TABLE"))
            throw DbError(ErrCode::UNSUPPORTED,
                          "仅支持 CREATE TABLE / CREATE VECTOR INDEX, 实际: " + peek().text);
        expect_kw("TABLE");
        auto stmt = std::make_unique<CreateTableStmt>();
        stmt->table = parse_ident();
        expect_sym("(");

        do {
            ColumnDef def;
            def.name = parse_ident();
            // 类型
            if (is_kw("BIGINT")) {
                advance();
                def.type = ColType::BIGINT;
            } else if (is_kw("VARCHAR")) {
                advance();
                expect_sym("(");
                if (peek().kind != Tok::NUMBER)
                    throw DbError(ErrCode::SYNTAX, "VARCHAR(n) 需要数字");
                def.varchar_len = (int)parse_int64(peek().text);
                advance();
                expect_sym(")");
                def.type = ColType::VARCHAR;
            } else if (is_kw("VECTOR")) {
                advance();
                expect_sym("(");
                if (peek().kind != Tok::NUMBER)
                    throw DbError(ErrCode::SYNTAX, "VECTOR(n) 需要数字");
                def.vector_dim = (int)parse_int64(peek().text);
                advance();
                expect_sym(")");
                def.type = ColType::VECTOR;
            } else {
                throw DbError(ErrCode::UNSUPPORTED,
                              "不支持或未知的列类型: " + peek().text);
            }

            // 可选 PRIMARY KEY
            if (is_kw("PRIMARY")) {
                advance();
                expect_kw("KEY");
                def.is_primary = true;
            }

            // 未支持的列级约束显式报错 (NOT NULL 等)
            if (is_kw("NOT") || is_kw("NULL") || is_kw("UNIQUE") || is_kw("DEFAULT")) {
                throw DbError(ErrCode::UNSUPPORTED,
                              "不支持的列约束: " + peek().text);
            }

            stmt->columns.push_back(std::move(def));

            if (is_sym(",")) { advance(); continue; }
            break;
        } while (true);

        expect_sym(")");
        return stmt;
    }

    std::unique_ptr<Stmt> parse_create_vector_index() {
        expect_kw("VECTOR");
        expect_kw("INDEX");
        auto stmt = std::make_unique<CreateVectorIndexStmt>();
        stmt->index_name = parse_ident();
        expect_kw("ON");
        stmt->table = parse_ident();
        expect_sym("(");
        stmt->column = parse_ident();
        expect_sym(")");
        return stmt;
    }

    // ---------- INSERT ----------
    std::unique_ptr<Stmt> parse_insert() {
        expect_kw("INSERT");
        expect_kw("INTO");
        auto stmt = std::make_unique<InsertStmt>();
        stmt->table = parse_ident();

        // 可选列名列表
        if (is_sym("(")) {
            advance();
            if (!is_sym(")")) {
                do {
                    stmt->columns.push_back(parse_ident());
                    if (is_sym(",")) { advance(); continue; }
                    break;
                } while (true);
            }
            expect_sym(")");
        }

        expect_kw("VALUES");

        // 多行: (v,...), (v,...)
        do {
            expect_sym("(");
            std::vector<Value> row;
            if (!is_sym(")")) {
                do {
                    row.push_back(parse_value());
                    if (is_sym(",")) { advance(); continue; }
                    break;
                } while (true);
            }
            expect_sym(")");
            stmt->rows.push_back(std::move(row));
            if (is_sym(",")) { advance(); continue; }
            break;
        } while (true);

        return stmt;
    }

    // ---------- WHERE 条件 (AND 组合) ----------
    // 条件形如 col op literal; 若遇到 L2_DISTANCE 则不是普通条件 (由调用方处理)
    void parse_where_conditions(std::vector<Condition>& out) {
        // 前置: 已消费 WHERE
        do {
            Condition cond;
            cond.col_name = parse_ident();
            CmpOp op;
            if (is_sym("=")) { op = CmpOp::EQ; advance(); }
            else if (is_sym("<=")) { op = CmpOp::LE; advance(); }
            else if (is_sym("<")) { op = CmpOp::LT; advance(); }
            else if (is_sym(">=")) { op = CmpOp::GE; advance(); }
            else if (is_sym(">")) { op = CmpOp::GT; advance(); }
            else
                throw DbError(ErrCode::SYNTAX,
                              "WHERE 条件期望比较运算符, 实际: " + peek().text);
            cond.op = op;
            cond.value = parse_value();
            out.push_back(std::move(cond));

            if (is_kw("AND")) { advance(); continue; }
            break;
        } while (true);

        // 不支持 OR 等
        if (is_kw("OR") || is_kw("NOT"))
            throw DbError(ErrCode::UNSUPPORTED, "不支持 OR/NOT 组合条件: " + peek().text);
    }

    // ---------- SELECT ----------
    std::unique_ptr<Stmt> parse_select() {
        expect_kw("SELECT");
        auto stmt = std::make_unique<SelectStmt>();

        // 投影列表 或 *
        if (is_sym("*")) {
            advance();
            stmt->select_star = true;
        } else {
            do {
                SelectItem item;
                if (is_kw("L2_DISTANCE")) {
                    // 投影里出现 L2_DISTANCE(vector_col,'[..]') => 距离项 + 向量查询标记
                    std::string col; Vector vec;
                    parse_l2_distance(col, vec);
                    item.col_name = col;
                    item.is_distance = true;
                    item.query_vec = vec.v;
                    stmt->has_vector_query = true;
                    if (stmt->query_vector.empty()) stmt->query_vector = vec.v;
                } else {
                    item.col_name = parse_ident();
                }
                // 可选 AS 别名
                if (is_kw("AS")) {
                    advance();
                    item.alias = parse_ident();
                }
                stmt->projection.push_back(std::move(item));
                if (is_sym(",")) { advance(); continue; }
                break;
            } while (true);
        }

        expect_kw("FROM");
        stmt->table = parse_ident();

        // WHERE
        if (is_kw("WHERE")) {
            advance();
            // 首项是否为 L2_DISTANCE(...)? 形如: WHERE L2_DISTANCE(col,'[..]') [<op> K] [AND cond...]
            if (is_kw("L2_DISTANCE")) {
                std::string col; Vector vec;
                parse_l2_distance(col, vec);
                stmt->has_vector_query = true;
                if (stmt->query_vector.empty()) stmt->query_vector = vec.v;
                stmt->vector_col_idx = -1;  // 由 executor 结合 schema 解析 col
                // 可选比较: L2_DISTANCE(...) <op> K, 解析掉 (K 仅作提示, topk 以 LIMIT 为准)
                if (is_sym("<") || is_sym("<=") || is_sym(">") ||
                    is_sym(">=") || is_sym("=")) {
                    advance();
                    if (peek().kind == Tok::NUMBER) {
                        int k = (int)parse_int64(peek().text);
                        advance();
                        if (stmt->topk < 0) stmt->topk = k;
                    } else {
                        throw DbError(ErrCode::SYNTAX, "L2_DISTANCE 比较后需数字");
                    }
                }
                // 后续 AND 普通条件
                while (is_kw("AND")) {
                    advance();
                    parse_one_condition_into(stmt->conditions);
                }
            } else {
                parse_where_conditions(stmt->conditions);
            }
        }

        // ORDER BY
        if (is_kw("ORDER")) {
            advance();
            expect_kw("BY");
            stmt->has_order_by = true;
            if (is_kw("L2_DISTANCE")) {
                std::string col; Vector vec;
                parse_l2_distance(col, vec);
                stmt->has_vector_query = true;
                if (stmt->query_vector.empty()) stmt->query_vector = vec.v;
                stmt->order_by = col;               // 记录向量列名
                stmt->order_by_is_distance = true;
            } else {
                stmt->order_by = parse_ident();
                stmt->order_by_is_distance = false;
            }
            // ASC / DESC
            if (is_kw("ASC")) { advance(); stmt->asc = true; }
            else if (is_kw("DESC")) { advance(); stmt->asc = false; }
        }

        // LIMIT n
        if (is_kw("LIMIT")) {
            advance();
            if (peek().kind != Tok::NUMBER)
                throw DbError(ErrCode::SYNTAX, "LIMIT 需要非负整数");
            stmt->limit = (int)parse_int64(peek().text);
            advance();
        }

        // 向量查询: topk 取 LIMIT (若已有 WHERE 比较给出的 K 且无 LIMIT 也保留)
        if (stmt->has_vector_query && stmt->limit >= 0) {
            stmt->topk = stmt->limit;
        }

        // 不支持的尾部子句
        if (is_kw("JOIN") || is_kw("GROUP") || is_kw("HAVING") || is_kw("UNION"))
            throw DbError(ErrCode::UNSUPPORTED, "不支持的 SELECT 子句: " + peek().text);

        return stmt;
    }

    // 解析单个普通比较条件并追加
    void parse_one_condition_into(std::vector<Condition>& out) {
        Condition cond;
        cond.col_name = parse_ident();
        if (is_sym("=")) { cond.op = CmpOp::EQ; advance(); }
        else if (is_sym("<=")) { cond.op = CmpOp::LE; advance(); }
        else if (is_sym("<")) { cond.op = CmpOp::LT; advance(); }
        else if (is_sym(">=")) { cond.op = CmpOp::GE; advance(); }
        else if (is_sym(">")) { cond.op = CmpOp::GT; advance(); }
        else throw DbError(ErrCode::SYNTAX,
                           "条件期望比较运算符, 实际: " + peek().text);
        cond.value = parse_value();
        out.push_back(std::move(cond));
    }

    // ---------- UPDATE ----------
    std::unique_ptr<Stmt> parse_update() {
        expect_kw("UPDATE");
        auto stmt = std::make_unique<UpdateStmt>();
        stmt->table = parse_ident();
        expect_kw("SET");
        do {
            std::string col = parse_ident();
            expect_sym("=");
            Value v = parse_value();
            stmt->sets.emplace_back(std::move(col), std::move(v));
            if (is_sym(",")) { advance(); continue; }
            break;
        } while (true);

        if (is_kw("WHERE")) {
            advance();
            parse_where_conditions(stmt->conditions);
        }
        return stmt;
    }

    // ---------- DELETE ----------
    std::unique_ptr<Stmt> parse_delete() {
        expect_kw("DELETE");
        expect_kw("FROM");
        auto stmt = std::make_unique<DeleteStmt>();
        stmt->table = parse_ident();
        if (is_kw("WHERE")) {
            advance();
            parse_where_conditions(stmt->conditions);
        }
        return stmt;
    }
};

} // namespace

// ======================= 对外入口 =======================
std::unique_ptr<Stmt> parse_statement(const std::string& sql, bool ann_mode) {
    std::vector<Token> toks = tokenize(sql);
    Parser p(std::move(toks));
    auto st = p.parse();
    if (st && st->kind == StmtKind::SELECT && ann_mode) {
        static_cast<SelectStmt*>(st.get())->is_ann = true;
    }
    return st;
}

} // namespace mvdb
