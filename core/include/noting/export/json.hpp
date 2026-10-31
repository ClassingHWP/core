#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "noting/model/document.hpp"
#include "noting/parse_result.hpp"

namespace noting {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(std::string_view name);

    void value(std::string_view s);
    void value(const char* s) { value(std::string_view(s)); }
    void value(const std::string& s) { value(std::string_view(s)); }
    void value(bool b);
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    void value(T v) {
        if constexpr (std::is_signed_v<T>) writeSigned(static_cast<std::int64_t>(v));
        else writeUnsigned(static_cast<std::uint64_t>(v));
    }
    void null();

    template <typename T>
    void field(std::string_view name, const T& v) {
        key(name);
        value(v);
    }

    [[nodiscard]] const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void beforeValue();
    void newline();
    void writeString(std::string_view s);
    void writeSigned(std::int64_t v);
    void writeUnsigned(std::uint64_t v);

    std::string out_;
    bool pretty_;
    std::vector<bool> hasItems_;
    bool afterKey_ = false;
};

struct JsonOptions {
    bool pretty = false;
    bool includeLineSegs = false;
    bool includeBody = true;
};

std::string documentToJson(const model::Document& doc, const JsonOptions& options = {});

// {"ok", "error", "warnings", "document"} 형태. WASM 바인딩이 그대로 JS 로 넘긴다.
std::string parseResultToJson(const ParseResult& result, const JsonOptions& options = {});

}  // namespace noting
