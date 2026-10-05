#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include "util.h"

void format_file_size(char* buffer, size_t buffer_size, double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_index = 0;

    while (bytes >= 1024 && unit_index < 5) {
        bytes /= 1024;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, buffer_size, "%.0f %s", bytes, units[unit_index]);
    } else {
        snprintf(buffer, buffer_size, "%.2f %s", bytes, units[unit_index]);
    }
}

double now_seconds(void) {
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static double last_progress_time = 0;
static int last_progress_length = 0;

bool progress_due(void) {
    double now = now_seconds();
    if (now - last_progress_time < 0.1) return false;
    last_progress_time = now;
    return true;
}

void progress_print(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) return;
    if (length >= (int)sizeof(line)) length = sizeof(line) - 1;

    // clear leftovers from a longer previous line
    int padding = last_progress_length > length ? last_progress_length - length : 0;
    printf("\r%s%*s", line, padding, "");
    fflush(stdout);
    last_progress_length = length;
}

void progress_end(void) {
    printf("\n");
    last_progress_length = 0;
    last_progress_time = 0;
}

void utf8_truncate(char* out, size_t out_size, const char* in, size_t max_bytes) {
    size_t length = strlen(in);
    if (max_bytes > out_size - 1) max_bytes = out_size - 1;
    if (length > max_bytes) {
        length = max_bytes;
        // don't split a character
        while (length > 0 && ((unsigned char)in[length] & 0xC0) == 0x80) length--;
    }
    memcpy(out, in, length);
    out[length] = '\0';
}

wchar_t* utf8_to_wide(const char* s, int len) {
    if (len == 0) {
        wchar_t* empty = malloc(sizeof(wchar_t));
        if (empty) empty[0] = L'\0';
        return empty;
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, NULL, 0);
    if (wide_len <= 0) return NULL;

    // count already includes the terminator
    int alloc_len = (len == -1) ? wide_len : wide_len + 1;
    wchar_t* out = malloc(alloc_len * sizeof(wchar_t));
    if (!out) return NULL;

    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, out, wide_len);
    out[alloc_len - 1] = L'\0';
    return out;
}

char* wide_to_utf8(const wchar_t* s, int len, int* out_len) {
    int utf8_len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, len, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) return NULL;

    int alloc_len = (len == -1) ? utf8_len : utf8_len + 1;
    char* out = malloc(alloc_len);
    if (!out) return NULL;

    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, len, out, utf8_len, NULL, NULL);
    out[alloc_len - 1] = '\0';
    if (out_len) *out_len = alloc_len - 1;
    return out;
}

wchar_t* make_long_path(const wchar_t* path) {
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) return NULL;

    wchar_t* full = malloc(needed * sizeof(wchar_t));
    if (!full) return NULL;
    GetFullPathNameW(path, needed, full, NULL);

    size_t full_len = wcslen(full);
    while (full_len > 0 && (full[full_len - 1] == L'\\' || full[full_len - 1] == L'/')) {
        full[--full_len] = L'\0';
    }

    const wchar_t* prefix;
    const wchar_t* rest;
    if (wcsncmp(full, L"\\\\?\\", 4) == 0) {
        prefix = L"";
        rest = full;
    } else if (wcsncmp(full, L"\\\\", 2) == 0) {
        prefix = L"\\\\?\\UNC\\"; // \\server\share -> \\?\UNC\server\share
        rest = full + 2;
    } else {
        prefix = L"\\\\?\\";
        rest = full;
    }

    size_t out_len = wcslen(prefix) + wcslen(rest) + 1;
    wchar_t* out = malloc(out_len * sizeof(wchar_t));
    if (out) {
        wcscpy(out, prefix);
        wcscat(out, rest);
    }
    free(full);
    return out;
}

wchar_t* join_path(const wchar_t* base, const char* rel_utf8, int rel_len) {
    wchar_t* rel = utf8_to_wide(rel_utf8, rel_len);
    if (!rel) return NULL;

    size_t base_len = wcslen(base);
    size_t rel_wide_len = wcslen(rel);
    wchar_t* out = malloc((base_len + 1 + rel_wide_len + 1) * sizeof(wchar_t));
    if (out) {
        memcpy(out, base, base_len * sizeof(wchar_t));
        size_t pos = base_len;
        if (rel_wide_len > 0) {
            out[pos++] = L'\\';
            for (size_t i = 0; i < rel_wide_len; i++) {
                out[pos++] = (rel[i] == L'/') ? L'\\' : rel[i];
            }
        }
        out[pos] = L'\0';
    }
    free(rel);
    return out;
}
