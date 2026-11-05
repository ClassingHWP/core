#include "chwp/app/doc_session.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>

#include "chwp/export/json.hpp"
#include "chwp/reader.hpp"
#include "chwp/writer.hpp"
#include "export/font_metrics.hpp"

namespace chwp::app {

namespace {

bool endsWithNoCase(const std::string& text, std::string_view tail) {
    if (text.size() < tail.size()) return false;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const char c = text[text.size() - tail.size() + i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != tail[i]) return false;
    }
    return true;
}

// 처음 고칠 때 치는 글자가 문서 글꼴에 없으면 쓰는 대체 글꼴.
constexpr const char* kEditFallbackFaces[] = {"함초롬바탕", "함초롬돋움"};

std::unordered_map<std::string, std::string>& swapPreferences() {
    static std::unordered_map<std::string, std::string> prefs;
    return prefs;
}

}  // namespace

void setLicenseSwapPreferences(const std::string& json) {
    swapPreferences().clear();
    layout::collectFaceFiles(json, swapPreferences());
}

std::string licenseSwapPreferences() {
    JsonWriter writer(false);
    writer.beginObject();
    for (const auto& [from, to] : swapPreferences()) writer.field(from.c_str(), to);
    writer.endObject();
    return writer.take();
}

bool DocSession::open(Bytes bytes, std::string* error, std::vector<std::string>* warnings) {
    close();
    file = std::move(bytes);
    ParseResult result = parseDocument(file);
    if (!result.ok) {
        if (error) *error = result.error;
        file.clear();
        file.shrink_to_fit();
        return false;
    }
    doc = std::move(result.document);
    sourceFormat = detectFormat(file);
    if (warnings) *warnings = std::move(result.warnings);
    render::setPictureFile(file);
    return true;
}

void DocSession::openBlank() {
    close();
    doc = model::blankDocument();
    sourceFormat = model::SourceFormat::Hwpx;
}

void DocSession::close() {
    const render::EditScope scope(edit);
    render::setPictureFile({});
    doc = model::Document{};
    pages.clear();
    file.clear();
    file.shrink_to_fit();
    render::clearUndoHistory();
    edit.clearCursor();
    sourceFormat = model::SourceFormat::Unknown;
    fontWants_.clear();
    editedOnce_ = false;
    savedOnce_ = false;
    gapRounds_ = 0;
    gapWants_.clear();
}

void DocSession::layout(bool lineSeg) {
    storedLineHeight = lineSeg;
    pages = render::layoutPages(doc, -1, lineSeg, lineSeg);
}

void DocSession::relayoutAll() {
    render::rebuildStoryMap(doc);
    render::invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    render::clearPageRenderCache();
    pages = render::layoutPages(doc, -1, storedLineHeight, storedLineHeight);
}

std::optional<Bytes> DocSession::save(model::SourceFormat format, std::string* error) const {
    ExportOptions options;
    options.format = format;
    options.sourceData = file;
    model::Document saved = doc;
    render::fillMissingLineSegs(saved, pages);
    ExportResult result = writeDocument(saved, options);
    if (!result.ok) {
        if (error) *error = result.error;
        return std::nullopt;
    }
    return std::move(result.data);
}

std::optional<model::SourceFormat> DocSession::saveFormatFor(const std::string& name, const std::string& wanted) const {
    std::string lower = wanted;
    for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (lower == "hwp") return model::SourceFormat::Hwp;
    if (lower == "hwpx") return model::SourceFormat::Hwpx;
    if (!lower.empty()) return std::nullopt;
    if (endsWithNoCase(name, ".hwpx")) return model::SourceFormat::Hwpx;
    if (endsWithNoCase(name, ".hwp")) return model::SourceFormat::Hwp;
    return sourceFormat == model::SourceFormat::Unknown ? model::SourceFormat::Hwpx : sourceFormat;
}

void DocSession::wantFont(const std::string& face) {
    if (face.empty() || std::find(fontWants_.begin(), fontWants_.end(), face) != fontWants_.end()) return;
    fontWants_.push_back(face);
}

void DocSession::noteEdit(const render::EditResult& result, std::vector<std::string> faces) {
    if (!result.documentChanged) return;
    if (!editedOnce_) {
        editedOnce_ = true;
        for (const char* face : kEditFallbackFaces) wantFont(face);
    }
    for (const std::string& face : faces) wantFont(face);
}

std::vector<std::string> DocSession::takeFontWants() { return std::exchange(fontWants_, {}); }

std::vector<std::string> DocSession::missingFonts() const {
    // 받지 못한 글꼴뿐 아니라, 글꼴 목록이 없어 받으려 하지도 않은 글꼴도 넣는다.
    // 굵은 짝 이름은 파일이 없어도 보통 글꼴을 굵게 그리므로, 보통 글꼴까지 없을 때만 넣는다.
    std::vector<std::string> missing;
    for (const std::string& face : layout::documentFontFaces(doc)) {
        if (layout::fontFaceAvailable(face)) continue;
        std::string base = face;
        for (const std::string_view tail : {std::string_view(" Bold"), std::string_view(" 볼드")}) {
            if (base.size() > tail.size() && base.compare(base.size() - tail.size(), tail.size(), tail) == 0) {
                base.resize(base.size() - tail.size());
            }
        }
        if (base != face && layout::fontFaceAvailable(base)) continue;
        if (std::find(missing.begin(), missing.end(), base) == missing.end()) missing.push_back(base);
    }
    std::sort(missing.begin(), missing.end());
    return missing;
}

std::vector<std::string> DocSession::fontGaps() {
    if (gapRounds_ >= kFontGapRounds) return {};
    ++gapRounds_;
    std::vector<std::string> gaps = layout::documentFontGaps(doc);
    for (const std::string& face : gaps) {
        if (std::find(gapWants_.begin(), gapWants_.end(), face) == gapWants_.end()) gapWants_.push_back(face);
    }
    return gaps;
}

bool DocSession::fontsLanded(const std::vector<std::string>& faces) {
    // replace.json: 문서 글꼴 대신 받은 글꼴이 들어왔으면 문서의 글꼴 이름을 그 글꼴로 바꾼다.
    bool renamed = false;
    for (const std::string& face : layout::documentFontFaces(doc)) {
        const std::string next = layout::fontReplacement(face);
        if (next.empty() || next == face || !layout::fontFaceAvailable(next)) continue;
        renamed = model::replaceFontFace(doc, face, next) > 0 || renamed;
    }
    if (pages.empty()) return false;
    bool used = renamed;
    if (!used) {
        const std::vector<std::string> inDoc = layout::documentFontFaces(doc);
        for (const std::string& face : faces) {
            if (std::find(inDoc.begin(), inDoc.end(), face) != inDoc.end() ||
                std::find(gapWants_.begin(), gapWants_.end(), face) != gapWants_.end()) {
                used = true;
                break;
            }
        }
    }
    if (!used) return false;
    render::clearGlyphWarm();
    relayoutAll();
    return true;
}

render::EditResult DocSession::insertText(const std::string& text, bool composing) {
    const render::EditScope scope(edit);
    render::EditResult result = render::editInsertText(doc, pages, text, composing, storedLineHeight);
    noteEdit(result);
    return result;
}

std::vector<render::EditResult> DocSession::insertPlainText(const std::string& text) {
    // 줄 끝을 하나로 모은 뒤 줄마다 넣고 줄 사이에서 문단을 나눈다. 문단 안 줄바꿈(U+2028)은 편집기의 "\n" 이다.
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            normalized += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            normalized += text[i];
        }
    }
    std::vector<render::EditResult> results;
    // 줄이 하나면 글 넣기 한 번이다. 묶음을 열지 않아야 이어 친 글자가 되돌리기 한 칸에 잇는다.
    if (normalized.find('\n') == std::string::npos) {
        std::string piece = normalized;
        for (std::size_t at = piece.find("\xE2\x80\xA8"); at != std::string::npos; at = piece.find("\xE2\x80\xA8", at)) {
            piece.replace(at, 3, "\n");
        }
        // 빈 글도 넘긴다. 조합 중이면 조합 글을 비우고 조합을 끝낸다(입력기가 조합을 지우고 끝낼 때).
        results.push_back(insertText(piece));
        return results;
    }
    // 여러 줄은 되돌리기 한 칸으로 묶는다. 바깥(agent apply 등)에서 묶음을 열었으면 그 묶음에 든다.
    bool ownGroup = false;
    {
        const render::EditScope scope(edit);
        ownGroup = !render::undoGroupOpen();
    }
    if (ownGroup) beginEditGroup();
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t end = std::min(normalized.find('\n', start), normalized.size());
        std::string piece = normalized.substr(start, end - start);
        for (std::size_t at = piece.find("\xE2\x80\xA8"); at != std::string::npos; at = piece.find("\xE2\x80\xA8", at)) {
            piece.replace(at, 3, "\n");
        }
        if (!piece.empty()) {
            results.push_back(insertText(piece));
            if (!results.back().documentChanged) break;
        }
        if (end == normalized.size()) break;
        results.push_back(keyDown(render::EditKey::Enter));
        if (!results.back().documentChanged) break;
        start = end + 1;
    }
    if (ownGroup) endEditGroup();
    return results;
}

render::EditResult DocSession::keyDown(render::EditKey key, bool shift, bool ctrl, bool alt) {
    const render::EditScope scope(edit);
    render::EditResult result = render::editKeyDown(doc, pages, key, shift, ctrl, alt, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::applyFormat(const render::FormatPatch& asked) {
    const render::EditScope scope(edit);
    // 켜고 끄기는 지금 고른 글의 모양으로 정한다. 섞여 있으면 켠다. 밑줄을 켤 때는 쓰던 밑줄 종류를 살리고, 없으면 아래 밑줄이다.
    render::FormatPatch patch = asked;
    if (!patch.toggles.empty()) {
        const render::CursorStyle now = render::cursorStyle(doc, pages);
        for (const std::string& name : patch.toggles) {
            if (name == "bold") {
                patch.hasBold = true;
                patch.bold = !now.bold || now.mixed.bold;
            } else if (name == "italic") {
                patch.hasItalic = true;
                patch.italic = !now.italic || now.mixed.italic;
            } else if (name == "strikeout") {
                patch.hasStrikeout = true;
                patch.strikeout = !now.strikeout || now.mixed.strikeout;
            } else if (name == "underline") {
                const bool on = !now.underline || now.mixed.underline;
                patch.hasUnderline = true;
                patch.underline = on;
                patch.hasUnderlineType = true;
                patch.underlineType = on ? (now.underlineType.empty() || now.underlineType == "NONE" ? "BOTTOM" : now.underlineType)
                                         : "NONE";
            }
        }
        patch.toggles.clear();
    }
    render::EditResult result = render::editApplyFormat(doc, pages, patch, storedLineHeight);
    noteEdit(result, patch.hasFont ? std::vector<std::string>{patch.font} : std::vector<std::string>{});
    return result;
}

render::EditResult DocSession::insertTable(int rows, int cols) {
    const render::EditScope scope(edit);
    render::EditResult result = render::editInsertTable(doc, pages, rows, cols, storedLineHeight);
    noteEdit(result);
    return result;
}

std::vector<std::pair<std::string, std::string>> DocSession::licenseSwapTargets() const {
    const std::vector<std::string> pickable = layout::pickableFonts();
    std::vector<std::pair<std::string, std::string>> out;
    for (const layout::FontSwap& swap : layout::licenseFontSwaps(doc)) {
        std::string to = swap.to;
        if (const auto pref = swapPreferences().find(swap.from);
            pref != swapPreferences().end() && std::find(pickable.begin(), pickable.end(), pref->second) != pickable.end()) {
            to = pref->second;
        }
        out.emplace_back(swap.from, std::move(to));
    }
    return out;
}

render::EditResult DocSession::swapLicenseFonts(const std::vector<std::pair<std::string, std::string>>& chosen) {
    // 고르지 않은 글꼴은 전에 고른 글꼴이나 기본 짝으로 바꾼다. 기본 짝과 다르게 고른 것만 기억한다.
    std::vector<std::pair<std::string, std::string>> picks = licenseSwapTargets();
    for (const layout::FontSwap& swap : layout::licenseFontSwaps(doc)) {
        for (const auto& [from, to] : chosen) {
            if (from != swap.from) continue;
            if (!to.empty() && to != swap.to) swapPreferences()[from] = to;
            else swapPreferences().erase(from);
            for (auto& [pickFrom, pickTo] : picks) {
                if (pickFrom == from && !to.empty()) pickTo = to;
            }
        }
    }
    // 바꿔 넣을 글꼴은 바꾼 뒤 받아야 한다(편집이 부른 글꼴).
    std::vector<std::string> targets;
    for (const auto& [from, to] : picks) targets.push_back(to);
    const render::EditScope scope(edit);
    render::EditResult result = render::editSwapLicenseFonts(doc, pages, storedLineHeight, picks);
    noteEdit(result, std::move(targets));
    return result;
}

render::EditResult DocSession::insertPicture(const Bytes& image) {
    const render::EditScope scope(edit);
    render::EditResult result = render::editInsertPicture(doc, pages, image, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::applyPageSetup(const render::FormatFields& fields, bool whole) {
    const render::EditScope scope(edit);
    render::EditResult result = render::editApplyPageSetup(doc, pages, fields, whole, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::commitComposition() {
    const render::EditScope scope(edit);
    render::EditResult result = render::editCommitComposition(doc, pages);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertDrawnShape() {
    const render::EditScope scope(edit);
    render::EditResult result = render::editInsertDrawnShape(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::moveObject() {
    const render::EditScope scope(edit);
    render::EditResult result = render::editMoveObject(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::resizeObject() {
    const render::EditScope scope(edit);
    render::EditResult result = render::editResizeObject(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::resizeTableBorder() {
    const render::EditScope scope(edit);
    render::EditResult result = render::editResizeTableBorder(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::PageImage DocSession::renderPage(int index, int pixelHeight, render::FrameCache& frames) {
    if (index < 0 || index >= static_cast<int>(pages.size())) return {};
    render::Node& page = pages[static_cast<std::size_t>(index)];
    if (page.spansLater) render::ensurePageSpans(doc, page);
    render::PageImage image = render::encodePageImage(render::renderPageGpu(doc, pages, index, pixelHeight, frames));
    if (image.png.empty()) image = render::renderPagePng(doc, page, pixelHeight);
    return image;
}

render::EditResult DocSession::undo() {
    const render::EditScope scope(edit);
    return render::editUndo(doc, pages, storedLineHeight);
}

render::EditResult DocSession::redo() {
    const render::EditScope scope(edit);
    return render::editRedo(doc, pages, storedLineHeight);
}

bool DocSession::canUndo() {
    const render::EditScope scope(edit);
    return render::canUndo();
}

bool DocSession::canRedo() {
    const render::EditScope scope(edit);
    return render::canRedo();
}

std::string DocSession::documentState() {
    const render::EditScope scope(edit);
    if (!render::undoAtSaved()) return "dirty";
    return savedOnce_ ? "saved" : "clean";
}

void DocSession::markSaved() {
    const render::EditScope scope(edit);
    render::markUndoSaved();
    savedOnce_ = true;
}

int DocSession::beginEditGroup() {
    const render::EditScope scope(edit);
    return render::beginUndoGroup();
}

void DocSession::endEditGroup() {
    const render::EditScope scope(edit);
    render::endUndoGroup();
}

int DocSession::topEditGroup() {
    const render::EditScope scope(edit);
    return render::topUndoGroup();
}

void DocSession::dropRedo() {
    const render::EditScope scope(edit);
    render::clearRedoHistory();
}

}  // namespace chwp::app
