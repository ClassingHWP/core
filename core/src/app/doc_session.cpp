#include "chwp/app/doc_session.hpp"

#include "chwp/render/link_commands.hpp"
#include "chwp/render/track_commands.hpp"
#include "chwp/export/pdf.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

bool DocSession::open(Bytes bytes, std::string* error, std::vector<std::string>* warnings,
                      const std::string& password) {
    close();
    file = std::move(bytes);
    ParseOptions options;
    options.password = password;
    ParseResult result = parseDocument(file, options);
    if (!result.ok) {
        if (result.needPassword) {
            lockedFile_ = std::move(file);
            wrongPassword_ = result.wrongPassword;
            if (error) *error = result.wrongPassword ? "암호가 틀렸습니다." : "암호가 걸린 문서입니다.";
        } else if (error) {
            *error = result.error;
        }
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

bool DocSession::unlock(const std::string& password, std::string* error, std::vector<std::string>* warnings) {
    if (lockedFile_.empty()) {
        if (error) *error = "암호를 기다리는 문서가 없습니다.";
        return false;
    }
    Bytes bytes = std::move(lockedFile_);
    lockedFile_.clear();
    return open(std::move(bytes), error, warnings, password);
}

void DocSession::openBlank() {
    close();
    doc = model::blankDocument();
    sourceFormat = model::SourceFormat::Hwpx;
}

void DocSession::close() {
    stacker_.reset();
    const render::EditScope scope(edit);
    render::setPictureFile({});
    doc = model::Document{};
    pages.clear();
    file.clear();
    file.shrink_to_fit();
    render::clearUndoHistory();
    edit.clearCursor();
    sourceFormat = model::SourceFormat::Unknown;
    lockedFile_.clear();
    lockedFile_.shrink_to_fit();
    wrongPassword_ = false;
    fontWants_.clear();
    editedOnce_ = false;
    savedOnce_ = false;
    gapRounds_ = 0;
    gapWants_.clear();
}

void DocSession::layout(bool lineSeg) {
    layoutBegin(lineSeg);
    finishLayout();
}

void DocSession::layoutBegin(bool lineSeg) {
    stacker_.reset();
    storedLineHeight = lineSeg;
    pages.clear();
    if (pages.capacity() < kPageReserve) pages.reserve(kPageReserve);
    pagesMoved_ = true;
    stacker_ = std::make_unique<render::PageStacker>(doc, lineSeg, lineSeg);
}

namespace {

// 쌓기 한 번이 붙일 수 있는 쪽 수의 여유(구역 끝의 미주 쪽까지). 자리가 이보다 적게 남으면 새로 잡는다.
constexpr std::size_t kStepRoom = 128;

}  // namespace

int DocSession::layoutStep(double budgetMs) {
    if (!stacker_) return 0;
    const auto started = std::chrono::steady_clock::now();
    const std::size_t before = pages.size();
    do {
        if (pages.capacity() - pages.size() < kStepRoom) {
            pages.reserve(pages.capacity() * 2);
            pagesMoved_ = true;
        }
        if (stacker_->step(pages) == 0 || stacker_->done()) {
            stacker_.reset();
            break;
        }
    } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() < budgetMs);
    return static_cast<int>(pages.size() - before);
}

int DocSession::layoutUntil(int count) {
    const std::size_t before = pages.size();
    while (stacker_ && static_cast<int>(pages.size()) < count) layoutStep(0);
    return static_cast<int>(pages.size() - before);
}

void DocSession::finishLayout() {
    while (stacker_) layoutStep(1e9);
}

bool DocSession::takePagesMoved() {
    const bool moved = pagesMoved_;
    pagesMoved_ = false;
    return moved;
}

void DocSession::ensureFullLayout() {
    if (!stacker_) return;
    if (finishLayoutHook) finishLayoutHook();
    finishLayout();
}

void DocSession::relayoutAll() {
    // 쌓던 쪽은 버리고 처음부터 끝까지 다시 놓는다.
    stacker_.reset();
    render::rebuildStoryMap(doc);
    render::invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    render::clearPageRenderCache();
    pages = render::layoutPages(doc, -1, storedLineHeight, storedLineHeight);
    // 캐럿 자리와 블록 칠은 옛 쪽의 줄에서 잰 것이다. 글꼴이 들어와 글자 폭이 바뀌면 칠이 옛 폭으로 남는다.
    // 같은 논리 자리로 새 쪽에서 다시 놓는다.
    const render::EditScope scope(edit);
    const render::PointerCursor cur = render::pointerCursor();
    if (cur.active && cur.story > 0 && render::storyParagraphs(doc, cur.story)) {
        render::setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.anchorParagraph, cur.anchorOffset},
                                 model::CursorPos{cur.story, cur.focusParagraph, cur.focusOffset});
    }
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

std::vector<std::string> DocSession::openFontFaces() const {
    std::vector<std::string> faces = layout::documentFontFaces(doc);
    if (doc.sections.empty() || doc.sections.front().paragraphs.empty()) return faces;
    const model::Paragraph& first = doc.sections.front().paragraphs.front();
    const std::uint32_t shape = first.runs.empty() ? 0 : first.runs.front().charShapeId;
    const auto add = [&](const std::string& face) {
        if (std::find(faces.begin(), faces.end(), face) == faces.end()) faces.push_back(face);
    };
    for (const std::string& face : layout::charShapeFaces(doc, shape)) {
        add(face);
        // 굵은 짝은 파일이 없으면 보통 글꼴을 굵게 그린다.
        if (endsWithNoCase(face, " Bold") || endsWithNoCase(face, " 볼드")) continue;
        // 글꼴 파일이 없으면(폭만 잰 글꼴 포함) 그리기는 함초롬바탕이다(documentFontFaces 와 같은 판단).
        const std::string replaced = layout::fontReplacement(face);
        const std::string path = layout::fontCatalogPath(replaced.empty() ? face : replaced);
        if (path.rfind("full/", 0) != 0 && path.rfind("standin/", 0) != 0) add("함초롬바탕");
    }
    return faces;
}

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
    // 글꼴이 들어오기 전에 그린 글자는 "그 글꼴 없음"으로 대체를 열어 둔다. 문서가 아직 그 글꼴을 쓰지 않아도
    // (빈 새 문서에서 첫 편집이 부른 글꼴이 글을 치기 전에 들어온 때) 그 기억을 지운다. 지우지 않으면 뒤에 친
    // 글자가 계속 없는 글꼴로 그려져 보이지 않는다.
    if (!faces.empty() || renamed) render::clearGlyphWarm();
    // 수식 글꼴을 받는 중에 넣거나 고친 수식은 그때 잰 크기다. 글꼴이 다 왔으면 다시 잰다(한글이 재는 크기).
    const int remeasured = render::remeasurePendingEquations(doc);
    if (pages.empty()) return false;
    bool used = renamed || remeasured > 0;
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
    relayoutAll();
    return true;
}

void DocSession::viewMarksChanged(const render::ViewMarks& before) {
    const render::ViewMarks now = render::viewMarks();
    if (before == now) return;
    // 이름표 폭은 줄 자리(placeLine)에 든다. 지난 자리 기록은 옛 표시로 잰 것이다.
    render::invalidateLinePlaces({}, true);
    if (pages.empty()) return;
    if (before.memo != now.memo || before.control != now.control) render::placeMemos(doc, pages);
}

render::EditResult DocSession::insertText(const std::string& text, bool composing) {
    const render::EditScope scope = wholeScope();
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
        const render::EditScope scope = wholeScope();
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
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editKeyDown(doc, pages, key, shift, ctrl, alt, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::followLinkAt(int page, int x, int y, std::string* url, std::string* why) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editFollowLinkAt(doc, pages, page, x, y, storedLineHeight, url, why);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertClickHere(const std::string& direction, const std::string& memo,
                                                const std::string& name, std::string* why) {
    const render::EditScope bound = wholeScope();
    render::EditResult result = render::editInsertClickHere(doc, pages, direction, memo, name, storedLineHeight, why);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::clickHereView() {
    const render::EditScope bound = wholeScope();
    return render::clickHereFields(doc, pages);
}

render::EditResult DocSession::modifyClickHere(const render::FormatFields& fields, std::string* why) {
    const render::EditScope bound = wholeScope();
    render::EditResult result = render::editModifyClickHere(doc, pages, fields, storedLineHeight, why);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::syncClickHereGuide() {
    {
        // 가벼운 판단은 쪽을 다 쌓지 않고 한다. 맞출 것이 있을 때만 문서 전체 쪽을 잡는다.
        const render::EditScope scope(edit);
        if (!render::clickHereGuideNeedsSync(doc, pages)) return {};
    }
    const render::EditScope scope = wholeScope();
    return render::editSyncClickHereGuide(doc, pages, storedLineHeight);
}

render::EditResult DocSession::revealClickHereGuide() {
    {
        const render::EditScope scope(edit);
        if (!render::clickHereGuideHidden()) return {};
    }
    const render::EditScope scope = wholeScope();
    return render::editRevealClickHereGuide(doc, pages);
}

render::EditResult DocSession::toggleTrackChanges() {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editToggleTrackChanges(doc, pages);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::resolveChanges(bool accept, const std::string& scope, std::string* why) {
    const render::EditScope bound = wholeScope();
    render::EditResult result = render::editResolveChangesUndo(doc, pages, accept, scope, storedLineHeight, why);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::moveToChange(bool forward) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editMoveToChange(doc, pages, forward);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::linkHistory(bool forward) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editLinkHistory(doc, pages, forward);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::moveToView(const std::vector<render::ViewBand>& bands, bool end, bool shift) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editMoveToView(doc, pages, bands, end, shift);
    noteEdit(result);
    return result;
}

namespace {

// 서식 탭 밑줄·취소선·글자 테두리가 기억하는 모양과 색(한글 2024 앱 설정 0x48·0x44·0x53·0x45·0x59·0x2E).
// 문서가 아니라 앱에 붙은 값이라 문서를 바꿔도 남는다. 처음 값은 한글 2024 새 설치에서 잰 것(실선·검정)이다.
struct CharLineMemory {
    std::string underlineShape = "SOLID";
    std::string underlineColor = "#000000";
    std::string strikeShape = "SOLID";
    std::string strikeColor = "#000000";
    std::string borderType = "SOLID";
    std::string borderColor = "#000000";
};
CharLineMemory& charLineMemory() {
    static CharLineMemory memory;
    return memory;
}

// 글자 테두리 네 변(한글 2024 TextBorderStyle). 굵기는 0.1mm 이고, 이중·삼중선
// (선 목록 8~11 번: DOUBLE_SLIM, SLIM_THICK, THICK_SLIM, SLIM_THICK_SLIM)은 그 번호의 굵기 목록 칸이다.
void putCharBorder(render::FormatPatch& patch, const std::string& type, const std::string& color) {
    static const char* const kDouble[] = {"DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM", "SLIM_THICK_SLIM"};
    static const double kWidthMm[] = {0.6, 0.7, 1.0, 1.5};  // 굵기 목록 8~11 번
    double mm = 0.1;
    for (int i = 0; i < 4; ++i) {
        if (type == kDouble[i]) mm = kWidthMm[i];
    }
    const double hu = std::round(mm * 7200.0 / 25.4);
    for (const char* side : {"Left", "Right", "Top", "Bottom"}) {
        const std::string base = std::string("char.border") + side;
        patch.fields.emplace_back(base + "Type", render::FormatValue{type});
        patch.fields.emplace_back(base + "Width", render::FormatValue{hu});
        patch.fields.emplace_back(base + "Color", render::FormatValue{color});
    }
}

}  // namespace

render::EditResult DocSession::applyFormat(const render::FormatPatch& asked) {
    const render::EditScope scope = wholeScope();
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
                // 한글 2024 CharShapeStrikeout: 모두 켜져 있으면 끄고, 아니면 기억한 모양으로
                // 켠다. 색은 고른 글의 취소선 색이 하나면 기억한 색, 섞였으면 검정이다.
                patch.hasStrikeout = true;
                patch.strikeout = !now.strikeout || now.mixed.strikeout;
                if (patch.strikeout) {
                    patch.fields.emplace_back("strikeShape", render::FormatValue{charLineMemory().strikeShape});
                    patch.fields.emplace_back("strikeColor", render::FormatValue{now.mixed.strikeColor
                                                                                     ? std::string("#000000")
                                                                                     : charLineMemory().strikeColor});
                }
            } else if (name == "charBorder") {
                // 네 변이 모두 없으면 기억한 선으로 켜고, 한 변이라도 있으면 모두 끈다.
                if (!now.charBorder && !now.mixed.charBorder) {
                    putCharBorder(patch, charLineMemory().borderType, charLineMemory().borderColor);
                } else {
                    putCharBorder(patch, "NONE", "#000000");
                }
            } else if (name == "superscript" || name == "subscript" || name == "superSub") {
                // 한글 2024: 위·아래 첨자는 하나만 켜진다. superSub 는 보통 → 위 → 아래 → 보통.
                bool super = false;
                bool sub = false;
                if (name == "superscript") {
                    super = !now.superscript || now.mixed.superscript;
                } else if (name == "subscript") {
                    sub = !now.subscript || now.mixed.subscript;
                } else if (now.superscript && !now.mixed.superscript) {
                    sub = true;
                } else if (!(now.subscript && !now.mixed.subscript)) {
                    super = true;
                }
                patch.fields.emplace_back("superscript", render::FormatValue{super});
                patch.fields.emplace_back("subscript", render::FormatValue{sub});
            } else if (name == "underline") {
                const bool on = !now.underline || now.mixed.underline;
                patch.hasUnderline = true;
                patch.underline = on;
                patch.hasUnderlineType = true;
                // 한글 2024 CharShapeUnderline: 켤 때는 아래 밑줄, 기억한 모양, 색은 고른 글의
                // 밑줄 색이 하나면 기억한 색, 섞였으면 검정이다.
                patch.underlineType = on ? "BOTTOM" : "NONE";
                if (on) {
                    patch.fields.emplace_back("underlineShape", render::FormatValue{charLineMemory().underlineShape});
                    patch.fields.emplace_back("underlineColor", render::FormatValue{now.mixed.underlineColor
                                                                                        ? std::string("#000000")
                                                                                        : charLineMemory().underlineColor});
                }
            }
        }
        patch.toggles.clear();
    }
    // 목록에서 고른 모양·색: 기억하고 그 줄을 켠다(한글 2024 화면에서 확인: 모양을 고르면 아래 밑줄로 켜지고 색은
    // 기억한 색, 색을 고르면 기억한 모양으로 켜진다. 다시 켜는 단추도 고른 모양을 쓴다).
    for (const auto& [name, value] : patch.linePicks) {
        CharLineMemory& memory = charLineMemory();
        if (name == "UnderlineShape" || name == "UnderlineColor") {
            (name == "UnderlineShape" ? memory.underlineShape : memory.underlineColor) = value;
            patch.hasUnderline = true;
            patch.underline = true;
            patch.hasUnderlineType = true;
            patch.underlineType = "BOTTOM";
            patch.fields.emplace_back("underlineShape", render::FormatValue{memory.underlineShape});
            patch.fields.emplace_back("underlineColor", render::FormatValue{memory.underlineColor});
        } else if (name == "StrikeShape" || name == "StrikeColor") {
            (name == "StrikeShape" ? memory.strikeShape : memory.strikeColor) = value;
            patch.hasStrikeout = true;
            patch.strikeout = true;
            patch.fields.emplace_back("strikeShape", render::FormatValue{memory.strikeShape});
            patch.fields.emplace_back("strikeColor", render::FormatValue{memory.strikeColor});
        } else if (name == "CharBorderType") {
            // 없음을 고르면 네 변을 지운다. 없음은 기억하지 않는다.
            if (value != "NONE") memory.borderType = value;
            putCharBorder(patch, value, memory.borderColor);
        } else if (name == "CharBorderColor") {
            memory.borderColor = value;
            putCharBorder(patch, memory.borderType, memory.borderColor);
        }
    }
    patch.linePicks.clear();
    render::EditResult result = render::editApplyFormat(doc, pages, patch, storedLineHeight);
    noteEdit(result, patch.hasFont ? std::vector<std::string>{patch.font} : std::vector<std::string>{});
    return result;
}

render::EditResult DocSession::insertTable(int rows, int cols) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertTable(doc, pages, rows, cols, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::editTable(const render::TableEditCommand& cmd, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editTable(doc, pages, cmd, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertBreak(render::BreakKind kind) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertBreak(doc, pages, kind, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::pageControl(render::PageControlKind kind, const render::FormatFields& fields,
                                           std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editPageControl(doc, pages, kind, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::leaveNote() {
    const render::EditScope scope = wholeScope();
    return render::editLeaveNote(doc, pages);
}

render::EditResult DocSession::noteShape(const render::FormatFields& fields, bool endnote, bool wholeDocument,
                                         std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editNoteShape(doc, pages, fields, endnote, wholeDocument, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::noteShapeView(bool endnote) {
    const render::EditScope scope = wholeScope();
    return render::noteShapeView(doc, endnote);
}

render::EditResult DocSession::pageBorder(const render::FormatFields& fields, bool wholeDocument, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editPageBorder(doc, pages, fields, wholeDocument, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::beginStyleDraft(int editing) {
    const render::EditScope scope = wholeScope();
    styleDraft = render::beginStyleDraft(doc, pages, editing);
    return render::styleDraftFields(styleDraft);
}

render::FormatFields DocSession::styleDraftFields() const { return render::styleDraftFields(styleDraft); }

render::FormatFields DocSession::setStyleDraftFields(const render::FormatFields& fields) {
    render::setStyleDraftFields(styleDraft, fields);
    return render::styleDraftFields(styleDraft);
}

render::FormatView DocSession::styleDraftFormatView(bool para, const render::FormatFields& patch) {
    const render::EditScope scope = wholeScope();
    const render::FormatViewKind kind = para ? render::FormatViewKind::Para : render::FormatViewKind::Char;
    return render::formatView(styleDraft.scratch, {}, kind, patch, &styleDraft.place);
}

void DocSession::applyStyleDraftFormat(bool para, const render::FormatFields& patch) {
    const render::EditScope scope = wholeScope();
    const render::FormatViewKind kind = para ? render::FormatViewKind::Para : render::FormatViewKind::Char;
    const render::FormatFields resolved =
        render::resolveFormatView(styleDraft.scratch, {}, kind, patch, &styleDraft.place);
    render::applyStyleDraftFormat(styleDraft, para, resolved);
}

render::PageBitmap DocSession::styleDraftPreview(bool para, const render::FormatFields& patch, int width, int height) {
    const render::EditScope scope = wholeScope();
    const render::FormatViewKind kind = para ? render::FormatViewKind::Para : render::FormatViewKind::Char;
    return render::renderFormatPreview(styleDraft.scratch, {}, kind, patch, width, height, &styleDraft.place);
}

render::EditResult DocSession::commitStyleDraft(std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editCommitStyle(doc, pages, styleDraft, storedLineHeight, error);
    noteEdit(result);
    if (result.documentChanged) styleDraft = render::StyleDraft{};
    return result;
}

void DocSession::endStyleDraft() { styleDraft = render::StyleDraft{}; }

render::EditResult DocSession::deleteStyle(std::uint32_t styleId, std::uint32_t replacement, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editDeleteStyle(doc, pages, styleId, replacement, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::moveStyle(std::uint32_t styleId, int delta, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editMoveStyle(doc, pages, styleId, delta, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::styleFromCaret(std::uint32_t styleId, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editStyleFromCaret(doc, pages, styleId, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::importStyles(const model::Document& source, const std::vector<std::string>& names,
                                            std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editImportStyles(doc, pages, source, names, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::makeToc(const render::TocOptions& options, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editMakeToc(doc, pages, options, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::updateToc(bool all, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editUpdateToc(doc, pages, all, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::clearCharStyle(std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editClearCharStyle(doc, pages, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::caption(const std::string& caption, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editCaption(doc, pages, caption, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::bookmark(const std::string& action, const std::string& name, std::string* error,
                                        const std::string& newName) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editBookmark(doc, pages, action, name, storedLineHeight, error, newName);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::goToPage(int page, std::string* error) {
    const render::EditScope scope = wholeScope();
    return render::editGoToPage(doc, pages, page, error);
}

render::EditResult DocSession::goToSection(int section, std::string* error) {
    const render::EditScope scope = wholeScope();
    return render::editGoToSection(doc, pages, section, error);
}

render::EditResult DocSession::goTo(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    std::string why;
    render::EditResult result = render::goToNow(doc, pages, fields, why);
    if (error) *error = why;
    return result;
}

render::EditResult DocSession::copyShape(const std::string& action, int type, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editCopyShape(doc, pages, action, type, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::deleteMarks(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editDeleteMarks(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::tableText(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editTableText(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

bool DocSession::copiedShapeReady() {
    const render::EditScope scope(edit);
    return render::copiedShapeReady();
}

render::EditResult DocSession::exchangeNotes(int mode, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editExchangeNotes(doc, pages, mode, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::goToView() {
    const render::EditScope scope = wholeScope();
    return render::goToView(doc, pages);
}

render::FormatFields DocSession::bookmarkView() {
    const render::EditScope scope = wholeScope();
    std::string names;
    for (const render::BookmarkEntry& entry : render::bookmarkList(doc)) {
        if (!names.empty()) names += '\n';
        names += entry.name;
    }
    render::FormatFields out;
    out.emplace_back("names", names);
    out.emplace_back("defaultName", render::bookmarkDefaultName(doc, pages));
    // 찾아가기 목록 상자: 쪽 수·구역 수.
    out.emplace_back("pageCount", static_cast<double>(pages.size()));
    out.emplace_back("sectionCount", static_cast<double>(doc.sections.size()));
    return out;
}

render::EditResult DocSession::tableProperties(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editTableProperties(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::glyphGroup(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editGlyphGroup(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::convertText(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editConvertText(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::sortParagraphs(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editSortParagraphs(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::sortView(const render::FormatFields& fields) {
    const render::EditScope scope = wholeScope();
    return render::sortView(doc, pages, fields);
}

render::FormatFields DocSession::tableFormulaView() {
    const render::EditScope scope = wholeScope();
    return render::tableFormulaView(doc, pages);
}

render::FormatFields DocSession::glyphGroupView() {
    const render::EditScope scope = wholeScope();
    return render::glyphGroupView(doc, pages);
}

render::FormatFields DocSession::tablePropertiesView() {
    const render::EditScope scope = wholeScope();
    return render::tablePropertiesView(doc);
}

render::EditResult DocSession::sectionSettings(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editSectionSettings(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::sectionSettingsView() {
    const render::EditScope scope = wholeScope();
    return render::sectionSettingsView(doc);
}

render::EditResult DocSession::columns(const render::FormatFields& fields, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editColumns(doc, pages, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::columnView(const render::FormatFields& asked) {
    const render::EditScope scope = wholeScope();
    return render::columnView(doc, pages, asked);
}

render::EditResult DocSession::insertNote(bool endnote, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertNote(doc, pages, endnote, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertHyperlink(const std::string& text, const std::string& url, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertHyperlink(doc, pages, text, url, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::FormatFields DocSession::equationView() {
    const render::EditScope scope = wholeScope();
    return render::equationView(doc);
}

render::EditResult DocSession::insertEquation(const std::string& script, std::int32_t baseUnit, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertEquation(doc, pages, script, baseUnit, storedLineHeight, error);
    // 수식 글꼴(HancomEQN·HyhwpEQ·한컴바탕)은 문서에 수식이 처음 생길 때 받아야 한다.
    noteEdit(result, layout::equationFontFaces("HancomEQN"));
    return result;
}

render::EditResult DocSession::editEquation(const std::string& script, std::int32_t baseUnit, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editEquation(doc, pages, script, baseUnit, storedLineHeight, error);
    noteEdit(result, layout::equationFontFaces("HancomEQN"));
    return result;
}

render::EditResult DocSession::editEquationAt(int story, int paragraph, int item, const std::string& script,
                                              std::int32_t baseUnit, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result =
        render::editEquationAt(doc, pages, story, paragraph, item, script, baseUnit, storedLineHeight, error);
    noteEdit(result, layout::equationFontFaces("HancomEQN"));
    return result;
}

render::FormatFields DocSession::pageControlFields(render::PageControlKind kind) {
    const render::EditScope scope = wholeScope();
    return render::pageControlFields(doc, pages, kind);
}

render::EditResult DocSession::findNext(const std::string& text, bool matchCase, bool backward, bool* wrapped) {
    const render::EditScope scope = wholeScope();
    return render::selectNextText(doc, pages, text, matchCase, backward, wrapped);
}

render::EditResult DocSession::replaceNext(const std::string& text, const std::string& replacement, bool matchCase,
                                           bool* replaced, bool* found) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editReplaceText(doc, pages, text, replacement, matchCase, storedLineHeight);
    noteEdit(result);
    if (replaced) *replaced = result.documentChanged;
    // 막힌 글꼴이 남아 바꾸지 못했으면 셸이 글꼴 바꾸기를 묻도록 그 결과를 그대로 준다.
    if (result.needsFontSwap) {
        if (found) *found = false;
        return result;
    }
    const render::EditResult next = render::selectNextText(doc, pages, text, matchCase, false);
    if (found) *found = next.handled;
    if (!result.documentChanged) return next;
    // 바꾼 결과(다시 그릴 곳)에 다음 찾은 곳의 캐럿·선택을 얹는다.
    if (next.handled) {
        result.cursor = next.cursor;
        result.caret = next.caret;
    }
    return result;
}

render::EditResult DocSession::replaceAll(const std::string& text, const std::string& replacement, bool matchCase,
                                          int* count) {
    const render::EditScope scope = wholeScope();
    render::EditResult result =
        render::editReplaceAllText(doc, pages, text, replacement, matchCase, storedLineHeight, count);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::editTableAtCursor(render::TableEditCommand cmd, std::string* error) {
    const render::EditScope scope = wholeScope();
    if (!render::tableCommandAtCursor(doc, cmd)) {
        if (error) *error = "캐럿이 표 칸에 있지 않습니다.";
        return {};
    }
    render::EditResult result = render::editTable(doc, pages, cmd, storedLineHeight, error);
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
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editSwapLicenseFonts(doc, pages, storedLineHeight, picks);
    noteEdit(result, std::move(targets));
    return result;
}

render::EditResult DocSession::insertPicture(const Bytes& image, std::int32_t width, std::int32_t height) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertPicture(doc, pages, image, storedLineHeight, width, height);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::applyPageSetup(const render::FormatFields& fields, bool whole) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editApplyPageSetup(doc, pages, fields, whole, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::commitComposition() {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editCommitComposition(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertShape(int tool, int page, std::int32_t x0, std::int32_t y0, std::int32_t x1,
                                           std::int32_t y1) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertShape(doc, pages, tool, page, x0, y0, x1, y1, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertShapePoints(int tool, int page, const std::vector<model::ShapePoint>& points) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertShapePoints(doc, pages, tool, page, points, storedLineHeight);
    noteEdit(result);
    return result;
}

bool DocSession::selectObject(int story, int paragraph, int item) {
    const render::EditScope scope = wholeScope();
    render::clearObjectSelection();
    return render::selectAnchoredObject(doc, pages, story, paragraph, item);
}

render::FormatFields DocSession::objectPropertiesView(int story, int paragraph, int item) {
    return render::objectPropertiesView(doc, story, paragraph, item);
}

render::EditResult DocSession::objectProperties(int story, int paragraph, int item, const render::FormatFields& fields,
                                                std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editObjectProperties(doc, pages, story, paragraph, item, fields, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::deleteObject(int story, int paragraph, int item, std::string* error) {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editDeleteObject(doc, pages, story, paragraph, item, storedLineHeight, error);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::insertDrawnShape() {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editInsertDrawnShape(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::moveObject() {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editMoveObject(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::resizeObject() {
    const render::EditScope scope = wholeScope();
    render::EditResult result = render::editResizeObject(doc, pages, storedLineHeight);
    noteEdit(result);
    return result;
}

render::EditResult DocSession::resizeTableBorder() {
    const render::EditScope scope = wholeScope();
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

Bytes DocSession::exportPdf(int dpi, render::FrameCache& frames, int from, int to) {
    const render::EditScope scope = wholeScope();
    if (pages.empty()) return {};
    const int last = static_cast<int>(pages.size()) - 1;
    from = std::clamp(from, 0, last);
    to = to < 0 ? last : std::clamp(to, from, last);
    dpi = std::clamp(dpi, 72, 600);
    exporter::PdfWriter writer(doc.metadata.title);
    for (int index = from; index <= to; ++index) {
        render::Node& page = pages[static_cast<std::size_t>(index)];
        if (page.spansLater) render::ensurePageSpans(doc, page);
        const int pixelHeight = std::clamp(static_cast<int>(std::lround(page.height * dpi / 7200.0)), 32, 8000);
        render::PageBitmap bitmap = render::renderPageGpu(doc, pages, index, pixelHeight, frames);
        if (bitmap.rgba.empty()) bitmap = render::renderPageBitmap(doc, page, pixelHeight);
        exporter::PdfPage out;
        out.paperWidth = page.width;
        out.paperHeight = page.height;
        out.pixelWidth = bitmap.width;
        out.pixelHeight = bitmap.height;
        out.rgba = std::move(bitmap.rgba);
        if (!writer.addPage(out)) return {};
    }
    return writer.finish();
}

render::EditResult DocSession::undo() {
    const render::EditScope scope = wholeScope();
    return remeasureRestored(render::editUndo(doc, pages, storedLineHeight));
}

render::EditResult DocSession::redo() {
    const render::EditScope scope = wholeScope();
    return remeasureRestored(render::editRedo(doc, pages, storedLineHeight));
}

render::EditResult DocSession::remeasureRestored(render::EditResult result) {
    // 되돌리기 기록은 수식을 잰 그때의 크기를 품는다. 글꼴을 받는 중에 잰 수식이 돌아왔는데 이제 글꼴이 다 왔으면
    // 다시 재고 쪽을 처음부터 놓는다.
    if (!result.documentChanged || render::remeasurePendingEquations(doc) == 0) return result;
    relayoutAll();
    result.fullInvalidation = true;
    result.pageListChanged = true;
    return result;
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
    const render::EditScope scope = wholeScope();
    return render::beginUndoGroup();
}

void DocSession::endEditGroup() {
    const render::EditScope scope = wholeScope();
    render::endUndoGroup();
}

int DocSession::topEditGroup() {
    const render::EditScope scope = wholeScope();
    return render::topUndoGroup();
}

void DocSession::dropRedo() {
    const render::EditScope scope = wholeScope();
    render::clearRedoHistory();
}

}  // namespace chwp::app
