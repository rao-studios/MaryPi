#include "index/kind.h"

#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const char *const IMAGE_EXT[] = { "png", "jpg", "jpeg", "gif", "svg", "webp", "bmp", "tiff", "tif", "heic", "ico", "sketch", NULL };
static const char *const MUSIC_EXT[] = { "mp3", "m4a", "wav", "flac", "ogg", "aac", "aiff", "opus", NULL };
static const char *const VIDEO_EXT[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg", "ogv", "wmv", NULL };
static const char *const PDF_EXT[] = { "pdf", NULL };
static const char *const CODE_EXT[] = { "c", "h", "cpp", "hpp", "cc", "ts", "tsx", "js", "jsx", "mjs", "cjs", "json", "sh", "py", "swift", "css",
    "conf", "toml", "yaml", "yml", "rs", "go", "java", "rb", "sql", "mk", "cmake", "ini", "cfg", "xml", "html", "htm", NULL };
static const char *const TEXT_EXT[] = { "txt", "md", "markdown", "text", "log", "csv", "tsv", "rtf", "tex", "nfo", "readme", NULL };

const char *ix_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return "";
    return dot + 1;
}

static bool ext_in(const char *ext, const char *const *list) {
    for (size_t i = 0; list[i]; i++) {
        size_t n = strlen(list[i]);
        if (strlen(ext) != n) continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++) if (tolower((unsigned char)ext[k]) != list[i][k]) same = false;
        if (same) return true;
    }
    return false;
}

static bool valid_utf8(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) return false;
        if (i + len > n) return true;               /* a character cut by the 8 KB window */
        for (size_t k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static bool sniff_text(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    unsigned char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0) return false;
    if (n == 0) return true;
    if (memchr(buf, 0, (size_t)n)) return false;
    return valid_utf8(buf, (size_t)n);
}

const char *ix_kind_of(const char *path) {
    const char *ext = extension(ix_basename(path));
    if (ext_in(ext, IMAGE_EXT)) return "image";
    if (ext_in(ext, MUSIC_EXT)) return "audio";
    if (ext_in(ext, VIDEO_EXT)) return "video";
    if (ext_in(ext, PDF_EXT)) return "pdf";
    if (ext_in(ext, CODE_EXT)) return "code";
    if (ext_in(ext, TEXT_EXT)) return "text";
    return sniff_text(path) ? "text" : "document";
}

bool ix_is_text(const char *path) {
    const char *kind = ix_kind_of(path);
    return strcmp(kind, "text") == 0 || strcmp(kind, "code") == 0;
}
