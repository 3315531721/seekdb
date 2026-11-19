// json.h — 极简 JSON 解析/序列化 (无外部依赖)
// 只覆盖协议所需: null/bool/number/string/array/object。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace mvdb {

class Json {
public:
    enum class Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Type type = Type::NUL;
    bool b = false;
    double num = 0.0;
    bool is_integer = false;     // number 是否为整数(无小数/指数)
    int64_t inum = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    static Json make_null() { return Json(); }
    static Json make_bool(bool v) { Json j; j.type = Type::BOOL; j.b = v; return j; }
    static Json make_int(int64_t v) { Json j; j.type = Type::NUMBER; j.is_integer = true; j.inum = v; j.num = (double)v; return j; }
    static Json make_num(double v) { Json j; j.type = Type::NUMBER; j.is_integer = false; j.num = v; return j; }
    static Json make_str(std::string v) { Json j; j.type = Type::STRING; j.str = std::move(v); return j; }
    static Json make_arr() { Json j; j.type = Type::ARRAY; return j; }
    static Json make_obj() { Json j; j.type = Type::OBJECT; return j; }

    void set(const std::string& k, Json v);
    const Json* get(const std::string& k) const;
    void push(Json v) { arr.push_back(std::move(v)); }

    // 解析; 失败抛 std::runtime_error
    static Json parse(const std::string& text);
    std::string dump() const;
};

} // namespace mvdb
