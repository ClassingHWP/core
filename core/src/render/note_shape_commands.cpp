// 주석 모양(한글 2024 주석 모양 대화 상자의 각주 모양·미주 모양 탭).
//
// 칸은 한글 2024 대화 상자에서 확인했다. 번호 모양 목록 0~16 번은 HWPX 이름 DIGIT … DECAGON_CIRCLE_HANJA 와 같은
// 차례다. 구분선 길이 목록은 5cm(-1)·2cm(-2)·단 너비의 1/3(-3)·단 너비(-4)·사용자(HWPUNIT)다. 여백은 mm 로 보인다
// (구분선 위 3.0, 각주 사이 1.0, 구분선 아래 2.0). 번호 매기기는 앞 구역에 이어서·현재 구역부터 새로 시작(시작 번호)·
// 쪽마다 새로 시작, 각주 내용 번호 속성은 보통·작게, 각주 세로 위치는 꼬리말 바로 위·본문 아래,
// 단 각주 위치는 각 단 아래·전단·오른쪽 단 아래, 미주 위치는 문서의 끝·구역의 끝이다.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
    }
    return fallback;
}
bool flagOf(const FormatFields& fields, const std::string& key, bool fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const bool* b = std::get_if<bool>(v)) return *b;
    }
    return fallback;
}

double huToMm1(std::int32_t hu) { return std::round(hu * 254.0 / 7200.0) / 10.0; }
std::int32_t mmToHuRound(double mm) { return static_cast<std::int32_t>(std::lround(mm * 7200.0 / 25.4)); }

const char* const kNumberings[] = {"CONTINUOUS", "ON_SECTION", "ON_PAGE"};
const char* const kFootPlaces[] = {"EACH_COLUMN", "MERGED_COLUMN", "RIGHT_MOST_COLUMN"};
const char* const kEndPlaces[] = {"END_OF_DOCUMENT", "END_OF_SECTION"};

std::string lengthKind(std::int32_t length) {
    if (length == -1) return "5CM";
    if (length == -2) return "2CM";
    if (length == -3) return "THIRD";
    if (length == -4) return "COLUMN";
    return "USER";
}

int indexOf(const char* const* names, int count, const std::string& name, int fallback) {
    for (int i = 0; i < count; ++i) {
        if (name == names[i]) return i;
    }
    return fallback;
}

void applyShape(model::NoteShape& shape, const FormatFields& fields, bool endnote) {
    shape.numFormat = textOf(fields, "numFormat", shape.numFormat);
    shape.userChar = textOf(fields, "userChar", shape.userChar);
    shape.prefix = textOf(fields, "prefix", shape.prefix);
    shape.suffix = textOf(fields, "suffix", shape.suffix);
    const bool line = flagOf(fields, "line", shape.lineLength != 0);
    if (!line) {
        shape.lineLength = 0;
    } else {
        const std::string kind = textOf(fields, "lineLength", shape.lineLength == 0 ? "5CM" : lengthKind(shape.lineLength));
        if (kind == "5CM") shape.lineLength = -1;
        else if (kind == "2CM") shape.lineLength = -2;
        else if (kind == "THIRD") shape.lineLength = -3;
        else if (kind == "COLUMN") shape.lineLength = -4;
        else shape.lineLength = std::max(1, mmToHuRound(numberOf(fields, "lineLengthMm", huToMm1(std::max(1, shape.lineLength)))));
    }
    shape.lineType = textOf(fields, "lineType", shape.lineType);
    shape.lineWidth = static_cast<std::int32_t>(numberOf(fields, "lineWidth", shape.lineWidth));
    shape.lineColor = textOf(fields, "lineColor", shape.lineColor);
    shape.above = std::max(0, mmToHuRound(numberOf(fields, "aboveMm", huToMm1(shape.above))));
    shape.between = std::max(0, mmToHuRound(numberOf(fields, "betweenMm", huToMm1(shape.between))));
    shape.below = std::max(0, mmToHuRound(numberOf(fields, "belowMm", huToMm1(shape.below))));
    shape.numbering = indexOf(kNumberings, 3, textOf(fields, "numbering", kNumberings[std::clamp(shape.numbering, 0, 2)]), 0);
    shape.startNumber = static_cast<std::uint16_t>(
        std::clamp(std::lround(numberOf(fields, "startNumber", shape.startNumber)), 1L, 65535L));
    shape.contentSuperscript = flagOf(fields, "superscript", shape.contentSuperscript);
    shape.beneathText = flagOf(fields, "beneathText", shape.beneathText);
    if (endnote) {
        shape.place = indexOf(kEndPlaces, 2, textOf(fields, "place", kEndPlaces[std::clamp(shape.place, 0, 1)]), 0);
    } else {
        shape.place = indexOf(kFootPlaces, 3, textOf(fields, "place", kFootPlaces[std::clamp(shape.place, 0, 2)]), 0);
    }
}

}  // namespace

FormatFields noteShapeView(const model::Document& doc, bool endnote) {
    FormatFields out;
    if (doc.sections.empty()) return out;
    const model::Section& section = doc.sections[static_cast<std::size_t>(std::max(0, cursorSection(doc)))];
    const model::NoteShape& shape = endnote ? section.endnote : section.footnote;
    out.emplace_back("numFormat", shape.numFormat.empty() ? std::string("DIGIT") : shape.numFormat);
    out.emplace_back("userChar", shape.userChar);
    out.emplace_back("prefix", shape.prefix);
    out.emplace_back("suffix", shape.suffix);
    out.emplace_back("line", shape.lineLength != 0);
    out.emplace_back("lineLength", shape.lineLength == 0 ? std::string("5CM") : lengthKind(shape.lineLength));
    out.emplace_back("lineLengthMm", shape.lineLength > 0 ? huToMm1(shape.lineLength) : 50.0);
    out.emplace_back("lineType", shape.lineType.empty() || shape.lineType == "NONE" ? std::string("SOLID") : shape.lineType);
    out.emplace_back("lineWidth", static_cast<double>(shape.lineWidth));
    out.emplace_back("lineColor", shape.lineColor.empty() ? std::string("#000000") : shape.lineColor);
    out.emplace_back("aboveMm", huToMm1(shape.above));
    out.emplace_back("betweenMm", huToMm1(shape.between));
    out.emplace_back("belowMm", huToMm1(shape.below));
    out.emplace_back("numbering", std::string(kNumberings[std::clamp(shape.numbering, 0, 2)]));
    out.emplace_back("startNumber", static_cast<double>(shape.startNumber));
    out.emplace_back("superscript", shape.contentSuperscript);
    out.emplace_back("beneathText", shape.beneathText);
    out.emplace_back("place", std::string(endnote ? kEndPlaces[std::clamp(shape.place, 0, 1)]
                                                  : kFootPlaces[std::clamp(shape.place, 0, 2)]));
    out.emplace_back("sectionCount", static_cast<double>(doc.sections.size()));
    return out;
}

EditResult noteShapeNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool endnote,
                        bool wholeDocument, bool useStoredLineHeight, std::string& why) {
    if (readOnly() || doc.sections.empty()) return {};
    if (pointerHold().bandStory > 0) {
        why = "머리말·꼬리말을 고치는 중에는 할 수 없습니다.";
        return {};
    }
    clearComposition(pointerHold());
    const auto applyOne = [&](model::Section& section) {
        model::NoteShape& shape = endnote ? section.endnote : section.footnote;
        applyShape(shape, fields, endnote);
        // 저장은 구역 정의 컨트롤 값을 보는 곳이 있다. 같이 고친다.
        if (section.paragraphs.empty()) return;
        for (model::Run& run : section.paragraphs.front().runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::SectionDef) {
                    (endnote ? item.control.endnoteShape : item.control.footnoteShape) = shape;
                }
            }
        }
    };
    if (wholeDocument) {
        for (model::Section& section : doc.sections) applyOne(section);
    } else {
        applyOne(doc.sections[static_cast<std::size_t>(std::max(0, cursorSection(doc)))]);
    }
    const PointerCursor before = pointerCursor();
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (before.active && before.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{before.story, before.paragraph, before.offset});
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

}  // namespace chwp::render
