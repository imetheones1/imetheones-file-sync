#ifndef MANIFEST_H_
#define MANIFEST_H_
#include <stdint.h>


// single file
typedef struct ManifestFile {
    uint64_t size;
    uint64_t modified_time;
    uint8_t  checksum[32];
    uint16_t path_length;
    char*    path;
} ManifestFile;

// entire manifest
typedef struct {
    uint8_t     magic[4];   // 'S', 'Y', 'N', 'C'
    uint8_t     version;    // 0x01, in case protocol is ever changed
    uint32_t    file_count;
    uint32_t    capacity;   // for dynamic array sizing
    ManifestFile* records;
} Manifest;

// hash a file using sha256.c
void calculate_file_hash(const char* full_path, uint8_t* out_hash);

// create empty manifest
Manifest* create_manifest();

// append file to manifest
void add_record(Manifest* m, uint64_t size, uint64_t mod_time, const char* path, const uint8_t* checksum);

// scan a directory into a manifest. base_dir is the directory to scan. call rel_path with "", since its used internally for recursion
void scan_directory(Manifest* m, const char* base_dir, const char* rel_path);

// file format
//  - "SYNC"     - 4 bytes (4 uint8 )
//  - version    - 1 byte  (1 uint8 )
//  - file count - 4 bytes (1 uint32)
//  - every file - 50 + length of path
//      - uint64 size
//      - uint64 modified time
//      - 32 uint8 checksum
//      - uint16 length of path
//      - multiple char for path


// encode manifest to bytes
uint8_t* encode_manifest(Manifest* m, size_t* out_size);

// decode manifest from bytes
Manifest* decode_manifest(uint8_t* buffer);

#endif