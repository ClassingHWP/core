// 메모 필드 명령과 모양.
#include "chwp/model/memo.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace chwp::model {

namespace {

// 명령 칸은 '/' 로 나뉘고, 작성자 안의 '/' 등은 '\' 로 막아 둔다(한글 2024 와 같다).
std::vector<std::string> splitCommand(const std::string& command, std::size_t limit, std::string& rest) {
    std::vector<std::string> parts;
    std::string field;
    std::size_t i = 0;
    for (; i < command.size() && parts.size() + 1 < limit; ++i) {
        const char c = command[i];
        if (c == '\\' && i + 1 < command.size()) {
            field += command[++i];
        } else if (c == '/') {
            parts.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    if (parts.size() + 1 < limit) {
        parts.push_back(field);
        rest.clear();
    } else {
        rest = command.substr(i);
        parts.push_back(field);
    }
    return parts;
}

std::uint32_t toU32(const std::string& s, std::uint32_t fallback) {
    if (s.empty()) return fallback;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return fallback;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) return fallback;
    }
    return static_cast<std::uint32_t>(v);
}

std::string escapeAuthor(const std::string& author) {
    std::string out;
    for (const char c : author) {
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == '|' || c == ':' || c == ';') out += '\\';
        out += c;
    }
    return out;
}

void walkMemos(std::vector<Paragraph>& paras, std::vector<Control*>& out);

void walkControl(Control& control, std::vector<Control*>& out) {
    if (isMemoField(control)) out.push_back(&control);
    if (control.table) {
        for (TableCell& cell : control.table->cells) walkMemos(cell.content.paragraphs, out);
    }
    if (!isMemoField(control)) {
        for (SubList& sub : control.subLists) walkMemos(sub.paragraphs, out);
    }
    for (Control& child : control.children) walkControl(child, out);
}

void walkMemos(std::vector<Paragraph>& paras, std::vector<Control*>& out) {
    for (Paragraph& para : paras) {
        for (Run& run : para.runs) {
            for (InlineItem& item : run.items) {
                if (item.type == InlineItem::Type::Control) walkControl(item.control, out);
            }
        }
    }
}

std::tm localOf(std::uint64_t filetime) {
    // FILETIME 은 1601-01-01 부터 100ns 단위다. 유닉스 시각으로 바꿔 현지 시각을 구한다.
    const std::int64_t unix = static_cast<std::int64_t>(filetime / 10000000ull) - 11644473600ll;
    const std::time_t t = static_cast<std::time_t>(unix);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

}  // namespace

bool isMemoField(const Control& control) {
    return control.kind == ControlKind::Field && control.ctrlId.size() == 4 && control.ctrlId.compare(0, 3, "%%m") == 0;
}

MemoCommand parseMemoCommand(const std::string& command) {
    MemoCommand memo;
    std::string rest;
    const std::vector<std::string> parts = splitCommand(command, 7, rest);
    if (!parts.empty() && !parts[0].empty()) memo.kind = parts[0];
    if (parts.size() > 1) memo.shapeRef = toU32(parts[1], kMemoShapeFollowSection);
    if (parts.size() > 2) memo.number = toU32(parts[2], 0);
    if (parts.size() > 4) {
        memo.created = static_cast<std::uint64_t>(toU32(parts[3], 0)) |
                       (static_cast<std::uint64_t>(toU32(parts[4], 0)) << 32);
    }
    if (parts.size() > 5) memo.author = parts[5];
    if (parts.size() > 6) memo.rest = parts[6] + rest;
    return memo;
}

std::string memoCommandText(const MemoCommand& memo) {
    return memo.kind + "/" + std::to_string(memo.shapeRef) + "/" + std::to_string(memo.number) + "/" +
           std::to_string(static_cast<std::uint32_t>(memo.created & 0xFFFFFFFFull)) + "/" +
           std::to_string(static_cast<std::uint32_t>(memo.created >> 32)) + "/" + escapeAuthor(memo.author) + "/" +
           memo.rest;
}

MemoShape defaultMemoShape() { return MemoShape{}; }

MemoShape memoShapeFor(const Document& doc, const Section& section, const Control& memo) {
    const MemoCommand cmd = parseMemoCommand(memo.command);
    std::uint32_t id = cmd.shapeRef != kMemoShapeFollowSection ? cmd.shapeRef : section.memoShapeId;
    if (id >= 1 && id <= doc.header.memoShapes.size()) return doc.header.memoShapes[id - 1];
    return defaultMemoShape();
}

std::vector<Control*> collectMemos(Document& doc) {
    std::vector<Control*> out;
    for (Section& section : doc.sections) walkMemos(section.paragraphs, out);
    return out;
}

std::vector<const Control*> collectMemos(const Document& doc) {
    std::vector<Control*> all = collectMemos(const_cast<Document&>(doc));
    return {all.begin(), all.end()};
}

std::string memoHeadDate(std::uint64_t filetime) {
    if (filetime == 0) return {};
    const std::tm t = localOf(filetime);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min);
    return buf;
}

std::string memoCreateDateTime(std::uint64_t filetime) {
    if (filetime == 0) return {};
    const std::tm t = localOf(filetime);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min, t.tm_sec);
    return buf;
}

std::uint64_t filetimeNow() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const std::int64_t unix100ns = std::chrono::duration_cast<std::chrono::microseconds>(since).count() * 10;
    return static_cast<std::uint64_t>(unix100ns + 116444736000000000ll);
}

}  // namespace chwp::model
