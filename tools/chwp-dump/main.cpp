// chwp-dump: HWP/HWPX 파일 구조를 확인하는 네이티브 CLI

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/container/cfb.hpp"
#include "chwp/container/zip.hpp"
#include "chwp/export/json.hpp"
#include "chwp/export/text.hpp"
#include "chwp/hwp/records.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"
#include "chwp/writer.hpp"
#include "render/equation_layout.hpp"
#include "chwp/render/paint.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace chwp;
using namespace chwp::model;

namespace {

constexpr const char* kUsage = R"(사용법: chwp-dump <명령> <파일> [옵션]

명령:
  info      형식, 버전, 메타데이터, 통계
  text      본문 텍스트 추출            [--header-footer] [--no-notes]
  styles    글꼴, 글자 모양, 문단 모양, 스타일
  tree      문서 구조 트리              [--max-text N]
  eq        수식 파일 크기와 측정값 JSON 한 줄씩
  json      통합 문서 모델 JSON         [--pretty] [--line-segs] [--no-body]
  raw       컨테이너 원시 구조 (HWP: CFB 스트림/레코드, HWPX: ZIP 엔트리)
  extract   BinData(그림 등) 추출       [--out-dir DIR]
  png       쪽을 PNG로 저장             [--out-dir DIR] [--height N]
  export    HWP 또는 HWPX로 내보내기    -o <출력파일.hwp|.hwpx>

공통 옵션:
  -o, --output FILE   결과를 파일로 저장 (UTF-8)
)";

struct Args {
    std::string command;
    fs::path input;
    fs::path output;
    fs::path outDir;
    bool pretty = false;
    bool lineSegs = false;
    bool noBody = false;
    bool headerFooter = false;
    bool noNotes = false;
    std::size_t maxText = 60;
    int height = 1403;
};

fs::path pathFromUtf8(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

bool parseArgs(int argc, char** argv, Args& args) {
    if (argc < 3) return false;
    args.command = argv[1];
    args.input = pathFromUtf8(argv[2]);
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "-o" || a == "--output") {
            const char* v = next();
            if (!v) return false;
            args.output = pathFromUtf8(v);
        } else if (a == "--out-dir") {
            const char* v = next();
            if (!v) return false;
            args.outDir = pathFromUtf8(v);
        } else if (a == "--max-text") {
            const char* v = next();
            if (!v) return false;
            args.maxText = static_cast<std::size_t>(std::stoul(v));
        } else if (a == "--pretty") {
            args.pretty = true;
        } else if (a == "--line-segs") {
            args.lineSegs = true;
        } else if (a == "--no-body") {
            args.noBody = true;
        } else if (a == "--header-footer") {
            args.headerFooter = true;
        } else if (a == "--no-notes") {
            args.noNotes = true;
        } else if (a == "--height") {
            const char* v = next();
            if (!v) return false;
            args.height = std::stoi(v);
        } else {
            std::cerr << "알 수 없는 옵션: " << a << "\n";
            return false;
        }
    }
    return true;
}

bool readFile(const fs::path& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool writeFile(const fs::path& path, ByteView data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string mm(std::uint32_t hwpunit) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", huToMm(static_cast<std::int32_t>(hwpunit), 1));
    return buf;
}

std::string pt(std::int32_t height) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fpt", huToPt(height, 1));
    return buf;
}

std::string color(const Color& c) {
    if (!c) return "none";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", *c & 0xFFFFFF);
    return buf;
}

// UTF-8 코드 포인트 단위로 자르고 제어 문자를 보이게 바꾼다.
std::string preview(const std::string& s, std::size_t maxChars) {
    std::string out;
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            if (count == maxChars) {
                out += "…";
                break;
            }
            ++count;
        }
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += s[i];
    }
    return out;
}

// ---------------------------------------------------------------------------

void cmdInfo(const Args& args, const ParseResult& r, std::ostream& os) {
    const Document& d = r.document;
    os << "파일        : " << pathToUtf8(args.input) << "\n";
    os << "형식        : " << formatName(d.format.format) << " " << d.format.version;
    if (d.format.compressed) os << " (압축)";
    if (d.format.distribution) os << " (배포용)";
    if (d.format.encrypted) os << " (암호)";
    os << "\n";
    if (!d.format.application.empty()) os << "작성 프로그램: " << d.format.application << "\n";

    const Metadata& m = d.metadata;
    os << "\n[메타데이터]\n";
    os << "  제목      : " << m.title << "\n";
    os << "  작성자    : " << m.author << "\n";
    os << "  마지막 저장: " << m.lastSavedBy << "\n";
    os << "  만든 날짜 : " << m.createdAt << "\n";
    os << "  고친 날짜 : " << m.modifiedAt << "\n";

    const DocHeader& h = d.header;
    os << "\n[문서 정보]\n";
    os << "  구역 수   : " << h.sectionCount << "\n";
    os << "  글꼴 수   :";
    for (std::size_t i = 0; i < kLangCount; ++i) {
        os << " " << langName(static_cast<Lang>(i)) << "=" << h.fonts[i].size();
    }
    os << "\n";
    os << "  테두리/배경: " << h.borderFillCount << "\n";
    os << "  글자 모양 : " << h.charShapes.size() << "\n";
    os << "  문단 모양 : " << h.paraShapes.size() << "\n";
    os << "  스타일    : " << h.styles.size() << "\n";
    os << "  바이너리  : " << h.binItems.size() << "\n";

    for (std::size_t i = 0; i < d.sections.size(); ++i) {
        if (const auto& p = d.sections[i].page) {
            os << "  구역 " << i << " 용지: " << mm(p->width) << " x " << mm(p->height)
               << " mm, 여백(좌/우/위/아래) " << mm(p->marginLeft) << "/" << mm(p->marginRight)
               << "/" << mm(p->marginTop) << "/" << mm(p->marginBottom) << " mm\n";
        }
    }

    const DocumentStats s = computeStats(d);
    os << "\n[본문 통계] (표 셀/글상자 등 중첩 포함)\n";
    os << "  문단      : " << s.paragraphs << "\n";
    os << "  글자      : " << s.characters << "\n";
    os << "  표        : " << s.tables << " (셀 " << s.tableCells << ")\n";
    os << "  그림      : " << s.pictures << "\n";
    os << "  도형      : " << s.shapes << "\n";
    os << "  수식      : " << s.equations << "\n";
    os << "  머리말/꼬리말: " << s.headersFooters << "\n";
    os << "  각주/미주 : " << s.notes << "\n";
    os << "  기타 컨트롤: " << s.otherControls << "\n";
}

void cmdStyles(const ParseResult& r, std::ostream& os) {
    const DocHeader& h = r.document.header;
    os << "[글꼴]\n";
    for (std::size_t i = 0; i < kLangCount; ++i) {
        os << "  " << langName(static_cast<Lang>(i)) << ":";
        for (const auto& f : h.fonts[i]) {
            os << " [" << f.id << "]" << f.face;
            if (!f.subst.empty()) os << "(대체:" << f.subst << ")";
            if (!f.base.empty()) os << "(기본:" << f.base << ")";
        }
        os << "\n";
    }

    auto fontName = [&](std::size_t lang, std::uint32_t id) -> std::string {
        const auto& list = h.fonts[lang];
        return id < list.size() ? list[id].face : "?";
    };

    os << "\n[스타일]\n";
    for (const auto& st : h.styles) {
        os << "  [" << st.id << "] " << st.name;
        if (!st.engName.empty()) os << " (" << st.engName << ")";
        os << "  type=" << st.type << " paraShape=" << st.paraShapeId
           << " charShape=" << st.charShapeId << " next=" << st.nextStyleId << "\n";
    }

    os << "\n[글자 모양] id: 크기 한글글꼴/영문글꼴 장평 자간 색 속성\n";
    for (const auto& cs : h.charShapes) {
        os << "  [" << cs.id << "] " << pt(cs.height) << " " << fontName(0, cs.fontRef[0]) << "/"
           << fontName(1, cs.fontRef[1]) << " 장평=" << cs.ratio[0] << "% 자간=" << cs.spacing[0]
           << "% " << color(cs.textColor);
        if (cs.bold) os << " 진하게";
        if (cs.italic) os << " 기울임";
        if (cs.underline != "NONE") os << " 밑줄(" << cs.underline << ")";
        if (cs.strikeout) os << " 취소선";
        if (cs.superscript) os << " 위첨자";
        if (cs.subscript) os << " 아래첨자";
        os << "\n";
    }

    os << "\n[문단 모양] id: 정렬 왼쪽/오른쪽/들여쓰기 위/아래 줄간격 줄나눔(영/한)\n";
    for (const auto& ps : h.paraShapes) {
        os << "  [" << ps.id << "] " << ps.align << " " << ps.marginLeft << "/" << ps.marginRight
           << "/" << ps.indent << " " << ps.spacingPrev << "/" << ps.spacingNext << " "
           << ps.lineSpacingType << ":" << ps.lineSpacing << " " << ps.breakLatinWord << "/"
           << ps.breakNonLatinWord;
        if (ps.headingType != "NONE") os << " 머리=" << ps.headingType << "(" << ps.headingLevel << ")";
        os << "\n";
    }
}

class TreePrinter {
public:
    TreePrinter(const Document& doc, std::size_t maxText, std::ostream& os)
        : doc_(doc), maxText_(maxText), os_(os) {}

    void print() {
        for (std::size_t i = 0; i < doc_.sections.size(); ++i) {
            const Section& s = doc_.sections[i];
            os_ << "구역 " << i;
            if (s.page) os_ << " (" << mm(s.page->width) << " x " << mm(s.page->height) << " mm)";
            os_ << "\n";
            paragraphs(s.paragraphs, 1);
        }
    }

private:
    void indent(int depth) { os_ << std::string(static_cast<std::size_t>(depth) * 2, ' '); }

    std::string styleName(std::uint32_t id) const {
        const auto& styles = doc_.header.styles;
        return id < styles.size() ? styles[id].name : std::to_string(id);
    }

    void paragraphs(const std::vector<Paragraph>& list, int depth) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Paragraph& p = list[i];
            indent(depth);
            os_ << "문단 " << i << " [" << styleName(p.styleId) << ", 문단모양 " << p.paraShapeId
                << "] \"" << preview(p.text(), maxText_) << "\"\n";
            for (const auto& run : p.runs) {
                for (const auto& item : run.items) {
                    if (item.type == InlineItem::Type::Control) control(item.control, depth + 1);
                }
            }
        }
    }

    void control(const Control& c, int depth) {
        indent(depth);
        os_ << "- " << controlKindName(c.kind) << " '" << c.ctrlId << "'";
        if (!c.shapeType.empty()) os_ << " " << c.shapeType;
        if (c.table) os_ << " " << c.table->rowCount << "x" << c.table->colCount << " (셀 " << c.table->cells.size() << ")";
        if (!c.binItemId.empty()) os_ << " bin=" << c.binItemId;
        if (c.kind == ControlKind::Equation) os_ << " \"" << preview(c.script, maxText_) << "\"";
        if (!c.applyPageType.empty()) os_ << " " << c.applyPageType;
        if (!c.numType.empty()) os_ << " " << c.numType << "=" << c.number;
        if (!c.name.empty()) os_ << " name=\"" << preview(c.name, maxText_) << "\"";
        if (!c.command.empty()) os_ << " command=\"" << preview(c.command, maxText_) << "\"";
        if (!c.text.empty()) os_ << " \"" << preview(c.text, maxText_) << "\"";
        if (!c.subText.empty()) os_ << " sub=\"" << preview(c.subText, maxText_) << "\"";
        if (c.placement) {
            os_ << (c.placement->treatAsChar ? " [글자처럼]" : "") << " " << mm(c.placement->width)
                << "x" << mm(c.placement->height) << "mm";
        }
        os_ << "\n";
        if (c.caption) {
            indent(depth + 1);
            os_ << "캡션 " << c.caption->side << "\n";
            paragraphs(c.caption->content.paragraphs, depth + 2);
        }
        if (c.table) {
            for (const auto& cell : c.table->cells) {
                indent(depth + 1);
                os_ << "셀(" << cell.row << "," << cell.col << ")";
                if (cell.rowSpan > 1 || cell.colSpan > 1) os_ << " span " << cell.rowSpan << "x" << cell.colSpan;
                os_ << "\n";
                paragraphs(cell.content.paragraphs, depth + 2);
            }
        }
        for (const auto& sub : c.subLists) paragraphs(sub.paragraphs, depth + 1);
        for (const auto& child : c.children) control(child, depth + 1);
    }

    const Document& doc_;
    std::size_t maxText_;
    std::ostream& os_;
};

int cmdRaw(const Args& args, ByteView data, std::ostream& os) {
    const SourceFormat format = detectFormat(data);
    if (format == SourceFormat::Hwpx) {
        zip::Archive archive;
        std::string error;
        if (!archive.open(data, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        os << "ZIP 엔트리 (" << archive.entries().size() << ")\n";
        for (const auto& e : archive.entries()) {
            os << "  " << e.name << "  " << e.size << " (압축 " << e.compressedSize << ")\n";
        }
        return 0;
    }
    if (format != SourceFormat::Hwp) {
        std::cerr << "HWP/HWPX 파일이 아닙니다: " << pathToUtf8(args.input) << "\n";
        return 1;
    }

    cfb::CompoundFile cf;
    std::string error;
    if (!cf.open(data, &error)) {
        std::cerr << "CFB: " << error << "\n";
        return 1;
    }
    os << "CFB 엔트리 (" << cf.entries().size() << ")\n";
    for (const auto& e : cf.entries()) {
        const char* type = e.type == cfb::EntryType::Storage ? "DIR " : e.type == cfb::EntryType::Root ? "ROOT" : "    ";
        std::string shown = e.path;
        for (char& ch : shown) {
            if (static_cast<unsigned char>(ch) < 0x20) ch = '?';
        }
        os << "  " << type << " " << (shown.empty() ? "/" : shown) << "  " << e.size << "\n";
    }

    Bytes header;
    bool compressed = true;
    if (cf.readStream("FileHeader", header) && header.size() >= 40) {
        compressed = (readU32LE(header.data() + 36) & 1) != 0;
    }

    auto dumpRecords = [&](const std::string& path) {
        Bytes raw;
        Bytes stream;
        if (!cf.readStream(path, raw)) return;
        if (compressed) {
            if (!hwp::decompressStream(raw, stream)) {
                os << "\n" << path << ": 압축 해제 실패\n";
                return;
            }
        } else {
            stream = raw;
        }
        std::vector<hwp::Record> records;
        std::string err;
        const bool ok = hwp::parseRecords(stream, records, &err);
        std::map<std::uint16_t, std::size_t> counts;
        for (const auto& rec : records) ++counts[rec.tag];
        os << "\n" << path << ": " << stream.size() << " bytes, 레코드 " << records.size();
        if (!ok) os << " (오류: " << err << ")";
        os << "\n";
        for (const auto& [tagId, count] : counts) {
            os << "  " << tagId << " " << hwp::tagName(tagId) << " x" << count << "\n";
        }
    };
    dumpRecords("DocInfo");
    for (int i = 0; cf.find("BodyText/Section" + std::to_string(i)); ++i) {
        dumpRecords("BodyText/Section" + std::to_string(i));
    }
    return 0;
}

int cmdExtract(const Args& args, const ParseResult& r) {
    fs::path dir = args.outDir;
    if (dir.empty()) dir = args.input.parent_path() / (args.input.stem().u8string() + u8"_bindata");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "폴더를 만들 수 없습니다: " << pathToUtf8(dir) << "\n";
        return 1;
    }
    std::size_t written = 0;
    for (const auto& item : r.document.header.binItems) {
        if (item.data.empty()) continue;
        const fs::path name = pathFromUtf8(item.path).filename();
        if (writeFile(dir / name, item.data)) ++written;
    }
    std::cout << written << "개 파일을 " << pathToUtf8(dir) << " 에 저장했습니다.\n";
    return 0;
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
            out += buf;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

void dumpOneEquation(const Control& control, int& index, std::ostream& os) {
    const std::int32_t fileW = control.placement ? static_cast<std::int32_t>(control.placement->width) : 0;
    const std::int32_t fileH = control.placement ? static_cast<std::int32_t>(control.placement->height) : 0;
    const int fileBase = control.fileBaseLine >= 0 ? control.fileBaseLine : control.baseLine;
    const render::EquationSize ours = render::measureHangulEquation(control.script, control.baseUnit, control.equationFont);
    const render::EquationSize hancom = render::measureHangulEquation(control.script, control.baseUnit, "HancomEQN");
    os << "{\"i\":" << index++
       << ",\"base\":" << control.baseUnit
       << ",\"font\":\"" << jsonEscape(control.equationFont) << "\""
       << ",\"fileW\":" << fileW
       << ",\"fileH\":" << fileH
       << ",\"fileBase\":" << fileBase
       << ",\"ourW\":" << ours.width
       << ",\"ourH\":" << ours.height
       << ",\"ourBase\":" << ours.baseLine
       << ",\"eqnW\":" << hancom.width
       << ",\"eqnH\":" << hancom.height
       << ",\"eqnBase\":" << hancom.baseLine
       << ",\"script\":\"" << jsonEscape(control.script) << "\"}\n";
}

void dumpControl(const Control& control, int& index, std::ostream& os);

void dumpEquations(const std::vector<Paragraph>& list, int& index, std::ostream& os) {
    for (const Paragraph& para : list) {
        for (const Run& run : para.runs) {
            for (const InlineItem& item : run.items) {
                if (item.type == InlineItem::Type::Control) dumpControl(item.control, index, os);
            }
        }
    }
}

void dumpControl(const Control& control, int& index, std::ostream& os) {
    if (control.kind == ControlKind::Equation) dumpOneEquation(control, index, os);
    if (control.caption) dumpEquations(control.caption->content.paragraphs, index, os);
    if (control.table) {
        for (const TableCell& cell : control.table->cells) dumpEquations(cell.content.paragraphs, index, os);
    }
    for (const SubList& sub : control.subLists) dumpEquations(sub.paragraphs, index, os);
    for (const Control& child : control.children) dumpControl(child, index, os);
}

int run(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::cerr << kUsage;
        return 2;
    }

    Bytes data;
    if (!readFile(args.input, data)) {
        std::cerr << "파일을 읽을 수 없습니다: " << pathToUtf8(args.input) << "\n";
        return 1;
    }

    std::ostringstream out;
    int code = 0;
    if (args.command == "raw") {
        code = cmdRaw(args, data, out);
    } else {
        ParseOptions options;
        options.loadBinData = args.command == "extract" || args.command == "png" || args.command == "export";
        options.keepLineSegs = (args.command == "json" && args.lineSegs) || args.command == "export";
        const ParseResult result = parseDocument(data, options);
        if (!result.ok) {
            std::cerr << "파싱 실패: " << result.error << "\n";
            return 1;
        }
        for (const auto& w : result.warnings) std::cerr << "경고: " << w << "\n";

        if (args.command == "export") {
            if (args.output.empty()) {
                std::cerr << "export 명령은 -o <출력파일.hwp|.hwpx> 옵션이 필요합니다.\n";
                return 2;
            }
            std::string ext = pathToUtf8(args.output.extension());
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            ExportOptions expOpts;
            expOpts.sourceData = data;
            if (ext == ".hwp") expOpts.format = SourceFormat::Hwp;
            else if (ext == ".hwpx") expOpts.format = SourceFormat::Hwpx;
            else {
                std::cerr << "지원하지 않는 확장자입니다 (.hwp 또는 .hwpx): " << ext << "\n";
                return 2;
            }
            const ExportResult exported = writeDocument(result.document, expOpts);
            if (!exported.ok) {
                std::cerr << "내보내기 실패: " << exported.error << "\n";
                return 1;
            }
            if (!writeFile(args.output, exported.data)) {
                std::cerr << "저장 실패: " << pathToUtf8(args.output) << "\n";
                return 1;
            }
            std::cout << pathToUtf8(args.output) << " (" << exported.data.size() << " bytes)\n";
            return 0;
        } else if (args.command == "info") {
            cmdInfo(args, result, out);
        } else if (args.command == "text") {
            TextOptions to;
            to.includeHeaderFooter = args.headerFooter;
            to.includeNotes = !args.noNotes;
            out << documentToText(result.document, to);
        } else if (args.command == "styles") {
            cmdStyles(result, out);
        } else if (args.command == "tree") {
            TreePrinter(result.document, args.maxText, out).print();
        } else if (args.command == "json") {
            JsonOptions jo;
            jo.pretty = args.pretty;
            jo.includeLineSegs = args.lineSegs;
            jo.includeBody = !args.noBody;
            out << documentToJson(result.document, jo) << "\n";
        } else if (args.command == "extract") {
            return cmdExtract(args, result);
        } else if (args.command == "eq") {
            int index = 0;
            for (const Section& section : result.document.sections) {
                dumpEquations(section.paragraphs, index, out);
                for (const MasterPage& page : section.masterPages) dumpEquations(page.content.paragraphs, index, out);
            }
        } else if (args.command == "png") {
            const fs::path dir = args.outDir.empty() ? fs::path("pages") : args.outDir;
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::vector<render::Node> pages = render::layoutPages(result.document);
            const int height = args.height > 0 ? args.height : 1403;
            for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
                const render::PageImage image = render::renderPagePng(result.document, pages[static_cast<std::size_t>(i)], height);
                if (image.png.empty()) {
                    std::cerr << "그리기 실패: " << (i + 1) << "쪽\n";
                    return 1;
                }
                const fs::path file = dir / (std::to_string(i + 1) + ".png");
                if (!writeFile(file, image.png)) {
                    std::cerr << "저장 실패: " << pathToUtf8(file) << "\n";
                    return 1;
                }
            }
            std::cout << pages.size() << "\n";
            return 0;
        } else {
            std::cerr << "알 수 없는 명령: " << args.command << "\n\n" << kUsage;
            return 2;
        }
    }

    const std::string text = out.str();
    if (!args.output.empty()) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
        if (!writeFile(args.output, ByteView(bytes, text.size()))) {
            std::cerr << "저장 실패: " << pathToUtf8(args.output) << "\n";
            return 1;
        }
    } else {
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    return code;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    return run(argc, argv);
}
