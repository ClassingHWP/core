#include "chwp/export/pdf.hpp"

#include <cstdio>
#include <cstring>

#include "miniz.h"

// PDF 1.4. 쪽마다 Page, 내용 흐름(그림을 종이에 꽉 채우는 cm·Do), 그림 XObject 를 둔다.
// 그림은 RGB 를 PNG 위 줄(Up) 거르기로 바꿔 zlib 으로 묶는다(Predictor 15). 흰 바탕이 많은 쪽은 잘 줄어든다.

namespace chwp::exporter {

namespace {

// PDF 글(괄호 문자열)은 UTF-16BE + BOM 을 16진수로 쓴다.
std::string pdfTextHex(const std::string& utf8) {
    std::string out = "<FEFF";
    char buf[8];
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        }
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            std::snprintf(buf, sizeof(buf), "%04X", 0xD800 + (cp >> 10));
            out += buf;
            std::snprintf(buf, sizeof(buf), "%04X", 0xDC00 + (cp & 0x3FF));
            out += buf;
        } else {
            std::snprintf(buf, sizeof(buf), "%04X", cp);
            out += buf;
        }
    }
    return out + ">";
}

std::string number(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", value);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

}  // namespace

PdfWriter::PdfWriter(std::string title) : title_(std::move(title)) {
    write("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    // 1 카탈로그, 2 쪽 묶음은 끝에 쓴다. 번호만 잡아 둔다.
    offsets_.resize(2, 0);
}

void PdfWriter::write(const std::string& text) { out_.insert(out_.end(), text.begin(), text.end()); }

void PdfWriter::write(const std::uint8_t* data, std::size_t size) { out_.insert(out_.end(), data, data + size); }

int PdfWriter::beginObject() {
    offsets_.push_back(out_.size());
    const int id = static_cast<int>(offsets_.size());
    write(std::to_string(id) + " 0 obj\n");
    return id;
}

void PdfWriter::endObject() { write("endobj\n"); }

bool PdfWriter::addPage(const PdfPage& page) {
    if (finished_ || page.pixelWidth <= 0 || page.pixelHeight <= 0 || page.paperWidth <= 0 || page.paperHeight <= 0) {
        return false;
    }
    const std::size_t w = static_cast<std::size_t>(page.pixelWidth);
    const std::size_t h = static_cast<std::size_t>(page.pixelHeight);
    if (page.rgba.size() < w * h * 4) return false;
    // 줄마다 거르기 바이트(2 = Up) + RGB. 투명은 흰 바탕에 얹는다.
    std::vector<std::uint8_t> raw((w * 3 + 1) * h);
    std::vector<std::uint8_t> prev(w * 3, 0);
    std::vector<std::uint8_t> row(w * 3);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* src = page.rgba.data() + y * w * 4;
        for (std::size_t x = 0; x < w; ++x) {
            const unsigned a = src[x * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                const unsigned v = src[x * 4 + c];
                row[x * 3 + c] = static_cast<std::uint8_t>((v * a + 255 * (255 - a) + 127) / 255);
            }
        }
        std::uint8_t* dst = raw.data() + y * (w * 3 + 1);
        dst[0] = 2;
        for (std::size_t i = 0; i < w * 3; ++i) dst[1 + i] = static_cast<std::uint8_t>(row[i] - prev[i]);
        prev.swap(row);
    }
    mz_ulong packedSize = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> packed(packedSize);
    if (mz_compress2(packed.data(), &packedSize, raw.data(), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK) return false;
    packed.resize(packedSize);
    raw.clear();
    raw.shrink_to_fit();

    // 종이 크기를 pt 로(1pt = 100 HWPUNIT).
    const double widthPt = page.paperWidth / 100.0;
    const double heightPt = page.paperHeight / 100.0;

    const int image = beginObject();
    write("<< /Type /XObject /Subtype /Image /Width " + std::to_string(w) + " /Height " + std::to_string(h) +
          " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3"
          " /BitsPerComponent 8 /Columns " +
          std::to_string(w) + " >> /Length " + std::to_string(packed.size()) + " >>\nstream\n");
    write(packed.data(), packed.size());
    write("\nendstream\n");
    endObject();

    const std::string content = "q " + number(widthPt) + " 0 0 " + number(heightPt) + " 0 0 cm /Im0 Do Q\n";
    const int contents = beginObject();
    write("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream\n");
    endObject();

    const int pageId = beginObject();
    write("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + number(widthPt) + " " + number(heightPt) +
          "] /Resources << /XObject << /Im0 " + std::to_string(image) + " 0 R >> >> /Contents " +
          std::to_string(contents) + " 0 R >>\n");
    endObject();
    pageObjects_.push_back(pageId);
    return true;
}

Bytes PdfWriter::finish() {
    if (finished_) return out_;
    finished_ = true;
    offsets_[1] = out_.size();
    std::string kids;
    for (int id : pageObjects_) kids += std::to_string(id) + " 0 R ";
    write("2 0 obj\n<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageObjects_.size()) + " >>\nendobj\n");
    offsets_[0] = out_.size();
    write("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    const int info = beginObject();
    write("<< /Producer (ClassingHWP)");
    if (!title_.empty()) write(" /Title " + pdfTextHex(title_));
    write(" >>\n");
    endObject();
    const std::size_t xref = out_.size();
    write("xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n");
    char line[24];
    for (std::size_t offset : offsets_) {
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
        write(line);
    }
    write("trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root 1 0 R /Info " + std::to_string(info) +
          " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
    return out_;
}

}  // namespace chwp::exporter
