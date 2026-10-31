#include "noting/export/json.hpp"

#include <cstdio>

#include "noting/reader.hpp"

namespace noting {

void JsonWriter::newline() {
    if (!pretty_) return;
    out_ += '\n';
    out_.append(hasItems_.size() * 2, ' ');
}

void JsonWriter::beforeValue() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (!hasItems_.empty()) {
        if (hasItems_.back()) out_ += ',';
        hasItems_.back() = true;
        newline();
    }
}

void JsonWriter::beginObject() {
    beforeValue();
    out_ += '{';
    hasItems_.push_back(false);
}

void JsonWriter::endObject() {
    const bool had = hasItems_.back();
    hasItems_.pop_back();
    if (had) newline();
    out_ += '}';
}

void JsonWriter::beginArray() {
    beforeValue();
    out_ += '[';
    hasItems_.push_back(false);
}

void JsonWriter::endArray() {
    const bool had = hasItems_.back();
    hasItems_.pop_back();
    if (had) newline();
    out_ += ']';
}

void JsonWriter::key(std::string_view name) {
    beforeValue();
    writeString(name);
    out_ += pretty_ ? ": " : ":";
    afterKey_ = true;
}

void JsonWriter::value(std::string_view s) {
    beforeValue();
    writeString(s);
}

void JsonWriter::writeString(std::string_view s) {
    out_ += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            case '\b': out_ += "\\b"; break;
            case '\f': out_ += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out_ += buf;
                } else {
                    out_ += ch;
                }
        }
    }
    out_ += '"';
}

void JsonWriter::value(bool b) {
    beforeValue();
    out_ += b ? "true" : "false";
}

void JsonWriter::null() {
    beforeValue();
    out_ += "null";
}

void JsonWriter::writeSigned(std::int64_t v) {
    beforeValue();
    out_ += std::to_string(v);
}

void JsonWriter::writeUnsigned(std::uint64_t v) {
    beforeValue();
    out_ += std::to_string(v);
}

namespace {

using namespace noting::model;

void writeColor(JsonWriter& w, std::string_view name, const Color& color) {
    w.key(name);
    if (!color) {
        w.null();
        return;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", *color & 0xFFFFFF);
    w.value(buf);
}

template <typename T>
void writePerLang(JsonWriter& w, std::string_view name, const PerLang<T>& values) {
    w.key(name);
    w.beginArray();
    for (const auto& v : values) w.value(v);
    w.endArray();
}

void writePage(JsonWriter& w, const PageDef& p) {
    w.beginObject();
    w.field("width", p.width);
    w.field("height", p.height);
    w.field("landscape", p.landscape);
    w.field("marginLeft", p.marginLeft);
    w.field("marginRight", p.marginRight);
    w.field("marginTop", p.marginTop);
    w.field("marginBottom", p.marginBottom);
    w.field("marginHeader", p.marginHeader);
    w.field("marginFooter", p.marginFooter);
    w.field("marginGutter", p.marginGutter);
    w.endObject();
}

void writeParagraphs(JsonWriter& w, const std::vector<Paragraph>& paragraphs, const JsonOptions& o);

void writeControl(JsonWriter& w, const Control& c, const JsonOptions& o) {
    w.beginObject();
    w.field("kind", controlKindName(c.kind));
    w.field("ctrlId", c.ctrlId);
    if (!c.shapeType.empty()) w.field("shapeType", c.shapeType);
    if (c.placement) {
        const Placement& p = *c.placement;
        w.key("placement");
        w.beginObject();
        w.field("treatAsChar", p.treatAsChar);
        w.field("textWrap", p.textWrap);
        w.field("textFlow", p.textFlow);
        w.field("vertRelTo", p.vertRelTo);
        w.field("horzRelTo", p.horzRelTo);
        w.field("vertAlign", p.vertAlign);
        w.field("horzAlign", p.horzAlign);
        w.field("vertOffset", p.vertOffset);
        w.field("horzOffset", p.horzOffset);
        w.field("width", p.width);
        w.field("height", p.height);
        w.field("zOrder", p.zOrder);
        w.endObject();
        if (!c.strokeType.empty() || c.strokeWidth != 0) {
            w.key("stroke");
            w.beginObject();
            w.field("type", c.strokeType);
            w.field("color", c.strokeColor);
            w.field("width", c.strokeWidth);
            w.endObject();
        }
        if (!c.fillColor.empty()) w.field("fillColor", c.fillColor);
        if (!c.gradColor.empty()) {
            w.field("gradColor", c.gradColor);
            w.field("gradAngle", c.gradAngle);
        }
        if (c.boxPadLeft || c.boxPadRight || c.boxPadTop || c.boxPadBottom) {
            w.field("boxPadLeft", c.boxPadLeft);
            w.field("boxPadRight", c.boxPadRight);
            w.field("boxPadTop", c.boxPadTop);
            w.field("boxPadBottom", c.boxPadBottom);
        }
    }
    if (c.page) {
        w.key("page");
        writePage(w, *c.page);
    }
    if (c.kind == ControlKind::ColumnDef) {
        w.field("columnCount", c.columnCount);
        w.field("columnGap", c.columnGap);
        w.field("columnSameSize", c.columnSameSize);
        if (!c.columnType.empty()) w.field("columnType", c.columnType);
        if (!c.columnLayout.empty()) w.field("columnLayout", c.columnLayout);
        if (!c.columnWidths.empty()) {
            w.key("columnWidths");
            w.beginArray();
            for (const auto& slice : c.columnWidths) {
                w.beginObject();
                w.field("width", slice.width);
                w.field("gap", slice.gap);
                w.endObject();
            }
            w.endArray();
        }
        if (!c.columnLineType.empty()) {
            w.field("columnLineType", c.columnLineType);
            w.field("columnLineWidth", c.columnLineWidth);
            if (!c.columnLineColor.empty()) w.field("columnLineColor", c.columnLineColor);
        }
    }
    if (c.table) {
        const Table& t = *c.table;
        w.key("table");
        w.beginObject();
        w.field("rowCount", t.rowCount);
        w.field("colCount", t.colCount);
        w.field("cellSpacing", t.cellSpacing);
        w.field("borderFillId", t.borderFillId);
        w.field("repeatHeader", t.repeatHeader);
        w.field("marginLeft", t.marginLeft);
        w.field("marginRight", t.marginRight);
        w.field("marginTop", t.marginTop);
        w.field("marginBottom", t.marginBottom);
        w.key("cells");
        w.beginArray();
        for (const auto& cell : t.cells) {
            w.beginObject();
            w.field("row", cell.row);
            w.field("col", cell.col);
            w.field("rowSpan", cell.rowSpan);
            w.field("colSpan", cell.colSpan);
            w.field("width", cell.width);
            w.field("height", cell.height);
            w.field("borderFillId", cell.borderFillId);
            w.field("hasMargin", cell.hasMargin);
            w.field("marginLeft", cell.marginLeft);
            w.field("marginRight", cell.marginRight);
            w.field("marginTop", cell.marginTop);
            w.field("marginBottom", cell.marginBottom);
            if (!cell.vertAlign.empty()) w.field("vertAlign", cell.vertAlign);
            w.key("paragraphs");
            writeParagraphs(w, cell.content.paragraphs, o);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    if (!c.binItemId.empty()) w.field("binItemId", c.binItemId);
    if (c.picture.clipRight > c.picture.clipLeft && c.picture.clipBottom > c.picture.clipTop) {
        w.field("clipLeft", c.picture.clipLeft);
        w.field("clipTop", c.picture.clipTop);
        w.field("clipRight", c.picture.clipRight);
        w.field("clipBottom", c.picture.clipBottom);
    }
    if (c.picture.originWidth > 0) w.field("originWidth", c.picture.originWidth);
    if (c.picture.originHeight > 0) w.field("originHeight", c.picture.originHeight);
    if (c.picture.effect != 0) w.field("effect", c.picture.effect);
    if (c.picture.bright != 0) w.field("bright", c.picture.bright);
    if (c.picture.contrast != 0) w.field("contrast", c.picture.contrast);
    if (c.picture.transparency != 0) w.field("transparency", c.picture.transparency);
    if (c.kind == ControlKind::Equation) w.field("script", c.script);
    if (!c.applyPageType.empty()) w.field("applyPageType", c.applyPageType);
    if (!c.numType.empty()) {
        w.field("numType", c.numType);
        w.field("number", c.number);
    }
    if (!c.name.empty()) w.field("name", c.name);
    if (!c.command.empty()) w.field("command", c.command);
    if (c.kind == ControlKind::Dutmal || c.kind == ControlKind::Compose) w.field("text", c.text);
    if (c.kind == ControlKind::Dutmal) w.field("subText", c.subText);
    if (c.caption) {
        const Caption& cap = *c.caption;
        w.key("caption");
        w.beginObject();
        w.field("side", cap.side);
        w.field("fullSize", cap.fullSize);
        w.field("width", cap.width);
        w.field("gap", cap.gap);
        w.field("lastWidth", cap.lastWidth);
        w.key("paragraphs");
        writeParagraphs(w, cap.content.paragraphs, o);
        w.endObject();
    }
    if (!c.subLists.empty()) {
        w.key("subLists");
        w.beginArray();
        for (const auto& sub : c.subLists) writeParagraphs(w, sub.paragraphs, o);
        w.endArray();
    }
    if (!c.children.empty()) {
        w.key("children");
        w.beginArray();
        for (const auto& child : c.children) writeControl(w, child, o);
        w.endArray();
    }
    w.endObject();
}

void writeParagraphs(JsonWriter& w, const std::vector<Paragraph>& paragraphs, const JsonOptions& o) {
    w.beginArray();
    for (const auto& para : paragraphs) {
        w.beginObject();
        w.field("paraShapeId", para.paraShapeId);
        w.field("styleId", para.styleId);
        if (para.pageBreak) w.field("pageBreak", true);
        if (para.columnBreak) w.field("columnBreak", true);
        w.key("runs");
        w.beginArray();
        for (const auto& run : para.runs) {
            w.beginObject();
            w.field("charShapeId", run.charShapeId);
            w.key("items");
            w.beginArray();
            for (const auto& item : run.items) {
                w.beginObject();
                if (item.type == InlineItem::Type::Text) {
                    w.field("text", item.text);
                } else {
                    w.key("control");
                    writeControl(w, item.control, o);
                }
                w.endObject();
            }
            w.endArray();
            w.endObject();
        }
        w.endArray();
        if (o.includeLineSegs && !para.lineSegs.empty()) {
            w.key("lineSegs");
            w.beginArray();
            for (const auto& s : para.lineSegs) {
                w.beginArray();
                w.value(s.textPos);
                w.value(s.vertPos);
                w.value(s.vertSize);
                w.value(s.textHeight);
                w.value(s.baseline);
                w.value(s.spacing);
                w.value(s.horzPos);
                w.value(s.horzSize);
                w.value(s.flags);
                w.endArray();
            }
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();
}

void writeDocument(JsonWriter& w, const Document& doc, const JsonOptions& o) {
    w.beginObject();

    w.key("format");
    w.beginObject();
    w.field("type", formatName(doc.format.format));
    w.field("version", doc.format.version);
    w.field("application", doc.format.application);
    w.field("compressed", doc.format.compressed);
    w.field("encrypted", doc.format.encrypted);
    w.field("distribution", doc.format.distribution);
    w.endObject();

    const Metadata& m = doc.metadata;
    w.key("metadata");
    w.beginObject();
    w.field("title", m.title);
    w.field("subject", m.subject);
    w.field("author", m.author);
    w.field("keywords", m.keywords);
    w.field("comments", m.comments);
    w.field("lastSavedBy", m.lastSavedBy);
    w.field("createdAt", m.createdAt);
    w.field("modifiedAt", m.modifiedAt);
    w.field("language", m.language);
    w.endObject();

    const DocHeader& h = doc.header;
    w.key("header");
    w.beginObject();
    w.field("sectionCount", h.sectionCount);
    w.key("beginNumbers");
    w.beginObject();
    w.field("page", h.beginNumbers.page);
    w.field("footnote", h.beginNumbers.footnote);
    w.field("endnote", h.beginNumbers.endnote);
    w.field("picture", h.beginNumbers.picture);
    w.field("table", h.beginNumbers.table);
    w.field("equation", h.beginNumbers.equation);
    w.endObject();

    w.key("fonts");
    w.beginObject();
    for (std::size_t i = 0; i < kLangCount; ++i) {
        w.key(langName(static_cast<Lang>(i)));
        w.beginArray();
        for (const auto& f : h.fonts[i]) {
            w.beginObject();
            w.field("id", f.id);
            w.field("face", f.face);
            if (!f.type.empty()) w.field("type", f.type);
            if (!f.subst.empty()) w.field("subst", f.subst);
            if (!f.base.empty()) w.field("base", f.base);
            w.endObject();
        }
        w.endArray();
    }
    w.endObject();
    w.field("borderFillCount", h.borderFillCount);
    if (!h.borderFills.empty()) {
        w.key("borderFills");
        w.beginArray();
        for (const auto& fill : h.borderFills) {
            auto side = [&](const char* name, const BorderSide& edge) {
                w.key(name);
                w.beginObject();
                w.field("type", edge.type);
                w.field("color", edge.color);
                w.field("width", edge.width);
                w.endObject();
            };
            w.beginObject();
            w.field("id", fill.id);
            side("left", fill.left);
            side("right", fill.right);
            side("top", fill.top);
            side("bottom", fill.bottom);
            if (!fill.faceColor.empty()) w.field("faceColor", fill.faceColor);
            if (!fill.gradColor.empty()) {
                w.field("gradColor", fill.gradColor);
                w.field("gradAngle", fill.gradAngle);
            }
            w.endObject();
        }
        w.endArray();
    }

    w.key("charShapes");
    w.beginArray();
    for (const auto& cs : h.charShapes) {
        w.beginObject();
        w.field("id", cs.id);
        w.field("height", cs.height);
        writePerLang(w, "fontRef", cs.fontRef);
        writePerLang(w, "ratio", cs.ratio);
        writePerLang(w, "spacing", cs.spacing);
        writePerLang(w, "relSize", cs.relSize);
        writePerLang(w, "offset", cs.offset);
        writeColor(w, "textColor", cs.textColor);
        writeColor(w, "shadeColor", cs.shadeColor);
        w.field("bold", cs.bold);
        w.field("italic", cs.italic);
        w.field("underline", cs.underline);
        w.field("strikeout", cs.strikeout);
        w.field("superscript", cs.superscript);
        w.field("subscript", cs.subscript);
        w.field("useFontSpace", cs.useFontSpace);
        w.field("borderFillId", cs.borderFillId);
        w.endObject();
    }
    w.endArray();

    w.key("paraShapes");
    w.beginArray();
    for (const auto& ps : h.paraShapes) {
        w.beginObject();
        w.field("id", ps.id);
        w.field("align", ps.align);
        w.field("vertAlign", ps.vertAlign);
        w.field("indent", ps.indent);
        w.field("marginLeft", ps.marginLeft);
        w.field("marginRight", ps.marginRight);
        w.field("spacingPrev", ps.spacingPrev);
        w.field("spacingNext", ps.spacingNext);
        w.field("lineSpacingType", ps.lineSpacingType);
        w.field("lineSpacing", ps.lineSpacing);
        w.field("breakLatinWord", ps.breakLatinWord);
        w.field("breakNonLatinWord", ps.breakNonLatinWord);
        w.field("condense", ps.condense);
        w.field("widowOrphan", ps.widowOrphan);
        w.field("keepWithNext", ps.keepWithNext);
        w.field("keepLines", ps.keepLines);
        w.field("pageBreakBefore", ps.pageBreakBefore);
        w.field("headingType", ps.headingType);
        w.field("headingIdRef", ps.headingIdRef);
        w.field("headingLevel", ps.headingLevel);
        w.field("tabDefId", ps.tabDefId);
        w.field("borderFillId", ps.borderFillId);
        w.field("snapToGrid", ps.snapToGrid);
        w.field("fontLineHeight", ps.fontLineHeight);
        w.field("autoSpacingEng", ps.autoSpacingEng);
        w.field("autoSpacingNum", ps.autoSpacingNum);
        w.field("lineWrap", ps.lineWrap);
        w.endObject();
    }
    w.endArray();

    w.key("styles");
    w.beginArray();
    for (const auto& st : h.styles) {
        w.beginObject();
        w.field("id", st.id);
        w.field("type", st.type);
        w.field("name", st.name);
        w.field("engName", st.engName);
        w.field("paraShapeId", st.paraShapeId);
        w.field("charShapeId", st.charShapeId);
        w.field("nextStyleId", st.nextStyleId);
        w.field("langId", st.langId);
        w.endObject();
    }
    w.endArray();

    w.key("numberings");
    w.beginArray();
    for (const auto& numbering : h.numberings) {
        w.beginObject();
        w.field("id", numbering.id);
        w.field("start", numbering.start);
        w.key("levels");
        w.beginArray();
        for (const auto& head : numbering.levels) {
            w.beginObject();
            w.field("level", head.level);
            w.field("start", head.start);
            w.field("numFormat", head.numFormat);
            w.field("format", head.format);
            w.field("align", head.align);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();

    w.key("bullets");
    w.beginArray();
    for (const auto& bullet : h.bullets) {
        w.beginObject();
        w.field("id", bullet.id);
        w.field("text", bullet.text);
        if (!bullet.checkedText.empty()) w.field("checkedChar", bullet.checkedText);
        if (!bullet.imageId.empty()) w.field("image", bullet.imageId);
        w.endObject();
    }
    w.endArray();

    w.key("binItems");
    w.beginArray();
    for (const auto& b : h.binItems) {
        w.beginObject();
        w.field("id", b.id);
        w.field("storage", b.storage);
        w.field("path", b.path);
        w.field("extension", b.extension);
        w.field("mediaType", b.mediaType);
        w.field("size", b.size);
        w.endObject();
    }
    w.endArray();
    w.endObject();  // header

    if (o.includeBody) {
        w.key("sections");
        w.beginArray();
        for (const auto& section : doc.sections) {
            w.beginObject();
            w.key("page");
            if (section.page) writePage(w, *section.page);
            else w.null();
            w.key("paragraphs");
            writeParagraphs(w, section.paragraphs, o);
            w.endObject();
        }
        w.endArray();
    }

    w.field("previewText", doc.previewText);
    w.endObject();
}

}  // namespace

std::string documentToJson(const Document& doc, const JsonOptions& options) {
    JsonWriter w(options.pretty);
    writeDocument(w, doc, options);
    return w.take();
}

std::string parseResultToJson(const ParseResult& result, const JsonOptions& options) {
    JsonWriter w(options.pretty);
    w.beginObject();
    w.field("ok", result.ok);
    w.field("error", result.error);
    w.key("warnings");
    w.beginArray();
    for (const auto& warning : result.warnings) w.value(warning);
    w.endArray();
    w.key("document");
    writeDocument(w, result.document, options);
    w.endObject();
    return w.take();
}

}  // namespace noting
