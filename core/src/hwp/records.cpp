#include "chwp/hwp/records.hpp"

#include "chwp/container/zip.hpp"

namespace chwp::hwp {

const char* tagName(std::uint16_t t) {
    switch (t) {
        case tag::DocumentProperties: return "DOCUMENT_PROPERTIES";
        case tag::IdMappings: return "ID_MAPPINGS";
        case tag::BinData: return "BIN_DATA";
        case tag::FaceName: return "FACE_NAME";
        case tag::BorderFill: return "BORDER_FILL";
        case tag::CharShape: return "CHAR_SHAPE";
        case tag::TabDef: return "TAB_DEF";
        case tag::Numbering: return "NUMBERING";
        case tag::Bullet: return "BULLET";
        case tag::ParaShape: return "PARA_SHAPE";
        case tag::Style: return "STYLE";
        case tag::DocData: return "DOC_DATA";
        case tag::DistributeDocData: return "DISTRIBUTE_DOC_DATA";
        case tag::CompatibleDocument: return "COMPATIBLE_DOCUMENT";
        case tag::LayoutCompatibility: return "LAYOUT_COMPATIBILITY";
        case tag::TrackChange: return "TRACKCHANGE";
        case tag::MemoShape: return "MEMO_SHAPE";
        case tag::ForbiddenChar: return "FORBIDDEN_CHAR";
        case tag::TrackChangeContent: return "TRACK_CHANGE";
        case tag::TrackChangeAuthor: return "TRACK_CHANGE_AUTHOR";
        case tag::ParaHeader: return "PARA_HEADER";
        case tag::ParaText: return "PARA_TEXT";
        case tag::ParaCharShape: return "PARA_CHAR_SHAPE";
        case tag::ParaLineSeg: return "PARA_LINE_SEG";
        case tag::ParaRangeTag: return "PARA_RANGE_TAG";
        case tag::CtrlHeader: return "CTRL_HEADER";
        case tag::ListHeader: return "LIST_HEADER";
        case tag::PageDef: return "PAGE_DEF";
        case tag::FootnoteShape: return "FOOTNOTE_SHAPE";
        case tag::PageBorderFill: return "PAGE_BORDER_FILL";
        case tag::ShapeComponent: return "SHAPE_COMPONENT";
        case tag::Table: return "TABLE";
        case tag::ShapeComponentLine: return "SHAPE_COMPONENT_LINE";
        case tag::ShapeComponentRectangle: return "SHAPE_COMPONENT_RECTANGLE";
        case tag::ShapeComponentEllipse: return "SHAPE_COMPONENT_ELLIPSE";
        case tag::ShapeComponentArc: return "SHAPE_COMPONENT_ARC";
        case tag::ShapeComponentPolygon: return "SHAPE_COMPONENT_POLYGON";
        case tag::ShapeComponentCurve: return "SHAPE_COMPONENT_CURVE";
        case tag::ShapeComponentOle: return "SHAPE_COMPONENT_OLE";
        case tag::ShapeComponentPicture: return "SHAPE_COMPONENT_PICTURE";
        case tag::ShapeComponentContainer: return "SHAPE_COMPONENT_CONTAINER";
        case tag::CtrlData: return "CTRL_DATA";
        case tag::EqEdit: return "EQEDIT";
        case tag::ShapeComponentTextArt: return "SHAPE_COMPONENT_TEXTART";
        case tag::FormObject: return "FORM_OBJECT";
        case tag::MemoList: return "MEMO_LIST";
        case tag::ChartData: return "CHART_DATA";
        case tag::VideoData: return "VIDEO_DATA";
        case tag::ShapeComponentUnknown: return "SHAPE_COMPONENT_UNKNOWN";
        default: return "UNKNOWN";
    }
}

bool parseRecords(ByteView stream, std::vector<Record>& out, std::string* error) {
    out.clear();
    ByteReader r(stream);
    while (!r.atEnd()) {
        const std::size_t offset = r.position();
        const std::uint32_t header = r.u32();
        std::uint32_t size = header >> 20;
        if (size == 0xFFF) size = r.u32();
        if (!r.ok() || size > r.remaining()) {
            if (error) *error = "truncated record at offset " + std::to_string(offset);
            return false;
        }
        Record rec;
        rec.tag = static_cast<std::uint16_t>(header & 0x3FF);
        rec.level = static_cast<std::uint16_t>((header >> 10) & 0x3FF);
        rec.offset = offset;
        rec.data = r.bytes(size);
        out.push_back(rec);
    }
    return true;
}

RecordTree RecordTree::build(const std::vector<Record>& records) {
    RecordTree tree;
    tree.nodes.reserve(records.size() + 1);
    tree.nodes.emplace_back();

    // (노드 인덱스, 레벨). 가상 루트는 레벨 -1
    std::vector<std::pair<std::size_t, int>> stack{{0, -1}};
    for (const auto& rec : records) {
        const int level = rec.level;
        while (stack.size() > 1 && stack.back().second >= level) stack.pop_back();
        const std::size_t index = tree.nodes.size();
        Node node;
        node.record = &rec;
        tree.nodes.push_back(std::move(node));
        tree.nodes[stack.back().first].children.push_back(index);
        stack.emplace_back(index, level);
    }
    return tree;
}

std::string ctrlIdToString(std::uint32_t id) {
    std::string s(4, ' ');
    s[0] = static_cast<char>((id >> 24) & 0xFF);
    s[1] = static_cast<char>((id >> 16) & 0xFF);
    s[2] = static_cast<char>((id >> 8) & 0xFF);
    s[3] = static_cast<char>(id & 0xFF);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
    }
    return s;
}

bool decompressStream(ByteView input, Bytes& out) {
    return zip::inflateRaw(input, out);
}

}  // namespace chwp::hwp
