#include "json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace bn::json {

namespace {

struct P {
    const char *s; const char *e; std::string err;
    void skip() { while (s < e && std::isspace((unsigned char)*s)) ++s; }
    bool eof() const { return s >= e; }
    bool fail(const char *m) { err = m; return false; }
};

bool parse_value(P &p, Value &v);

bool parse_string(P &p, std::string &out) {
    if (p.eof() || *p.s != '"') return p.fail("expected string");
    ++p.s;
    while (!p.eof() && *p.s != '"') {
        char c = *p.s++;
        if (c == '\\') {
            if (p.eof()) return p.fail("bad escape");
            char esc = *p.s++;
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    // 4 hex digits → UTF-8 (BMP only).
                    if (p.e - p.s < 4) return p.fail("bad \\u");
                    char buf[5] = {p.s[0], p.s[1], p.s[2], p.s[3], 0};
                    p.s += 4;
                    unsigned cp = (unsigned)std::strtoul(buf, nullptr, 16);
                    if (cp < 0x80) { out += (char)cp; }
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return p.fail("bad escape");
            }
        } else {
            out += c;
        }
    }
    if (p.eof()) return p.fail("unterminated string");
    ++p.s;
    return true;
}

bool parse_number(P &p, double &out) {
    const char *start = p.s;
    if (!p.eof() && *p.s == '-') ++p.s;
    while (!p.eof() && std::isdigit((unsigned char)*p.s)) ++p.s;
    if (!p.eof() && *p.s == '.') {
        ++p.s;
        while (!p.eof() && std::isdigit((unsigned char)*p.s)) ++p.s;
    }
    if (!p.eof() && (*p.s == 'e' || *p.s == 'E')) {
        ++p.s;
        if (!p.eof() && (*p.s == '+' || *p.s == '-')) ++p.s;
        while (!p.eof() && std::isdigit((unsigned char)*p.s)) ++p.s;
    }
    std::string num(start, p.s - start);
    out = std::strtod(num.c_str(), nullptr);
    return true;
}

bool parse_array(P &p, Value &v) {
    if (p.eof() || *p.s != '[') return p.fail("expected [");
    ++p.s; p.skip();
    v = Value::make_arr();
    if (!p.eof() && *p.s == ']') { ++p.s; return true; }
    while (true) {
        p.skip();
        Value elt;
        if (!parse_value(p, elt)) return false;
        v.arr.push_back(std::move(elt));
        p.skip();
        if (p.eof()) return p.fail("unterminated array");
        if (*p.s == ',') { ++p.s; continue; }
        if (*p.s == ']') { ++p.s; return true; }
        return p.fail("expected , or ]");
    }
}

bool parse_object(P &p, Value &v) {
    if (p.eof() || *p.s != '{') return p.fail("expected {");
    ++p.s; p.skip();
    v = Value::make_obj();
    if (!p.eof() && *p.s == '}') { ++p.s; return true; }
    while (true) {
        p.skip();
        std::string key;
        if (!parse_string(p, key)) return false;
        p.skip();
        if (p.eof() || *p.s != ':') return p.fail("expected :");
        ++p.s; p.skip();
        Value val;
        if (!parse_value(p, val)) return false;
        v.obj.emplace(std::move(key), std::move(val));
        p.skip();
        if (p.eof()) return p.fail("unterminated object");
        if (*p.s == ',') { ++p.s; continue; }
        if (*p.s == '}') { ++p.s; return true; }
        return p.fail("expected , or }");
    }
}

bool parse_value(P &p, Value &v) {
    p.skip();
    if (p.eof()) return p.fail("eof");
    char c = *p.s;
    if (c == '{') return parse_object(p, v);
    if (c == '[') return parse_array(p, v);
    if (c == '"') {
        v.type = Type::String;
        return parse_string(p, v.s);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        v.type = Type::Number;
        return parse_number(p, v.n);
    }
    if (p.e - p.s >= 4 && std::string(p.s, 4) == "true") {
        p.s += 4; v.type = Type::Bool; v.b = true; return true;
    }
    if (p.e - p.s >= 5 && std::string(p.s, 5) == "false") {
        p.s += 5; v.type = Type::Bool; v.b = false; return true;
    }
    if (p.e - p.s >= 4 && std::string(p.s, 4) == "null") {
        p.s += 4; v.type = Type::Null; return true;
    }
    return p.fail("unexpected token");
}

void write_str(std::ostringstream &o, const std::string &s) {
    o << '"';
    for (char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n";  break;
            case '\t': o << "\\t";  break;
            case '\r': o << "\\r";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o << buf;
                } else o << c;
        }
    }
    o << '"';
}

void write_value(std::ostringstream &o, const Value &v, bool pretty, int indent) {
    auto pad = [&](int n){ if (pretty) for (int i=0;i<n;i++) o << "  "; };
    switch (v.type) {
        case Type::Null:   o << "null"; break;
        case Type::Bool:   o << (v.b ? "true" : "false"); break;
        case Type::Number: {
            char buf[32];
            if (v.n == (double)(long long)v.n)
                std::snprintf(buf, sizeof(buf), "%lld", (long long)v.n);
            else
                std::snprintf(buf, sizeof(buf), "%.6g", v.n);
            o << buf;
            break;
        }
        case Type::String: write_str(o, v.s); break;
        case Type::Array: {
            o << '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) o << (pretty ? ",\n" : ",");
                else if (pretty) o << '\n';
                pad(indent + 1);
                write_value(o, v.arr[i], pretty, indent + 1);
            }
            if (pretty && !v.arr.empty()) { o << '\n'; pad(indent); }
            o << ']';
            break;
        }
        case Type::Object: {
            o << '{';
            size_t i = 0;
            for (auto &kv : v.obj) {
                if (i++) o << (pretty ? ",\n" : ",");
                else if (pretty) o << '\n';
                pad(indent + 1);
                write_str(o, kv.first);
                o << (pretty ? ": " : ":");
                write_value(o, kv.second, pretty, indent + 1);
            }
            if (pretty && !v.obj.empty()) { o << '\n'; pad(indent); }
            o << '}';
            break;
        }
    }
}

} // namespace

bool parse(const std::string &src, Value &out, std::string *err) {
    P p{src.data(), src.data() + src.size(), {}};
    if (!parse_value(p, out)) { if (err) *err = p.err; return false; }
    p.skip();
    return true;
}

std::string serialize(const Value &v, bool pretty) {
    std::ostringstream o;
    write_value(o, v, pretty, 0);
    if (pretty) o << '\n';
    return o.str();
}

} // namespace bn::json
