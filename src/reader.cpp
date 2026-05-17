#include "reader.h"
#include "display.h"
#include "config.h"
#include "settings.h"
#include "storage_utils.h"
#include "inline_image.h"
#include "debug_trace.h"
#include "kosync_hash.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

static Preferences _prefs;

static const int MAX_WRAP_TEXT_CHARS = 120000;

static int countWords(const String& text) {
    int words = 0;
    bool inWord = false;
    for (int i = 0; i < (int)text.length(); i++) {
        char c = text[i];
        bool isWordChar = isalnum((unsigned char)c) || c == '\'';
        if (isWordChar && !inWord) {
            words++;
            inWord = true;
        } else if (!isWordChar) {
            inWord = false;
        }
    }
    return words;
}

bool BookReader::openBook(const char* filepath) {
    debug_trace_mark("reader:openBook:start", filepath ? filepath : "");
    closeBook();
    debug_trace_mark("reader:openBook:after_close");
    if (!_parser.open(filepath)) {
        debug_trace_mark("reader:openBook:parser_open_failed", filepath ? filepath : "");
        return false;
    }
    debug_trace_mark("reader:openBook:parser_open_ok");

    _filepath = String(filepath);
    _title = _parser.getTitle();
    _author = _parser.getAuthor();

    // Reset kosync state; loadProgress() below will repopulate from JSON if
    // the saved progress file is v2. Hash itself is computed lazily on first
    // getDocumentHash() call to avoid extra SD reads at open time.
    _documentHash = "";
    _lastSyncTimestamp = 0;
    _kosyncEpubSize = 0;

    recalculateLayout();
    debug_trace_mark("reader:openBook:after_layout");
    _pageTurnsSinceSave = 0;
    _sessionStartMs = millis();
    _lastTimeUpdateMs = _sessionStartMs;
    loadProgress();
    debug_trace_mark("reader:openBook:after_loadProgress", String(_currentChapter) + ":" + String(_currentPage));
    loadChapter(_currentChapter);
    debug_trace_mark("reader:openBook:after_loadChapter", String(_currentChapter));

    // Update last-read order (monotonic counter in NVS). If NVS is
    // unavailable the counter silently stays at 0 and breaks the library's
    // "most recently read" sort — log so the failure is visible.
    if (_prefs.begin("ereader", false)) {
        _lastReadOrder = _prefs.getUInt("readOrder", 0) + 1;
        _prefs.putUInt("readOrder", _lastReadOrder);
        _prefs.end();
    } else {
        Serial.println("Reader: NVS open failed — last-read order not persisted");
    }

    return true;
}

void BookReader::recalculateLayout() {
    const Settings& s = settings_get();

    // Switch the actual font in the display module
    int level = s.fontSizeLevel;
    if (level < 0) level = 0;
    if (level >= FONT_SIZE_LEVEL_COUNT) level = FONT_SIZE_LEVEL_COUNT - 1;
    display_set_font(level, s.fontFamily);

    // Margins follow font size, but leading is user-adjustable.
    uint8_t spacingLevel = s.lineSpacingLevel;
    if (spacingLevel >= LINE_SPACING_LEVEL_COUNT) spacingLevel = 2;
    int marginX = FONT_MARGIN_X_VALUES[level];

    int bodyFontH = display_font_height();
    int lineHeight = (bodyFontH * LINE_SPACING_PCT[spacingLevel] + 99) / 100;
    if (lineHeight < bodyFontH) lineHeight = bodyFontH;
    int lineSpacing = lineHeight - bodyFontH;
    // The footer already reserves its own area, so only a tiny extra buffer is
    // needed above it now that draw-time spacing matches pagination.
    int usableHeight = display_height() - HEADER_HEIGHT - FOOTER_HEIGHT - MARGIN_Y * 2 - 4;
    int usableWidth  = display_width() - marginX * 2;

    _lineSpacing = lineSpacing;
    _linesPerPage = usableHeight / (bodyFontH + lineSpacing);
    if (_linesPerPage < 1) _linesPerPage = 1;
    _maxLineWidth = usableWidth;

    Serial.printf("Layout: fontLevel=%d, family=%u, fontH=%d, lineSpacing=%d, linesPerPage=%d, maxWidth=%d\n",
                  s.fontSizeLevel, (unsigned)s.fontFamily, bodyFontH, lineSpacing, _linesPerPage, _maxLineWidth);
}

void BookReader::closeBook() {
    updateReadingTime();
    saveProgress();
    _parser.close();
    _title = "";
    _author = "";
    _filepath = "";
    _currentChapter = 0;
    _currentPage = 0;
    _totalPages = 0;
    _totalLines = 0;
    _lineOffsets.clear();
    _pages.clear();
    _currentPageLines.clear();
    _bookmarks.clear();
    _history.clear();
    _lineCachePath = "";
    _totalReadingTimeSec = 0;
    _totalPagesRead = 0;
    _sessionStartMs = 0;
    _lastTimeUpdateMs = 0;
    _pageShownAtMs = 0;
    _recentPageTimesMs.clear();
    _avgPageTimeMs = 0;
    _currentChapterWordCount = 0;
    _documentHash = "";
    _lastSyncTimestamp = 0;
    _kosyncEpubSize = 0;
}

void BookReader::loadChapter(int chapter) {
    debug_trace_mark("reader:loadChapter:start", String(chapter));
    if (chapter < 0 || chapter >= _parser.getChapterCount()) {
        debug_trace_mark("reader:loadChapter:invalid", String(chapter));
        return;
    }

    _currentChapter = chapter;
    _lineOffsets.clear();
    _pages.clear();
    _currentPageLines.clear();
    _totalLines = 0;

    Serial.printf("Chapter %d: loading (heap: %d, psram: %d)\n",
                  chapter, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
    yield();

    debug_trace_mark("reader:loadChapter:before_getText", String(chapter));
    String text = _parser.getChapterText(chapter);
    debug_trace_mark("reader:loadChapter:after_getText", String(text.length()));
    Serial.printf("Chapter %d: text %d chars (heap: %d)\n",
                  chapter, (int)text.length(), (int)ESP.getFreeHeap());

    if (text.length() == 0) {
        Serial.printf("Chapter %d: empty text\n", chapter);
        text = "[Could not load chapter text]\n\nThe chapter may be too large.";
        _lastChapterFailed = true;   // Bug Nest A: don't trap progress here
    } else {
        _lastChapterFailed = false;
    }

    _currentChapterWordCount = countWords(text);

    debug_trace_mark("reader:loadChapter:before_wrap", String(text.length()));
    wrapTextToFile(text);
    debug_trace_mark("reader:loadChapter:after_wrap", String(_totalLines));
    text = String();  // free source text

    Serial.printf("Chapter %d: %d lines on SD (heap: %d)\n",
                  chapter, _totalLines, (int)ESP.getFreeHeap());

    debug_trace_mark("reader:loadChapter:before_paginate");
    paginateLines();
    _totalPages = _pages.size();
    if (_totalPages == 0) _totalPages = 1;
    if (_currentPage >= _totalPages) _currentPage = 0;
    debug_trace_mark("reader:loadChapter:after_paginate", String(_totalPages));
    updatePageLines();
    debug_trace_mark("reader:loadChapter:after_updatePageLines", String(_currentPage));
    notePageShown();
}

void BookReader::recordPageTurnTime() {
    unsigned long now = millis();
    if (_pageShownAtMs == 0 || now <= _pageShownAtMs) return;

    uint32_t elapsedMs = now - _pageShownAtMs;
    if (elapsedMs < 1000UL) elapsedMs = 1000UL;
    if (elapsedMs > 15UL * 60UL * 1000UL) {
        _pageShownAtMs = now;
        return;
    }

    _recentPageTimesMs.push_back(elapsedMs);
    if (_recentPageTimesMs.size() > 10) {
        _recentPageTimesMs.erase(_recentPageTimesMs.begin());
    }

    uint64_t totalMs = 0;
    for (uint32_t sample : _recentPageTimesMs) totalMs += sample;
    if (!_recentPageTimesMs.empty()) {
        _avgPageTimeMs = (uint32_t)(totalMs / _recentPageTimesMs.size());
    }
    _pageShownAtMs = now;
}

void BookReader::notePageShown() {
    _pageShownAtMs = millis();
}

void BookReader::pushHistoryPoint() {
    if (_title.length() == 0) return;
    ReaderLocation loc;
    loc.chapter = _currentChapter;
    loc.page = _currentPage;
    if (!_history.empty()) {
        const ReaderLocation& last = _history.back();
        if (last.chapter == loc.chapter && last.page == loc.page) return;
    }
    _history.push_back(loc);
    if (_history.size() > 10) {
        _history.erase(_history.begin());
    }
}

void BookReader::updatePageLines() {
    debug_trace_mark("reader:updatePageLines:start", String(_currentPage));
    _currentPageLines.clear();
    if (_currentPage < (int)_pages.size()) {
        const PageRange& pr = _pages[_currentPage];
        Serial.printf("updatePageLines: page %d, lines %d-%d from %s\n",
                      _currentPage, pr.lineStart, pr.lineEnd, _lineCachePath.c_str());

        // Read all lines for this page in a single file open
        File f = SD.open(_lineCachePath, FILE_READ);
        if (f) {
            for (int i = pr.lineStart; i < pr.lineEnd && i < _totalLines; i++) {
                if (i < (int)_lineOffsets.size()) {
                    f.seek(_lineOffsets[i]);
                    String line = f.readStringUntil('\n');
                    _currentPageLines.push_back(line);
                }
            }
            f.close();
            Serial.printf("updatePageLines: read %d lines (heap: %d)\n",
                          (int)_currentPageLines.size(), (int)ESP.getFreeHeap());
            debug_trace_mark("reader:updatePageLines:read_ok", String(_currentPageLines.size()));
        } else {
            Serial.printf("updatePageLines: FAILED to open %s\n", _lineCachePath.c_str());
            debug_trace_mark("reader:updatePageLines:file_open_failed", _lineCachePath);
        }
    }

    if (_currentPageLines.empty()) {
        debug_trace_mark("reader:updatePageLines:empty_result", String(_currentPage));
        // Distinguish "SD card couldn't supply text" (cache file missing or
        // unreadable) from "this chapter actually has no content" — the
        // user gets one of two clear messages instead of the generic
        // placeholder that hid Bug Nest B.
        if (!SD.exists(_lineCachePath.c_str())) {
            _currentPageLines.push_back("[Storage error — SD card may be full or removed]");
        } else {
            _currentPageLines.push_back("[No readable text in this section]");
        }
    }
}

String BookReader::readLineFromCache(int lineIndex) {
    if (lineIndex < 0 || lineIndex >= _totalLines) return "";
    if (lineIndex >= (int)_lineOffsets.size()) return "";

    File f = SD.open(_lineCachePath, FILE_READ);
    if (!f) {
        Serial.printf("readLineFromCache: cannot open %s\n", _lineCachePath.c_str());
        return "[SD read error]";
    }

    f.seek(_lineOffsets[lineIndex]);
    String line = f.readStringUntil('\n');
    f.close();
    return line;
}

void BookReader::paginateLines() {
    _pages.clear();
    if (_totalLines <= 0) {
        _totalPages = 1;
        return;
    }

    File f = SD.open(_lineCachePath, FILE_READ);
    if (!f) {
        Serial.printf("paginateLines: cannot open %s, falling back to fixed pagination\n",
                      _lineCachePath.c_str());
        int i = 0;
        while (i < _totalLines) {
            PageRange pr;
            pr.lineStart = i;
            pr.lineEnd = min(i + _linesPerPage, _totalLines);
            _pages.push_back(pr);
            i = pr.lineEnd;
        }
        _totalPages = _pages.size();
        if (_totalPages == 0) _totalPages = 1;
        return;
    }

    int pageStart = 0;
    int usedLines = 0;
    int lineIndex = 0;
    unsigned long lastYieldMs = millis();

    while (lineIndex < _totalLines) {
        if (millis() - lastYieldMs >= 25) { yield(); lastYieldMs = millis(); }

        String line;
        if (lineIndex < (int)_lineOffsets.size()) {
            f.seek(_lineOffsets[lineIndex]);
            line = f.readStringUntil('\n');
        }

        int consumes = 1;
        String imgPath;
        int imgW = 0, imgH = 0, imgLines = 0;
        bool isImage = inline_image_parse_enriched(line, imgPath, imgW, imgH, imgLines);
        if (isImage) {
            consumes = max(1, imgLines);
        } else if (inline_image_is_continuation(line)) {
            consumes = 1;
        }

        if (usedLines > 0 && usedLines + consumes > _linesPerPage) {
            PageRange pr;
            pr.lineStart = pageStart;
            pr.lineEnd = lineIndex;
            _pages.push_back(pr);
            pageStart = lineIndex;
            usedLines = 0;
            continue;
        }

        usedLines += consumes;
        lineIndex += isImage ? consumes : 1;
    }

    f.close();

    if (pageStart < _totalLines) {
        PageRange pr;
        pr.lineStart = pageStart;
        pr.lineEnd = _totalLines;
        _pages.push_back(pr);
    }
    _totalPages = _pages.size();
    if (_totalPages == 0) _totalPages = 1;
    Serial.printf("Paginated: %d pages from %d lines (%d lines/page, image-aware)\n",
                  _totalPages, _totalLines, _linesPerPage);
}

// Write a single line to the cache file, tracking its offset.
// Returns false if any write failed (SD full / card pulled mid-stream) so
// callers can abort wrapping rather than building a corrupt cache.
static bool writeLine(File& f, std::vector<uint32_t>& offsets, const String& line) {
    uint32_t pos = (uint32_t)f.position();
    size_t want = line.length();
    size_t got1 = f.print(line);
    size_t got2 = f.print('\n');
    if (got1 != want || got2 != 1) {
        Serial.printf("Line cache: short write (got %u/%u + %u/1) — aborting\n",
                      (unsigned)got1, (unsigned)want, (unsigned)got2);
        return false;
    }
    offsets.push_back(pos);
    return true;
}

void BookReader::wrapTextToFile(const String& text) {
    // Ensure cache directory exists. Without checking the mkdir result the
    // subsequent SD.open() failure was indistinguishable from "permissions"
    // or "card removed" — log explicitly which step failed.
    if (!SD.exists(LINE_CACHE_DIR)) {
        if (!SD.mkdir(LINE_CACHE_DIR)) {
            Serial.printf("ERROR: mkdir %s failed (card writable?)\n", LINE_CACHE_DIR);
            return;
        }
    }

    _lineCachePath = String(LINE_CACHE_DIR) + "/ch" + String(_currentChapter) + ".txt";
    _lineOffsets.clear();
    _totalLines = 0;

    File f = SD.open(_lineCachePath, FILE_WRITE);
    if (!f) {
        Serial.printf("ERROR: cannot open line cache for writing: %s\n", _lineCachePath.c_str());
        return;
    }

    String workingText = text;
    if ((int)workingText.length() > MAX_WRAP_TEXT_CHARS) {
        workingText = workingText.substring(0, MAX_WRAP_TEXT_CHARS);
        workingText += "\n\n[Section truncated for device stability]";
    }

    int textLen = workingText.length();
    if (textLen == 0) { f.close(); return; }

    // ESP32 Arduino's File::print never sets the underlying Print error flag,
    // so File::getWriteError() is permanently 0. We instead route every
    // line write through `wl()` which checks writeLine's per-call return
    // value and trips a local `writeFailed` flag the loop bails on.
    bool writeFailed = false;
    auto wl = [&](const String& s) {
        if (writeFailed) return;
        if (!writeLine(f, _lineOffsets, s)) {
            writeFailed = true;
        } else {
            _totalLines++;
        }
    };

    int start = 0;
    unsigned long lastYieldMs = millis();
    while (start < textLen && !writeFailed) {
        unsigned long nowMs = millis();
        if (nowMs - lastYieldMs >= 50) { yield(); lastYieldMs = nowMs; }

        int nl = workingText.indexOf('\n', start);
        if (nl < 0) nl = textLen;

        String paragraph = workingText.substring(start, nl);
        paragraph.trim();

        if (paragraph.length() == 0) {
            wl("");
            start = nl + 1;
            continue;
        }

        // Handle image markers
        if (paragraph[0] == IMG_MARKER_BYTE) {
            String imgPath;
            if (inline_image_parse_raw(paragraph, imgPath)) {
                bool probed = false;
                if ((int)ESP.getFreeHeap() > 30000) {
                    int lineH = display_font_height() + _lineSpacing;
                    int maxImgH = _linesPerPage * lineH;
                    InlineImageInfo info;
                    if (inline_image_probe(_parser, _filepath, imgPath, _maxLineWidth, maxImgH, info)) {
                        info.linesConsumed = max(1, (info.displayH + lineH - 1) / lineH);
                        wl(inline_image_build_marker(
                            info.assetPath, info.displayW, info.displayH, info.linesConsumed));
                        for (int j = 1; j < info.linesConsumed; j++) {
                            wl(IMG_CONT_MARKER);
                        }
                        probed = true;
                    }
                }
                if (!probed) {
                    wl("[Image]");
                }
                start = nl + 1;
                continue;
            }
        }

        // Word-wrap the paragraph
        String currentLine;
        int currentWidth = 0;
        int spaceWidth = display_text_width(" ");
        int indentWidth = spaceWidth * 3;
        bool firstLine = true;

        int wStart = 0;
        int paraLen = paragraph.length();
        while (wStart < paraLen) {
            while (wStart < paraLen && paragraph[wStart] == ' ') wStart++;
            if (wStart >= paraLen) break;

            int wEnd = wStart;
            while (wEnd < paraLen && paragraph[wEnd] != ' ') wEnd++;

            String word = paragraph.substring(wStart, wEnd);
            int wordWidth = display_text_width(word.c_str());

            if (currentLine.length() == 0) {
                if (firstLine) { currentLine = "   "; currentWidth = indentWidth; }
                if (currentWidth + wordWidth > _maxLineWidth) {
                    String partial = currentLine;
                    int pw = currentWidth;
                    for (int ci = 0; ci < (int)word.length(); ci++) {
                        char ch[2] = { word[ci], 0 };
                        int cw = display_text_width(ch);
                        if (pw + cw > _maxLineWidth && partial.length() > 0) {
                            wl(partial);
                            firstLine = false;
                            partial = "";
                            pw = 0;
                        }
                        partial += word[ci];
                        pw += cw;
                    }
                    if (partial.length() > 0) { currentLine = partial; currentWidth = pw; }
                } else {
                    currentLine += word;
                    currentWidth += wordWidth;
                    firstLine = false;
                }
            } else if (currentWidth + spaceWidth + wordWidth <= _maxLineWidth) {
                currentLine += " " + word;
                currentWidth += spaceWidth + wordWidth;
            } else {
                wl(currentLine);
                firstLine = false;
                currentLine = word;
                currentWidth = wordWidth;
            }
            wStart = wEnd;
        }

        if (currentLine.length() > 0) {
            wl(currentLine);
        }

        start = nl + 1;
    }

    // Flush before close so the tracked f.position() values in _lineOffsets
    // line up with what's actually on disk; the SD lib buffers writes and
    // position() can otherwise drift by the buffered amount.
    f.flush();
    f.close();
    if (writeFailed) {
        // Any short write during the loop poisons every later offset. Discard
        // the partial cache so the reader doesn't render garbage on top of a
        // truncated file. paginateLines will then take the empty-chapter
        // branch and the user sees the SD-error placeholder rather than
        // corrupt text (Bug Nest B distinct error message).
        Serial.printf("Line cache: short writes during chapter %d — discarding\n",
                      _currentChapter);
        SD.remove(_lineCachePath);
        _lineOffsets.clear();
        _totalLines = 0;
    }
}

bool BookReader::nextPage() {
    recordPageTurnTime();
    bool chapterChange = false;
    if (_currentPage + 1 < _totalPages) {
        _currentPage++;
    } else if (_currentChapter + 1 < _parser.getChapterCount()) {
        _currentPage = 0;
        chapterChange = true;
        Serial.printf("nextPage: advancing to chapter %d (heap free: %d, PSRAM free: %d)\n",
                      _currentChapter + 1, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
        loadChapter(_currentChapter + 1);
    } else {
        return false;
    }

    _totalPagesRead++;
    _chapterChanged = chapterChange;
    updatePageLines();
    notePageShown();
    if (++_pageTurnsSinceSave >= SAVE_EVERY_N_TURNS) {
        updateReadingTime();
        saveProgress();
        _pageTurnsSinceSave = 0;
    }
    return true;
}

bool BookReader::prevPage() {
    recordPageTurnTime();
    bool chapterChange = false;
    if (_currentPage > 0) {
        _currentPage--;
    } else if (_currentChapter > 0) {
        chapterChange = true;
        Serial.printf("prevPage: going back to chapter %d (heap free: %d, PSRAM free: %d)\n",
                      _currentChapter - 1, (int)ESP.getFreeHeap(), (int)ESP.getFreePsram());
        loadChapter(_currentChapter - 1);
        _currentPage = _totalPages - 1;
    } else {
        return false;
    }

    _totalPagesRead++;
    _chapterChanged = chapterChange;
    updatePageLines();
    notePageShown();
    if (++_pageTurnsSinceSave >= SAVE_EVERY_N_TURNS) {
        updateReadingTime();
        saveProgress();
        _pageTurnsSinceSave = 0;
    }
    return true;
}

void BookReader::updateReadingTime() {
    unsigned long now = millis();
    if (_lastTimeUpdateMs > 0 && now > _lastTimeUpdateMs) {
        unsigned long elapsedMs = now - _lastTimeUpdateMs;
        // Cap at 5 minutes per interval to avoid counting idle/sleep time
        if (elapsedMs > 300000UL) elapsedMs = 300000UL;
        _totalReadingTimeSec += elapsedMs / 1000;
    }
    _lastTimeUpdateMs = now;
}

void BookReader::jumpToChapter(int chapter, bool rememberHistory) {
    if (chapter < 0 || chapter >= _parser.getChapterCount()) return;
    if (rememberHistory) pushHistoryPoint();
    recordPageTurnTime();
    _currentPage = 0;
    _currentPageLines.clear();
    loadChapter(chapter);
}

void BookReader::restorePage(int page) {
    if (page < 0) page = 0;
    if (page >= _totalPages) page = _totalPages - 1;
    _currentPage = page;
    updatePageLines();
    notePageShown();
}

void BookReader::restoreLocation(int chapter, int page) {
    recordPageTurnTime();
    if (chapter < 0) chapter = 0;
    if (chapter >= _parser.getChapterCount()) chapter = _parser.getChapterCount() - 1;
    _currentPage = 0;
    loadChapter(chapter);
    restorePage(page);
}

bool BookReader::jumpToBookProgressPercent(int percent, bool rememberHistory) {
    if (_parser.getChapterCount() <= 0) return false;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (rememberHistory) pushHistoryPoint();

    int chapterCount = _parser.getChapterCount();
    int targetChapter = (percent >= 100) ? (chapterCount - 1) : ((percent * chapterCount) / 100);
    if (targetChapter >= chapterCount) targetChapter = chapterCount - 1;
    int withinPct = (percent >= 100) ? 100 : ((percent * chapterCount) % 100);

    restoreLocation(targetChapter, 0);
    int targetPage = (_totalPages > 1) ? ((withinPct * (_totalPages - 1)) / 100) : 0;
    restorePage(targetPage);
    return true;
}

bool BookReader::jumpToApproxBookPage(int page, bool rememberHistory) {
    int totalApproxPages = getApproxBookPageCount();
    if (totalApproxPages <= 0) return false;
    if (page < 1) page = 1;
    if (page > totalApproxPages) page = totalApproxPages;
    int percent = (page - 1) * 100 / max(1, totalApproxPages - 1);
    return jumpToBookProgressPercent(percent, rememberHistory);
}

const std::vector<String>& BookReader::getPageLines() const {
    return _currentPageLines;
}

String BookReader::getChapterTitle(int index) {
    return _parser.getChapterTitle(index);
}

String BookReader::getTocLabel(int index) {
    return _parser.getTocLabel(index);
}

int BookReader::getTocChapterIndex(int index) {
    return _parser.getTocChapterIndex(index);
}

// ─── Bookmarks ──────────────────────────────────────────────────────

void BookReader::addBookmark() {
    // Don't add duplicate
    for (const auto& bm : _bookmarks) {
        if (bm.chapter == _currentChapter && bm.page == _currentPage) return;
    }
    Bookmark bm;
    bm.chapter = _currentChapter;
    bm.page = _currentPage;
    String chapterTitle = _parser.getChapterTitle(_currentChapter);
    chapterTitle.trim();
    if (chapterTitle.length() > 24) {
        chapterTitle = chapterTitle.substring(0, 21) + "...";
    }
    bm.label = String("Ch ") + String(_currentChapter + 1) + ", Pg " + String(_currentPage + 1);
    if (chapterTitle.length() > 0) {
        bm.label += " - ";
        bm.label += chapterTitle;
    }
    _bookmarks.push_back(bm);
    saveProgress();
    Serial.printf("Bookmark added: %s\n", bm.label.c_str());
}

void BookReader::removeBookmark(int idx) {
    if (idx >= 0 && idx < (int)_bookmarks.size()) {
        _bookmarks.erase(_bookmarks.begin() + idx);
        saveProgress();
    }
}

bool BookReader::jumpToBookmark(int idx) {
    if (idx < 0 || idx >= (int)_bookmarks.size()) return false;
    const Bookmark& bm = _bookmarks[idx];
    pushHistoryPoint();
    restoreLocation(bm.chapter, bm.page);
    return true;
}

bool BookReader::isCurrentPageBookmarked() const {
    for (const auto& bm : _bookmarks) {
        if (bm.chapter == _currentChapter && bm.page == _currentPage) return true;
    }
    return false;
}

bool BookReader::goBackInHistory() {
    if (_history.empty()) return false;
    ReaderLocation loc = _history.back();
    _history.pop_back();
    restoreLocation(loc.chapter, loc.page);
    return true;
}

uint32_t BookReader::getEstimatedChapterRemainingMs() const {
    if (_avgPageTimeMs == 0) return 0;
    int remainingPages = _totalPages - _currentPage - 1;
    if (remainingPages < 0) remainingPages = 0;
    return remainingPages * _avgPageTimeMs;
}

uint32_t BookReader::getEstimatedBookRemainingMs() const {
    if (_avgPageTimeMs == 0) return 0;
    int remainingCurrent = _totalPages - _currentPage - 1;
    if (remainingCurrent < 0) remainingCurrent = 0;
    int remainingChapters = _parser.getChapterCount() - _currentChapter - 1;
    if (remainingChapters < 0) remainingChapters = 0;
    int approxPagesPerChapter = max(1, _totalPages);
    uint32_t remainingPages = remainingCurrent + remainingChapters * approxPagesPerChapter;
    return remainingPages * _avgPageTimeMs;
}

int BookReader::getApproxBookPercent() const {
    int total = getApproxBookPageCount();
    if (total <= 1) return 0;
    return (getApproxBookPage() * 100) / (total - 1);
}

int BookReader::getApproxBookPage() const {
    return _currentChapter * max(1, _totalPages) + _currentPage + 1;
}

int BookReader::getApproxBookPageCount() const {
    int chapters = _parser.getChapterCount();
    if (chapters <= 0) return max(1, _totalPages);
    return max(1, chapters * max(1, _totalPages));
}

// ─── Progress save/load ────────────────────────────────────────────

static String progressPath(const String& filepath) {
    String name = filepath;
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    return String(PROGRESS_DIR) + "/" + name + ".json";
}

static String progressTmpPath(const String& filepath) {
    String name = filepath;
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    return String(PROGRESS_DIR) + "/." + name + ".tmp";
}

bool BookReader::saveProgress() {
    if (_filepath.length() == 0) return false;

    String path = progressPath(_filepath);
    String tmpPath = progressTmpPath(_filepath);

    int chapterCount = _parser.getChapterCount();
    // Size dynamically: base fields (~512 B) + per-bookmark overhead.
    // Each bookmark is roughly chapter+page numbers + label (max 64 chars)
    // + JSON object overhead → budget ~200 B/bookmark with margin.
    size_t docSize = 1024 + (size_t)_bookmarks.size() * 200;
    if (docSize < 2048) docSize = 2048;
    DynamicJsonDocument doc(docSize);
    doc["chapter"] = _currentChapter;
    doc["page"] = _currentPage;
    doc["total_chapters"] = chapterCount;
    doc["last_read"] = _lastReadOrder;

    // Reading statistics
    doc["reading_time_sec"] = _totalReadingTimeSec;
    doc["pages_read"] = _totalPagesRead;

    // Cache invalidation: record EPUB file size so stale cache is discarded
    // if the file is replaced. Schema v2 (WP-11) adds kosync hash + last-sync
    // timestamp; older v1 files still load via loadProgress() with defaults.
    doc["cache_version"] = 2;
    File ef = SD.open(_filepath.c_str(), FILE_READ);
    if (ef) {
        doc["epub_size"] = (uint32_t)ef.size();
        ef.close();
    }

    // kosync fields. Empty hash / zero timestamp are valid defaults — they
    // simply indicate the hash hasn't been computed and the book has never
    // been synced yet.
    doc["kosync_document_hash"] = _documentHash;
    doc["kosync_last_sync"] = _lastSyncTimestamp;

    // Chapter title cache persistence removed to save heap — titles are
    // loaded lazily from ZIP when the TOC screen is opened.

    // Save bookmarks
    if (!_bookmarks.empty()) {
        JsonArray bArr = doc.createNestedArray("bookmarks");
        for (const auto& bm : _bookmarks) {
            JsonObject obj = bArr.createNestedObject();
            obj["chapter"] = bm.chapter;
            obj["page"] = bm.page;
            obj["label"] = bm.label;
        }
    }

    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(path, tmpPath, json)) {
        Serial.printf("Progress saved atomically: ch%d pg%d -> %s\n",
                      _currentChapter, _currentPage, path.c_str());
        return true;
    }
    Serial.printf("Progress save failed: %s\n", path.c_str());
    return false;
}

void BookReader::loadProgress() {
    if (_filepath.length() == 0) return;

    String path = progressPath(_filepath);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        _currentChapter = 0;
        _currentPage = 0;
        return;
    }

    int chapterCount = _parser.getChapterCount();
    size_t docSize = 2048;
    DynamicJsonDocument doc(docSize);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        // Corrupt progress file silently lost the user's position; surface
        // the cause in the log AND remove the bad file so the next save
        // doesn't re-encounter it.
        Serial.printf("Progress: parse error (%s) on %s — resetting position\n",
                      err.c_str(), path.c_str());
        SD.remove(path);
        _currentChapter = 0;
        _currentPage = 0;
        return;
    }

    _currentChapter = doc["chapter"] | 0;
    _currentPage = doc["page"] | 0;
    _lastReadOrder = doc["last_read"] | (uint32_t)0;
    _totalReadingTimeSec = doc["reading_time_sec"] | (uint32_t)0;
    _totalPagesRead = doc["pages_read"] | (uint32_t)0;

    // kosync fields (schema v2). Missing keys on v1 files yield safe defaults;
    // the next saveProgress() bumps the on-disk schema to v2 automatically.
    _documentHash = doc["kosync_document_hash"] | String("");
    _lastSyncTimestamp = doc["kosync_last_sync"] | (uint32_t)0;
    // Restore cached EPUB size so kosync_hash_is_valid() can decide if the
    // cached hash still matches the current file.
    _kosyncEpubSize = (size_t)(doc["epub_size"] | (uint32_t)0);

    if (_currentChapter >= chapterCount) {
        _currentChapter = 0;
        _currentPage = 0;
    }

    // Chapter title cache restore disabled — the 72+ String objects consumed
    // ~60KB of heap on books with many spine entries, leaving <11KB for chapter
    // loading which caused OOM crashes.  Titles are loaded lazily from ZIP when
    // the TOC screen is opened instead.
    Serial.printf("Progress loaded (heap: %d)\n", (int)ESP.getFreeHeap());

    // Load bookmarks
    _bookmarks.clear();
    if (doc.containsKey("bookmarks")) {
        JsonArray bArr = doc["bookmarks"].as<JsonArray>();
        for (JsonObject obj : bArr) {
            Bookmark bm;
            bm.chapter = obj["chapter"] | 0;
            bm.page = obj["page"] | 0;
            bm.label = obj["label"].as<String>();
            if (bm.label.length() == 0) {
                bm.label = String("Ch") + String(bm.chapter + 1) + " Pg" + String(bm.page + 1);
            }
            _bookmarks.push_back(bm);
        }
    }

    Serial.printf("Progress loaded: ch%d pg%d, %d bookmarks\n",
                  _currentChapter, _currentPage, _bookmarks.size());
}

// ─── KOReader sync (kosync) ─────────────────────────────────────────

String BookReader::getDocumentHash() {
    if (_filepath.length() == 0) return String();

    // Reuse cached hash only if the EPUB file size still matches what we
    // recorded when the hash was computed. A size mismatch means the file
    // was replaced; the cached digest is stale.
    if (_documentHash.length() == 32 &&
        kosync_hash_is_valid(_filepath, _kosyncEpubSize)) {
        return _documentHash;
    }

    String hash = kosync_compute_document_hash(_filepath);
    if (hash.length() != 32) {
        // Compute failed (file unreadable, partial read, etc). Leave any
        // existing cached hash in place but do not return a half-good value.
        Serial.println("Reader: kosync hash compute failed");
        return String();
    }

    _documentHash = hash;
    // Record file size at compute-time so future kosync_hash_is_valid()
    // checks can detect replaced EPUBs.
    File ef = SD.open(_filepath.c_str(), FILE_READ);
    if (ef) {
        _kosyncEpubSize = (size_t)ef.size();
        ef.close();
    }

    // Persist the freshly-computed hash so the next session doesn't have
    // to recompute. Failure here is non-fatal — the hash remains in RAM
    // for the duration of this session.
    saveProgress();
    return _documentHash;
}

void BookReader::setLastSyncTimestamp(uint32_t ts) {
    _lastSyncTimestamp = ts;
}

BookReader::ApplyResult BookReader::applyRemoteProgress(int chapter, int page,
                                                       float percentage) {
    if (_filepath.length() == 0) return ApplyResult::OutOfBounds;

    // Upfront, side-effect-free validation: chapter index and percentage.
    int chapterCount = _parser.getChapterCount();
    if (chapter < 0 || chapter >= chapterCount) return ApplyResult::OutOfBounds;
    if (page < 0) return ApplyResult::OutOfBounds;
    if (percentage < 0.0f || percentage > 1.0f) return ApplyResult::OutOfBounds;

    // Page count is only known after a chapter is loaded (lines are
    // paginated lazily on SD). Snapshot the current location so we can
    // revert if the remote page falls outside the target chapter's range.
    int prevChapter = _currentChapter;
    int prevPage = _currentPage;

    if (chapter != _currentChapter) {
        loadChapter(chapter);
    }

    if (page >= _totalPages) {
        // Revert: restore the previous chapter/page so the caller sees no
        // visible state change for an out-of-bounds remote update.
        if (chapter != prevChapter) {
            loadChapter(prevChapter);
        }
        if (prevPage >= 0 && prevPage < _totalPages) {
            _currentPage = prevPage;
        }
        return ApplyResult::OutOfBounds;
    }

    _currentPage = page;
    // Intentionally do not call updatePageLines() or notePageShown(): the
    // caller owns the redraw decision (per WP-11 contract).

    if (!saveProgress()) return ApplyResult::SaveFailed;
    return ApplyResult::Ok;
}

void BookReader::releaseParserForSync() {
    _parser.release_for_sync();
}

bool BookReader::restoreParserAfterSync() {
    return _parser.restore_after_sync();
}
