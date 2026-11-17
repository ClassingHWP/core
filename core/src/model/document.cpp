#include "chwp/model/document.hpp"

#include <cmath>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>

namespace chwp::model {

const char* langName(Lang lang) {
    switch (lang) {
        case Lang::Hangul: return "HANGUL";
        case Lang::Latin: return "LATIN";
        case Lang::Hanja: return "HANJA";
        case Lang::Japanese: return "JAPANESE";
        case Lang::Other: return "OTHER";
        case Lang::Symbol: return "SYMBOL";
        case Lang::User: return "USER";
    }
    return "UNKNOWN";
}

const char* objectNumberingType(ControlKind kind, const std::string& stored) {
    for (const char* name : {"NONE", "PICTURE", "TABLE", "EQUATION"}) {
        if (stored == name) return name;
    }
    if (kind == ControlKind::Table) return "TABLE";
    if (kind == ControlKind::Equation) return "EQUATION";
    if (kind == ControlKind::Picture) return "PICTURE";
    return "NONE";
}

namespace {

constexpr const char* kDutmalPos[] = {"TOP", "BOTTOM", "CENTER"};
constexpr const char* kDutmalAlign[] = {"JUSTIFY", "LEFT", "RIGHT", "CENTER", "DISTRIBUTE", "DISTRIBUTE_SPACE"};
constexpr const char* kComposeCircle[] = {
    "CHAR", "SHAPE_CIRCLE", "SHAPE_REVERSAL_CIRCLE", "SHAPE_RECTANGLE", "SHAPE_REVERSAL_RECTANGLE",
    "SHAPE_TRIANGLE", "SHAPE_REVERSAL_TIRANGLE", "SHAPE_LIGHT", "SHAPE_RHOMBUS", "SHAPE_REVERSAL_RHOMBUS",
    "SHAPE_ROUNDED_RECTANGLE", "SHAPE_EMPTY_CIRCULATE_TRIANGLE", "SHAPE_THIN_CIRCULATE_TRIANGLE",
    "SHAPE_THICK_CIRCULATE_TRIANGLE"};

template <std::size_t N>
const char* nameAt(const char* const (&names)[N], std::uint32_t code) {
    return code < N ? names[code] : names[0];
}
template <std::size_t N>
std::uint32_t codeOf(const char* const (&names)[N], const std::string& name) {
    for (std::uint32_t i = 0; i < N; ++i) {
        if (name == names[i]) return i;
    }
    return 0;
}

}  // namespace

const char* dutmalPosName(std::uint32_t code) { return nameAt(kDutmalPos, code); }
std::uint32_t dutmalPosCode(const std::string& name) { return codeOf(kDutmalPos, name); }
const char* dutmalAlignName(std::uint32_t code) { return nameAt(kDutmalAlign, code); }
std::uint32_t dutmalAlignCode(const std::string& name) { return codeOf(kDutmalAlign, name); }
const char* composeCircleName(std::uint32_t code) { return nameAt(kComposeCircle, code); }
std::uint32_t composeCircleCode(const std::string& name) { return codeOf(kComposeCircle, name); }

const char* controlKindName(ControlKind kind) {
    switch (kind) {
        case ControlKind::Unknown: return "unknown";
        case ControlKind::FieldEnd: return "fieldEnd";
        case ControlKind::SectionDef: return "sectionDef";
        case ControlKind::ColumnDef: return "columnDef";
        case ControlKind::Table: return "table";
        case ControlKind::Picture: return "picture";
        case ControlKind::Shape: return "shape";
        case ControlKind::Container: return "container";
        case ControlKind::Ole: return "ole";
        case ControlKind::Equation: return "equation";
        case ControlKind::Header: return "header";
        case ControlKind::Footer: return "footer";
        case ControlKind::Footnote: return "footnote";
        case ControlKind::Endnote: return "endnote";
        case ControlKind::AutoNumber: return "autoNumber";
        case ControlKind::NewNumber: return "newNumber";
        case ControlKind::PageHiding: return "pageHiding";
        case ControlKind::PageNumPos: return "pageNumPos";
        case ControlKind::Bookmark: return "bookmark";
        case ControlKind::Field: return "field";
        case ControlKind::HiddenComment: return "hiddenComment";
        case ControlKind::Dutmal: return "dutmal";
        case ControlKind::Compose: return "compose";
    }
    return "unknown";
}

std::string Paragraph::text() const {
    std::string out;
    for (const auto& run : runs) {
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) {
                out += item.text;
            }
        }
    }
    return out;
}

namespace {

std::size_t countCodePoints(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

void accumulate(const std::vector<Paragraph>& paragraphs, DocumentStats& stats);

void accumulate(const Control& control, DocumentStats& stats) {
    switch (control.kind) {
        case ControlKind::Table:
            ++stats.tables;
            if (control.table) {
                stats.tableCells += control.table->cells.size();
                for (const auto& cell : control.table->cells) {
                    accumulate(cell.content.paragraphs, stats);
                }
            }
            break;
        case ControlKind::Picture: ++stats.pictures; break;
        case ControlKind::Shape:
        case ControlKind::Container:
        case ControlKind::Ole: ++stats.shapes; break;
        case ControlKind::Equation: ++stats.equations; break;
        case ControlKind::Header:
        case ControlKind::Footer: ++stats.headersFooters; break;
        case ControlKind::Footnote:
        case ControlKind::Endnote: ++stats.notes; break;
        default: ++stats.otherControls; break;
    }
    if (control.caption) accumulate(control.caption->content.paragraphs, stats);
    for (const auto& sub : control.subLists) {
        accumulate(sub.paragraphs, stats);
    }
    for (const auto& child : control.children) {
        accumulate(child, stats);
    }
}

void accumulate(const std::vector<Paragraph>& paragraphs, DocumentStats& stats) {
    for (const auto& para : paragraphs) {
        ++stats.paragraphs;
        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (item.type == InlineItem::Type::Text) {
                    stats.characters += countCodePoints(item.text);
                } else {
                    accumulate(item.control, stats);
                }
            }
        }
    }
}

}  // namespace

namespace {

void visitParagraphs(std::vector<Paragraph>& paragraphs, const std::function<void(Paragraph&)>& fn);

void visitControl(Control& control, const std::function<void(Paragraph&)>& fn) {
    if (control.table) {
        for (auto& cell : control.table->cells) visitParagraphs(cell.content.paragraphs, fn);
    }
    if (control.caption) visitParagraphs(control.caption->content.paragraphs, fn);
    for (auto& sub : control.subLists) visitParagraphs(sub.paragraphs, fn);
    for (auto& child : control.children) visitControl(child, fn);
}

void visitParagraphs(std::vector<Paragraph>& paragraphs, const std::function<void(Paragraph&)>& fn) {
    for (auto& para : paragraphs) {
        fn(para);
        for (auto& run : para.runs) {
            for (auto& item : run.items) {
                if (item.type == InlineItem::Type::Control) visitControl(item.control, fn);
            }
        }
    }
}

std::atomic<std::uint32_t> gNextParagraphId{1};

}  // namespace

void forEachParagraph(Document& doc, const std::function<void(Paragraph&)>& fn) {
    for (auto& section : doc.sections) {
        for (auto& master : section.masterPages) visitParagraphs(master.content.paragraphs, fn);
        visitParagraphs(section.paragraphs, fn);
    }
}

std::string trackDateFromLocal(int year, int month, int day, int hour, int minute) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    const std::time_t t = std::mktime(&local);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:00Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min);
    return buf;
}

void trackDateToLocal(const std::string& iso, int& year, int& month, int& day, int& hour, int& minute) {
    std::tm utc{};
    int sec = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &utc.tm_year, &utc.tm_mon, &utc.tm_mday, &utc.tm_hour, &utc.tm_min,
                    &sec) < 5) {
        year = month = day = hour = minute = 0;
        return;
    }
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&utc);
    std::tm local{};
    localtime_s(&local, &t);
#else
    const std::time_t t = timegm(&utc);
    std::tm local{};
    localtime_r(&t, &local);
#endif
    year = local.tm_year + 1900;
    month = local.tm_mon + 1;
    day = local.tm_mday;
    hour = local.tm_hour;
    minute = local.tm_min;
}

int objectNumber(const Document& doc, const Control* target) {
    if (!target) return 1;
    const bool autoNumber = target->kind == ControlKind::AutoNumber;
    const std::string type = autoNumber ? target->numType
                                        : std::string(objectNumberingType(
                                              target->kind, target->placement ? target->placement->numberingType : ""));
    if (type != "TABLE" && type != "PICTURE" && type != "EQUATION") return 1;
    int value = 0;
    int result = 0;
    bool found = false;
    // owner: 이 문단들이 type 범주 개체의 캡션이면 그 개체 번호, 아니면 -1.
    std::function<void(const std::vector<Paragraph>&, int)> walk = [&](const std::vector<Paragraph>& paras, int owner) {
        for (const Paragraph& para : paras) {
            for (const Run& run : para.runs) {
                for (const InlineItem& item : run.items) {
                    if (found) return;
                    if (item.type != InlineItem::Type::Control) continue;
                    const Control& c = item.control;
                    if (c.kind == ControlKind::AutoNumber && c.numType == type) {
                        const int number = owner >= 0 ? owner : ++value;
                        if (&c == target) {
                            found = true;
                            result = number;
                            return;
                        }
                    } else if (c.kind == ControlKind::NewNumber && c.numType == type && c.number > 0) {
                        value = static_cast<int>(c.number) - 1;
                    }
                    int mine = -1;
                    if (c.placement && objectNumberingType(c.kind, c.placement->numberingType) == type) {
                        mine = ++value;
                        if (&c == target) {
                            found = true;
                            result = mine;
                            return;
                        }
                    }
                    if (c.caption) walk(c.caption->content.paragraphs, mine);
                    if (c.table) {
                        for (const auto& cell : c.table->cells) walk(cell.content.paragraphs, -1);
                    }
                    for (const auto& sub : c.subLists) walk(sub.paragraphs, -1);
                    for (const auto& child : c.children) {
                        for (const auto& sub : child.subLists) walk(sub.paragraphs, -1);
                    }
                }
            }
        }
    };
    for (const auto& section : doc.sections) {
        if (found) break;
        // 구역 설정의 개체 시작 번호(0 은 이어서).
        const std::uint16_t start = type == "PICTURE" ? section.pictureStart
                                    : type == "TABLE" ? section.tableStart
                                                      : section.equationStart;
        if (start > 0) value = static_cast<int>(start) - 1;
        walk(section.paragraphs, -1);
    }
    return found ? std::max(1, result) : std::max<int>(1, static_cast<int>(target->number));
}

int ensureParagraphIds(Document& doc) {
    // 본 번호 표. 번호는 1 부터 차례로 나오므로 번호를 칸 번호로 쓴다. 다 쓰고 나면 찍은 칸만 지운다.
    thread_local std::vector<std::uint8_t> seen;
    thread_local std::vector<std::uint32_t> marked;
    marked.clear();
    int assigned = 0;
    forEachParagraph(doc, [&](Paragraph& para) {
        if (para.id != 0 && para.id < seen.size() && seen[para.id]) para.id = 0;
        if (para.id == 0) {
            para.id = gNextParagraphId.fetch_add(1, std::memory_order_relaxed);
            ++assigned;
        }
        if (para.id >= seen.size()) seen.resize(std::max<std::size_t>(para.id + 1, seen.size() * 2), 0);
        seen[para.id] = 1;
        marked.push_back(para.id);
    });
    for (const std::uint32_t id : marked) seen[id] = 0;
    return assigned;
}

DocumentStats computeStats(const Document& doc) {
    DocumentStats stats;
    stats.sections = doc.sections.size();
    for (const auto& section : doc.sections) {
        accumulate(section.paragraphs, stats);
    }
    return stats;
}

const char* storyKindName(StoryKind kind) {
    switch (kind) {
        case StoryKind::None: return "none";
        case StoryKind::Section: return "section";
        case StoryKind::TableCell: return "tableCell";
        case StoryKind::SubList: return "subList";
        case StoryKind::Caption: return "caption";
        case StoryKind::MasterPage: return "masterPage";
    }
    return "none";
}

void StoryMap::clear() {
    doc_ = nullptr;
    slots_.clear();
    byPtr_.clear();
    byId_.clear();
}

void StoryMap::build(Document& doc) {
    clear();
    doc_ = &doc;
    slots_.push_back(StorySlot{});  // 0번은 빈 슬롯 (유효한 story 번호는 1부터 시작)

    auto registerSlot = [&](StorySlot slot) -> int {
        const int id = static_cast<int>(slots_.size());
        slot.story = id;
        if (slot.paragraphs) byPtr_[slot.paragraphs] = id;
        slots_.push_back(slot);
        return id;
    };

    auto walkParagraphs = [&](auto&& self, std::vector<Paragraph>& paragraphs, int sectionIndex,
                              int currentStory) -> void {
        auto walkControl = [&](auto&& selfCtrl, Control& control, int parentParagraph) -> void {
            if (control.table) {
                for (std::size_t ci = 0; ci < control.table->cells.size(); ++ci) {
                    TableCell& cell = control.table->cells[ci];
                    StorySlot slot;
                    slot.kind = StoryKind::TableCell;
                    slot.paragraphs = &cell.content.paragraphs;
                    slot.sectionIndex = sectionIndex;
                    slot.parentStory = currentStory;
                    slot.parentParagraph = parentParagraph;
                    slot.control = &control;
                    slot.tableCell = &cell;
                    slot.subList = &cell.content;
                    slot.subIndex = static_cast<int>(ci);
                    const int cellStory = registerSlot(slot);
                    self(self, cell.content.paragraphs, sectionIndex, cellStory);
                }
            }
            for (Control& child : control.children) {
                selfCtrl(selfCtrl, child, parentParagraph);
            }
            for (std::size_t si = 0; si < control.subLists.size(); ++si) {
                SubList& sub = control.subLists[si];
                StorySlot slot;
                slot.kind = StoryKind::SubList;
                slot.paragraphs = &sub.paragraphs;
                slot.sectionIndex = sectionIndex;
                slot.parentStory = currentStory;
                slot.parentParagraph = parentParagraph;
                slot.control = &control;
                slot.subList = &sub;
                slot.subIndex = static_cast<int>(si);
                const int subStory = registerSlot(slot);
                self(self, sub.paragraphs, sectionIndex, subStory);
            }
            if (control.caption) {
                Caption& cap = *control.caption;
                StorySlot slot;
                slot.kind = StoryKind::Caption;
                slot.paragraphs = &cap.content.paragraphs;
                slot.sectionIndex = sectionIndex;
                slot.parentStory = currentStory;
                slot.parentParagraph = parentParagraph;
                slot.control = &control;
                slot.caption = &cap;
                slot.subList = &cap.content;
                slot.subIndex = 0;
                const int capStory = registerSlot(slot);
                self(self, cap.content.paragraphs, sectionIndex, capStory);
            }
        };

        for (std::size_t pi = 0; pi < paragraphs.size(); ++pi) {
            Paragraph& para = paragraphs[pi];
            for (Run& run : para.runs) {
                for (InlineItem& item : run.items) {
                    if (item.type == InlineItem::Type::Control) {
                        walkControl(walkControl, item.control, static_cast<int>(pi));
                    }
                }
            }
        }
    };

    for (std::size_t si = 0; si < doc.sections.size(); ++si) {
        Section& section = doc.sections[si];
        StorySlot secSlot;
        secSlot.kind = StoryKind::Section;
        secSlot.paragraphs = &section.paragraphs;
        secSlot.sectionIndex = static_cast<int>(si);
        secSlot.subIndex = static_cast<int>(si);
        const int secStory = registerSlot(secSlot);
        walkParagraphs(walkParagraphs, section.paragraphs, static_cast<int>(si), secStory);

        for (std::size_t mi = 0; mi < section.masterPages.size(); ++mi) {
            MasterPage& master = section.masterPages[mi];
            StorySlot masterSlot;
            masterSlot.kind = StoryKind::MasterPage;
            masterSlot.paragraphs = &master.content.paragraphs;
            masterSlot.sectionIndex = static_cast<int>(si);
            masterSlot.parentStory = secStory;
            masterSlot.masterPage = &master;
            masterSlot.subList = &master.content;
            masterSlot.subIndex = static_cast<int>(mi);
            const int masterStory = registerSlot(masterSlot);
            walkParagraphs(walkParagraphs, master.content.paragraphs, static_cast<int>(si), masterStory);
        }
    }
    // 문단 번호 색인. 별칭 없이 처음 붙인 글 묶음 번호를 쓴다.
    for (std::size_t story = 1; story < slots_.size(); ++story) {
        const std::vector<Paragraph>* paragraphs = slots_[story].paragraphs;
        if (!paragraphs) continue;
        for (std::size_t pi = 0; pi < paragraphs->size(); ++pi) {
            const std::uint32_t id = (*paragraphs)[pi].id;
            if (id != 0) byId_.try_emplace(id, ParagraphAt{static_cast<int>(story), static_cast<int>(pi)});
        }
    }
}

StoryMap::ParagraphAt StoryMap::findParagraph(std::uint32_t id) const {
    const auto found = byId_.find(id);
    return found == byId_.end() ? ParagraphAt{} : found->second;
}

void StoryMap::build(const Document& doc) {
    build(const_cast<Document&>(doc));
}

const StorySlot* StoryMap::slot(int story) const {
    if (!valid(story)) return nullptr;
    return &slots_[static_cast<std::size_t>(story)];
}

StorySlot* StoryMap::slot(int story) {
    if (!valid(story)) return nullptr;
    return &slots_[static_cast<std::size_t>(story)];
}

const std::vector<Paragraph>* StoryMap::paragraphs(int story) const {
    const StorySlot* s = slot(story);
    return s ? s->paragraphs : nullptr;
}

std::vector<Paragraph>* StoryMap::paragraphs(int story) {
    StorySlot* s = slot(story);
    return s ? s->paragraphs : nullptr;
}

const Paragraph* StoryMap::paragraph(int story, int paragraphIndex) const {
    const std::vector<Paragraph>* list = paragraphs(story);
    if (!list || paragraphIndex < 0 || paragraphIndex >= static_cast<int>(list->size())) return nullptr;
    return &(*list)[static_cast<std::size_t>(paragraphIndex)];
}

Paragraph* StoryMap::paragraph(int story, int paragraphIndex) {
    std::vector<Paragraph>* list = paragraphs(story);
    if (!list || paragraphIndex < 0 || paragraphIndex >= static_cast<int>(list->size())) return nullptr;
    return &(*list)[static_cast<std::size_t>(paragraphIndex)];
}

CursorTarget StoryMap::resolve(CursorPos pos) {
    CursorTarget target;
    target.story = pos.story;
    target.paragraphIndex = pos.paragraph;
    target.offset = pos.offset;
    target.slot = slot(pos.story);
    if (!target.slot || !target.slot->paragraphs) return target;
    target.paragraphs = target.slot->paragraphs;
    if (pos.paragraph >= 0 && pos.paragraph < static_cast<int>(target.paragraphs->size())) {
        target.paragraph = &(*target.paragraphs)[static_cast<std::size_t>(pos.paragraph)];
    }
    return target;
}

int StoryMap::storyOf(const std::vector<Paragraph>* paragraphs) const {
    if (!paragraphs) return -1;
    const auto it = byPtr_.find(paragraphs);
    return it != byPtr_.end() ? it->second : -1;
}

int StoryMap::aliasStory(int baseStory) {
    if (!valid(baseStory)) return reserveSynthetic();
    if (slots_.empty()) slots_.push_back(StorySlot{});
    StorySlot copy = slots_[static_cast<std::size_t>(baseStory)];
    const int id = static_cast<int>(slots_.size());
    copy.story = id;
    slots_.push_back(copy);
    return id;
}

int StoryMap::reserveSynthetic() {
    if (slots_.empty()) slots_.push_back(StorySlot{});
    const int id = static_cast<int>(slots_.size());
    StorySlot empty;
    empty.story = id;
    slots_.push_back(empty);
    return id;
}

void StoryMap::bindTempPointer(const std::vector<Paragraph>* tempPtr, int story) {
    if (tempPtr && story > 0) byPtr_[tempPtr] = story;
}

void StoryMap::unbindTempPointer(const std::vector<Paragraph>* tempPtr) {
    if (tempPtr) byPtr_.erase(tempPtr);
}

int StoryMap::rootSectionStory(int story) const {
    int cur = story;
    for (int depth = 0; depth < 32 && valid(cur); ++depth) {
        const StorySlot& s = slots_[static_cast<std::size_t>(cur)];
        if (s.kind == StoryKind::Section) {
            const int canonical = storyOf(s.paragraphs);
            return canonical > 0 ? canonical : s.story;
        }
        if (s.parentStory <= 0) break;
        cur = s.parentStory;
    }
    return -1;
}

int StoryMap::rootSectionParagraph(int story) const {
    int cur = story;
    for (int depth = 0; depth < 32 && valid(cur); ++depth) {
        const StorySlot& s = slots_[static_cast<std::size_t>(cur)];
        if (s.parentStory <= 0 || !valid(s.parentStory)) return -1;
        const StorySlot& parent = slots_[static_cast<std::size_t>(s.parentStory)];
        if (parent.kind == StoryKind::Section) return s.parentParagraph;
        cur = s.parentStory;
    }
    return -1;
}

namespace {

template <std::size_t N>
const char* nameAt(const char* const (&names)[N], int code) {
    return code >= 0 && code < static_cast<int>(N) ? names[code] : "";
}

template <std::size_t N>
int codeOf(const char* const (&names)[N], const std::string& name, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (name == names[i]) return static_cast<int>(i);
    }
    return fallback;
}

constexpr const char* kTextArtAligns[] = {"LEFT", "RIGHT", "CENTER", "FULL", "TABLE"};
// 글맵시 그림자 종류. 한글 2024 는 HWP 번호를 그리기 개체 그림자 이름으로 HWPX 에 쓴다(1 이 PARELLEL_LEFTTOP).
constexpr const char* kTextArtShadows[] = {"NONE",          "PARELLEL_LEFTTOP",  "PARELLEL_RIGHTTOP",
                                           "PARELLEL_LEFTBOTTOM", "PARELLEL_RIGHTBOTTOM", "SHEAR_LEFTTOP",
                                           "SHEAR_RIGHTTOP", "SHEAR_LEFTBOTTOM", "SHEAR_RIGHTBOTTOM"};
constexpr const char* kOleTypes[] = {"UNKNOWN", "EMBEDDED", "LINK", "STATIC", "EQUATION"};

}  // namespace

const char* textArtAlignName(int code) {
    const char* name = nameAt(kTextArtAligns, code);
    return *name ? name : "LEFT";
}
int textArtAlignCode(const std::string& name) { return codeOf(kTextArtAligns, name, 0); }
const char* textArtShadowName(int code) {
    const char* name = nameAt(kTextArtShadows, code);
    return *name ? name : "NONE";
}
int textArtShadowCode(const std::string& name) {
    // 한글은 DROP(비연속)·CONTINUOUS(연속) 도 읽는다(1·2). 모르는 이름(ENLARGE 등)은 없음이다.
    if (name == "DROP") return 1;
    if (name == "CONTINUOUS") return 2;
    return codeOf(kTextArtShadows, name, 0);
}
const char* oleObjectTypeName(int code) {
    const char* name = nameAt(kOleTypes, code);
    return *name ? name : "UNKNOWN";
}
int oleObjectTypeCode(const std::string& name) { return codeOf(kOleTypes, name, 0); }
// 보이는 면(DVASPECT): 1 CONTENT, 2 THUMB_NAIL, 4 ICON, 8 DOC_PRINT.
const char* oleAspectName(int code) {
    switch (code) {
        case 2: return "THUMB_NAIL";
        case 4: return "ICON";
        case 8: return "DOC_PRINT";
        default: return "CONTENT";
    }
}
int oleAspectCode(const std::string& name) {
    if (name == "THUMB_NAIL") return 2;
    if (name == "ICON") return 4;
    if (name == "DOC_PRINT") return 8;
    return 1;
}

// 글맵시 모양 번호 → textShape 이름. 번호가 자리다(한글 2024 로 HWPX 를 열어 HWP 로 저장해 쟀다).
constexpr const char* kTextArtShapes[] = {
    "PARALLELOGRAM", "INVERTED_PARALLELOGRAM", "INVERTED_UPWARD_CASCADE", "INVERTED_DOWNWARD_CASCADE",
    "UPWARD_CASCADE", "DOWNWARD_CASCADE", "REDUCE_RIGHT", "REDUCE_LEFT", "ISOSCELES_TRAPEZOID",
    "INVERTED_ISOSCELES_TRAPEZOID", "TOP_RIBBON_RECTANGLE", "BOTTOM_RIBBON_RECTANGLE", "CHEVRON_DOWN", "CHEVRON",
    "BOW_TIE", "HEXAGON", "WAVE1", "WAVE2", "WAVE3", "WAVE4", "LEFT_TILT_CYLINDER", "RIGHT_TILT_CYLINDER",
    "BOTTOM_WIDE_CYLINDER", "TOP_WIDE_CYLINDER", "THIN_CURVE_UP1", "THIN_CURVE_UP2", "THIN_CURVE_DOWN1",
    "THIN_CURVE_DOWN2", "INVERSED_FINGERNAIL", "FINGERNAIL", "GINKO_LEAF1", "GINKO_LEAF2", "INFLATE_RIGHT",
    "INFLATE_LEFT", "INFLATE_UP_CONVEX", "INFLATE_BOTTOM_CONVEX", "DEFLATE_TOP", "DEFLATE_BOTTOM", "DEFLATE",
    "INFLATE", "INFLATE_TOP", "INFLATE_BOTTOM", "RECTANGLE", "LEFT_CYLINDER", "CYLINDER", "RIGHT_CYLINDER",
    "CIRCLE", "CURVE_DOWN", "ARCH_UP", "ARCH_DOWN", "SINGLE_LINE_CIRCLE1", "SINGLE_LINE_CIRCLE2",
    "TRIPLE_LINE_CIRCLE1", "TRIPLE_LINE_CIRCLE2", "DOUBLE_LINE_CIRCLE",
};

const char* textArtShapeName(int code) { return nameAt(kTextArtShapes, code); }
int textArtShapeCode(const std::string& name) { return name.empty() ? -1 : codeOf(kTextArtShapes, name, -1); }

Document blankDocument() {
    Document doc;
    doc.format.format = SourceFormat::Hwpx;
    doc.format.version = "5.1.1.0";
    doc.header.sectionCount = 1;

    PageDef page;
    page.width = 59528;
    page.height = 84186;
    page.marginLeft = 8504;
    page.marginRight = 8504;
    page.marginTop = 5668;
    page.marginBottom = 4252;
    page.marginHeader = 4252;
    page.marginFooter = 4252;
    page.marginGutter = 0;

    for (std::size_t lang = 0; lang < kLangCount; ++lang) {
        Font dotum;
        dotum.id = 0;
        dotum.face = "함초롬돋움";
        dotum.type = "TTF";
        Font batang;
        batang.id = 1;
        batang.face = "함초롬바탕";
        batang.type = "TTF";
        doc.header.fonts[lang].push_back(std::move(dotum));
        doc.header.fonts[lang].push_back(std::move(batang));
    }

    addDefaultStyleHeader(doc);

    Section section;
    section.page = page;
    section.defaultTab = 4000;
    section.columnRegionGap = 1134;
    section.pageBorders = defaultPageBorders();

    Control sec;
    sec.kind = ControlKind::SectionDef;
    sec.ctrlId = "secd";
    sec.page = page;
    sec.defaultTab = 4000;
    sec.columnRegionGap = 1134;
    sec.pageBorders = section.pageBorders;

    Control column;
    column.kind = ControlKind::ColumnDef;
    column.ctrlId = "cold";
    column.columnCount = 1;
    column.columnSameSize = true;
    column.columnType = "NEWSPAPER";
    column.columnLayout = "LEFT";

    Paragraph paragraph;
    Run controls;
    controls.charShapeId = 0;
    controls.items.push_back(InlineItem::makeControl(std::move(sec)));
    controls.items.push_back(InlineItem::makeControl(std::move(column)));
    Run text;
    text.charShapeId = 0;
    paragraph.runs.push_back(std::move(controls));
    paragraph.runs.push_back(std::move(text));

    LineSeg line;
    line.vertSize = 1000;
    line.textHeight = 1000;
    line.baseline = 850;
    line.spacing = 600;
    line.horzSize = 42520;
    line.flags = 393216;
    paragraph.lineSegs.push_back(line);
    section.paragraphs.push_back(std::move(paragraph));
    doc.sections.push_back(std::move(section));
    ensureParagraphIds(doc);
    return doc;
}

int replaceFontFace(Document& doc, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return 0;
    int changed = 0;
    auto rename = [&](std::string& name) {
        if (name != from) return;
        name = to;
        ++changed;
    };
    for (auto& group : doc.header.fonts) {
        for (auto& font : group) {
            rename(font.face);
            rename(font.subst);
        }
    }
    return changed;
}

void rotatedBox(std::uint32_t width, std::uint32_t height, std::int32_t degrees, std::uint32_t& outW, std::uint32_t& outH) {
    const int turn = ((degrees % 360) + 360) % 360;
    if (turn == 0) {
        outW = width;
        outH = height;
        return;
    }
    const double rad = turn * 3.14159265358979323846 / 180.0;
    const double c = std::fabs(std::cos(rad));
    const double s = std::fabs(std::sin(rad));
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    outW = static_cast<std::uint32_t>(w * c + h * s + 1e-6);
    outH = static_cast<std::uint32_t>(w * s + h * c + 1e-6);
}

std::array<double, 6> rotationMatrix(std::uint32_t width, std::uint32_t height, std::int32_t degrees) {
    const int turn = ((degrees % 360) + 360) % 360;
    const double rad = turn * 3.14159265358979323846 / 180.0;
    // 한글은 cos·sin 을 소수 여섯째 자리로 반올림한 값을 쓴다(30도: 0.866025, 0.5 — HWP 의 double 도 그 값이다).
    const double c = std::round(std::cos(rad) * 1e6) / 1e6;
    const double s = std::round(std::sin(rad) * 1e6) / 1e6;
    const double cx = static_cast<double>(width / 2);
    const double cy = static_cast<double>(height / 2);
    return {c, -s, cx - c * cx + s * cy, s, c, cy - s * cx - c * cy};
}

namespace {

double round6(double v) { return std::round(v * 1e6) / 1e6; }

std::array<double, 6> multiply(const std::array<double, 6>& a, const std::array<double, 6>& b) {
    return {a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4], a[0] * b[2] + a[1] * b[5] + a[2],
            a[3] * b[0] + a[4] * b[3], a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5]};
}

}  // namespace

namespace {

ObjectGeometry baseGeometry(std::uint32_t width, std::uint32_t height, std::int32_t rotation, std::int32_t skewX,
                            std::int32_t skewY) {
    ObjectGeometry g;
    const int turn = ((rotation % 360) + 360) % 360;
    const double pi = 3.14159265358979323846;
    const double tx = skewX != 0 ? round6(std::tan(skewX * pi / 180.0)) : 0.0;
    const double ty = skewY != 0 ? round6(std::tan(skewY * pi / 180.0)) : 0.0;
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    g.boxWidth = width;
    g.boxHeight = height;
    g.centerX = static_cast<std::int32_t>(width / 2);
    g.centerY = static_cast<std::int32_t>(height / 2);
    if (turn == 0 && tx == 0 && ty == 0) return g;
    g.shaped = true;
    g.skew = {1, tx != 0 ? -tx : 0.0, 0, ty != 0 ? -ty : 0.0, 1, 0};  // 0 을 "-0" 으로 적지 않게
    // 기울인 정수 상자.
    const double xs[] = {0, w, -tx * h, w - tx * h};
    const double ys[] = {0, -ty * w, h, h - ty * w};
    double minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, xs[i]);
        maxX = std::max(maxX, xs[i]);
        minY = std::min(minY, ys[i]);
        maxY = std::max(maxY, ys[i]);
    }
    // 한글은 반올림한다(가로 40도: 11339 + 7135.7 → 18475, offset 7136).
    const auto skewOffX = static_cast<std::int32_t>(std::llround(-minX));
    const auto skewOffY = static_cast<std::int32_t>(std::llround(-minY));
    const auto skewW = static_cast<std::uint32_t>(std::llround(maxX) + skewOffX);
    const auto skewH = static_cast<std::uint32_t>(std::llround(maxY) + skewOffY);
    if (turn == 0) {
        g.boxWidth = skewW;
        g.boxHeight = skewH;
        g.transX = skewOffX;
        g.transY = skewOffY;
    } else if (tx == 0 && ty == 0) {
        rotatedBox(width, height, turn, g.boxWidth, g.boxHeight);
        g.rotation = rotationMatrix(width, height, turn);
        g.transX = (static_cast<std::int32_t>(g.boxWidth) - static_cast<std::int32_t>(width)) / 2;
        g.transY = (static_cast<std::int32_t>(g.boxHeight) - static_cast<std::int32_t>(height)) / 2;
    } else {
        const double rad = turn * pi / 180.0;
        const double c = round6(std::cos(rad));
        const double s = round6(std::sin(rad));
        const double cx = static_cast<double>(static_cast<std::int32_t>(skewW / 2) - skewOffX);
        const double cy = static_cast<double>(static_cast<std::int32_t>(skewH / 2) - skewOffY);
        g.rotation = {c, -s, cx - c * cx + s * cy, s, c, cy - s * cx - c * cy};
        const std::array<double, 6> rs = multiply(g.rotation, g.skew);
        double lo[2] = {0, 0}, hi[2] = {0, 0};
        const double px[] = {0, w, 0, w};
        const double py[] = {0, 0, h, h};
        for (int i = 0; i < 4; ++i) {
            const double qx = rs[0] * px[i] + rs[1] * py[i] + rs[2];
            const double qy = rs[3] * px[i] + rs[4] * py[i] + rs[5];
            if (i == 0 || qx < lo[0]) lo[0] = qx;
            if (i == 0 || qx > hi[0]) hi[0] = qx;
            if (i == 0 || qy < lo[1]) lo[1] = qy;
            if (i == 0 || qy > hi[1]) hi[1] = qy;
        }
        g.transX = static_cast<std::int32_t>(std::llround(-lo[0]));
        g.transY = static_cast<std::int32_t>(std::llround(-lo[1]));
        g.boxWidth = static_cast<std::uint32_t>(std::max<long long>(1, std::llround(hi[0] + g.transX)));
        g.boxHeight = static_cast<std::uint32_t>(std::max<long long>(1, std::llround(hi[1] + g.transY)));
    }
    g.centerX = static_cast<std::int32_t>(g.boxWidth / 2);
    g.centerY = static_cast<std::int32_t>(g.boxHeight / 2);
    g.local = multiply({1, 0, static_cast<double>(g.transX), 0, 1, static_cast<double>(g.transY)},
                       multiply(g.rotation, g.skew));
    return g;
}

}  // namespace

ObjectGeometry objectGeometry(std::uint32_t width, std::uint32_t height, std::int32_t rotation, std::int32_t skewX,
                              std::int32_t skewY, bool flipH, bool flipV) {
    // 숨긴 한글 2024 ShapeObjHorzFlip·VertFlip: 30도 돌린 11339×8504 를 좌우로 뒤집으면 각 330, 상자·offset 그대로,
    // 회전 행렬 = 대칭(e3 11338)·30도 행렬. 둘 다 뒤집으면 각 30 그대로. 기울인 개체(가로 20도)는 대칭 축이
    // 5669 - 0.36397×4252 → 4121(e3 8242), 세로 15도는 4252 - 0.267949×5669 → 2733(e6 5466).
    const int turn = ((rotation % 360) + 360) % 360;
    const bool odd = flipH != flipV;
    ObjectGeometry g = baseGeometry(width, height, odd ? (360 - turn) % 360 : turn, skewX, skewY);
    if (!flipH && !flipV) return g;
    const double cx = static_cast<double>(width / 2);
    const double cy = static_cast<double>(height / 2);
    const double ax = static_cast<double>(2 * std::llround(cx + g.skew[1] * cy));
    const double ay = static_cast<double>(2 * std::llround(g.skew[3] * cx + cy));
    const std::array<double, 6> mirror{flipH ? -1.0 : 1.0, 0, flipH ? ax : 0.0, 0, flipV ? -1.0 : 1.0, flipV ? ay : 0.0};
    g.rotation = multiply(mirror, g.rotation);
    if (g.shaped) {
        // 모델의 점은 상자 안에서 뒤집혀 있다. 파일의 점으로 되돌린 뒤 한글 차례(이동·회전·기울이기)로 옮긴다.
        const std::array<double, 6> unflip{flipH ? -1.0 : 1.0, 0, flipH ? static_cast<double>(width) : 0.0,
                                           0, flipV ? -1.0 : 1.0, flipV ? static_cast<double>(height) : 0.0};
        g.local = multiply({1, 0, static_cast<double>(g.transX), 0, 1, static_cast<double>(g.transY)},
                           multiply(g.rotation, multiply(g.skew, unflip)));
    }
    return g;
}

}  // namespace chwp::model
