#pragma once

// scene.cpp 와 scene_edit.cpp 가 함께 쓰는 조판 내부 함수. 정의는 scene.cpp 에 있다.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"
#include "render/line_breaker.hpp"

namespace chwp::render::detail {

using chwp::model::Bullet;
using chwp::model::Caption;
using chwp::model::CharShape;
using chwp::model::Control;
using chwp::model::ControlKind;
using chwp::model::Document;
using chwp::model::InlineItem;
using chwp::model::Lang;
using chwp::model::MasterPage;
using chwp::model::Numbering;
using chwp::model::PageDef;
using chwp::model::PageBorderFill;
using chwp::model::BorderFill;
using chwp::model::BorderSide;
using chwp::model::ParaHead;
using chwp::model::Paragraph;
using chwp::model::Run;
using chwp::model::SubList;
using chwp::model::ParaShape;
using chwp::model::Table;
using chwp::model::TableCell;
using chwp::model::Placement;
using chwp::model::LineSeg;
using chwp::model::NoteShape;
using chwp::model::Section;

struct HeadingState {
    struct Group {
        std::string type;
        std::uint32_t id = 0;
        std::vector<int> values = std::vector<int>(10, 0);
    };
    std::vector<Group> groups;

    Group& group(const std::string& type, std::uint32_t id) {
        for (auto& item : groups) {
            if (item.type == type && item.id == id) return item;
        }
        groups.push_back(Group{type, id, {}});
        groups.back().values.assign(10, 0);
        return groups.back();
    }
};

struct GlyphFace {
    std::string font;
    std::int32_t ratio = 100;
    std::int32_t relSize = 100;
    std::int32_t spacing = 0;
};

enum class SpacingKind { Percent, Fixed, Between, AtLeast };

// 원자가 없는 조판 부호와 문단 끝 표시. 보이지 않아도 한글은 그 글자 모양 크기를 줄 글자 상자에 넣는다.
struct UnseenMark {
    int pos = 0;
    int fontSize = 0;
};

// 문단 글자를 줄 나누기 원자로 바꾸는 방식.
struct AtomOptions {
    // lineseg textpos 칸으로 센다. 탭과 개체는 8칸, 보조 평면 글자는 2칸이다.
    bool storedPositions = false;
    bool deferText = false;    // 글꼴 이름은 그 쪽을 그릴 때 채운다
    bool lazyMetrics = false;  // 폭과 글꼴은 줄에 올릴 때 measureLazyAtom 으로 잰다
    int sectionIndex = -1;
    int pageParagraph = 0;
    std::vector<UnseenMark>* unseen = nullptr;  // 있으면 원자 없는 글자 위치와 크기를 모은다
};

// 글 묶음 번호를 문단과 그 줄에 적는다.
void stampStory(Node& paragraph, int story);

const CharShape* findCharShape(const Document& doc, std::uint32_t id);
const ParaShape* findParaShape(const Document& doc, std::uint32_t id);
const CharShape& fallbackCharShape();
bool nextCodePoint(std::string_view text, std::size_t& index, char32_t& out);

GlyphFace glyphFace(const Document& doc, const CharShape& shape, char32_t cp, bool probeGlyph, bool copyName = true);
std::int32_t glyphWidth(const Document& doc, char32_t cp, const CharShape& face);

bool textAvoids(const std::string& wrap);
bool visualObject(const Control& control);
bool occupiesBodyLine(const Control& control);
// deferSpans 면 안쪽 칸·글상자·캡션의 lineSeg 가 있는 줄은 글자 묶음을 그 쪽을 그릴 때 채운다.
Node makeObject(const Control& control, std::int32_t x, std::int32_t y, const Document& doc, int depth,
                std::int32_t defaultTab, bool useStoredLineHeight, bool deferSpans = false);
// 덧말(tdut)·글자 겹치기(tcps). 글자처럼 줄에 앉는 글자 묶음 개체다(glyph_group.cpp).
bool glyphGroupControl(const Control& control);
std::int32_t glyphGroupWidth(const Document& doc, const Control& control, std::uint32_t charShapeId);
Node makeGlyphGroup(const Control& control, std::uint32_t charShapeId, const Document& doc, std::int32_t x);
// 글자처럼 넣은 원자의 개체 노드. 덧말·글자 겹치기는 글자 묶음, 나머지는 makeObject 다.
Node atomObject(const Atom& atom, std::int32_t x, const Document& doc, int depth, std::int32_t defaultTab,
                bool useStoredLineHeight, bool deferSpans = false);
// 세로쓰기 틀의 개체 자리와 상자(scene.cpp). 개체는 세운 그대로라 틀에서는 너비·높이가 바뀐다.
Placement turnedPlacement(const Placement& p);
// 문단 runs 의 항목을 차례로 센 번호. 없으면 -1 이다. 개체 노드가 문서의 그 컨트롤을 가리킬 때 쓴다.
int itemOrdinal(const model::Paragraph& para, const model::Control* control);
Node uprightObject(Node object);
// 세로쓰기 틀의 원자 폭. 세운 글자는 1em, 눕힌 글자는 가로 폭, 모두 4의 배수로 내리고 자간은 그 폭으로 잰다.
// 글자처럼 넣은 개체는 세운 높이와 위·아래 여백이다. 원자마다 한 번만 넣는다.
void verticalAtom(const Document& doc, Atom& atom, int direction);

bool paragraphStartsPage(const Paragraph& para, const Document& doc);
void applyParagraphBorder(Node& paragraph, const Document& doc, const ParaShape* paraShape);
// counted 면 이 문단 번호는 이미 셌다. 쪽을 넘어 이어지는 부분은 번호를 다시 세지 않고 머리 자리만 받는다.
void applyHeading(Node& paragraph, const Paragraph& para, const Document& doc, HeadingState& state,
                  bool counted = false);
std::int32_t numberingPush(const Node& paragraph, const Document& doc, const Paragraph& para);
void attachHeading(Node& line, const Node& paragraph, const Document& doc, const Paragraph& para, bool shrink);

SpacingKind spacingKindOf(const ParaShape* para);
int spacingValueOf(const ParaShape* para);
int pitchFor(SpacingKind kind, int value, int em);
void placeInlineObjects(Node& line, SpacingKind kind, int spacingValue, bool keepHeight = false);
struct ParagraphSpan {
    std::int32_t marginLeft = 0;
    std::int32_t width = 0;
};
// 문단의 왼쪽 여백과 글 폭. 여백을 뺀 폭이 0.2인치보다 좁으면 왼쪽 여백 없이 0.2인치다.
ParagraphSpan paragraphSpan(std::int32_t columnWidth, const ParaShape* shape);

std::int32_t lineTextBox(const Node& line);
std::int32_t scanAtomHeight(const Atom& atom);
// 각 원자부터 문단 끝까지 가장 큰 scanAtomHeight. 끝 칸은 0이다.
std::vector<std::int32_t> tallestAfter(const std::vector<Atom>& atoms);
std::int32_t scannedVertSize(const Document& doc, const std::vector<Atom>& atoms,
                             const std::vector<std::int32_t>& tallest, std::size_t from, std::size_t to,
                             std::int32_t own, std::int32_t pen, std::int32_t limit);
std::int32_t lineCharBox(const Node& line);

std::int32_t rowContentBottom(const Node& node, bool keepPitch = false);
// 문서에 개요 정의가 없을 때 쓰는 한컴 기본 번호 모양(1. 가. 1) 가) (1) (가) ① ㉮). 번호 문단을 새로 만들 때도 이것을 베낀다.
const model::Numbering& builtinOutline();
std::int32_t columnContentBottom(const Node& body, bool keepPitch = false);
std::int32_t columnCharBottom(const Node& column);

std::vector<Atom> buildAtoms(const Document& doc, const Paragraph& para, const ParaShape* paraShape,
                             const AtomOptions& options);
void measureLazyAtom(const Document& doc, Atom& atom);
// [from, to) 위치에 걸친 원자 없는 글자 중 가장 큰 크기. 없으면 0이다.
int unseenSize(const std::vector<UnseenMark>& marks, int from, int to);

void stampPageChrome(Node& paper, const Document& doc, const Section& section, int pageNumber);

}  // namespace chwp::render::detail
