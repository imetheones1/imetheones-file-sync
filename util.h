#ifndef UTIL_H_
#define UTIL_H_
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wchar.h>

void format_file_size(char* buffer, size_t buffer_size, double bytes);

double now_seconds(void);

// progress lines overwrite themselves. the console is slow, so only print when progress_due()
bool progress_due(void);
void progress_print(const char* format, ...);
// print a message on its own line, ending the progress line first if one is showing
void progress_message(const char* format, ...);
void progress_end(void);

bool ends_with(const char* s, size_t length, const char* suffix);

// combine the high and low halves Windows splits sizes and times into
uint64_t make_uint64(uint32_t high, uint32_t low);

// truncate without splitting a character
void utf8_truncate(char* out, size_t out_size, const char* in, size_t max_bytes);

// results are malloc'd, NULL if invalid. len -1 for null terminated
wchar_t* utf8_to_wide(const char* s, int len);
char* wide_to_utf8(const wchar_t* s, int len, int* out_len);

// absolute path with the \\?\ prefix so long paths work
wchar_t* make_long_path(const wchar_t* path);

// base\rel, rel is UTF-8 with '/' separators
wchar_t* join_path(const wchar_t* base, const char* rel_utf8, int rel_len);

#endif
