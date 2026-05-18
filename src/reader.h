#pragma once

#include <Arduino.h>
#include <vector>
#include "epub.h"

struct PageRange {
    int lineStart;
    int lineEnd;  // exclusive
};

struct Bookmark {
    int chapter;
    int page;
    String label;
};

struct ReaderLocation {
    int chapter = 0;
    int page = 0;
};

// ChapterCache removed — wrapped lines are now stored on SD card
// to avoid heap exhaustion on chapters with 500+ lines.

class BookReader {
public:
    bool openBook(const char* filepath);
    void closeBook();

    const String& getTitle() const { return _title; }
    const String& getFilepath() const { return _filepath; }
    int  getCurrentPage() const { return _currentPage; }
    int  getTotalPages() const { return _totalPages; }
    int  getCurrentChapter() const { return _currentChapter; }
    int  getTotalChapters() const { return _parser.getChapterCount(); }
    int  getTocCount() const { return _parser.getTocCount(); }

    bool nextPage();
    bool prevPage();
    void jumpToChapter(int chapter, bool rememberHistory = false);
    void restorePage(int page);  // Set page within current chapter (clamped)
    void restoreLocation(int chapter, int page);
    bool jumpToBookProgressPercent(int percent, bool rememberHistory = false);
    bool jumpToApproxBookPage(int page, bool rememberHistory = false);
    bool didChapterChange() const { return _chapterChanged; }  // true if last page turn crossed a chapter boundary

    const std::vector<String>& getPageLines() const;

    bool saveProgress();   // false on SD write failure (caller can surface a UI warning)
    void loadProgress();
    // Canonical "is a book open?" predicate. Prefer over the pre-existing
    // `getTitle().length() > 0` checks scattered across UI code, which are
    // fragile to a future "book opened but title metadata empty" state.
    bool isOpen() const { return _filepath.length() > 0; }
    // True iff the last loadChapter() call rendered the "[Could not load …]"
    // fallback. Reader / sleep paths skip saveProgress() in this state to
    // avoid trapping the user at a permanently-broken position (Bug Nest A).
    bool lastChapterLoadFailed() const { return _lastChapterFailed; }

    // Pre-cache removed — lines stored on SD card now

    // Bookmarks
    void addBookmark();
    void removeBookmark(int idx);
    bool jumpToBookmark(int idx);
    const std::vector<Bookmark>& getBookmarks() const { return _bookmarks; }
    bool isCurrentPageBookmarked() const;
    bool hasNavigationHistory() const { return !_history.empty(); }
    bool goBackInHistory();

    // Chapter title access (delegates to parser)
    String getChapterTitle(int index);
    String getTocLabel(int index);
    int getTocChapterIndex(int index);
    const std::vector<SpineItem>& getSpine() const { return _parser.getSpine(); }

    // For font size changes
    void recalculateLayout();

    // Last-read ordering
    uint32_t getLastReadOrder() const { return _lastReadOrder; }

    // Reading statistics
    uint32_t getTotalReadingTimeSec() const { return _totalReadingTimeSec; }
    uint32_t getTotalPagesRead() const { return _totalPagesRead; }
    void updateReadingTime();  // Call periodically to accumulate session time
    uint32_t getAveragePageTimeMs() const { return _avgPageTimeMs; }
    uint32_t getEstimatedChapterRemainingMs() const;
    uint32_t getEstimatedBookRemainingMs() const;
    int getApproxBookPercent() const;
    int getApproxBookPage() const;
    int getApproxBookPageCount() const;
    const String& getAuthor() const { return _author; }

    // Parser access for inline image rendering
    EpubParser& getParser() { return _parser; }

    // ─── KOReader sync (kosync) ────────────────────────────────────────
    // Tri-state result of applying remote progress.
    enum class ApplyResult { Ok, OutOfBounds, SaveFailed };

    // Get cached partial-MD5 of the current book. Computes on demand if empty.
    // Returns empty String if no book is open or hash computation fails.
    String getDocumentHash();

    // Last-sync timestamp accessors (epoch seconds; 0 = never synced).
    void setLastSyncTimestamp(uint32_t ts);
    uint32_t getLastSyncTimestamp() const { return _lastSyncTimestamp; }

    // Apply remote progress from a kosync sync. Validates bounds; does NOT
    // redraw (caller's responsibility). Persists via saveProgress() on success.
    ApplyResult applyRemoteProgress(int chapter, int page, float percentage);

    // WP-10 Plan H: full BookReader release for sync. Closes the EPUB
    // entirely (parser + cached state), freeing all DMA-cap RAM the
    // reader was holding. Must be paired with restoreAfterSync(), which
    // re-opens the book at the same chapter+page. Cost: ~600 ms re-open
    // delay when sync completes (one-time, rare).
    void releaseForSync();
    bool restoreAfterSync();

private:
    EpubParser _parser;
    String _title;
    String _author;
    String _filepath;
    int _currentChapter = 0;
    int _currentPage = 0;
    int _totalPages = 0;
    int _totalLines = 0;

    // Lines are stored on SD card to avoid heap exhaustion.
    // _lineOffsets[i] = byte offset in the cache file for line i.
    std::vector<uint32_t> _lineOffsets;  // ~2KB for 500 lines vs ~28KB for Strings
    std::vector<PageRange> _pages;
    std::vector<String> _currentPageLines;
    String _lineCachePath;  // SD path for current chapter's line cache

    int _linesPerPage = 1;
    int _maxLineWidth = 1;
    int _lineSpacing = 4;

    // Chapter caching disabled — lines live on SD card now, not RAM.
    // Pre-caching is no longer needed since SD reads are fast.

    // Bookmarks
    std::vector<Bookmark> _bookmarks;
    std::vector<ReaderLocation> _history;

    // Last-read order (monotonic counter)
    uint32_t _lastReadOrder = 0;

    // Auto-save counter: save every N page turns
    int _pageTurnsSinceSave = 0;
    static const int SAVE_EVERY_N_TURNS = 5;
    bool _chapterChanged = false;
    bool _lastChapterFailed = false;   // sticky from loadChapter's fallback path

    // Reading statistics
    uint32_t _totalReadingTimeSec = 0;
    uint32_t _totalPagesRead = 0;
    unsigned long _sessionStartMs = 0;
    unsigned long _lastTimeUpdateMs = 0;
    unsigned long _pageShownAtMs = 0;
    std::vector<uint32_t> _recentPageTimesMs;
    uint32_t _avgPageTimeMs = 0;
    int _currentChapterWordCount = 0;

    // KOReader sync state. Hash is 32-char lowercase hex; populated lazily
    // on first sync need (deferred to avoid SD reads on every book open).
    // _kosyncEpubSize tracks the file size at hash-compute time so cached
    // hashes can be invalidated when the EPUB file is replaced.
    String _documentHash;
    uint32_t _lastSyncTimestamp = 0;
    size_t _kosyncEpubSize = 0;

    // Plan H full-book-release state. Set in releaseForSync(), cleared in
    // restoreAfterSync(). Stored separately from _filepath/_currentChapter
    // because closeBook() zeros those out.
    bool   _releasedForSync = false;
    String _syncSavedFilepath;
    int    _syncSavedChapter = 0;
    int    _syncSavedPage = 0;

    void loadChapter(int chapter);
    void updatePageLines();
    void paginateLines();
    void wrapTextToFile(const String& text);  // write wrapped lines to SD cache
    String readLineFromCache(int lineIndex);   // read single line from SD cache
    void recordPageTurnTime();
    void notePageShown();
    void pushHistoryPoint();
};
