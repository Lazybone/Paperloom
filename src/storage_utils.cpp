#include "storage_utils.h"

#include <cstdio>
#include <unistd.h>      // fsync, close
#include <fcntl.h>       // open, O_RDONLY

static String vfs_path(const String& path) {
    if (path.startsWith("/sd")) return path;
    return String("/sd") + path;
}

// Force the parent directory's metadata (FAT entries) to disk, so the
// rename is durable across power loss. POSIX-style; relies on the ESP-IDF
// VFS layer exposing fsync() on FAT mounts.
static void fsync_dir(const String& vfsPath) {
    int slash = vfsPath.lastIndexOf('/');
    String dir = (slash > 0) ? vfsPath.substring(0, slash) : String("/sd");
    int dfd = open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

bool storage_write_text_atomic(const String& finalPath, const String& tmpPath, const String& contents) {
    String tmpVfs = vfs_path(tmpPath);
    String finalVfs = vfs_path(finalPath);

    FILE* f = fopen(tmpVfs.c_str(), "wb");
    if (!f) {
        return false;
    }

    size_t len = contents.length();
    size_t written = fwrite(contents.c_str(), 1, len, f);
    // Force user-space buffer + kernel-side cache to the SD controller
    // before the rename — without this the rename can complete while the
    // tmp file's data sectors are still in the card's volatile cache, and
    // a power loss leaves a zero-sized "atomic" file.
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (written != len) {
        remove(tmpVfs.c_str());
        return false;
    }

    remove(finalVfs.c_str());
    if (rename(tmpVfs.c_str(), finalVfs.c_str()) == 0) {
        // Sync the parent directory so the new dirent itself is durable.
        fsync_dir(finalVfs);
        return true;
    }

    // Fallback for filesystems that reject rename semantics unexpectedly.
    FILE* src = fopen(tmpVfs.c_str(), "rb");
    if (!src) {
        remove(tmpVfs.c_str());
        return false;
    }
    FILE* dst = fopen(finalVfs.c_str(), "wb");
    if (!dst) {
        fclose(src);
        remove(tmpVfs.c_str());
        return false;
    }

    char buf[512];
    bool ok = true;
    while (!feof(src)) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) {
            ok = false;
            break;
        }
        if (ferror(src)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        fflush(dst);
        fsync(fileno(dst));
    }
    fclose(src);
    fclose(dst);
    remove(tmpVfs.c_str());
    if (ok) fsync_dir(finalVfs);
    return ok;
}
