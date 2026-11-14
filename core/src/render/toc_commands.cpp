// 차례 만들기·새로 고침(한글 2024 MakeContentsDlg·UpdateTOC·UpdateAllTOC). 한글 화면에서 만들어 저장한 HWPX 로 잰 동작:
// - 캐럿 문단 뒤에 차례 문단을 넣는다. 필드로 넣으면 첫 제목 문단 맨 앞에 차례 필드(%toc, TABLEOFCONTENTS)가 열리고
//   마지막 빈 문단 맨 앞에서 닫힌다. 필드 명령에 만든 설정이 남아 새로 고칠 때 다시 쓴다. 문자열로 넣으면 필드가 없다.
// - 묶음마다 "차례 제목" 스타일 제목(<제목 차례>·<표 차례>·<그림 차례>·<수식 차례>)과 항목, 빈 문단 하나다.
// - 항목은 "차례 1·2·3" 스타일(개요 수준 1·2·3, 더 깊으면 차례 3)이고 글은 "머리 글 + 빈칸 + 문단 글 + 탭 + 쪽 번호"다.
//   탭은 단 너비 끝의 오른쪽 탭(채울 모양은 설정, 기본 점선 3)이거나 문단 오른쪽 끝 자동 탭이다.
// - 하이퍼링크로 만들면 항목 글을 "?#대상id;0;종류;0;" 하이퍼링크 필드로 감싼다. 개요는 종류 1 에 문단 id(대상 문단에
//   문단 id 를 준다), 표·그림·수식은 종류 2·3·4 에 개체 id(HWP instance id = HWPX 개체 id)다.
//   스타일로 모으면 한글은 하이퍼링크를 만들지 않는다.
// - 표·그림·수식 차례는 번호 범주가 같은 개체를 문서 차례로 모두 넣는다(캡션이 없어도). 항목 글은 캡션 첫 문단 글을
//   번호를 채워 그대로 쓰고(앞뒤 빈칸도 그대로), 캡션이 없으면 "표 2 " 처럼 범주 이름, 빈칸, 개체 번호, 빈칸이다.
//   쪽 번호는 개체가 놓인 쪽이다.
// - 설정 명령: "TableOfContents:set:N:ContentsMake:uint:M ContentsStyles:wstring:L:이름; ... ContentsLevel:int:수준
//   ContentsAutoTabRight:int:0|1 ContentsLeader:int:채움 ContentsHyperlink:bool:0|1  ". M 비트: 1 제목 차례, 16 개요 문단,
//   32 스타일, 2 표, 4 그림, 8 수식. N 은 뒤 글 길이 - 1, L 은 이름들 글자 수(UTF-16).
// 쪽 번호는 차례를 넣고 조판한 뒤 채우고, 바뀌지 않을 때까지(세 번까지) 다시 놓는다. 되돌리기는 구역째 한 칸이다.

#include "chwp/render/toc_commands.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/scene_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

constexpr std::uint32_t kMakeTitle = 1;
constexpr std::uint32_t kMakeTables = 2;
constexpr std::uint32_t kMakePictures = 4;
constexpr std::uint32_t kMakeEquations = 8;
constexpr std::uint32_t kMakeOutline = 16;
constexpr std::uint32_t kMakeStyles = 32;

std::size_t utf16Length(const std::string& text) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

const model::Style* styleByName(const model::Document& doc, const std::string& name) {
    for (const model::Style& st : doc.header.styles) {
        if (st.name == name) return &st;
    }
    return nullptr;
}

bool isTocField(const model::InlineItem& item) {
    return item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::Field &&
           item.control.ctrlId == "%toc";
}

// 하이퍼링크 대상 종류(한글 "?#id;0;종류;0;" 의 셋째 값).
constexpr int kTargetOutline = 1;
constexpr int kTargetTable = 2;
constexpr int kTargetFigure = 3;
constexpr int kTargetEquation = 4;

// 차례에 넣을 항목 하나.
struct Entry {
    int target = kTargetOutline;
    std::uint32_t paragraphId = 0;  // 개요: model::Paragraph::id (문단 고유 번호)
    std::uint32_t objectId = 0;     // 표·그림·수식: Placement::instanceId
    int level = 0;                  // 0 이 차례 1
    std::string text;               // 머리 + 문단 글(개요) 또는 캡션 글(개체)
};

std::string paragraphPlainText(const model::Paragraph& para) {
    std::string out;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type != model::InlineItem::Type::Text || item.synthetic) continue;
            for (const char ch : item.text) {
                if (ch == '\t' || ch == '\n') out += ' ';
                else out += ch;
            }
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// 쪽 배치 나무에서 본문 문단의 머리 글과 첫 쪽 번호 글.
struct ParagraphPlace {
    std::string heading;
    std::string page;
};

void collectPlaces(const Node& node, const std::string& page, std::map<std::uint32_t, ParagraphPlace>& out) {
    if (node.kind == NodeKind::Paragraph && node.paragraphId != 0 && !out.count(node.paragraphId)) {
        out[node.paragraphId] = ParagraphPlace{node.ext().headingText, page};
    }
    for (const Node& child : node.children) collectPlaces(child, page, out);
}

// 쪽 배치에서 찾은 자리: 문단(고유 번호) → 머리·첫 쪽, 개체(instance id) → 첫 쪽.
struct Places {
    std::map<std::uint32_t, ParagraphPlace> paragraphs;
    std::map<std::uint32_t, std::string> objects;
};

void collectObjectPages(model::Document& doc, const Node& node, const std::string& page,
                        std::map<std::uint32_t, std::string>& out) {
    if (node.kind == NodeKind::Object && node.ext().obj().anchorStory >= 0) {
        const ObjectExtra& o = node.ext().obj();
        const model::Control* c = anchoredControl(doc, o.anchorStory, o.anchorParagraph, o.anchorItem);
        if (c && c->placement && c->placement->instanceId != 0 && !out.count(c->placement->instanceId)) {
            out[c->placement->instanceId] = page;
        }
    }
    for (const Node& child : node.children) collectObjectPages(doc, child, page, out);
}

Places paragraphPlaces(model::Document& doc, const std::vector<Node>& pages) {
    Places out;
    for (const Node& paper : pages) {
        const std::string label = !paper.ext().pageLabel.empty() ? paper.ext().pageLabel : std::to_string(paper.pageNumber);
        collectPlaces(paper, label, out.paragraphs);
        collectObjectPages(doc, paper, label, out.objects);
    }
    return out;
}

const model::ParaShape* shapeOf(const model::Document& doc, const model::Paragraph& para) {
    return Painter::findParaShape(doc, para.paraShapeId);
}

// 본문(모든 구역)에서 개요·스타일 차례 항목을 모은다. 차례 필드 안 문단은 뺀다.
int tocEndParagraph(const std::vector<model::Paragraph>& paras, int begin);
const model::Control* tocFieldIn(const model::Paragraph& para);

std::vector<Entry> outlineEntries(model::Document& doc, const TocOptions& options) {
    std::vector<Entry> out;
    for (model::Section& section : doc.sections) {
        // 이미 있는 차례 필드 안 문단은 모으지 않는다.
        std::vector<bool> inToc(section.paragraphs.size(), false);
        for (int p = 0; p < static_cast<int>(section.paragraphs.size()); ++p) {
            if (!tocFieldIn(section.paragraphs[static_cast<std::size_t>(p)])) continue;
            const int end = tocEndParagraph(section.paragraphs, p);
            for (int q = p; q <= (end < 0 ? p : end); ++q) inToc[static_cast<std::size_t>(q)] = true;
        }
        for (std::size_t index = 0; index < section.paragraphs.size(); ++index) {
            model::Paragraph& para = section.paragraphs[index];
            if (inToc[index]) continue;
            const model::ParaShape* ps = shapeOf(doc, para);
            const bool outline = ps && ps->headingType == "OUTLINE";
            const int outlineLevel = outline ? static_cast<int>(ps->headingLevel) : 0;
            bool take = options.outline && outline && outlineLevel <= options.level;
            if (!take && options.byStyle) {
                if (const model::Style* st = Painter::findStyle(doc, para.styleId)) {
                    take = std::find(options.styles.begin(), options.styles.end(), st->name) != options.styles.end();
                }
            }
            if (!take) continue;
            const std::string text = paragraphPlainText(para);
            if (text.empty()) continue;
            Entry entry;
            entry.paragraphId = para.id;
            entry.level = outline ? outlineLevel : 0;
            entry.text = text;
            out.push_back(std::move(entry));
        }
    }
    return out;
}

// 캡션 글: 첫 캡션 문단의 글과 표·그림·수식 번호(그 값으로 채운다). 다른 조판 부호는 뺀다.
std::string captionText(const model::Document& doc, const model::Paragraph& para) {
    std::string out;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text && !item.synthetic) {
                for (const char ch : item.text) out += ch == '\t' || ch == '\n' ? ' ' : ch;
            } else if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::AutoNumber &&
                       (item.control.numType == "TABLE" || item.control.numType == "PICTURE" ||
                        item.control.numType == "EQUATION")) {
                const model::Control& c = item.control;
                out += c.prefix +
                       detail::formatNumber(model::objectNumber(doc, &c), c.numFormat.empty() ? "DIGIT" : c.numFormat) +
                       c.suffix;
            }
        }
    }
    return out;
}

// 번호 범주 type 의 개체 차례(표·그림·수식). 캡션 없는 개체도 넣는다. 개체 id 가 없거나 앞 개체와 겹치면 새로 준다.
std::vector<Entry> captionEntries(model::Document& doc, const std::string& type, int target) {
    std::uint32_t maxId = 0;
    std::vector<model::Control*> objects;
    std::function<void(std::vector<model::Paragraph>&, bool)> walk = [&](std::vector<model::Paragraph>& paras, bool take) {
        for (model::Paragraph& para : paras) {
            for (model::Run& run : para.runs) {
                for (model::InlineItem& item : run.items) {
                    if (item.type != model::InlineItem::Type::Control) continue;
                    model::Control& c = item.control;
                    if (c.placement) {
                        maxId = std::max(maxId, c.placement->instanceId);
                        if (take && model::objectNumberingType(c.kind, c.placement->numberingType) == type) {
                            objects.push_back(&c);
                        }
                    }
                    if (c.caption) walk(c.caption->content.paragraphs, take);
                    if (c.table) {
                        for (model::TableCell& cell : c.table->cells) walk(cell.content.paragraphs, take);
                    }
                    for (model::SubList& sub : c.subLists) walk(sub.paragraphs, take);
                    for (model::Control& child : c.children) {
                        for (model::SubList& sub : child.subLists) walk(sub.paragraphs, take);
                    }
                }
            }
        }
    };
    for (model::Section& section : doc.sections) {
        for (model::MasterPage& master : section.masterPages) walk(master.content.paragraphs, false);
        walk(section.paragraphs, true);
    }
    const char* label = type == "TABLE" ? "표" : type == "EQUATION" ? "수식" : "그림";
    std::vector<std::uint32_t> seen;
    std::vector<Entry> out;
    for (model::Control* c : objects) {
        std::uint32_t& id = c->placement->instanceId;
        if (id == 0 || std::find(seen.begin(), seen.end(), id) != seen.end()) id = ++maxId;
        seen.push_back(id);
        Entry entry;
        entry.target = target;
        entry.objectId = id;
        if (c->caption && !c->caption->content.paragraphs.empty()) {
            entry.text = captionText(doc, c->caption->content.paragraphs.front());
        } else {
            entry.text = std::string(label) + " " + std::to_string(model::objectNumber(doc, c)) + " ";
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// 차례 문단 탭: 단 너비 끝 오른쪽 탭(채울 모양) 또는 자동 오른쪽 탭.
std::uint32_t tocTab(model::Document& doc, const TocOptions& options, std::int32_t width) {
    model::TabDef want;
    if (options.autoTab) {
        want.autoTabRight = true;
    } else {
        want.autoTabLeft = true;
        model::TabStop stop;
        stop.pos = width;
        stop.type = "RIGHT";
        stop.leader = tabLeaderName(options.leader);
        want.stops.push_back(stop);
    }
    std::uint32_t maxId = 0;
    for (const model::TabDef& t : doc.header.tabDefs) {
        maxId = std::max(maxId, t.id);
        if (t.autoTabLeft == want.autoTabLeft && t.autoTabRight == want.autoTabRight && t.stops.size() == want.stops.size() &&
            (t.stops.empty() || (t.stops[0].pos == want.stops[0].pos && t.stops[0].type == want.stops[0].type &&
                                 t.stops[0].leader == want.stops[0].leader))) {
            return t.id;
        }
    }
    want.id = doc.header.tabDefs.empty() ? 0 : maxId + 1;
    doc.header.tabDefs.push_back(want);
    return want.id;
}

std::uint32_t tabbedParaShape(model::Document& doc, std::uint32_t paraShapeId, std::uint32_t tab) {
    const model::ParaShape* base = Painter::findParaShape(doc, paraShapeId);
    model::ParaShape copy = base ? *base : model::ParaShape{};
    copy.tabDefId = tab;
    return internParaShape(doc, copy);
}

// 하이퍼링크 항목 글 모양: 스타일 글자 모양에 파란 글자와 파란 아래 밑줄(한글이 만든 차례 그대로).
std::uint32_t linkCharShape(model::Document& doc, std::uint32_t base) {
    const model::CharShape* shape = Painter::findCharShape(doc, base);
    model::CharShape copy = shape ? *shape : model::CharShape{};
    copy.textColor = 0x0000FFu;
    copy.underline = "BOTTOM";
    copy.underlineShape = "SOLID";
    copy.underlineColor = 0x0000FFu;
    return internCharShape(doc, copy);
}

model::Paragraph textParagraph(std::uint32_t styleId, std::uint32_t paraShape, std::uint32_t charShape,
                               const std::string& text) {
    model::Paragraph para;
    para.styleId = styleId;
    para.paraShapeId = paraShape;
    model::Run run;
    run.charShapeId = charShape;
    if (!text.empty()) run.items.push_back(model::InlineItem::makeText(text));
    para.runs.push_back(std::move(run));
    return para;
}

// 문단 고유 번호 → 문단(본문·캡션). 파일 문단 id 를 주려고 찾는다.
model::Paragraph* paragraphById(model::Document& doc, std::uint32_t id) {
    model::Paragraph* found = nullptr;
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (!found && para.id == id) found = &para;
    });
    return found;
}

std::uint32_t nextFileId(model::Document& doc) {
    std::uint32_t maxId = 0;
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (para.fileId < 0x7FFFFFF0u) maxId = std::max(maxId, para.fileId);
    });
    return maxId + 2;
}

// 쪽 번호를 채울 항목: 만든 문단 차례와 대상(개요 문단 고유 번호 또는 개체 id).
struct PageSlot {
    std::size_t index = 0;
    bool paragraph = true;
    std::uint32_t id = 0;
};

struct Built {
    std::vector<model::Paragraph> paragraphs;
    std::vector<PageSlot> pageSlots;
};

// 차례 문단들을 만든다. 쪽 번호 자리는 places 에서 채운다(없으면 빈칸).
Built buildToc(model::Document& doc, const TocOptions& options, std::int32_t width, const Places& places) {
    Built out;
    const model::Style* titleStyle = styleByName(doc, "차례 제목");
    const model::Style base = doc.header.styles.empty() ? model::Style{} : doc.header.styles.front();
    const std::uint32_t tab = tocTab(doc, options, width);
    const auto styleOr = [&](const char* name) { return styleByName(doc, name) ? *styleByName(doc, name) : base; };
    const model::Style title = titleStyle ? *titleStyle : base;
    const model::Style levels[3] = {styleOr("차례 1"), styleOr("차례 2"), styleOr("차례 3")};
    const std::uint32_t titleShape = tabbedParaShape(doc, title.paraShapeId, tab);
    const std::uint32_t blankShape = tabbedParaShape(doc, base.paraShapeId, tab);
    std::uint32_t levelShapes[3];
    for (int i = 0; i < 3; ++i) levelShapes[i] = tabbedParaShape(doc, levels[i].paraShapeId, tab);
    const bool links = options.hyperlink && !options.byStyle;

    const auto group = [&](const std::string& heading, const std::vector<Entry>& entries, bool outline) {
        out.paragraphs.push_back(textParagraph(title.id, titleShape, title.charShapeId, "<" + heading + ">"));
        for (const Entry& entry : entries) {
            const int level = std::clamp(entry.level, 0, 2);
            const model::Style& st = levels[level];
            std::string text = entry.text;
            std::string page;
            if (outline) {
                const auto place = places.paragraphs.find(entry.paragraphId);
                if (place != places.paragraphs.end()) {
                    if (!place->second.heading.empty()) text = place->second.heading + " " + text;
                    page = place->second.page;
                }
            } else if (const auto place = places.objects.find(entry.objectId); place != places.objects.end()) {
                page = place->second;
            }
            model::Paragraph para;
            para.styleId = st.id;
            para.paraShapeId = levelShapes[level];
            if (links) {
                std::uint32_t targetId = entry.objectId;
                if (outline) {
                    model::Paragraph* target = paragraphById(doc, entry.paragraphId);
                    if (target && target->fileId == 0) target->fileId = nextFileId(doc);
                    targetId = target ? target->fileId : 0;
                }
                static const char* targetTypes[] = {"", "HWPHYPERLINK_TARGET_OUTLINE", "HWPHYPERLINK_TARGET_TABLE",
                                                    "HWPHYPERLINK_TARGET_FIGURE", "HWPHYPERLINK_TARGET_EQUATION"};
                model::Control link;
                link.kind = model::ControlKind::Field;
                link.ctrlId = "%hlk";
                link.fieldType = "HYPERLINK";
                link.command = "?#" + std::to_string(targetId) + ";0;" + std::to_string(entry.target) + ";0;";
                link.fieldProps = 0x8000u;
                link.fieldStrings = {{"Category", "HWPHYPERLINK_TYPE_HWP"},
                                     {"TargetType", targetTypes[std::clamp(entry.target, 1, 4)]},
                                     {"DocOpenType", "HWPHYPERLINK_JUMP_CURRENTTAB"}};
                model::Run open;
                open.charShapeId = st.charShapeId;
                open.items.push_back(model::InlineItem::makeControl(std::move(link)));
                para.runs.push_back(std::move(open));
            }
            model::Run body;
            body.charShapeId = links ? linkCharShape(doc, st.charShapeId) : st.charShapeId;
            body.items.push_back(model::InlineItem::makeText(text + "\t" + page));
            para.runs.push_back(std::move(body));
            if (links) {
                model::Control end;
                end.kind = model::ControlKind::FieldEnd;
                end.ctrlId = "fend";
                model::Run close;
                close.charShapeId = st.charShapeId;
                close.items.push_back(model::InlineItem::makeControl(std::move(end)));
                para.runs.push_back(std::move(close));
            }
            out.pageSlots.push_back(PageSlot{out.paragraphs.size(), outline, outline ? entry.paragraphId : entry.objectId});
            out.paragraphs.push_back(std::move(para));
        }
        out.paragraphs.push_back(textParagraph(base.id, blankShape, base.charShapeId, ""));
    };

    if (options.outline || options.byStyle) group("제목 차례", outlineEntries(doc, options), true);
    if (options.tables) group("표 차례", captionEntries(doc, "TABLE", kTargetTable), false);
    if (options.pictures) group("그림 차례", captionEntries(doc, "PICTURE", kTargetFigure), false);
    if (options.equations) group("수식 차례", captionEntries(doc, "EQUATION", kTargetEquation), false);
    if (options.field && !out.paragraphs.empty()) {
        model::Control toc;
        toc.kind = model::ControlKind::Field;
        toc.ctrlId = "%toc";
        toc.fieldType = "TABLEOFCONTENTS";
        toc.command = tocCommand(options);
        toc.fieldProps = 1u;
        toc.fieldExtra = 8;
        model::Paragraph& first = out.paragraphs.front();
        first.runs.front().items.insert(first.runs.front().items.begin(), model::InlineItem::makeControl(std::move(toc)));
        model::Control end;
        end.kind = model::ControlKind::FieldEnd;
        end.ctrlId = "fend";
        model::Paragraph& last = out.paragraphs.back();
        last.runs.front().items.insert(last.runs.front().items.begin(), model::InlineItem::makeControl(std::move(end)));
    }
    return out;
}

std::int32_t bodyWidth(const model::Document& doc, int section) {
    if (section < 0 || section >= static_cast<int>(doc.sections.size())) return 42520;
    const auto& page = doc.sections[static_cast<std::size_t>(section)].page;
    if (!page) return 42520;
    const std::int64_t width = page->landscape ? page->height : page->width;
    return static_cast<std::int32_t>(std::max<std::int64_t>(
        1000, width - page->marginLeft - page->marginRight - page->marginGutter));
}

// 차례 문단 [first, first+count) 의 쪽 번호를 지금 쪽 배치로 다시 쓴다. 바뀌었으면 참이다.
bool refillPages(model::Document& doc, std::vector<model::Paragraph>& paras, std::size_t first, const Built& built,
                 const Places& places) {
    bool changed = false;
    for (const PageSlot& slot : built.pageSlots) {
        model::Paragraph& para = paras[first + slot.index];
        std::string page;
        if (slot.paragraph) {
            const auto place = places.paragraphs.find(slot.id);
            if (place != places.paragraphs.end()) page = place->second.page;
        } else if (const auto place = places.objects.find(slot.id); place != places.objects.end()) {
            page = place->second;
        }
        for (model::Run& run : para.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Text) continue;
                const std::size_t tab = item.text.rfind('\t');
                if (tab == std::string::npos) continue;
                if (item.text.compare(tab + 1, std::string::npos, page) != 0) {
                    item.text = item.text.substr(0, tab + 1) + page;
                    para.lineSegs.clear();
                    changed = true;
                }
            }
        }
    }
    (void)doc;
    return changed;
}

EditResult placeToc(model::Document& doc, std::vector<Node>& pages, int story, int afterPara, const TocOptions& options,
                    bool useStoredLineHeight, std::string& why) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    const model::StorySlot* slot = resolveStory(doc, story);
    if (!paras || !slot || slot->kind != model::StoryKind::Section) {
        why = "차례는 본문에 만듭니다.";
        return {};
    }
    model::ensureParagraphIds(doc);
    const std::int32_t width = bodyWidth(doc, slot->sectionIndex);
    Built built = buildToc(doc, options, width, paragraphPlaces(doc, pages));
    if (built.paragraphs.empty()) {
        why = "만들 차례가 없습니다.";
        return {};
    }
    const std::size_t first = static_cast<std::size_t>(afterPara + 1);
    paras->insert(paras->begin() + static_cast<std::ptrdiff_t>(first), built.paragraphs.begin(), built.paragraphs.end());
    rebuildStoryMap(doc);
    model::ensureParagraphIds(doc);
    for (int pass = 0; pass < 3; ++pass) {
        invalidateLinePlaces({}, true);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        std::vector<model::Paragraph>* now = storyParagraphs(doc, story);
        if (!now || !refillPages(doc, *now, first, built, paragraphPlaces(doc, pages))) break;
    }
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const int caretPara = static_cast<int>(first + built.paragraphs.size()) - 1;
    setPointerCursor(doc, pages, model::CursorPos{story, caretPara, 0});
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, story, caretPara, &full);
}

// story 문단 index 에서 열린 차례 필드의 끝 문단. 없으면 -1 이다.
int tocEndParagraph(const std::vector<model::Paragraph>& paras, int begin) {
    int depth = 0;
    for (int p = begin; p < static_cast<int>(paras.size()); ++p) {
        for (const model::Run& run : paras[static_cast<std::size_t>(p)].runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                if (item.control.kind == model::ControlKind::Field) ++depth;
                if (item.control.kind == model::ControlKind::FieldEnd && --depth == 0) return p;
            }
        }
    }
    return -1;
}

const model::Control* tocFieldIn(const model::Paragraph& para) {
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (isTocField(item)) return &item.control;
        }
    }
    return nullptr;
}

}  // namespace

std::string tabLeaderName(int leader) {
    // 한글 HWPX tabItem leader 이름과 번호(hp:tab leader). 3 은 한글 차례 기본 채움(점선)이다.
    // 탭 채움 번호는 테두리 선과 달리 2 가 점선, 3 이 파선이다(HWP 탭 정의와 같다). 한글 차례 기본 3 은 DASH 로 적힌다.
    static const char* names[] = {"NONE", "SOLID", "DOT", "DASH", "DASH_DOT", "DASH_DOT_DOT", "LONG_DASH", "CIRCLE"};
    return leader >= 0 && leader < 8 ? names[leader] : "DOT";
}

std::string tocCommand(const TocOptions& o) {
    std::uint32_t make = kMakeTitle;
    if (o.outline) make |= kMakeOutline;
    if (o.byStyle) make |= kMakeStyles;
    if (o.tables) make |= kMakeTables;
    if (o.pictures) make |= kMakePictures;
    if (o.equations) make |= kMakeEquations;
    std::string styles;
    for (const std::string& name : o.styles) styles += name + ";";
    std::string rest = "ContentsMake:uint:" + std::to_string(make) + " ContentsStyles:wstring:" +
                       std::to_string(utf16Length(styles)) + ":" + styles + " ContentsLevel:int:" + std::to_string(o.level) +
                       " ContentsAutoTabRight:int:" + (o.autoTab ? "1" : "0") + " ContentsLeader:int:" +
                       std::to_string(o.leader) + " ContentsHyperlink:bool:" + (o.hyperlink ? "1" : "0") + "  ";
    return "TableOfContents:set:" + std::to_string(utf16Length(rest) - 1) + ":" + rest;
}

TocOptions tocOptionsOf(const std::string& command) {
    TocOptions o;
    const auto value = [&](const std::string& key) -> std::string {
        const std::size_t at = command.find(key);
        if (at == std::string::npos) return {};
        std::size_t start = command.find(':', command.find(':', at + key.size()) + 1);
        // key:type:value
        start = command.find(':', at) + 1;
        start = command.find(':', start) + 1;
        const std::size_t end = command.find(' ', start);
        return command.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    const std::string make = value("ContentsMake");
    if (!make.empty()) {
        const std::uint32_t m = static_cast<std::uint32_t>(std::stoul(make));
        o.outline = (m & kMakeOutline) != 0;
        o.byStyle = (m & kMakeStyles) != 0;
        o.tables = (m & kMakeTables) != 0;
        o.pictures = (m & kMakePictures) != 0;
        o.equations = (m & kMakeEquations) != 0;
    }
    if (const std::string level = value("ContentsLevel"); !level.empty()) o.level = std::stoi(level);
    if (const std::string tab = value("ContentsAutoTabRight"); !tab.empty()) o.autoTab = tab == "1";
    if (const std::string leader = value("ContentsLeader"); !leader.empty()) o.leader = std::stoi(leader);
    if (const std::string link = value("ContentsHyperlink"); !link.empty()) o.hyperlink = link == "1";
    // 스타일 이름: "ContentsStyles:wstring:L:이름;이름; "
    if (const std::size_t at = command.find("ContentsStyles:wstring:"); at != std::string::npos) {
        const std::size_t colon = command.find(':', at + 23);
        const std::size_t end = command.find(" Contents", colon);
        const std::string names = command.substr(colon + 1, end == std::string::npos ? std::string::npos : end - colon - 1);
        std::size_t from = 0;
        while (from < names.size()) {
            const std::size_t semi = names.find(';', from);
            if (semi == std::string::npos) break;
            if (semi > from) o.styles.push_back(names.substr(from, semi - from));
            from = semi + 1;
        }
    }
    o.field = true;
    return o;
}

TocOptions tocOptionsOfFields(const FormatFields& fields) {
    TocOptions o;
    for (const auto& [key, value] : fields) {
        const bool* b = std::get_if<bool>(&value);
        const double* n = std::get_if<double>(&value);
        const std::string* t = std::get_if<std::string>(&value);
        if (b) {
            if (key == "field") o.field = *b;
            else if (key == "outline") o.outline = *b;
            else if (key == "byStyle") o.byStyle = *b;
            else if (key == "tables") o.tables = *b;
            else if (key == "pictures") o.pictures = *b;
            else if (key == "equations") o.equations = *b;
            else if (key == "autoTab") o.autoTab = *b;
            else if (key == "hyperlink") o.hyperlink = *b;
        } else if (n) {
            if (key == "level") o.level = std::clamp(static_cast<int>(*n), 0, 9);
            else if (key == "leader") o.leader = std::clamp(static_cast<int>(*n), 0, 7);
        } else if (t && key == "styles") {
            std::size_t from = 0;
            while (from <= t->size()) {
                const std::size_t end = std::min(t->find('\n', from), t->size());
                if (end > from) o.styles.push_back(t->substr(from, end - from));
                from = end + 1;
            }
        }
    }
    return o;
}

EditResult paint_detail::makeTocNow(model::Document& doc, std::vector<Node>& pages, const TocOptions& options,
                                    bool useStoredLineHeight, std::string& why) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) {
        why = "차례를 넣을 곳에 캐럿을 두십시오.";
        return {};
    }
    const int para = cur.hasSelection ? cur.endParagraph : cur.paragraph;
    return placeToc(doc, pages, cur.story, para, options, useStoredLineHeight, why);
}

EditResult paint_detail::updateTocNow(model::Document& doc, std::vector<Node>& pages, bool all, bool useStoredLineHeight,
                                      std::string& why) {
    const PointerCursor cur = pointerCursor();
    EditResult last;
    bool done = false;
    // 고치면 문단 수가 바뀌므로 하나씩 찾아 고친다(모두면 앞에서부터, 고친 것 뒤에서 다시 찾는다).
    for (std::size_t s = 0; s < doc.sections.size(); ++s) {
        const int story = static_cast<int>(s) + 1;
        int from = 0;
        for (;;) {
            std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
            if (!paras) break;
            int begin = -1;
            for (int p = from; p < static_cast<int>(paras->size()); ++p) {
                if (tocFieldIn((*paras)[static_cast<std::size_t>(p)])) {
                    begin = p;
                    break;
                }
            }
            if (begin < 0) break;
            const int end = tocEndParagraph(*paras, begin);
            if (end < 0) break;
            const bool atCaret = cur.story == story && cur.paragraph >= begin && cur.paragraph <= end;
            if (!all && !atCaret) {
                from = end + 1;
                continue;
            }
            const TocOptions options = tocOptionsOf(tocFieldIn((*paras)[static_cast<std::size_t>(begin)])->command);
            paras->erase(paras->begin() + begin, paras->begin() + end + 1);
            rebuildStoryMap(doc);
            invalidateLinePlaces({}, true);
            pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
            last = placeToc(doc, pages, story, begin - 1, options, useStoredLineHeight, why);
            done = done || last.documentChanged;
            std::vector<model::Paragraph>* now = storyParagraphs(doc, story);
            const int again = now ? tocEndParagraph(*now, begin) : -1;
            from = again < 0 ? begin + 1 : again + 1;
            if (!all) break;
        }
    }
    if (!done) {
        if (why.empty()) why = all ? "문서에 차례가 없습니다." : "캐럿이 차례 안에 있지 않습니다.";
        return {};
    }
    return last;
}

}  // namespace chwp::render
