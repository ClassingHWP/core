#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chwp/common/bytes.hpp"

namespace chwp::exporter {

// PDF 한 쪽: 종이 크기(HWPUNIT)와 그 쪽을 그린 픽셀(RGBA, 위에서 아래로).
struct PdfPage {
    std::int32_t paperWidth = 0;   // HWPUNIT (7200 = 1인치)
    std::int32_t paperHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<std::uint8_t> rgba;
};

// 쪽 그림을 차례로 받아 PDF 를 만든다. 쪽마다 그림 하나(DeviceRGB, FlateDecode)를 종이 크기에 꽉 채운다.
// 쪽 그림은 받는 대로 압축해 붙이므로 픽셀은 한 쪽씩만 들고 있다.
class PdfWriter {
 public:
    explicit PdfWriter(std::string title = {});
    // 쪽 하나를 붙인다. 픽셀이 비었거나 크기가 틀리면 거짓.
    bool addPage(const PdfPage& page);
    int pageCount() const { return static_cast<int>(pageObjects_.size()); }
    // 마무리한 PDF 바이트. 부른 뒤에는 더 붙이지 않는다.
    Bytes finish();

 private:
    int beginObject();
    void endObject();
    void write(const std::string& text);
    void write(const std::uint8_t* data, std::size_t size);

    Bytes out_;
    std::vector<std::size_t> offsets_;  // 객체 번호(1부터) 의 시작 자리
    std::vector<int> pageObjects_;
    std::string title_;
    bool finished_ = false;
};

}  // namespace chwp::exporter
