#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "chwp/agent.hpp"
#include "chwp/export/json.hpp"

// agentCall: JSON 요청 하나를 AgentDocument 호출 하나로 바꾸고, 결과를 JSON 한 줄로 쓴다.
// 요청·응답 모양은 skills/chwp-agent/reference.md 에 있다. 판단은 AgentDocument 가 하고 여기서는 모양만 바꾼다.

namespace chwp::agent {

namespace {

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

// ---------------------------------------------------------------------------
// 요청 읽기
// ---------------------------------------------------------------------------

Status bad(std::string message) {
    Status status;
    status.code = Code::BadArgument;
    status.message = std::move(message);
    return status;
}

Status readInt(const Json* v, const char* name, int& out) {
    if (!v || v->type != Json::Type::Number || v->number != std::floor(v->number) || std::fabs(v->number) > 2e9) {
        return bad(std::string(name) + " 은 정수여야 합니다.");
    }
    out = static_cast<int>(v->number);
    return {};
}

Status readOptionalInt(const Json& request, const char* name, int& out) {
    const Json* v = request.get(name);
    return v ? readInt(v, name, out) : Status{};
}

Status readText(const Json* v, const char* name, std::string& out) {
    if (!v || v->type != Json::Type::String) return bad(std::string(name) + " 은 글이어야 합니다.");
    out = v->text;
    return {};
}

// "1:0:3" 꼴. story:문단:글자.
bool parseColonPosition(const std::string& text, Position& out) {
    int values[3] = {0, 0, 0};
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t end = i < 2 ? text.find(':', start) : text.size();
        if (end == std::string::npos || end == start) return false;
        const std::string part = text.substr(start, end - start);
        char* tail = nullptr;
        const long v = std::strtol(part.c_str(), &tail, 10);
        if (tail != part.c_str() + part.size()) return false;
        values[i] = static_cast<int>(v);
        start = end + 1;
    }
    out = {values[0], values[1], values[2]};
    return true;
}

Status readPosition(const Json* v, const char* name, Position& out) {
    if (v && v->type == Json::Type::String) {
        if (parseColonPosition(v->text, out)) return {};
        return bad(std::string(name) + " 은 \"story:문단:글자\" 꼴이어야 합니다.");
    }
    if (!v || v->type != Json::Type::Object) return bad(std::string(name) + " 위치가 없습니다. {story, paragraph, char} 를 주세요.");
    if (Status s = readInt(v->get("story"), "story", out.story); !s.ok()) return s;
    if (Status s = readInt(v->get("paragraph"), "paragraph", out.paragraph); !s.ok()) return s;
    if (Status s = readInt(v->get("char"), "char", out.index); !s.ok()) return s;
    return {};
}

Status readRange(const Json* v, Range& out) {
    if (v && v->type == Json::Type::String) {
        const std::size_t dash = v->text.find('-', 1);
        if (dash != std::string::npos && parseColonPosition(v->text.substr(0, dash), out.from) &&
            parseColonPosition(v->text.substr(dash + 1), out.to)) {
            return {};
        }
        return bad("range 는 \"story:문단:글자-story:문단:글자\" 꼴이어야 합니다.");
    }
    if (!v || v->type != Json::Type::Object) return bad("range 가 없습니다. {from, to} 를 주세요.");
    if (Status s = readPosition(v->get("from"), "from", out.from); !s.ok()) return s;
    return readPosition(v->get("to"), "to", out.to);
}

Status readPage(const Json* v, PageRef& out) {
    if (v && v->type == Json::Type::Number) return readInt(v, "page", out.page);
    if (!v || v->type != Json::Type::Object) {
        return bad("page 가 없습니다. 쪽 번호, {page, section, nth} 또는 {order} 를 주세요.");
    }
    if (Status s = readOptionalInt(*v, "order", out.order); !s.ok()) return s;
    if (v->get("order")) {
        if (out.order < 1) return bad("order 는 1 이상이어야 합니다.");
        return {};
    }
    if (Status s = readInt(v->get("page"), "page", out.page); !s.ok()) return s;
    if (Status s = readOptionalInt(*v, "section", out.section); !s.ok()) return s;
    return readOptionalInt(*v, "nth", out.nth);
}

// set {이름: 값}. 값은 참거짓·숫자·글.
Status readSet(const Json* set, const std::string& what, Fields& out) {
    if (!set || set->type != Json::Type::Object) return bad(what + " 에는 set {이름: 값} 이 있어야 합니다.");
    for (const auto& [key, value] : set->fields) {
        switch (value.type) {
            case Json::Type::Bool: out.emplace_back(key, value.boolean); break;
            case Json::Type::Number: out.emplace_back(key, value.number); break;
            case Json::Type::String: out.emplace_back(key, value.text); break;
            default: return bad("값 " + key + " 은 참거짓·숫자·글이어야 합니다.");
        }
    }
    return {};
}

// 개체 이름. "story:문단:개체" 또는 {story, paragraph, index}.
Status readObject(const Json* v, ObjectRef& out) {
    if (v && v->type == Json::Type::String) {
        Position p;
        if (parseColonPosition(v->text, p)) {
            out = ObjectRef{p.story, p.paragraph, p.index};
            return {};
        }
        return bad("object 는 \"story:문단:개체\" 꼴이어야 합니다.");
    }
    if (!v || v->type != Json::Type::Object) return bad("object 가 없습니다. {story, paragraph, index} 를 주세요.");
    if (Status s = readInt(v->get("story"), "story", out.story); !s.ok()) return s;
    if (Status s = readInt(v->get("paragraph"), "paragraph", out.paragraph); !s.ok()) return s;
    return readInt(v->get("index"), "index", out.index);
}

// 편집기 명령 op. 자리(at·range·object·story·section·page)와 set 을 빼고 남은 글·수·참거짓은 명령 인자다.
// 도형 자리·크기(x·y·width·height)는 길이 글("50mm")도 받는다.
Status readCommand(const Json& v, const std::string& name, EditOp& op) {
    op.kind = OpKind::Command;
    op.command = name;
    for (const auto& [key, value] : v.fields) {
        if (key == "op" || key == "control" || key == "call" || key == "id") continue;
        if (key == "at") {
            if (Status s = readPosition(&value, "at", op.at); !s.ok()) return s;
            op.hasAt = true;
        } else if (key == "range") {
            if (Status s = readRange(&value, op.range); !s.ok()) return s;
            op.hasRange = true;
        } else if (key == "object") {
            if (Status s = readObject(&value, op.object); !s.ok()) return s;
            op.hasObject = true;
        } else if (key == "story") {
            if (Status s = readInt(&value, "story", op.story); !s.ok()) return s;
        } else if (key == "section") {
            if (Status s = readInt(&value, "section", op.section); !s.ok()) return s;
            if (op.section < 1) return bad("section 은 1 부터입니다.");
        } else if (key == "page") {
            if (Status s = readPage(&value, op.page); !s.ok()) return s;
        } else if (key == "set") {
            if (Status s = readSet(&value, name, op.format); !s.ok()) return s;
        } else if ((key == "x" || key == "y" || key == "width" || key == "height") && value.type == Json::Type::String) {
            const Result<std::int32_t> length = parseLength(value.text);
            if (!length.ok()) return bad(key + ": " + length.status.message);
            op.args.emplace_back(key, static_cast<double>(length.value));
        } else {
            switch (value.type) {
                case Json::Type::Bool: op.args.emplace_back(key, value.boolean); break;
                case Json::Type::Number: op.args.emplace_back(key, value.number); break;
                case Json::Type::String: op.args.emplace_back(key, value.text); break;
                default: return bad(name + " 의 " + key + " 은 참거짓·숫자·글이어야 합니다.");
            }
        }
    }
    return {};
}

// 명령 이름(op 또는 pageControl 의 control). 명령이 아니면 빈 글.
std::string commandOf(const Json& v, const std::string& name) {
    std::string command = name;
    if (name == "pageControl") {
        const Json* control = v.get("control");
        command = control && control->type == Json::Type::String ? control->text : std::string();
    }
    for (const std::string& known : commandNames()) {
        if (known == command) return command;
    }
    return {};
}

Status readOp(const Json& v, EditOp& op) {
    if (v.type != Json::Type::Object) return bad("ops 의 항목은 객체여야 합니다.");
    std::string name;
    if (Status s = readText(v.get("op"), "op", name); !s.ok()) return s;
    if (name == "insertText") {
        op.kind = OpKind::InsertText;
        if (Status s = readPosition(v.get("at"), "at", op.at); !s.ok()) return s;
        return readText(v.get("text"), "text", op.text);
    }
    if (name == "delete") {
        op.kind = OpKind::Delete;
        return readRange(v.get("range"), op.range);
    }
    if (name == "replace") {
        op.kind = OpKind::Replace;
        if (Status s = readRange(v.get("range"), op.range); !s.ok()) return s;
        return readText(v.get("text"), "text", op.text);
    }
    if (name == "format") {
        op.kind = OpKind::Format;
        if (Status s = readRange(v.get("range"), op.range); !s.ok()) return s;
        if (const Json* style = v.get("style")) {
            if (Status s = readText(style, "style", op.style); !s.ok()) return s;
        }
        if (const Json* set = v.get("set")) {
            if (set->type != Json::Type::Object) return bad("set 은 {이름: 값} 객체여야 합니다.");
            for (const auto& [key, value] : set->fields) {
                switch (value.type) {
                    case Json::Type::Bool: op.format.emplace_back(key, value.boolean); break;
                    case Json::Type::Number: op.format.emplace_back(key, value.number); break;
                    case Json::Type::String: op.format.emplace_back(key, value.text); break;
                    default: return bad("서식 " + key + " 의 값은 참거짓·숫자·글이어야 합니다.");
                }
            }
        }
        if (op.format.empty() && op.style.empty()) return bad("format 에 set 이나 style 이 없습니다.");
        return {};
    }
    if (name == "insertTable") {
        op.kind = OpKind::InsertTable;
        if (Status s = readPosition(v.get("at"), "at", op.at); !s.ok()) return s;
        if (Status s = readInt(v.get("rows"), "rows", op.rows); !s.ok()) return s;
        if (Status s = readInt(v.get("cols"), "cols", op.cols); !s.ok()) return s;
        if (const Json* cells = v.get("cells")) {
            if (cells->type != Json::Type::Array) return bad("cells 는 [[글, ...], ...] 이어야 합니다.");
            for (const Json& row : cells->items) {
                if (row.type != Json::Type::Array) return bad("cells 의 줄은 배열이어야 합니다.");
                op.cells.emplace_back();
                for (const Json& cell : row.items) {
                    if (cell.type != Json::Type::String) return bad("cells 의 칸은 글이어야 합니다.");
                    op.cells.back().push_back(cell.text);
                }
            }
        }
        return {};
    }
    if (name == "insertPicture") {
        op.kind = OpKind::InsertPicture;
        if (Status s = readPosition(v.get("at"), "at", op.at); !s.ok()) return s;
        int width = 0;
        int height = 0;
        if (Status s = readOptionalInt(v, "width", width); !s.ok()) return s;
        if (Status s = readOptionalInt(v, "height", height); !s.ok()) return s;
        op.width = width;
        op.height = height;
        // 그림은 base64(또는 data URL) 글이나 file 경로다. 경로는 셸이 읽어 준다(callApply).
        const Json* data = v.get("base64");
        const Json* file = v.get("file");
        if (data && file) return bad("insertPicture 는 base64 와 file 가운데 하나만 줍니다.");
        if (data) {
            if (data->type != Json::Type::String) return bad("base64 는 글이어야 합니다.");
            if (!decodeBase64(data->text, op.image)) return bad("base64 를 풀지 못했습니다.");
            return {};
        }
        if (file) return readText(file, "file", op.text);
        return bad("insertPicture 에 base64 나 file 이 없습니다.");
    }
    if (name == "setText") {
        op.kind = OpKind::SetText;
        if (Status s = readInt(v.get("story"), "story", op.story); !s.ok()) return s;
        return readText(v.get("text"), "text", op.text);
    }
    // 표 구조. story 는 표 칸이다.
    const auto tableOp = [&](OpKind kind) -> Status {
        op.kind = kind;
        if (Status s = readInt(v.get("story"), "story", op.story); !s.ok()) return s;
        if (const Json* to = v.get("toStory")) {
            if (Status s = readInt(to, "toStory", op.toStory); !s.ok()) return s;
        }
        return {};
    };
    const auto readSide = [&](const char* afterName, const char* beforeName) -> Status {
        if (const Json* side = v.get("side")) {
            if (side->type != Json::Type::String || (side->text != afterName && side->text != beforeName)) {
                return bad(std::string("side 는 \"") + afterName + "\" 또는 \"" + beforeName + "\" 입니다.");
            }
            op.after = side->text == afterName;
        }
        if (Status s = readOptionalInt(v, "count", op.count); !s.ok()) return s;
        return {};
    };
    if (name == "insertRows") {
        if (Status s = tableOp(OpKind::InsertRows); !s.ok()) return s;
        return readSide("below", "above");
    }
    if (name == "insertColumns") {
        if (Status s = tableOp(OpKind::InsertColumns); !s.ok()) return s;
        return readSide("right", "left");
    }
    if (name == "insertBreak") {
        op.kind = OpKind::InsertBreak;
        if (Status s = readPosition(v.get("at"), "at", op.at); !s.ok()) return s;
        if (const Json* kind = v.get("kind")) return readText(kind, "kind", op.breakKind);
        return {};
    }
    if (name == "pageSetup") {
        op.kind = OpKind::PageSetup;
        if (Status s = readInt(v.get("section"), "section", op.section); !s.ok()) return s;
        if (const Json* whole = v.get("whole")) {
            if (whole->type != Json::Type::Bool) return bad("whole 은 참거짓이어야 합니다.");
            op.whole = whole->boolean;
        }
        const Json* set = v.get("set");
        if (!set || set->type != Json::Type::Object) return bad("pageSetup 에는 set {이름: 값} 이 있어야 합니다.");
        for (const auto& [key, value] : set->fields) {
            switch (value.type) {
                case Json::Type::Bool: op.format.emplace_back(key, value.boolean); break;
                case Json::Type::Number: op.format.emplace_back(key, value.number); break;
                case Json::Type::String: op.format.emplace_back(key, value.text); break;
                default: return bad("값 " + key + " 은 참거짓·숫자·글이어야 합니다.");
            }
        }
        return {};
    }
    if (name == "replaceAll") {
        op.kind = OpKind::ReplaceAll;
        if (Status s = readText(v.get("find"), "find", op.find); !s.ok()) return s;
        if (Status s = readText(v.get("text"), "text", op.text); !s.ok()) return s;
        if (const Json* m = v.get("matchCase")) {
            if (m->type != Json::Type::Bool) return bad("matchCase 는 참거짓이어야 합니다.");
            op.matchCase = m->boolean;
        }
        return {};
    }
    if (name == "cellBorder") {
        if (Status s = tableOp(OpKind::CellBorder); !s.ok()) return s;
        if (Status s = readText(v.get("border"), "border", op.border); !s.ok()) return s;
        if (const Json* t = v.get("lineType")) {
            if (Status s = readText(t, "lineType", op.lineType); !s.ok()) return s;
        }
        if (Status s = readOptionalInt(v, "lineWidth", op.lineWidth); !s.ok()) return s;
        if (const Json* col = v.get("lineColor")) return readText(col, "lineColor", op.lineColor);
        return {};
    }
    if (name == "cellFill") {
        if (Status s = tableOp(OpKind::CellFill); !s.ok()) return s;
        return readText(v.get("color"), "color", op.color);
    }
    if (name == "equalWidth") return tableOp(OpKind::EqualWidth);
    if (name == "equalHeight") return tableOp(OpKind::EqualHeight);
    if (name == "deleteRows") return tableOp(OpKind::DeleteRows);
    if (name == "deleteColumns") return tableOp(OpKind::DeleteColumns);
    if (name == "mergeCells") {
        if (Status s = tableOp(OpKind::MergeCells); !s.ok()) return s;
        if (op.toStory < 0) return bad("mergeCells 에는 toStory(범위의 다른 끝 칸)가 있어야 합니다.");
        return {};
    }
    if (name == "splitCell") {
        if (Status s = tableOp(OpKind::SplitCell); !s.ok()) return s;
        op.rows = 1;
        op.cols = 1;
        if (Status s = readOptionalInt(v, "rows", op.rows); !s.ok()) return s;
        return readOptionalInt(v, "cols", op.cols);
    }
    if (const std::string command = commandOf(v, name); !command.empty()) return readCommand(v, command, op);
    std::string commands;
    for (const std::string& known : commandNames()) commands += ", " + known;
    return bad("op " + name + " 은 모릅니다. insertText, delete, replace, format, insertTable, setText, insertRows, "
                              "insertColumns, deleteRows, deleteColumns, mergeCells, splitCell, cellBorder, cellFill, equalWidth, "
                              "equalHeight, insertBreak, replaceAll, pageSetup, insertPicture" + commands + " 중 하나입니다.");
}

// ---------------------------------------------------------------------------
// 응답 쓰기
// ---------------------------------------------------------------------------

void writePosition(JsonWriter& w, const Position& p) {
    w.beginObject();
    w.field("story", p.story);
    w.field("paragraph", p.paragraph);
    w.field("char", p.index);
    w.endObject();
}

void writePage(JsonWriter& w, const PageRef& ref) {
    w.beginObject();
    w.field("page", ref.page);
    w.field("section", ref.section);
    if (ref.nth > 0) w.field("nth", ref.nth);
    w.endObject();
}

std::string mmText(std::int32_t hu) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", huToMm(hu));
    return buf;
}

const char* formatName(model::SourceFormat format) {
    switch (format) {
        case model::SourceFormat::Hwp: return "hwp";
        case model::SourceFormat::Hwpx: return "hwpx";
        default: return "unknown";
    }
}

void writeTextFormat(JsonWriter& w, const TextFormat& f) {
    w.beginObject();
    w.field("font", f.font);
    if (f.latinFont != f.font) w.field("latinFont", f.latinFont);
    w.field("size", f.size);
    if (f.bold) w.field("bold", true);
    if (f.italic) w.field("italic", true);
    if (!f.underline.empty() && f.underline != "NONE") w.field("underline", f.underline);
    if (f.strikeout) w.field("strikeout", true);
    if (!f.color.empty()) w.field("color", f.color);
    w.endObject();
}

// 서식 값 묶음을 {이름: 값} 으로.
void writeFields(JsonWriter& w, const Fields& fields) {
    w.beginObject();
    for (const auto& [key, value] : fields) {
        if (const bool* b = std::get_if<bool>(&value)) {
            w.field(key, *b);
        } else if (const double* d = std::get_if<double>(&value)) {
            // 정수 값은 정수로 쓴다.
            if (std::floor(*d) == *d && std::fabs(*d) < 2e9) w.field(key, static_cast<int>(*d));
            else w.field(key, *d);
        } else {
            w.field(key, std::get<std::string>(value));
        }
    }
    w.endObject();
}

void writeObjects(JsonWriter& w, const std::vector<ObjectInfo>& objects, bool full) {
    w.key("objects");
    w.beginArray();
    for (const ObjectInfo& o : objects) {
        w.beginObject();
        w.field("index", o.index);
        w.field("kind", o.kind);
        w.field("at", o.at);
        if (full) {
            w.field("inline", o.inlineChar);
            w.field("width", o.width);
            w.field("height", o.height);
            if (!o.placement.empty()) {
                w.key("placement");
                writeFields(w, o.placement);
            }
        }
        if (!o.script.empty()) w.field("script", o.script);
        if (!o.stories.empty()) {
            w.key("stories");
            w.beginArray();
            for (int s : o.stories) w.value(s);
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();
}

void writeStoryHead(JsonWriter& w, const StoryInfo& s) {
    w.field("story", s.story);
    w.field("kind", s.kind);
    w.field("owner", s.owner);
    w.field("section", s.section);
    w.field("paragraphs", s.paragraphs);
    if (s.parentStory >= 0) {
        w.field("parentStory", s.parentStory);
        w.field("parentParagraph", s.parentParagraph);
    }
    if (s.row >= 0) {
        w.field("row", s.row);
        w.field("col", s.col);
        if (s.rowSpan > 1) w.field("rowSpan", s.rowSpan);
        if (s.colSpan > 1) w.field("colSpan", s.colSpan);
    }
}

// 표 칸 그림. 표를 지웠으면 rows 가 0 이고 cells 가 비어 있다.
void writeTable(JsonWriter& w, const TableInfo& t) {
    w.beginObject();
    w.field("parentStory", t.parentStory);
    w.field("parentParagraph", t.parentParagraph);
    w.field("rows", t.rows);
    w.field("cols", t.cols);
    w.key("columnWidths");
    w.beginArray();
    for (const std::int32_t v : t.columnWidths) w.value(v);
    w.endArray();
    w.key("rowHeights");
    w.beginArray();
    for (const std::int32_t v : t.rowHeights) w.value(v);
    w.endArray();
    w.key("cells");
    w.beginArray();
    for (const TableCellInfo& c : t.cells) {
        w.beginObject();
        w.field("story", c.story);
        w.field("row", c.row);
        w.field("col", c.col);
        if (c.rowSpan > 1) w.field("rowSpan", c.rowSpan);
        if (c.colSpan > 1) w.field("colSpan", c.colSpan);
        w.field("width", c.width);
        w.field("height", c.height);
        w.field("text", c.text);
        w.key("format");
        writeFields(w, c.format);
        w.endObject();
    }
    w.endArray();
    w.key("format");
    writeFields(w, t.format);
    if (!t.placement.empty()) {
        w.key("placement");
        writeFields(w, t.placement);
    }
    w.endObject();
}

std::string joinPath(std::string_view dir, const std::string& name) {
    if (dir.empty()) return name;
    std::string path(dir);
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + name;
}

bool endsWith(const std::string& text, std::string_view tail) {
    if (text.size() < tail.size()) return false;
    std::string end = text.substr(text.size() - tail.size());
    std::transform(end.begin(), end.end(), end.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return end == tail;
}

// ---------------------------------------------------------------------------
// 호출
// ---------------------------------------------------------------------------

struct Call {
    AgentDocument& doc;
    const Json& request;
    std::string_view fileDir;
    JsonWriter& w;
    std::vector<CallFile>& files;
    const FileReader& readFile;
};

Status callInfo(Call& c) {
    c.w.field("format", formatName(c.doc.sourceFormat()));
    c.w.field("pages", static_cast<int>(c.doc.pages().size()));
    c.w.field("sections", static_cast<int>(c.doc.model().sections.size()));
    c.w.field("stories", static_cast<int>(c.doc.stories().size()));
    c.w.key("warnings");
    c.w.beginArray();
    for (const std::string& warning : c.doc.warnings()) c.w.value(warning);
    c.w.endArray();
    // 받지 못해 대체 글꼴로 재고 그리는 글꼴. 비어 있지 않으면 줄 나눔·그림이 한글과 다를 수 있다.
    c.w.key("missingFonts");
    c.w.beginArray();
    for (const std::string& face : c.doc.missingFonts()) c.w.value(face);
    c.w.endArray();
    // 어느 글꼴에도 없어 그리지 못하는 글자. 그림에서 비어 나온다. 많으면 앞의 kMaxMissingGlyphs 개만 낸다.
    constexpr std::size_t kMaxMissingGlyphs = 100;
    const std::vector<std::pair<std::string, char32_t>> glyphs = c.doc.missingGlyphs();
    c.w.key("missingGlyphs");
    c.w.beginArray();
    for (std::size_t i = 0; i < glyphs.size() && i < kMaxMissingGlyphs; ++i) {
        char code[16];
        std::snprintf(code, sizeof(code), "U+%04X", static_cast<unsigned>(glyphs[i].second));
        c.w.beginObject();
        c.w.field("font", glyphs[i].first);
        c.w.field("code", std::string(code));
        c.w.endObject();
    }
    c.w.endArray();
    c.w.field("missingGlyphCount", static_cast<int>(glyphs.size()));
    return {};
}

Status callPages(Call& c) {
    c.w.key("pages");
    c.w.beginArray();
    for (const PageInfo& p : c.doc.pages()) {
        c.w.beginObject();
        c.w.field("page", p.ref.page);
        c.w.field("section", p.ref.section);
        if (p.ref.nth > 0) c.w.field("nth", p.ref.nth);
        c.w.field("label", p.label);
        if (p.numberHidden) c.w.field("numberHidden", true);
        c.w.field("order", p.order);
        c.w.field("width", p.width);
        c.w.field("height", p.height);
        c.w.field("sizeMm", mmText(p.width) + "x" + mmText(p.height));
        if (p.landscape) c.w.field("landscape", true);
        c.w.endObject();
    }
    c.w.endArray();
    return {};
}

Status callStories(Call& c) {
    c.w.key("stories");
    c.w.beginArray();
    for (const StoryInfo& s : c.doc.stories()) {
        c.w.beginObject();
        writeStoryHead(c.w, s);
        c.w.endObject();
    }
    c.w.endArray();
    return {};
}

Status callParagraph(Call& c) {
    int story = 0;
    int paragraph = 0;
    if (Status s = readInt(c.request.get("story"), "story", story); !s.ok()) return s;
    if (Status s = readInt(c.request.get("paragraph"), "paragraph", paragraph); !s.ok()) return s;
    const Json* detailArg = c.request.get("detail");
    const bool detail = detailArg && detailArg->type == Json::Type::Bool && detailArg->boolean;
    const Result<ParagraphInfo> info = c.doc.paragraph(story, paragraph, detail);
    if (!info.ok()) return info.status;
    const ParagraphInfo& p = info.value;
    c.w.field("story", p.story);
    c.w.field("paragraph", p.paragraph);
    c.w.field("text", p.text);
    c.w.field("length", p.length);
    c.w.field("style", p.style);
    c.w.field("align", p.align);
    c.w.field("lineSpacingType", p.lineSpacingType);
    c.w.field("lineSpacing", p.lineSpacing);
    c.w.field("indent", p.indent);
    c.w.field("spacingPrev", p.spacingPrev);
    c.w.field("spacingNext", p.spacingNext);
    if (p.pageOrder > 0) c.w.field("pageOrder", p.pageOrder);
    c.w.key("runs");
    c.w.beginArray();
    for (const FormatRun& run : p.runs) {
        c.w.beginObject();
        c.w.field("from", run.from);
        c.w.field("to", run.to);
        if (detail) c.w.field("charShape", static_cast<int>(run.charShapeId));
        c.w.key("format");
        writeTextFormat(c.w, run.format);
        c.w.endObject();
    }
    c.w.endArray();
    writeObjects(c.w, p.objects, true);
    if (detail) {
        // 문단 모양 전부와, runs 의 charShape 번호마다 글자 모양 전부. 이름은 format 의 set 과 같다.
        c.w.key("paraFormat");
        writeFields(c.w, p.format);
        c.w.key("charShapes");
        c.w.beginObject();
        for (const auto& [id, fields] : p.charShapes) {
            c.w.key(std::to_string(id));
            writeFields(c.w, fields);
        }
        c.w.endObject();
    }
    return {};
}

// 여러 story 의 글을 한 번에. 글이 maxChars 를 넘으면 멈추고 다음에 읽을 자리를 next 로 준다.
Status callText(Call& c) {
    int only = 0;
    int startStory = 0;
    int from = 0;
    int count = 0;
    int maxChars = 30000;
    if (Status s = readOptionalInt(c.request, "story", only); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "startStory", startStory); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "from", from); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "count", count); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "maxChars", maxChars); !s.ok()) return s;
    // 빈 문단(글도 개체도 없음)은 기본으로 빼고 낸다. 문단 번호는 그대로다.
    const Json* empty = c.request.get("empty");
    const bool withEmpty = empty && empty->type == Json::Type::Bool && empty->boolean;
    const std::vector<StoryInfo> stories = c.doc.stories();
    if (only != 0 && std::none_of(stories.begin(), stories.end(), [&](const StoryInfo& s) { return s.story == only; })) {
        Status status;
        status.code = Code::PositionOutOfRange;
        status.message = "story " + std::to_string(only) + " 은 없습니다.";
        return status;
    }
    int used = 0;
    bool stopped = false;
    Position next;
    c.w.key("stories");
    c.w.beginArray();
    for (std::size_t i = 0; i < stories.size() && !stopped; ++i) {
        const StoryInfo& story = stories[i];
        if (only != 0 && story.story != only) continue;
        if (story.story < startStory) continue;
        // from·count 는 고른 첫 story(story 또는 next 로 받은 startStory)에만 쓴다.
        const bool firstStory = story.story == (only != 0 ? only : startStory);
        const int begin = firstStory ? std::max(0, from) : 0;
        const int end = firstStory && count > 0 ? std::min(story.paragraphs, begin + count) : story.paragraphs;
        c.w.beginObject();
        writeStoryHead(c.w, story);
        c.w.key("text");
        c.w.beginArray();
        for (int p = begin; p < end; ++p) {
            const Result<ParagraphInfo> info = c.doc.paragraph(story.story, p);
            if (!info.ok()) return info.status;
            if (!withEmpty && info.value.length == 0 && info.value.objects.empty()) continue;
            if (used > 0 && used + info.value.length > maxChars) {
                stopped = true;
                next = {story.story, p, 0};
                break;
            }
            used += info.value.length;
            c.w.beginObject();
            c.w.field("paragraph", p);
            c.w.field("text", info.value.text);
            if (info.value.pageOrder > 0) c.w.field("pageOrder", info.value.pageOrder);
            if (!info.value.objects.empty()) writeObjects(c.w, info.value.objects, false);
            c.w.endObject();
        }
        c.w.endArray();
        c.w.endObject();
    }
    c.w.endArray();
    if (stopped) {
        c.w.key("next");
        c.w.beginObject();
        c.w.field("startStory", next.story);
        c.w.field("from", next.paragraph);
        c.w.endObject();
    }
    return {};
}

Status callFind(Call& c) {
    FindQuery query;
    if (Status s = readText(c.request.get("text"), "text", query.text); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "story", query.story); !s.ok()) return s;
    if (Status s = readOptionalInt(c.request, "limit", query.limit); !s.ok()) return s;
    if (const Json* v = c.request.get("matchCase"); v && v->type == Json::Type::Bool) query.matchCase = v->boolean;
    const Result<std::vector<Match>> found = c.doc.find(query);
    if (!found.ok()) return found.status;
    c.w.key("matches");
    c.w.beginArray();
    for (const Match& m : found.value) {
        c.w.beginObject();
        c.w.key("from");
        writePosition(c.w, m.range.from);
        c.w.key("to");
        writePosition(c.w, m.range.to);
        if (m.page.page > 0) {
            c.w.key("page");
            writePage(c.w, m.page);
        }
        c.w.field("context", m.context);
        c.w.endObject();
    }
    c.w.endArray();
    return {};
}

Status callApply(Call& c) {
    const Json* list = c.request.get("ops");
    if (!list || list->type != Json::Type::Array) return bad("ops 는 배열이어야 합니다.");
    std::vector<EditOp> ops;
    for (std::size_t i = 0; i < list->items.size(); ++i) {
        EditOp op;
        if (Status s = readOp(list->items[i], op); !s.ok()) {
            s.message = "op " + std::to_string(i) + ": " + s.message;
            return s;
        }
        // insertPicture 의 file 은 셸이 읽는다(파일 읽기는 플랫폼 일이다).
        if (op.kind == OpKind::InsertPicture && op.image.empty()) {
            if (!c.readFile) return bad("op " + std::to_string(i) + ": 이 셸은 file 을 읽지 못합니다. base64 로 주세요.");
            if (!c.readFile(op.text, op.image)) {
                return bad("op " + std::to_string(i) + ": 그림 파일 " + op.text + " 을 읽지 못했습니다.");
            }
            op.text.clear();
        }
        ops.push_back(std::move(op));
    }
    const Result<EditReport> report = c.doc.apply(ops);
    if (!report.ok()) return report.status;
    c.w.field("pages", report.value.pages);
    if (!report.value.fontSwaps.empty()) {
        c.w.key("fontSwaps");
        c.w.beginArray();
        for (const auto& [from, to] : report.value.fontSwaps) {
            c.w.beginObject();
            c.w.field("from", from);
            c.w.field("to", to);
            c.w.endObject();
        }
        c.w.endArray();
    }
    c.w.key("ops");
    c.w.beginArray();
    for (const OpReport& op : report.value.ops) {
        c.w.beginObject();
        c.w.key("end");
        writePosition(c.w, op.end);
        if (!op.cells.empty()) {
            c.w.key("cells");
            c.w.beginArray();
            for (const std::vector<int>& row : op.cells) {
                c.w.beginArray();
                for (int s : row) c.w.value(s);
                c.w.endArray();
            }
            c.w.endArray();
        }
        if (op.table.parentStory >= 0) {
            c.w.key("table");
            writeTable(c.w, op.table);
        }
        if (op.replaced >= 0) c.w.field("replaced", op.replaced);
        if (op.object.story >= 0) {
            c.w.key("object");
            c.w.beginObject();
            c.w.field("story", op.object.story);
            c.w.field("paragraph", op.object.paragraph);
            c.w.field("index", op.object.index);
            c.w.endObject();
        }
        c.w.endObject();
    }
    c.w.endArray();
    return {};
}

// 명령 op 가 받는 값(대화 상자 처음 값). 요청이 곧 op 다: {"call":"view","op":"noteShape","section":1}.
Status callView(Call& c) {
    std::string name;
    if (Status s = readText(c.request.get("op"), "op", name); !s.ok()) return s;
    const std::string command = commandOf(c.request, name);
    if (command.empty()) return bad("view: " + name + " 은 명령 op 가 아닙니다.");
    EditOp op;
    if (Status s = readCommand(c.request, command, op); !s.ok()) return s;
    const Result<Fields> values = c.doc.commandView(op);
    if (!values.ok()) return values.status;
    c.w.key("values");
    writeFields(c.w, values.value);
    return {};
}

Status callSection(Call& c) {
    int section = 0;
    if (Status s = readInt(c.request.get("section"), "section", section); !s.ok()) return s;
    const Result<Fields> info = c.doc.section(section);
    if (!info.ok()) return info.status;
    c.w.field("section", section);
    c.w.key("format");
    writeFields(c.w, info.value);
    return {};
}

Status callTable(Call& c) {
    int story = 0;
    if (Status s = readInt(c.request.get("story"), "story", story); !s.ok()) return s;
    const Result<TableInfo> info = c.doc.table(story);
    if (!info.ok()) return info.status;
    c.w.key("table");
    writeTable(c.w, info.value);
    return {};
}

Status callLayout(Call& c) {
    PageRef page;
    if (Status s = readPage(c.request.get("page"), page); !s.ok()) return s;
    const Json* textArg = c.request.get("text");
    const bool withText = !(textArg && textArg->type == Json::Type::Bool && !textArg->boolean);
    const Result<PageLayoutInfo> info = c.doc.layout(page, withText);
    if (!info.ok()) return info.status;
    const PageLayoutInfo& p = info.value;
    c.w.key("page");
    c.w.beginObject();
    c.w.field("page", p.page.page);
    c.w.field("section", p.page.section);
    if (p.page.nth > 0) c.w.field("nth", p.page.nth);
    c.w.endObject();
    c.w.field("width", p.width);
    c.w.field("height", p.height);
    c.w.key("body");
    c.w.beginObject();
    c.w.field("x", p.bodyX);
    c.w.field("y", p.bodyY);
    c.w.field("width", p.bodyWidth);
    c.w.field("height", p.bodyHeight);
    c.w.endObject();
    c.w.key("lines");
    c.w.beginArray();
    for (const LayoutLineInfo& l : p.lines) {
        c.w.beginObject();
        c.w.field("story", l.from.story);
        c.w.field("paragraph", l.from.paragraph);
        c.w.field("from", l.from.index);
        c.w.field("to", l.to);
        c.w.field("x", l.x);
        c.w.field("y", l.y);
        c.w.field("width", l.width);
        c.w.field("height", l.height);
        c.w.field("inkRight", l.inkRight);
        if (!l.heading.empty()) c.w.field("heading", l.heading);
        if (withText) c.w.field("text", l.text);
        if (l.band) c.w.field("band", true);
        if (l.master) c.w.field("master", true);
        if (l.turned) c.w.field("turned", true);
        if (l.overflow) c.w.field("overflow", true);
        c.w.endObject();
    }
    c.w.endArray();
    c.w.key("boxes");
    c.w.beginArray();
    for (const LayoutBoxInfo& b : p.boxes) {
        c.w.beginObject();
        c.w.field("kind", b.kind);
        if (b.story > 0) c.w.field("story", b.story);
        if (b.paragraph >= 0) c.w.field("paragraph", b.paragraph);
        if (b.row >= 0) {
            c.w.field("row", b.row);
            c.w.field("col", b.col);
            if (b.rowSpan > 1) c.w.field("rowSpan", b.rowSpan);
            if (b.colSpan > 1) c.w.field("colSpan", b.colSpan);
        }
        c.w.field("x", b.x);
        c.w.field("y", b.y);
        c.w.field("width", b.width);
        c.w.field("height", b.height);
        if (b.inlineChar) c.w.field("inline", true);
        if (b.band) c.w.field("band", true);
        if (b.master) c.w.field("master", true);
        if (b.turned) c.w.field("turned", true);
        c.w.endObject();
    }
    c.w.endArray();
    return {};
}

Status callRender(Call& c) {
    PageRef page;
    if (Status s = readPage(c.request.get("page"), page); !s.ok()) return s;
    RenderOptions options;
    if (const Json* dpi = c.request.get("dpi")) {
        if (dpi->type != Json::Type::Number || dpi->number <= 0) return bad("dpi 는 양수여야 합니다.");
        options.dpi = dpi->number;
    }
    if (Status s = readOptionalInt(c.request, "pixelHeight", options.pixelHeight); !s.ok()) return s;
    std::string name;
    if (const Json* v = c.request.get("name")) {
        if (Status s = readText(v, "name", name); !s.ok()) return s;
        if (name.find_first_of("/\\:") != std::string::npos) return bad("name 은 폴더 없는 파일 이름이어야 합니다.");
    }
    const Result<Image> image = c.doc.renderPage(page, options);
    if (!image.ok()) return image.status;
    if (name.empty()) {
        name = "page-s" + std::to_string(image.value.page.section) + "-p" + std::to_string(image.value.page.page);
        if (image.value.page.nth > 0) name += "-n" + std::to_string(image.value.page.nth);
    }
    if (!endsWith(name, ".png")) name += ".png";
    const std::string path = joinPath(c.fileDir, name);
    c.w.field("file", path);
    c.w.key("page");
    writePage(c.w, image.value.page);
    c.w.field("width", image.value.width);
    c.w.field("height", image.value.height);
    c.files.push_back({path, image.value.png});
    return {};
}

Status callSave(Call& c) {
    std::string path;
    if (Status s = readText(c.request.get("path"), "path", path); !s.ok()) return s;
    std::string format;
    if (const Json* v = c.request.get("format")) {
        if (Status s = readText(v, "format", format); !s.ok()) return s;
    }
    // PDF: 편집기 인쇄·PDF 로 저장과 같은 쪽 그림. dpi 는 72~600(기본 200)이다.
    const bool pdfPath = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".pdf") == 0 ||
                                              path.compare(path.size() - 4, 4, ".PDF") == 0);
    if (format == "pdf" || (format.empty() && pdfPath)) {
        double dpi = 200;
        if (const Json* v = c.request.get("dpi")) {
            if (v->type != Json::Type::Number || v->number < 72 || v->number > 600) return bad("dpi 는 72~600 입니다.");
            dpi = v->number;
        }
        Result<Bytes> pdf = c.doc.exportPdf(static_cast<int>(dpi));
        if (!pdf.ok()) return pdf.status;
        c.w.field("file", path);
        c.w.field("format", std::string("pdf"));
        c.w.field("bytes", static_cast<std::int64_t>(pdf.value.size()));
        c.files.push_back({path, std::move(pdf.value)});
        return {};
    }
    const std::optional<model::SourceFormat> target = c.doc.saveFormatFor(path, format);
    if (!target) return bad("format 은 hwp, hwpx, pdf 입니다.");
    Result<Bytes> saved = c.doc.save(*target);
    if (!saved.ok()) return saved.status;
    c.w.field("file", path);
    c.w.field("format", formatName(*target));
    c.w.field("bytes", static_cast<std::int64_t>(saved.value.size()));
    c.files.push_back({path, std::move(saved.value)});
    return {};
}

void writeError(JsonWriter& w, const Status& status) {
    w.field("ok", false);
    w.key("error");
    w.beginObject();
    w.field("code", codeName(status.code));
    w.field("message", status.message);
    if (!status.candidates.empty()) {
        w.key("candidates");
        w.beginArray();
        for (const PageRef& ref : status.candidates) writePage(w, ref);
        w.endArray();
    }
    w.endObject();
}

// 바른 UTF-8 인지. 요청 글(이름·경로·넣을 글)은 모두 UTF-8 로 받는다.
bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        int extra = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(extra) >= text.size()) return false;
        for (int k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3F);
        }
        // 짧게 쓸 수 있는 글자를 길게 쓴 것, 대리 영역, U+10FFFF 너머는 받지 않는다.
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

}  // namespace

std::string statusJson(const Status& status) {
    JsonWriter w(false);
    w.beginObject();
    if (status.ok()) w.field("ok", true);
    else writeError(w, status);
    w.endObject();
    return w.take();
}

CallResult agentCall(AgentDocument& doc, std::string_view request, std::string_view fileDir, const FileReader& readFile) {
    CallResult out;
    Json json;
    std::string error;
    JsonWriter w(false);
    w.beginObject();
    if (!validUtf8(request)) {
        writeError(w, bad("요청이 UTF-8 이 아닙니다. 요청 파일과 글을 UTF-8 로 쓰세요."));
        w.endObject();
        out.json = w.take();
        return out;
    }
    if (!JsonReader(request).read(json, error) || json.type != Json::Type::Object) {
        writeError(w, bad(error.empty() ? "요청은 JSON 객체여야 합니다." : error));
        w.endObject();
        out.json = w.take();
        return out;
    }
    if (const Json* id = json.get("id")) {
        if (id->type == Json::Type::String) w.field("id", id->text);
        else if (id->type == Json::Type::Number) w.field("id", static_cast<std::int64_t>(id->number));
    }
    std::string name;
    Status status = readText(json.get("call"), "call", name);
    if (status.ok()) {
        // 응답 몸통은 따로 쓰고, 성공했을 때만 붙인다. 실패하면 error 만 남긴다.
        JsonWriter body(false);
        body.beginObject();
        std::vector<CallFile> files;
        Call call{doc, json, fileDir, body, files, readFile};
        if (name == "info") status = callInfo(call);
        else if (name == "pages") status = callPages(call);
        else if (name == "stories") status = callStories(call);
        else if (name == "paragraph") status = callParagraph(call);
        else if (name == "table") status = callTable(call);
        else if (name == "section") status = callSection(call);
        else if (name == "layout") status = callLayout(call);
        else if (name == "text") status = callText(call);
        else if (name == "find") status = callFind(call);
        else if (name == "apply") status = callApply(call);
        else if (name == "view") status = callView(call);
        else if (name == "undo" || name == "redo") {
            const bool done = name == "undo" ? doc.undo() : doc.redo();
            body.field("done", done);
            body.field("pages", static_cast<int>(doc.pages().size()));
        } else if (name == "render") status = callRender(call);
        else if (name == "save") status = callSave(call);
        else status = bad("call " + name + " 은 모릅니다. info, pages, stories, paragraph, table, section, layout, text, find, apply, view, undo, redo, render, save 중 하나입니다.");
        body.endObject();
        if (status.ok()) {
            out.ok = true;
            w.field("ok", true);
            // body 는 {…} 다. 바깥 객체에 그 안쪽 필드를 잇는다.
            std::string inner = body.take();
            inner = inner.substr(1, inner.size() - 2);
            out.files = std::move(files);
            w.endObject();
            out.json = w.take();
            out.json.pop_back();
            if (!inner.empty()) out.json += "," + inner;
            out.json += "}";
            return out;
        }
    }
    writeError(w, status);
    w.endObject();
    out.json = w.take();
    return out;
}

}  // namespace chwp::agent
