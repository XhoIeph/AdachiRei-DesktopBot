#include <Arduino.h>
#include <SPIFFS.h>

extern "C" {
#include <berry.h>

// ===== File I/O — 对接 SPIFFS =====
void* be_fopen(const char* path, const char* mode) {
    const char* m = (mode && mode[0] == 'r') ? FILE_READ : FILE_WRITE;
    File f = SPIFFS.open(path, m);
    if (!f) return nullptr;
    File* fp = new File(f);
    return fp;
}

int be_fclose(void* f) {
    if (!f) return -1;
    File* fp = (File*)f;
    fp->close();
    delete fp;
    return 0;
}

size_t be_fread(void* f, void* buf, size_t count) {
    if (!f) return 0;
    return ((File*)f)->read((uint8_t*)buf, count);
}

size_t be_fwrite(void* f, const void* buf, size_t count) {
    if (!f) return 0;
    return ((File*)f)->write((const uint8_t*)buf, count);
}

int be_fflush(void* f) {
    if (!f) return -1;
    ((File*)f)->flush();
    return 0;
}

int be_fseek(void* f, long offset, int whence) {
    if (!f) return -1;
    SeekMode m = SeekSet;
    if (whence == 1) m = SeekCur;
    else if (whence == 2) m = SeekEnd;
    return ((File*)f)->seek(offset, m) ? 0 : -1;
}

size_t be_fsize(void* f) {
    if (!f) return 0;
    return ((File*)f)->size();
}

long be_ftell(void* f) {
    if (!f) return -1;
    return ((File*)f)->position();
}

char* be_fgets(void* f, char* buf, int size) {
    if (!f || !buf || size < 2) return nullptr;
    File* fp = (File*)f;
    int i = 0;
    while (i < size - 1 && fp->available()) {
        char c = fp->read();
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : nullptr;
}

// ===== Console I/O — 对接 Serial =====
void be_writebuffer(const char* buf, size_t length) {
    Serial.write((const uint8_t*)buf, length);
}

char* be_readstring(char* buffer, size_t size) {
    if (!buffer || size < 2) return nullptr;
    size_t i = 0;
    while (i < size - 1) {
        while (!Serial.available()) { delay(1); }
        char c = Serial.read();
        buffer[i++] = c;
        Serial.write(c);
        if (c == '\n') break;
    }
    buffer[i] = '\0';
    return buffer;
}

} // extern "C"
