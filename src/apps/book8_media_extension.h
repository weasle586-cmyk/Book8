#ifndef BOOK8_MEDIA_EXTENSION_H
#define BOOK8_MEDIA_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>

#define BOOK8_MEDIA_URL_CAPACITY 256u
#define BOOK8_MEDIA_LABEL_CAPACITY 80u

enum book8_media_kind {
    BOOK8_MEDIA_OTHER,
    BOOK8_MEDIA_WAV,
    BOOK8_MEDIA_UNSUPPORTED_AUDIO,
    BOOK8_MEDIA_VIDEO
};

struct book8_media_source {
    char url[BOOK8_MEDIA_URL_CAPACITY];
    char label[BOOK8_MEDIA_LABEL_CAPACITY];
    enum book8_media_kind kind;
};

typedef bool (*book8_media_visitor)(void *context,
    const struct book8_media_source *source);

enum book8_media_kind book8_media_classify_url(const char *url);
uint32_t book8_media_scan_html(const char *html, uint32_t size,
    book8_media_visitor visitor, void *context);

#endif
