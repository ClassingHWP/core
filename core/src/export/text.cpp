#include "noting/export/text.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "export/layout.hpp"

namespace noting {

namespace {

using namespace noting::model;

void ensureNewline(std::string& out) {
    if (!out.empty() && out.back() != '\n') out += '\n';
}

std::string trimNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void writeParagraph(const Paragraph& para, const TextOptions& o, std::string& out);

std::string flatten(const std::vector<Paragraph>& paragraphs, const TextOptions& o,
                    const char* separator) {
    std::string out;
    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        std::string para;
        writeParagraph(paragraphs[i], o, para);
        if (i > 0) out += separator;
        out += trimNewlines(para);
    }
    return out;
}

void writeCaption(const Caption& caption, const TextOptions& o, std::string& out) {
    const std::string text = flatten(caption.content.paragraphs, o, "\n");
    if (text.empty()) return;
    ensureNewline(out);
    out += text;
    out += '\n';
}

void writeControlBody(const Control& c, const TextOptions& o, std::string& out);

void writeControl(const Control& c, const TextOptions& o, std::string& out) {
    const bool captionFirst =
        c.caption && (c.caption->side == "TOP" || c.caption->side == "LEFT");
    if (c.caption && o.includeCaptions && captionFirst) writeCaption(*c.caption, o, out);
    writeControlBody(c, o, out);
    if (c.caption && o.includeCaptions && !captionFirst) writeCaption(*c.caption, o, out);
}

void writeControlBody(const Control& c, const TextOptions& o, std::string& out) {
    switch (c.kind) {
        case ControlKind::Dutmal:
        case ControlKind::Compose:
            out += c.text;
            return;
        case ControlKind::Table: {
            if (!c.table) return;
            std::map<std::uint32_t, std::vector<const TableCell*>> rows;
            for (const auto& cell : c.table->cells) rows[cell.row].push_back(&cell);
            ensureNewline(out);
            for (auto& [row, cells] : rows) {
                std::sort(cells.begin(), cells.end(),
                          [](const TableCell* a, const TableCell* b) { return a->col < b->col; });
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    if (i > 0) out += '\t';
                    out += flatten(cells[i]->content.paragraphs, o, " ");
                }
                out += '\n';
            }
            return;
        }
        case ControlKind::Equation:
            if (o.includeEquations) out += trimNewlines(c.script);
            return;
        case ControlKind::Header:
        case ControlKind::Footer:
            if (!o.includeHeaderFooter) return;
            break;
        case ControlKind::Footnote:
        case ControlKind::Endnote:
            if (o.includeNotes) {
                for (const auto& sub : c.subLists) {
                    out += "[^" + flatten(sub.paragraphs, o, " ") + "]";
                }
            }
            return;
        case ControlKind::Picture:
        case ControlKind::Shape:
        case ControlKind::Container:
            if (!o.includeShapeText) return;
            break;
        default:
            break;
    }
    for (const auto& sub : c.subLists) {
        const std::string text = flatten(sub.paragraphs, o, "\n");
        if (!text.empty()) {
            ensureNewline(out);
            out += text;
            out += '\n';
        }
    }
    for (const auto& child : c.children) writeControl(child, o, out);
}

void writeParagraph(const Paragraph& para, const TextOptions& o, std::string& out) {
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) out += item.text;
            else writeControl(item.control, o, out);
        }
    }
}

}  // namespace

std::string paragraphsToText(const std::vector<Paragraph>& paragraphs, const TextOptions& options) {
    std::string out;
    for (const auto& para : paragraphs) {
        std::string line;
        writeParagraph(para, options, line);
        out += line;
        if (line.empty() || line.back() != '\n') out += '\n';
    }
    return out;
}

std::string documentToText(const Document& doc, const TextOptions& options) {
    std::string out;
    for (const auto& section : doc.sections) {
        out += paragraphsToText(section.paragraphs, options);
    }
    return out;
}

std::vector<std::string> documentToPages(const Document& doc, const TextOptions& options) {
    return layout::paginate(doc, options);
}

}  // namespace noting
