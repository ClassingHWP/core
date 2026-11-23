#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "chwp/common/unicode.hpp"
#include "chwp/hwp/records.hpp"
#include "hwp/hwp_detail.hpp"

namespace chwp::hwp::detail {

namespace {

struct FormKind {
    const char* type;
    std::uint32_t id;
};
constexpr FormKind kFormKinds[] = {
    {"btn", makeCtrlId('+', 'p', 'b', 't')},      {"checkBtn", makeCtrlId('+', 'c', 'b', 't')},
    {"radioBtn", makeCtrlId('+', 'r', 'b', 't')}, {"comboBox", makeCtrlId('+', 'c', 'o', 'b')},
    {"edit", makeCtrlId('+', 'e', 'd', 't')},     {"listBox", makeCtrlId('+', 'l', 's', 't')},
    {"scrollBar", makeCtrlId('+', 's', 'c', 'r')},
};

// COLORREF(0x00BBGGRR) 정수 ↔ #RRGGBB
std::string colorOf(long long v) {
    char buf[8];
    const unsigned c = static_cast<unsigned>(v);
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu);
    return buf;
}
long long colorRef(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return 0;
    const unsigned rgb = static_cast<unsigned>(std::strtoul(hex.c_str() + 1, nullptr, 16));
    return static_cast<long long>(((rgb >> 16) & 0xFFu) | (rgb & 0xFF00u) | ((rgb & 0xFFu) << 16));
}

// 속성 글 읽기. set 은 안을 다시 읽고, wstring 은 길이만큼, 나머지는 빈칸까지가 값이다.
struct PropReader {
    std::u16string_view s;
    std::size_t at = 0;
    model::FormObject& f;

    std::u16string until(char16_t stop) {
        const std::size_t end = s.find(stop, at);
        const std::size_t to = end == std::u16string_view::npos ? s.size() : end;
        std::u16string out(s.substr(at, to - at));
        at = to + (end == std::u16string_view::npos ? 0 : 1);
        return out;
    }
    void apply(const std::string& name, const std::string& value) {
        const long long n = std::atoll(value.c_str());
        const bool b = n != 0;
        if (name == "Name") f.name = value;
        else if (name == "GroupName") f.groupName = value;
        else if (name == "TabStop") f.tabStop = b;
        else if (name == "TabOrder") f.tabOrder = static_cast<int>(n);
        else if (name == "Command") f.command = value;
        else if (name == "Editable") f.editable = b;
        else if (name == "ForeColor") f.foreColor = colorOf(n);
        else if (name == "BackColor") f.backColor = colorOf(n);
        else if (name == "Enabled") f.enabled = b;
        else if (name == "BorderType") f.borderType = static_cast<int>(n);
        else if (name == "DrawFrame") f.drawFrame = b;
        else if (name == "Printable") f.printable = b;
        else if (name == "CharShapeID") f.charShapeId = static_cast<std::uint32_t>(n);
        else if (name == "FollowContext") f.followContext = b;
        else if (name == "AutoSize") f.autoSize = b;
        else if (name == "WordWrap") f.wordWrap = b;
        else if (name == "Caption") f.caption = value;
        else if (name == "RadioGroupName") f.radioGroupName = value;
        else if (name == "Value") f.value = static_cast<int>(n);
        else if (name == "TriState") f.triState = b;
        else if (name == "BackStyle") f.backStyle = static_cast<int>(n);
        else if (name == "ListBoxRows") f.listBoxRows = static_cast<int>(n);
        else if (name == "ListBoxWidth") f.listBoxWidth = static_cast<int>(n);
        else if (name == "EditEnable") f.editEnable = b;
        else if (name == "Text") f.text = value;
        else if (name == "MultiLine") f.multiLine = b;
        else if (name == "PasswordChar") f.passwordChar = value;
        else if (name == "MaxLength") f.maxLength = static_cast<std::int32_t>(n);
        else if (name == "ScrollBars") f.scrollBars = static_cast<int>(n);
        else if (name == "TabKeyBehavior") f.tabKeyBehavior = static_cast<int>(n);
        else if (name == "Number") f.numOnly = b;
        else if (name == "ReadOnly") f.readOnly = b;
        else if (name == "AlignText") f.alignText = static_cast<int>(n);
    }
    void items(std::size_t end) {
        while (at < end) {
            if (s[at] == u' ') {
                ++at;
                continue;
            }
            const std::string name = utf16ToUtf8(until(u':'));
            const std::string type = utf16ToUtf8(until(u':'));
            if (type == "set" || type == "wstring") {
                const auto len = static_cast<std::size_t>(std::atoll(utf16ToUtf8(until(u':')).c_str()));
                const std::size_t stop = std::min(s.size(), at + len);
                if (type == "set") {
                    items(stop);
                } else {
                    apply(name, utf16ToUtf8(s.substr(at, stop - at)));
                }
                at = stop;
            } else {
                apply(name, utf16ToUtf8(until(u' ')));
            }
        }
    }
};

// 속성 글 쓰기. 아이템마다 뒤에 빈칸 하나, 묶음(set) 뒤에도 빈칸 하나다.
std::u16string num(long long v) { return utf8ToUtf16(std::to_string(v)); }
std::u16string wstr(const std::string& v) {
    const std::u16string w = utf8ToUtf16(v);
    return num(static_cast<long long>(w.size())) + u":" + w;
}
void item(std::u16string& to, const char* name, const char* type, const std::u16string& value) {
    to += utf8ToUtf16(name);
    to += u':';
    to += utf8ToUtf16(type);
    to += u':';
    to += value;
    to += u' ';
}
void set(std::u16string& out, const char* name, const std::u16string& content) {
    out += utf8ToUtf16(name);
    out += u":set:";
    out += num(static_cast<long long>(content.size()));
    out += u':';
    out += content;
    out += u' ';
}

}  // namespace

bool readFormRecord(ByteView data, model::FormObject& out) {
    if (data.size() < 14) return false;
    ByteReader r(data);
    const std::uint32_t id = r.u32();
    r.u32();
    r.u32();
    const std::uint16_t len = r.u16();
    if (r.remaining() < static_cast<std::size_t>(len) * 2u) return false;
    std::u16string text(len, u'\0');
    for (std::uint16_t i = 0; i < len; ++i) text[i] = static_cast<char16_t>(r.u16());
    out = model::FormObject{};
    for (const FormKind& k : kFormKinds) {
        if (k.id == id) out.type = k.type;
    }
    if (out.type.empty()) out.type = "btn";
    PropReader p{text, 0, out};
    p.items(text.size());
    return true;
}

Bytes formRecordBytes(const model::FormObject& f) {
    std::uint32_t id = kFormKinds[0].id;
    for (const FormKind& k : kFormKinds) {
        if (f.type == k.type) id = k.id;
    }
    std::u16string out;
    std::u16string common;
    item(common, "Name", "wstring", wstr(f.name));
    item(common, "GroupName", "wstring", wstr(f.groupName));
    item(common, "TabStop", "bool", num(f.tabStop));
    item(common, "TabOrder", "int", num(f.tabOrder));
    item(common, "Command", "wstring", wstr(f.command));
    item(common, "Editable", "bool", num(f.editable));
    item(common, "ForeColor", "int", num(colorRef(f.foreColor)));
    item(common, "BackColor", "int", num(colorRef(f.backColor)));
    item(common, "Enabled", "bool", num(f.enabled));
    item(common, "BorderType", "int", num(f.borderType));
    item(common, "DrawFrame", "bool", num(f.drawFrame));
    item(common, "Printable", "bool", num(f.printable));
    set(out, "CommonSet", common);
    std::u16string chars;
    item(chars, "CharShapeID", "int", num(f.charShapeId));
    item(chars, "FollowContext", "bool", num(f.followContext));
    item(chars, "AutoSize", "bool", num(f.autoSize));
    item(chars, "WordWrap", "bool", num(f.wordWrap));
    set(out, "CharShapeSet", chars);
    std::u16string own;
    if (f.type == "btn") {
        item(own, "Caption", "wstring", wstr(f.caption));
        set(out, "ButtonSet", own);
    } else if (f.type == "checkBtn" || f.type == "radioBtn") {
        item(own, "Caption", "wstring", wstr(f.caption));
        if (f.type == "radioBtn") item(own, "RadioGroupName", "wstring", wstr(f.radioGroupName));
        item(own, "Value", "int", num(f.value));
        item(own, "TriState", "bool", num(f.triState));
        item(own, "BackStyle", "int", num(f.backStyle));
        set(out, "ButtonSet", own);
    } else if (f.type == "comboBox") {
        item(own, "ListBoxRows", "int", num(f.listBoxRows));
        item(own, "Text", "wstring", wstr(f.text));
        item(own, "ListBoxWidth", "int", num(f.listBoxWidth));
        item(own, "EditEnable", "bool", num(f.editEnable));
        set(out, "ComboBoxSet", own);
    } else if (f.type == "edit") {
        item(own, "Text", "wstring", wstr(f.text));
        item(own, "MultiLine", "bool", num(f.multiLine));
        item(own, "PasswordChar", "wstring", wstr(f.passwordChar));
        item(own, "MaxLength", "int", num(f.maxLength));
        item(own, "ScrollBars", "int", num(f.scrollBars));
        item(own, "TabKeyBehavior", "int", num(f.tabKeyBehavior));
        item(own, "Number", "bool", num(f.numOnly));
        item(own, "ReadOnly", "bool", num(f.readOnly));
        item(own, "AlignText", "int", num(f.alignText));
        set(out, "EditSet", own);
    }
    ByteWriter b;
    b.u32(id);
    b.u32(id);
    b.u32(static_cast<std::uint32_t>(out.size()));
    b.u16(static_cast<std::uint16_t>(out.size()));
    for (const char16_t ch : out) b.u16(static_cast<std::uint16_t>(ch));
    return b.take();
}

}  // namespace chwp::hwp::detail
