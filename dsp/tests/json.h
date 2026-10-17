// json.h - tiny JSON reader for the test tools (patches). Not part of the DSP core.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cctype>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    double num = 0.0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& key) const {
        if (type != Object) return nullptr;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double number(double def = 0.0) const { return type == Number ? num : (type == Bool ? (b ? 1.0 : 0.0) : def); }
    size_t size() const { return type == Array ? arr.size() : (type == Object ? obj.size() : 0); }
    const Json& operator[](size_t i) const { return arr[i]; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool parse(Json& out) { ws(); if (!value(out)) return false; ws(); return p_ == s_.size(); }
private:
    const std::string& s_; size_t p_ = 0;
    void ws() { while (p_ < s_.size() && std::isspace((unsigned char)s_[p_])) ++p_; }
    bool match(const char* lit) { size_t n = std::strlen(lit); if (s_.compare(p_, n, lit) == 0) { p_ += n; return true; } return false; }
    bool value(Json& v) {
        ws();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = Json::String; return string(v.str); }
        if (match("true"))  { v.type = Json::Bool; v.b = true; return true; }
        if (match("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (match("null"))  { v.type = Json::Null; return true; }
        return number(v);
    }
    bool number(Json& v) {
        const char* start = s_.c_str() + p_;
        char* end = nullptr;
        v.num = std::strtod(start, &end);
        if (end == start) return false;
        p_ += (size_t)(end - start);
        v.type = Json::Number;
        return true;
    }
    bool string(std::string& out) {
        if (s_[p_] != '"') return false;
        ++p_;
        out.clear();
        while (p_ < s_.size() && s_[p_] != '"') {
            if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
                ++p_;
                switch (s_[p_]) { case 'n': out += '\n'; break; case 't': out += '\t'; break; default: out += s_[p_]; }
            } else out += s_[p_];
            ++p_;
        }
        if (p_ >= s_.size()) return false;
        ++p_;
        return true;
    }
    bool array(Json& v) {
        v.type = Json::Array; ++p_; ws();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        for (;;) {
            Json e; if (!value(e)) return false; v.arr.push_back(std::move(e));
            ws(); if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == ']') { ++p_; return true; }
            return false;
        }
    }
    bool object(Json& v) {
        v.type = Json::Object; ++p_; ws();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        for (;;) {
            ws(); std::string k; if (!string(k)) return false;
            ws(); if (p_ >= s_.size() || s_[p_] != ':') return false; ++p_;
            Json e; if (!value(e)) return false;
            v.obj.emplace_back(std::move(k), std::move(e));
            ws(); if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == '}') { ++p_; return true; }
            return false;
        }
    }
};

inline bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

inline bool parseJsonFile(const std::string& path, Json& out) {
    std::string text;
    if (!readTextFile(path, text)) return false;
    JsonParser p(text);
    return p.parse(out);
}
