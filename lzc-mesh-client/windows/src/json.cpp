#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace {
struct Parser {
    const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char *w) {
        size_t n = strlen(w);
        if ((size_t)(end - p) < n || strncmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                if (++p >= end) return false;
                char c = *p++;
                switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += c;
                }
            } else {
                out += *p++;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json &v, int depth) {
        if (depth > 64) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            v.type = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}') { p++; return true; }
            while (true) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p++ != ':') return false;
                if (!value(v.o[k], depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.type = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']') { p++; return true; }
            while (true) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.type = Json::Str; return string(v.s); }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        const char *start = p;
        if (*p == '-') p++;
        if (p == end) return false;
        if (*p == '0') p++;
        else {
            if (*p < '1' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && *p == '.') {
            p++;
            if (p == end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            if (p == end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        const std::string num(start, p);
        double d = strtod(num.c_str(), nullptr);
        if (!std::isfinite(d)) return false;
        v.type = Json::Num;
        v.n = d;
        return true;
    }
};
}  // namespace

bool Json::parse(const std::string &text, Json &out) {
    Parser ps{text.data(), text.data() + text.size()};
    out = Json();
    if (!ps.value(out, 0)) return false;
    ps.ws();
    return ps.p == ps.end;
}

std::string Json::dump() const {
    switch (type) {
    case Null: return "null";
    case Bool: return b ? "true" : "false";
    case Num: {
        if (!std::isfinite(n)) return "null";
        char buf[32];
        snprintf(buf, sizeof buf, "%.17g", n);
        return buf;
    }
    case Str: return JsonOut::esc(s);
    case Arr: {
        std::string out = "[";
        for (size_t i = 0; i < a.size(); i++) { if (i) out += ','; out += a[i].dump(); }
        return out + "]";
    }
    case Obj: {
        std::string out = "{";
        for (const auto &entry : o) {
            if (out.size() > 1) out += ',';
            out += JsonOut::esc(entry.first) + ":" + entry.second.dump();
        }
        return out + "}";
    }
    }
    return "null";
}

std::string JsonOut::esc(const std::string &v) {
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

JsonOut &JsonOut::key(const char *k) {
    if (!first) buf += ",";
    first = false;
    buf += esc(k) + ":";
    return *this;
}

JsonOut &JsonOut::n(const char *k, double v) {
    key(k);
    char b[32];
    if (!std::isfinite(v)) v = 0;
    snprintf(b, sizeof b, "%.17g", v);
    buf += b;
    return *this;
}
