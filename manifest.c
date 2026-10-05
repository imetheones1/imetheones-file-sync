#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include "manifest.h"
#include "util.h"

#define HASH_BUFFER_SIZE (1024 * 1024)
#define MAX_HASH_THREADS 8
#define ENCODED_HEADER_SIZE 9  // magic (4) + version (1) + count (4)
#define ENCODED_RECORD_SIZE 50 // size (8) + time (8) + checksum (32) + path length (2)

Manifest* create_manifest(void) {
    Manifest* m = malloc(sizeof(Manifest));
    if (!m) return NULL;
    m->file_count = 0;
    m->capacity = 256;
    m->records = malloc(m->capacity * sizeof(ManifestFile));
    if (!m->records) {
        free(m);
        return NULL;
    }
    return m;
}

void free_manifest(Manifest* m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->file_count; i++) {
        free(m->records[i].path);
    }
    free(m->records);
    free(m);
}

ManifestFile* add_record(Manifest* m, uint64_t size, uint64_t mod_time, const char* path, uint16_t path_length, const uint8_t* checksum) {
    if (m->file_count >= m->capacity) {
        uint32_t new_capacity = m->capacity * 2;
        ManifestFile* new_records = realloc(m->records, new_capacity * sizeof(ManifestFile));
        if (!new_records) return NULL;
        m->records = new_records;
        m->capacity = new_capacity;
    }

    ManifestFile* record = &m->records[m->file_count];
    record->path = malloc(path_length + 1);
    if (!record->path) return NULL;
    memcpy(record->path, path, path_length);
    record->path[path_length] = '\0';
    record->path_length = path_length;
    record->size = size;
    record->modified_time = mod_time;

    if (checksum) {
        memcpy(record->checksum, checksum, HASH_SIZE);
    } else {
        memset(record->checksum, 0, HASH_SIZE);
    }

    m->file_count++;
    return record;
}

static int compare_records(const void* a, const void* b) {
    return strcmp(((const ManifestFile*)a)->path, ((const ManifestFile*)b)->path);
}

static int compare_path_to_record(const void* key, const void* record) {
    return strcmp((const char*)key, ((const ManifestFile*)record)->path);
}

void sort_manifest(Manifest* m) {
    qsort(m->records, m->file_count, sizeof(ManifestFile), compare_records);
}

ManifestFile* find_record(const Manifest* m, const char* path) {
    if (!m || m->file_count == 0) return NULL;
    return bsearch(path, m->records, m->file_count, sizeof(ManifestFile), compare_path_to_record);
}

// ---- scanning ----

typedef struct PathStack {
    char**  items;
    size_t  count;
    size_t  capacity;
} PathStack;

static bool push_path(PathStack* stack, char* path) {
    if (stack->count >= stack->capacity) {
        size_t new_capacity = stack->capacity ? stack->capacity * 2 : 64;
        char** new_items = realloc(stack->items, new_capacity * sizeof(char*));
        if (!new_items) return false;
        stack->items = new_items;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = path;
    return true;
}

// list every file without hashing
static bool list_files(const wchar_t* base_dir, Manifest* m) {
    PathStack stack = {0};
    char* root = calloc(1, 1);
    if (!root || !push_path(&stack, root)) {
        free(root);
        return false;
    }

    bool success = true;

    while (stack.count > 0) {
        char* rel_dir = stack.items[--stack.count];
        size_t rel_dir_length = strlen(rel_dir);
        bool is_root = rel_dir_length == 0;

        wchar_t* dir = join_path(base_dir, rel_dir, -1);
        size_t dir_length = dir ? wcslen(dir) : 0;
        wchar_t* search_path = dir ? malloc((dir_length + 3) * sizeof(wchar_t)) : NULL;
        if (!search_path) {
            progress_message("Out of memory while listing files\n");
            free(dir);
            free(rel_dir);
            success = false;
            break;
        }
        swprintf(search_path, dir_length + 3, L"%ls\\*", dir);
        free(dir);

        WIN32_FIND_DATAW find_data;
        HANDLE find_handle = FindFirstFileExW(search_path, FindExInfoBasic, &find_data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(search_path);

        if (find_handle == INVALID_HANDLE_VALUE) {
            progress_message("Failed to open folder \"%s\" with code %lu\n", is_root ? "." : rel_dir, GetLastError());
            free(rel_dir);
            if (is_root) {
                success = false;
                break;
            }
            continue;
        }

        do {
            const wchar_t* name = find_data.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

            int name_length;
            char* name_utf8 = wide_to_utf8(name, -1, &name_length);
            if (!name_utf8) {
                progress_message("Skipping file with invalid name in \"%s\"\n", is_root ? "." : rel_dir);
                continue;
            }

            size_t path_length = is_root ? (size_t)name_length : rel_dir_length + 1 + name_length;
            char* path = malloc(path_length + 1);
            if (!path) {
                free(name_utf8);
                continue;
            }
            if (is_root) {
                memcpy(path, name_utf8, name_length + 1);
            } else {
                memcpy(path, rel_dir, rel_dir_length);
                path[rel_dir_length] = '/';
                memcpy(path + rel_dir_length + 1, name_utf8, name_length + 1);
            }
            free(name_utf8);

            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // junctions can loop back up the tree
                if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    progress_message("Skipping linked folder \"%s\"\n", path);
                    free(path);
                } else if (!push_path(&stack, path)) {
                    free(path);
                }
                continue;
            }

            bool skip = (is_root && strcmp(path, CACHE_FILE_NAME) == 0) || ends_with(path, path_length, TEMP_SUFFIX);
            if (path_length > UINT16_MAX) {
                progress_message("Skipping file with path too long: \"%.100s...\"\n", path);
                skip = true;
            }

            if (!skip) {
                uint64_t file_size = make_uint64(find_data.nFileSizeHigh, find_data.nFileSizeLow);
                uint64_t file_time = make_uint64(find_data.ftLastWriteTime.dwHighDateTime, find_data.ftLastWriteTime.dwLowDateTime);

                if (!add_record(m, file_size, file_time, path, (uint16_t)path_length, NULL)) {
                    progress_message("Out of memory while listing files\n");
                    free(path);
                    success = false;
                    break;
                }

                if (progress_due()) progress_print("Listing files: %u found", m->file_count);
            }
            free(path);
        } while (FindNextFileW(find_handle, &find_data) != 0);

        FindClose(find_handle);
        free(rel_dir);
        if (!success) break;
    }

    while (stack.count > 0) free(stack.items[--stack.count]);
    free(stack.items);
    return success;
}

// also refreshes size and time from the open file
static bool hash_file(const wchar_t* path, uint8_t* buffer, ManifestFile* r, volatile LONG64* bytes_done) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(file, &info)) {
        r->size = make_uint64(info.nFileSizeHigh, info.nFileSizeLow);
        r->modified_time = make_uint64(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
    }

    BCRYPT_HASH_HANDLE hash;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0) != 0) {
        CloseHandle(file);
        return false;
    }

    bool success = true;
    for (;;) {
        DWORD bytes_read;
        if (!ReadFile(file, buffer, HASH_BUFFER_SIZE, &bytes_read, NULL)) {
            success = false;
            break;
        }
        if (bytes_read == 0) break;
        BCryptHashData(hash, buffer, bytes_read, 0);
        InterlockedExchangeAdd64(bytes_done, bytes_read);
    }

    if (success) BCryptFinishHash(hash, r->checksum, HASH_SIZE, 0);
    BCryptDestroyHash(hash);
    CloseHandle(file);
    return success;
}

typedef struct HashJob {
    const wchar_t*  base_dir;
    ManifestFile*   records;
    uint32_t*       todo;       // records to hash
    bool*           failed;     // parallel to todo
    uint32_t        todo_count;
    volatile LONG   next;
    volatile LONG   done;
    volatile LONG64 bytes_done;
} HashJob;

static DWORD WINAPI hash_worker(LPVOID param) {
    HashJob* job = param;
    uint8_t* buffer = malloc(HASH_BUFFER_SIZE);

    for (;;) {
        LONG i = InterlockedIncrement(&job->next) - 1;
        if ((uint32_t)i >= job->todo_count) break;

        ManifestFile* r = &job->records[job->todo[i]];
        wchar_t* full_path = join_path(job->base_dir, r->path, r->path_length);
        if (!buffer || !full_path || !hash_file(full_path, buffer, r, &job->bytes_done)) {
            job->failed[i] = true;
        }
        free(full_path);
        InterlockedIncrement(&job->done);
    }

    free(buffer);
    return 0;
}

// hard drives slow down with multiple readers
static bool has_seek_penalty(const wchar_t* path) {
    wchar_t volume_path[MAX_PATH + 1];
    wchar_t volume_name[MAX_PATH + 1];
    if (!GetVolumePathNameW(path, volume_path, MAX_PATH + 1)) return false;
    if (!GetVolumeNameForVolumeMountPointW(volume_path, volume_name, MAX_PATH + 1)) return false;

    // open the volume itself, not its root folder
    size_t length = wcslen(volume_name);
    if (length > 0 && volume_name[length - 1] == L'\\') volume_name[length - 1] = L'\0';

    HANDLE volume = CreateFileW(volume_name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (volume == INVALID_HANDLE_VALUE) return false;

    STORAGE_PROPERTY_QUERY query = { .PropertyId = StorageDeviceSeekPenaltyProperty, .QueryType = PropertyStandardQuery };
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = {0};
    DWORD returned;
    BOOL ok = DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                              &descriptor, sizeof(descriptor), &returned, NULL);
    CloseHandle(volume);
    return ok && descriptor.IncursSeekPenalty;
}

static uint32_t choose_thread_count(const wchar_t* base_dir, uint32_t todo_count) {
    if (todo_count <= 1 || has_seek_penalty(base_dir)) return 1;

    uint32_t threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (threads > MAX_HASH_THREADS) threads = MAX_HASH_THREADS;
    if (threads > todo_count) threads = todo_count;
    if (threads < 1) threads = 1;
    return threads;
}

static bool run_hash_job(HashJob* job, uint32_t thread_count, uint64_t total_bytes) {
    HANDLE threads[MAX_HASH_THREADS];
    uint32_t started = 0;
    for (uint32_t i = 0; i < thread_count; i++) {
        threads[started] = CreateThread(NULL, 0, hash_worker, job, 0, NULL);
        if (threads[started]) started++;
    }
    if (started == 0) return false;

    double start_time = now_seconds();
    char done_text[32], total_text[32], rate_text[32];
    format_file_size(total_text, sizeof(total_text), (double)total_bytes);

    for (;;) {
        DWORD wait = WaitForMultipleObjects(started, threads, TRUE, 100);
        bool finished = wait != WAIT_TIMEOUT;

        if (finished || progress_due()) {
            double elapsed = now_seconds() - start_time;
            double bytes = (double)job->bytes_done;
            format_file_size(done_text, sizeof(done_text), bytes);
            format_file_size(rate_text, sizeof(rate_text), elapsed > 0 ? bytes / elapsed : 0);
            progress_print("Hashing [%ld/%u] %s / %s (%s/s)", job->done, job->todo_count, done_text, total_text, rate_text);
        }
        if (finished) break;
    }
    progress_end();

    for (uint32_t i = 0; i < started; i++) CloseHandle(threads[i]);
    return true;
}

Manifest* scan_directory(const wchar_t* base_dir, const Manifest* cache) {
    double start_time = now_seconds();

    Manifest* m = create_manifest();
    if (!m) return NULL;

    if (!list_files(base_dir, m)) {
        free_manifest(m);
        return NULL;
    }
    progress_print("Listing files: %u found", m->file_count);
    progress_end();

    uint32_t* todo = malloc((m->file_count + 1) * sizeof(uint32_t));
    bool* failed = calloc(m->file_count + 1, sizeof(bool));
    if (!todo || !failed) {
        free(todo);
        free(failed);
        free_manifest(m);
        return NULL;
    }

    uint32_t todo_count = 0;
    uint32_t failed_count = 0;
    uint64_t todo_bytes = 0;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < m->file_count; i++) {
        ManifestFile* r = &m->records[i];
        total_bytes += r->size;

        ManifestFile* cached = find_record(cache, r->path);
        if (cached && cached->size == r->size && cached->modified_time == r->modified_time) {
            memcpy(r->checksum, cached->checksum, HASH_SIZE);
        } else {
            todo[todo_count++] = i;
            todo_bytes += r->size;
        }
    }

    if (todo_count > 0) {
        uint32_t thread_count = choose_thread_count(base_dir, todo_count);
        HashJob job = {
            .base_dir = base_dir,
            .records = m->records,
            .todo = todo,
            .failed = failed,
            .todo_count = todo_count,
        };
        if (!run_hash_job(&job, thread_count, todo_bytes)) {
            printf("Failed to start hashing threads\n");
            free(todo);
            free(failed);
            free_manifest(m);
            return NULL;
        }

        // unreadable files can't be sent
        for (uint32_t i = 0; i < todo_count; i++) {
            if (!failed[i]) continue;
            ManifestFile* r = &m->records[todo[i]];
            printf("Failed to read \"%s\", skipping it\n", r->path);
            free(r->path);
            r->path = NULL;
            failed_count++;
        }
        if (failed_count > 0) {
            uint32_t kept = 0;
            for (uint32_t i = 0; i < m->file_count; i++) {
                if (m->records[i].path) m->records[kept++] = m->records[i];
            }
            m->file_count = kept;
        }
    }

    sort_manifest(m);

    char total_text[32], hashed_text[32];
    format_file_size(total_text, sizeof(total_text), (double)total_bytes);
    format_file_size(hashed_text, sizeof(hashed_text), (double)todo_bytes);
    printf("Scanned %u files (%s) in %.2fs: %u hashed (%s), %u unchanged since last scan\n",
           m->file_count, total_text, now_seconds() - start_time,
           todo_count - failed_count, hashed_text, m->file_count - (todo_count - failed_count));

    free(todo);
    free(failed);
    return m;
}

// ---- cache file ----

// NULL if missing or invalid
static Manifest* load_cache(const wchar_t* base_dir) {
    wchar_t* path = join_path(base_dir, CACHE_FILE_NAME, -1);
    if (!path) return NULL;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    Manifest* m = NULL;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 0x7FFFFFFF) {
        uint8_t* buffer = malloc((size_t)size.QuadPart);
        DWORD bytes_read;
        if (buffer && ReadFile(file, buffer, (DWORD)size.QuadPart, &bytes_read, NULL) && bytes_read == size.QuadPart) {
            m = decode_manifest(buffer, bytes_read);
        }
        free(buffer);
    }
    CloseHandle(file);

    if (m) sort_manifest(m);
    return m;
}

Manifest* scan_with_cache(const wchar_t* base_dir) {
    Manifest* cache = load_cache(base_dir);
    Manifest* m = scan_directory(base_dir, cache);
    free_manifest(cache);
    return m;
}

void save_cache(const wchar_t* base_dir, const Manifest* m) {
    size_t encoded_size;
    uint8_t* encoded = encode_manifest(m, &encoded_size);
    wchar_t* path = join_path(base_dir, CACHE_FILE_NAME, -1);
    wchar_t* temp_path = join_path(base_dir, CACHE_FILE_NAME TEMP_SUFFIX, -1);
    bool success = false;

    if (encoded && path && temp_path) {
        HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            success = encoded_size < 0x7FFFFFFF
                && WriteFile(file, encoded, (DWORD)encoded_size, &written, NULL)
                && written == encoded_size;
            CloseHandle(file);

            // write then rename so the cache is never half written
            if (success) success = MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING);
            if (!success) DeleteFileW(temp_path);
        }
    }

    if (!success) printf("Warning: couldn't save the cache file, the next scan will hash everything again\n");

    free(path);
    free(temp_path);
    free(encoded);
}

// ---- encoding ----

uint8_t* encode_manifest(const Manifest* m, size_t* out_size) {
    size_t total_size = ENCODED_HEADER_SIZE;
    for (uint32_t i = 0; i < m->file_count; i++) {
        total_size += ENCODED_RECORD_SIZE + m->records[i].path_length;
    }

    uint8_t* buffer = malloc(total_size);
    if (!buffer) return NULL;
    uint8_t* ptr = buffer;

    uint8_t version = MANIFEST_VERSION;
    memcpy(ptr, "SYNC", 4);            ptr += 4;
    memcpy(ptr, &version, 1);          ptr += 1;
    memcpy(ptr, &m->file_count, 4);    ptr += 4;

    for (uint32_t i = 0; i < m->file_count; i++) {
        const ManifestFile* r = &m->records[i];

        memcpy(ptr, &r->size, 8);               ptr += 8;
        memcpy(ptr, &r->modified_time, 8);      ptr += 8;
        memcpy(ptr, r->checksum, HASH_SIZE);    ptr += HASH_SIZE;
        memcpy(ptr, &r->path_length, 2);        ptr += 2;
        memcpy(ptr, r->path, r->path_length);   ptr += r->path_length;
    }

    *out_size = total_size;
    return buffer;
}

Manifest* decode_manifest(const uint8_t* buffer, size_t size) {
    if (size < ENCODED_HEADER_SIZE) return NULL;
    const uint8_t* ptr = buffer;
    const uint8_t* end = buffer + size;

    if (memcmp(ptr, "SYNC", 4) != 0) return NULL;
    ptr += 4;
    if (*ptr != MANIFEST_VERSION) return NULL;
    ptr += 1;

    uint32_t file_count;
    memcpy(&file_count, ptr, 4);
    ptr += 4;

    // check count before allocating
    if (file_count > (size - ENCODED_HEADER_SIZE) / ENCODED_RECORD_SIZE) return NULL;

    Manifest* m = create_manifest();
    if (!m) return NULL;
    if (file_count > m->capacity) {
        ManifestFile* records = realloc(m->records, file_count * sizeof(ManifestFile));
        if (!records) {
            free_manifest(m);
            return NULL;
        }
        m->records = records;
        m->capacity = file_count;
    }

    for (uint32_t i = 0; i < file_count; i++) {
        if (end - ptr < ENCODED_RECORD_SIZE) goto invalid;

        uint64_t file_size, modified_time;
        uint16_t path_length;
        const uint8_t* checksum;
        memcpy(&file_size, ptr, 8);         ptr += 8;
        memcpy(&modified_time, ptr, 8);     ptr += 8;
        checksum = ptr;                     ptr += HASH_SIZE;
        memcpy(&path_length, ptr, 2);       ptr += 2;

        if (path_length == 0 || end - ptr < path_length) goto invalid;
        if (memchr(ptr, '\0', path_length)) goto invalid;

        if (!add_record(m, file_size, modified_time, (const char*)ptr, path_length, checksum)) goto invalid;
        ptr += path_length;
    }

    if (ptr != end) goto invalid;
    return m;

invalid:
    free_manifest(m);
    return NULL;
}
