// Minimal JSON value + parser + writer, enough for the hub protocol and the
// LazyCat client's local API. Not a general-purpose library.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json &operator[](const std::string &k) const {
        static Json null;
        auto it = o.find(k);
        return it == o.end() ? null : it->second;
    }
    std::string str(const std::string &def = "") const { return type == Str ? s : def; }
    double num(double def = 0) const { return type == Num ? n : def; }
    bool boolean(bool def = false) const { return type == Bool ? b : def; }
    bool isObj() const { return type == Obj; }

    static bool parse(const std::string &text, Json &out);
};

// Builds a JSON object string incrementally: JsonOut().s("t","key").n("code",30).str()
struct JsonOut {
    std::string buf = "{";
    bool first = true;
    static std::string esc(const std::string &v);
    JsonOut &key(const char *k);
    JsonOut &s(const char *k, const std::string &v) { key(k); buf += esc(v); return *this; }
    JsonOut &n(const char *k, double v);
    JsonOut &b(const char *k, bool v) { key(k); buf += v ? "true" : "false"; return *this; }
    std::string str() { return buf + "}"; }
};
