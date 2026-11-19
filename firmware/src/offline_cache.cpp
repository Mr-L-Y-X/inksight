#include "offline_cache.h"
#include <LittleFS.h>

static const char *CACHE_FILE = "/cache.bmp";
static bool fsReady = false;

bool cacheInit() {
    if (!LittleFS.begin(true)) {  // true = format on failure
        Serial.println("LittleFS mount failed");
        return false;
    }
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    Serial.printf("LittleFS: %u/%u bytes used\n", (unsigned)used, (unsigned)total);
    // A failed write leaves orphan blocks behind; repeated failures fill the
    // 128KB spiffs partition until nothing can be stored. The cache is
    // disposable, so reclaim the whole thing once it gets tight.
    if (total == 0 || used * 4 > total * 3) {
        Serial.println("LittleFS low on space -> formatting cache partition");
        LittleFS.format();
        if (!LittleFS.begin(true)) {
            Serial.println("LittleFS remount failed after format");
            return false;
        }
        Serial.printf("LittleFS after format: %u/%u bytes used\n",
                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    }
    fsReady = true;
    Serial.println("LittleFS ready");
    return true;
}

bool cacheSave(const uint8_t *buf, int len) {
    if (!fsReady) return false;
    // LittleFS is copy-on-write: overwriting a 100KB frame inside the 128KB
    // spiffs partition needs room for both the old and the new copy, which is
    // why a plain "w" open ended with "No more free space". Drop the old file
    // first so the write gets the whole partition.
    if (LittleFS.exists(CACHE_FILE)) {
        LittleFS.remove(CACHE_FILE);
    }
    File f = LittleFS.open(CACHE_FILE, "w");
    if (!f) {
        Serial.println("Cache write failed: cannot open file");
        return false;
    }
    size_t written = f.write(buf, len);
    f.close();
    Serial.printf("Cache saved: %d/%d bytes\n", (int)written, len);
    return (int)written == len;
}

bool cacheLoad(uint8_t *buf, int len) {
    if (!fsReady) return false;
    File f = LittleFS.open(CACHE_FILE, "r");
    if (!f) {
        Serial.println("No cache file found");
        return false;
    }
    size_t read = f.readBytes((char *)buf, len);
    f.close();
    Serial.printf("Cache loaded: %d bytes\n", read);
    return (int)read == len;
}

bool cacheExists() {
    if (!fsReady) return false;
    return LittleFS.exists(CACHE_FILE);
}
