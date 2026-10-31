#include "noting/common/unicode.hpp"

namespace noting {

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16ToUtf8(std::u16string_view text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size()) {
            const char32_t next = text[i + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++i;
                continue;
            }
        }
        appendUtf8(out, unit);
    }
    return out;
}

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        char32_t cp = 0xFFFD;
        std::size_t step = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xC0) == 0x80) {
                cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                if (cp < 0x80) cp = 0xFFFD;
                step = 2;
            }
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                step = 3;
            }
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            const auto b3 = static_cast<unsigned char>(text[i + 3]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
                if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
                step = 4;
            }
        }
        i += step;
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const char32_t v = cp - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        }
    }
    return out;
}

}  // namespace noting
