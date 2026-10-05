#ifndef MANIFEST_H_
#define MANIFEST_H_
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wchar.h>

#define HASH_SIZE 32 // sha256
#define MANIFEST_VERSION 2

// hashes from the last scan, so unchanged files aren't read again
#define CACHE_FILE_NAME ".filesync-cache"
// temporary files, never synced
#define TEMP_SUFFIX ".filesync-tmp"

// single file
typedef struct ManifestFile {
    uint64_t size;
    uint64_t modified_time; // FILETIME
    uint8_t  checksum[HASH_SIZE];
    uint16_t path_length;
    char*    path;          // relative, UTF-8, '/' separated
} ManifestFile;

// entire manifest
typedef struct Manifest {
    uint32_t      file_count;
    uint32_t      capacity; // for dynamic array sizing
    ManifestFile* records;
} Manifest;

// create empty manifest
Manifest* create_manifest(void);

void free_manifest(Manifest* m);

// append file to manifest
ManifestFile* add_record(Manifest* m, uint64_t size, uint64_t mod_time, const char* path, uint16_t path_length, const uint8_t* checksum);

// sort by path, needed for find_record
void sort_manifest(Manifest* m);

ManifestFile* find_record(const Manifest* m, const char* path);

// scan a folder into a sorted manifest. unchanged files reuse hashes from cache, which can be NULL
Manifest* scan_directory(const wchar_t* base_dir, const Manifest* cache);

// NULL if missing or invalid
Manifest* load_cache(const wchar_t* base_dir);

bool save_cache(const wchar_t* base_dir, const Manifest* m);

// file format (little endian)
//  - "SYNC"     - 4 bytes
//  - version    - 1 byte  (uint8)
//  - file count - 4 bytes (uint32)
//  - every file - 50 + length of path
//      - uint64 size
//      - uint64 modified time
//      - 32 uint8 checksum
//      - uint16 length of path
//      - path bytes (UTF-8, no null terminator)

// encode manifest to bytes
uint8_t* encode_manifest(const Manifest* m, size_t* out_size);

// decode manifest from bytes, NULL if invalid
Manifest* decode_manifest(const uint8_t* buffer, size_t size);

#endif
