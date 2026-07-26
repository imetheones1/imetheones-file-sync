#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "manifest.h"
#include "sha256.h"

void calculate_file_hash(const char* full_path, uint8_t* out_hash) {
    printf("hashing file \"%s\"\n", full_path);
    FILE* file = fopen(full_path, "rb");
    if (!file) {
        memset(out_hash, 0, SHA256_BLOCK_SIZE);
        return;
    }

    SHA256_CTX ctx;
    sha256_init(&ctx);

    BYTE buffer[4096];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sha256_update(&ctx, buffer, bytes_read);
    }

    sha256_final(&ctx, (BYTE*)out_hash);
    fclose(file);
}

Manifest* create_manifest() {
    Manifest* m = (Manifest*)malloc(sizeof(Manifest));
    m->magic[0] = 'S'; m->magic[1] = 'Y'; m->magic[2] = 'N'; m->magic[3] = 'C';
    m->version = 1;
    m->file_count = 0;
    m->capacity = 256; 
    m->records = (ManifestFile*)malloc(m->capacity * sizeof(ManifestFile));
    return m;
}

void add_record(Manifest* m, uint64_t size, uint64_t mod_time, const char* path, const uint8_t* checksum) {
    if (m->file_count >= m->capacity) {
        m->capacity *= 2;
        m->records = (ManifestFile*)realloc(m->records, m->capacity * sizeof(ManifestFile));
    }
    
    ManifestFile* record = &m->records[m->file_count];
    record->size = size;
    record->modified_time = mod_time;
    
    if (checksum) {
        memcpy(record->checksum, checksum, 32);
    } else {
        memset(record->checksum, 0, 32);
    }
    
    record->path_length = (uint16_t)strlen(path);
    record->path = (char*)malloc(record->path_length + 1);
    strcpy(record->path, path);
    
    m->file_count++;
}

void scan_directory(Manifest* m, const char* base_dir, const char* rel_path) {
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\%s\\*", base_dir, rel_path);

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path, &find_data);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }

        char item_rel_path[MAX_PATH];
        if (strlen(rel_path) > 0) {
            snprintf(item_rel_path, MAX_PATH, "%s/%s", rel_path, find_data.cFileName);
        } else {
            snprintf(item_rel_path, MAX_PATH, "%s", find_data.cFileName);
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory(m, base_dir, item_rel_path);
        } else {
            ULARGE_INTEGER file_size;
            file_size.LowPart = find_data.nFileSizeLow;
            file_size.HighPart = find_data.nFileSizeHigh;

            ULARGE_INTEGER file_time;
            file_time.LowPart = find_data.ftLastWriteTime.dwLowDateTime;
            file_time.HighPart = find_data.ftLastWriteTime.dwHighDateTime;

            char full_path[MAX_PATH];
            snprintf(full_path, MAX_PATH, "%s\\%s", base_dir, item_rel_path);

            uint8_t file_hash[SHA256_BLOCK_SIZE];
            calculate_file_hash(full_path, file_hash);

            add_record(m, file_size.QuadPart, file_time.QuadPart, item_rel_path, file_hash);
        }
    } while (FindNextFileA(hFind, &find_data) != 0);

    FindClose(hFind);
}

uint8_t* encode_manifest(Manifest* m, size_t* out_size) {
    size_t total_size = 4 + 1 + 4; // magic (4) + version (1) + count (4)
    for (uint32_t i = 0; i < m->file_count; i++) {
        // size (8) + time (8) + checksum (32) + path length (2) + path
        total_size += 50 + m->records[i].path_length;
    }

    *out_size = total_size;
    uint8_t* buffer = (uint8_t*)malloc(total_size);
    uint8_t* ptr = buffer;

    memcpy(ptr, m->magic, 4);          ptr += 4;
    memcpy(ptr, &m->version, 1);       ptr += 1;
    memcpy(ptr, &m->file_count, 4);    ptr += 4;

    for (uint32_t i = 0; i < m->file_count; i++) {
        ManifestFile* r = &m->records[i];
        
        memcpy(ptr, &r->size, 8);               ptr += 8;
        memcpy(ptr, &r->modified_time, 8);      ptr += 8;
        memcpy(ptr, r->checksum, 32);           ptr += 32;
        memcpy(ptr, &r->path_length, 2);        ptr += 2;
        memcpy(ptr, r->path, r->path_length);   ptr += r->path_length;
    }

    return buffer;
}

Manifest* decode_manifest(uint8_t* buffer) {
    Manifest* m = create_manifest();
    uint8_t* ptr = buffer;

    memcpy(m->magic, ptr, 4);       ptr += 4;
    if (strncmp(m->magic,"SYNC",4) != 0) return NULL;
    memcpy(&m->version, ptr, 1);    ptr += 1;
    memcpy(&m->file_count, ptr, 4); ptr += 4;

    m->capacity = m->file_count;
    free(m->records);
    m->records = (ManifestFile*)malloc(m->capacity * sizeof(ManifestFile));

    for (uint32_t i = 0; i < m->file_count; i++) {
        ManifestFile* r = &m->records[i];
        
        memcpy(&r->size, ptr, 8);           ptr += 8;
        memcpy(&r->modified_time, ptr, 8);  ptr += 8;
        memcpy(r->checksum, ptr, 32);       ptr += 32;
        memcpy(&r->path_length, ptr, 2);    ptr += 2;
        
        r->path = (char*)malloc(r->path_length + 1);
        memcpy(r->path, ptr, r->path_length);
        r->path[r->path_length] = '\0';     // add null terminator
        ptr += r->path_length;
    }

    return m;
}