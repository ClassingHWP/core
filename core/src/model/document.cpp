#include "chwp/model/document.hpp"

#include <algorithm>
#include <atomic>

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

const char* controlKindName(ControlKind kind) {
    switch (kind) {
        case ControlKind::Unknown: return "unknown";
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

    BorderFill emptyFill;
    emptyFill.id = 1;
    emptyFill.left.type = emptyFill.right.type = emptyFill.top.type = emptyFill.bottom.type = "NONE";
    doc.header.borderFills.push_back(emptyFill);

    CharShape shape;
    shape.id = 0;
    shape.height = 1000;
    shape.fontRef.fill(1);
    shape.textColor = 0x000000u;
    doc.header.charShapes.push_back(shape);

    ParaShape para;
    para.id = 0;
    para.align = "JUSTIFY";
    para.lineSpacingType = "PERCENT";
    para.lineSpacing = 160;
    para.snapToGrid = true;
    doc.header.paraShapes.push_back(para);

    Style style;
    style.id = 0;
    style.name = "바탕글";
    doc.header.styles.push_back(style);

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

}  // namespace chwp::model
