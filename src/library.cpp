#include "library.h"
#include "config.h"
#include "epub.h"
#include "settings.h"
#include "storage_utils.h"
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <esp_task_wdt.h>

static bool _mounted = false;
static const char* CACHE_PATH = "/books/.library_cache.json";
static const char* CACHE_TMP_PATH = "/books/.library_cache.tmp";

static bool ensure_dir(const char* path, const char* label) {
    if (SD.exists(path)) {
        Serial.printf("Storage: %s ready: %s\n", label, path);
        return true;
    }

    Serial.printf("Storage: %s missing, creating: %s\n", label, path);
    if (SD.mkdir(path)) {
        Serial.printf("Storage: %s created: %s\n", label, path);
        return true;
    }

    Serial.printf("Storage: failed to create %s: %s\n", label, path);
    return false;
}

bool library_init() {
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI)) {
        Serial.println("SD card mount failed");
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD card: %llu MB\n", cardSize);

    bool storageReady = true;
    storageReady &= ensure_dir(BOOKS_DIR, "books library");
    storageReady &= ensure_dir(PROGRESS_DIR, "reading progress cache");
    storageReady &= ensure_dir(LINE_CACHE_DIR, "line cache");
    storageReady &= ensure_dir(SLEEP_IMAGES_DIR, "sleep images");

    if (!storageReady) {
        Serial.println("Storage: one or more app folders are unavailable; continuing with existing fallbacks");
    }

    // Boot-time GC of orphan .tmp files in /books and /books/.progress.
    // These accumulate when a write was interrupted (power loss, card pull
    // mid-stream). Removing them keeps the directory tidy and prevents
    // stale partial uploads from being mistaken for real EPUBs.
    auto cleanupTmps = [](const char* dirPath) {
        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
        File entry;
        int removed = 0;
        while ((entry = dir.openNextFile())) {
            String name = entry.name();
            entry.close();
            int slash = name.lastIndexOf('/');
            String basename = (slash >= 0) ? name.substring(slash + 1) : name;
            if (basename.endsWith(".tmp")) {
                String full = String(dirPath);
                if (!full.endsWith("/")) full += "/";
                full += basename;
                if (SD.remove(full.c_str())) {
                    Serial.printf("Storage GC: removed orphan %s\n", full.c_str());
                    removed++;
                }
            }
        }
        dir.close();
        if (removed > 0) Serial.printf("Storage GC: %d orphan(s) in %s\n", removed, dirPath);
    };
    cleanupTmps(BOOKS_DIR);
    cleanupTmps(PROGRESS_DIR);

    _mounted = true;
    return true;
}

// ─── Metadata cache ─────────────────────────────────────────────────

struct CacheEntry {
    String path;
    size_t size;
    String title;
    String author;
    int chapters;
    bool hasCover;
    String coverPath;
};

static std::vector<CacheEntry> loadCache() {
    std::vector<CacheEntry> cache;
    File f = SD.open(CACHE_PATH, FILE_READ);
    if (!f) return cache;

    // Bound the parse cost BEFORE allocating the 48 KB JsonDocument: a
    // crafted .library_cache.json on the SD card could otherwise burn
    // ~50 KB of heap on every boot just to fail and trigger a full
    // re-scan (effective boot DoS).
    static const size_t MAX_CACHE_BYTES = 48 * 1024;
    if ((size_t)f.size() > MAX_CACHE_BYTES) {
        Serial.printf("Library cache: oversized (%u bytes) — discarding, re-scan\n",
                      (unsigned)f.size());
        f.close();
        return cache;
    }
    // Heap guard — DynamicJsonDocument allocates from DRAM. If we're called
    // mid-scan with PSRAM/DRAM under pressure, fall back rather than burning
    // 50 KB on a doomed allocation that silently constructs as size-0.
    if (ESP.getFreeHeap() < 60 * 1024) {
        Serial.printf("Library cache: low heap (%u), skipping load → re-scan\n",
                      (unsigned)ESP.getFreeHeap());
        f.close();
        return cache;
    }

    DynamicJsonDocument doc(MAX_CACHE_BYTES);
    if (doc.capacity() == 0) {
        Serial.println("Library cache: doc alloc failed → re-scan");
        f.close();
        return cache;
    }
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        // Surface the cause — without this, a corrupt cache file silently
        // forces a full re-scan every boot with no diagnosis trail.
        Serial.printf("Library cache: parse error %s — re-scanning\n", err.c_str());
        return cache;
    }

    // Accept either a bare array (legacy format) or a versioned envelope
    // {"cacheVersion":N,"entries":[...]}. Legacy → silently upgrade on next
    // save; future-versioned → discard and re-scan to avoid stale fields.
    static const int CACHE_VERSION = 1;
    JsonArray arr;
    if (doc.is<JsonObject>()) {
        int v = doc["cacheVersion"] | 0;
        if (v > CACHE_VERSION) {
            Serial.printf("Library cache: future version %d (we know %d) — re-scan\n",
                          v, CACHE_VERSION);
            return cache;
        }
        arr = doc["entries"].as<JsonArray>();
    } else {
        arr = doc.as<JsonArray>();
    }
    for (JsonObject obj : arr) {
        CacheEntry e;
        e.path     = obj["path"].as<String>();
        e.size     = obj["size"] | 0;
        e.title    = obj["title"].as<String>();
        e.author   = obj["author"].as<String>();
        e.chapters = obj["chapters"] | 0;
        e.hasCover = obj["hasCover"] | false;
        e.coverPath = obj["coverPath"].as<String>();
        cache.push_back(e);
    }
    Serial.printf("Cache: loaded %d entries\n", cache.size());
    return cache;
}

static void saveCache(const std::vector<CacheEntry>& cache) {
    // Size the doc for the actual entry count: each entry needs roughly
    // ~250 bytes (path + title + author + cover + numbers + JSON overhead).
    // Worst case: MAX_BOOKS=100 entries × 320 B = 32 KB. Bumping to 48 KB
    // keeps a safety margin and prevents the silent ArduinoJson truncation
    // that occurred with the previous fixed 8 KB doc.
    // 400 B/entry covers worst-case path 64 + title 64 + author 48 +
    // coverPath 64 + numbers + JSON overhead with margin. The previous
    // 320 was ~9% short of worst-case.
    // Add envelope overhead (cacheVersion + entries key) on top of the
    // per-entry budget.
    const size_t docSize = 256 + JSON_ARRAY_SIZE(cache.size()) +
                           cache.size() * (JSON_OBJECT_SIZE(7) + 400);
    const size_t MIN_DOC = 4096;
    DynamicJsonDocument doc(docSize > MIN_DOC ? docSize : MIN_DOC);
    doc["cacheVersion"] = 1;
    JsonArray arr = doc.createNestedArray("entries");
    for (const auto& e : cache) {
        JsonObject obj = arr.createNestedObject();
        obj["path"]     = e.path;
        obj["size"]     = e.size;
        obj["title"]    = e.title;
        obj["author"]   = e.author;
        obj["chapters"] = e.chapters;
        obj["hasCover"] = e.hasCover;
        obj["coverPath"] = e.coverPath;
    }
    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(CACHE_PATH, CACHE_TMP_PATH, json)) {
        Serial.printf("Cache: saved %d entries\n", cache.size());
    } else {
        Serial.println("Cache: atomic save failed");
    }
}

static const CacheEntry* findCacheEntry(const std::vector<CacheEntry>& cache,
                                         const String& path, size_t fileSize) {
    for (const auto& e : cache) {
        if (e.path == path && e.size == fileSize) return &e;
    }
    return nullptr;
}

// ─── Directory scanning ─────────────────────────────────────────────

static void scanDir(File& dir, const char* path,
                    std::vector<BookInfo>& books,
                    const std::vector<CacheEntry>& cache,
                    std::vector<CacheEntry>& newCache) {
    File entry;
    while ((entry = dir.openNextFile()) && books.size() < MAX_BOOKS) {
        const char* rawName = entry.name();
        if (rawName && rawName[0] == '.') {
            entry.close();
            continue;
        }
        if (entry.isDirectory()) {
            String subpath = String(path) + "/" + entry.name();
            scanDir(entry, subpath.c_str(), books, cache, newCache);
        } else {
            String name = String(entry.name());
            name.toLowerCase();
            if (name.endsWith(".epub")) {
                BookInfo book;
                // Normalize: SD paths must start with '/'. Without this, a
                // path "books/foo.epub" silently fails SD.open later and
                // shows as a missing book with no diagnostic.
                String fp = String(path) + "/" + entry.name();
                if (!fp.startsWith("/")) fp = "/" + fp;
                book.filepath = fp;
                book.fileSize = entry.size();

                // Check cache first
                const CacheEntry* cached = findCacheEntry(cache, book.filepath, book.fileSize);
                if (cached) {
                    book.title = cached->title;
                    book.author = cached->author;
                    book.totalChapters = cached->chapters;
                    book.hasCover = cached->hasCover;
                    book.coverPath = cached->coverPath;
                    Serial.printf("Cache hit: %s\n", book.title.c_str());
                } else {
                    // Cache miss — open EPUB to extract metadata. parseCentralDirectory()
                    // can do up to a 4 MB malloc + DEFLATE inflate per book; on a card
                    // with many cache misses this can starve the watchdog if we don't
                    // feed it between files.
                    esp_task_wdt_reset();
                    EpubParser parser;
                    if (parser.open(book.filepath.c_str())) {
                        book.title = parser.getTitle();
                        book.author = parser.getAuthor();
                        book.totalChapters = parser.getChapterCount();
                        book.hasCover = parser.hasCoverImage();
                        book.coverPath = parser.getCoverImagePath();
                        parser.close();
                    } else {
                        book.title = entry.name();
                        int dot = book.title.lastIndexOf('.');
                        if (dot > 0) book.title = book.title.substring(0, dot);
                    }
                    Serial.printf("Cache miss: %s -> %s\n", book.title.c_str(), book.filepath.c_str());
                }

                // Add to new cache
                CacheEntry ce;
                ce.path = book.filepath;
                ce.size = book.fileSize;
                ce.title = book.title;
                ce.author = book.author;
                ce.chapters = book.totalChapters;
                ce.hasCover = book.hasCover;
                ce.coverPath = book.coverPath;
                newCache.push_back(ce);

                books.push_back(book);
            }
        }
        entry.close();
    }
}

std::vector<BookInfo> library_scan() {
    std::vector<BookInfo> books;
    if (!_mounted) return books;

    unsigned long startMs = millis();

    // Load existing cache
    std::vector<CacheEntry> cache = loadCache();
    std::vector<CacheEntry> newCache;

    // Scan /books/ directory
    File booksDir = SD.open(BOOKS_DIR);
    if (booksDir) {
        scanDir(booksDir, BOOKS_DIR, books, cache, newCache);
        booksDir.close();
    }

    // Also scan root for epub files
    File root = SD.open("/");
    if (root) {
        File entry;
        while ((entry = root.openNextFile()) && books.size() < MAX_BOOKS) {
            const char* rawName = entry.name();
            if (rawName && rawName[0] == '.') {
                entry.close();
                continue;
            }
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                name.toLowerCase();
                if (name.endsWith(".epub")) {
                    String filepath = String("/") + entry.name();
                    bool dupe = false;
                    for (const auto& b : books) {
                        if (b.filepath == filepath) { dupe = true; break; }
                    }
                    if (!dupe) {
                        BookInfo book;
                        book.filepath = filepath;
                        book.fileSize = entry.size();

                        const CacheEntry* cached = findCacheEntry(cache, book.filepath, book.fileSize);
                        if (cached) {
                            book.title = cached->title;
                            book.author = cached->author;
                            book.totalChapters = cached->chapters;
                            book.hasCover = cached->hasCover;
                            book.coverPath = cached->coverPath;
                        } else {
                            EpubParser parser;
                            if (parser.open(book.filepath.c_str())) {
                                book.title = parser.getTitle();
                                book.author = parser.getAuthor();
                                book.totalChapters = parser.getChapterCount();
                                book.hasCover = parser.hasCoverImage();
                                book.coverPath = parser.getCoverImagePath();
                                parser.close();
                            } else {
                                book.title = entry.name();
                                int dot = book.title.lastIndexOf('.');
                                if (dot > 0) book.title = book.title.substring(0, dot);
                            }
                        }

                        CacheEntry ce;
                        ce.path = book.filepath;
                        ce.size = book.fileSize;
                        ce.title = book.title;
                        ce.author = book.author;
                        ce.chapters = book.totalChapters;
                        ce.hasCover = book.hasCover;
                        ce.coverPath = book.coverPath;
                        newCache.push_back(ce);

                        books.push_back(book);
                    }
                }
            }
            entry.close();
        }
        root.close();
    }

    // Save updated cache (stale entries automatically pruned since we only
    // add entries for files that still exist on disk)
    saveCache(newCache);

    // Load saved reading progress for each book
    for (auto& book : books) {
        String name = book.filepath;
        int ls = name.lastIndexOf('/');
        if (ls >= 0) name = name.substring(ls + 1);
        String progPath = String(PROGRESS_DIR) + "/" + name + ".json";

        File pf = SD.open(progPath, FILE_READ);
        if (pf) {
            StaticJsonDocument<512> doc;
            if (deserializeJson(doc, pf) == DeserializationError::Ok) {
                book.progressChapter = doc["chapter"] | 0;
                book.progressPage = doc["page"] | 0;
                book.totalChapters = doc["total_chapters"] | book.totalChapters;
                book.hasProgress = true;
                book.lastReadOrder = doc["last_read"] | (uint32_t)0;
            }
            pf.close();
        }
    }

    unsigned long elapsed = millis() - startMs;
    library_sort(books);
    Serial.printf("Library: %d books found in %lums\n", books.size(), elapsed);
    return books;
}

static String normalized_sort_key(const String& input) {
    String key = input;
    key.trim();
    key.toLowerCase();
    return key;
}

void library_sort(std::vector<BookInfo>& books) {
    const uint8_t sortOrder = settings_get().librarySortOrder;
    std::stable_sort(books.begin(), books.end(), [sortOrder](const BookInfo& a, const BookInfo& b) {
        switch (sortOrder) {
            case 1: {
                String aa = normalized_sort_key(a.author.length() > 0 ? a.author : a.title);
                String bb = normalized_sort_key(b.author.length() > 0 ? b.author : b.title);
                if (aa == bb) return normalized_sort_key(a.title) < normalized_sort_key(b.title);
                return aa < bb;
            }
            case 2:
                if (a.lastReadOrder != b.lastReadOrder) return a.lastReadOrder > b.lastReadOrder;
                return normalized_sort_key(a.title) < normalized_sort_key(b.title);
            case 3:
                if (a.fileSize != b.fileSize) return a.fileSize > b.fileSize;
                return normalized_sort_key(a.title) < normalized_sort_key(b.title);
            case 0:
            default:
                return normalized_sort_key(a.title) < normalized_sort_key(b.title);
        }
    });
}

std::vector<int> library_filter(const std::vector<BookInfo>& books,
                                 LibraryFilter filter) {
    std::vector<int> indices;
    // Treat any unknown enum value as "show everything" — UI casts a raw int
    // back to LibraryFilter (main.cpp), so an out-of-range value would
    // otherwise silently produce an empty library with no recovery path.
    bool unknown = (filter != FILTER_ALL && filter != FILTER_NEW &&
                    filter != FILTER_READING && filter != FILTER_FINISHED);
    if (unknown) filter = FILTER_ALL;

    for (int i = 0; i < (int)books.size(); i++) {
        const BookInfo& b = books[i];
        switch (filter) {
            case FILTER_ALL:
                indices.push_back(i);
                break;
            case FILTER_NEW:
                if (!b.hasProgress) indices.push_back(i);
                break;
            case FILTER_READING:
                if (b.hasProgress && b.totalChapters > 0 &&
                    b.progressChapter < b.totalChapters - 1) {
                    indices.push_back(i);
                }
                break;
            case FILTER_FINISHED:
                if (b.hasProgress && b.totalChapters > 0 &&
                    b.progressChapter >= b.totalChapters - 1) {
                    indices.push_back(i);
                }
                break;
        }
    }
    return indices;
}

int library_find_current_book(const std::vector<BookInfo>& books) {
    // Find the book with the highest lastReadOrder (most recently read)
    int bestIdx = -1;
    uint32_t bestOrder = 0;

    for (int i = 0; i < (int)books.size(); i++) {
        if (books[i].hasProgress) {
            if (books[i].lastReadOrder > bestOrder || bestIdx < 0) {
                bestOrder = books[i].lastReadOrder;
                bestIdx = i;
            }
        }
    }
    return bestIdx;
}
