#pragma once

// 에이전트 호출(agent_call.cpp)과 MCP 서버(mcp_server.cpp)가 같이 쓰는 작은 JSON 읽기. core 안에서만 쓴다.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chwp::agent::json_detail {

// ---------------------------------------------------------------------------
// 작은 JSON 읽기
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(std::string_view key) const {
        for (const auto& [name, value] : fields) {
            if (name == key) return &value;
        }
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool read(Json& out, std::string& error) {
        skip();
        if (!value(out, 0)) {
            error = "JSON 을 읽지 못했습니다(" + std::to_string(pos_) + "번째 바이트 근처).";
            return false;
        }
        skip();
        if (pos_ != text_.size()) {
            error = "JSON 뒤에 남은 글이 있습니다(" + std::to_string(pos_) + "번째 바이트).";
            return false;
        }
        return true;
    }

private:
    void skip() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                                       text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > 64 || pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.type = Json::Type::Bool;
            return true;
        }
        if (literal("null")) return true;
        return number(out);
    }

    bool object(Json& out, int depth) {
        out.type = Json::Type::Object;
        ++pos_;
        skip();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !string(key)) return false;
            skip();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            skip();
            Json item;
            if (!value(item, depth + 1)) return false;
            out.fields.emplace_back(std::move(key), std::move(item));
            skip();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] != '}') return false;
            ++pos_;
            return true;
        }
    }

    bool array(Json& out, int depth) {
        out.type = Json::Type::Array;
        ++pos_;
        skip();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip();
            Json item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] != ']') return false;
            ++pos_;
            return true;
        }
    }

    bool number(Json& out) {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && std::string_view("+-0123456789.eE").find(text_[pos_]) != std::string_view::npos) {
            ++pos_;
        }
        if (pos_ == start) return false;
        const std::string digits(text_.substr(start, pos_ - start));
        char* end = nullptr;
        out.number = std::strtod(digits.c_str(), &end);
        if (end != digits.c_str() + digits.size()) return false;
        out.type = Json::Type::Number;
        return true;
    }

    static void putUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && text_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace chwp::agent::json_detail
