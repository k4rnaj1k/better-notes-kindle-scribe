#pragma once
// Tiny JSON reader/writer tailored for BetterNotes save files.
// Subset only: objects, arrays, strings, numbers, true/false/null. No nesting
// of arbitrary depth beyond what the schema needs. Intentionally not generic
// — keeps the dependency footprint zero.
//
// Reader is strict-enough to round-trip files we wrote ourselves and is
// tolerant of whitespace.

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace bn::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type        type = Type::Null;
    bool        b = false;
    double      n = 0;
    std::string s;
    std::vector<Value>           arr;
    std::map<std::string, Value> obj;

    static Value make_obj()  { Value v; v.type = Type::Object; return v; }
    static Value make_arr()  { Value v; v.type = Type::Array;  return v; }
    static Value make_str(const std::string &x) { Value v; v.type = Type::String; v.s = x; return v; }
    static Value make_num(double x) { Value v; v.type = Type::Number; v.n = x; return v; }
    static Value make_bool(bool x)  { Value v; v.type = Type::Bool;   v.b = x; return v; }

    // Convenience accessors with defaults.
    const Value *get(const std::string &k) const {
        auto it = obj.find(k); return it == obj.end() ? nullptr : &it->second;
    }
    std::string str(const std::string &k, const std::string &def = "") const {
        auto p = get(k); return (p && p->type == Type::String) ? p->s : def;
    }
    double num(const std::string &k, double def = 0) const {
        auto p = get(k); return (p && p->type == Type::Number) ? p->n : def;
    }
};

bool parse(const std::string &src, Value &out, std::string *err = nullptr);
std::string serialize(const Value &v, bool pretty = true);

} // namespace bn::json
