#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#include "manifest.h"
#include "util.h"

#define PORT 2026 // todo choose a good port
#define PORT_STRING "2026"

#define MAX_MANIFEST_SIZE   (512ull * 1024 * 1024)
#define NET_BUFFER_SIZE     (1024 * 1024)
#define SMALL_FILE_SIZE     (256 * 1024)       // batched into one send
#define TRANSMIT_CHUNK_SIZE (64u * 1024 * 1024) // TransmitFile max is 2^31 - 2 bytes

// protocol (little endian)
//  client -> server: uint64 manifest size, manifest (see manifest.h)
//  server -> client: uint32 number of files that will be sent, uint64 total bytes
//  server -> client: any number of messages, each starting with a uint8 type
//      MSG_FILE: uint64 size, uint64 modified time, uint16 path length, path, file contents
//      MSG_END:  uint32 number of files actually sent. ends the stream
//  client -> server: uint32 files written, uint32 files failed
enum { MSG_END = 0, MSG_FILE = 1 };

// ---- socket helpers ----

static bool send_all(SOCKET sock, const void* data, size_t size) {
    const char* ptr = data;
    while (size > 0) {
        int chunk = size > INT_MAX ? INT_MAX : (int)size;
        int sent = send(sock, ptr, chunk, 0);
        if (sent == SOCKET_ERROR) {
            printf("\nFailed to send data with code %d\n", WSAGetLastError());
            return false;
        }
        ptr += sent;
        size -= sent;
    }
    return true;
}

static bool recv_all(SOCKET sock, void* data, size_t size) {
    char* ptr = data;
    while (size > 0) {
        int chunk = size > INT_MAX ? INT_MAX : (int)size;
        int received = recv(sock, ptr, chunk, 0);
        if (received == 0) {
            printf("\nConnection closed unexpectedly\n");
            return false;
        }
        if (received == SOCKET_ERROR) {
            printf("\nFailed to receive data with code %d\n", WSAGetLastError());
            return false;
        }
        ptr += received;
        size -= received;
    }
    return true;
}

// batches small writes into large sends
typedef struct SendBuffer {
    SOCKET   sock;
    uint8_t* data;
    size_t   used;
} SendBuffer;

static bool flush_buffer(SendBuffer* b) {
    bool success = send_all(b->sock, b->data, b->used);
    b->used = 0;
    return success;
}

// flushes if size doesn't fit
static bool reserve_buffer(SendBuffer* b, size_t size) {
    if (b->used + size > NET_BUFFER_SIZE) return flush_buffer(b);
    return true;
}

static bool append_buffer(SendBuffer* b, const void* data, size_t size) {
    if (!reserve_buffer(b, size)) return false;
    memcpy(b->data + b->used, data, size);
    b->used += size;
    return true;
}

// buffered so small fields don't each need a recv
typedef struct Reader {
    SOCKET   sock;
    uint8_t* data;
    size_t   start;
    size_t   end;
} Reader;

static bool fill_reader(Reader* r) {
    int received = recv(r->sock, (char*)r->data, NET_BUFFER_SIZE, 0);
    if (received == 0) {
        printf("\nConnection closed unexpectedly\n");
        return false;
    }
    if (received == SOCKET_ERROR) {
        printf("\nFailed to receive data with code %d\n", WSAGetLastError());
        return false;
    }
    r->start = 0;
    r->end = received;
    return true;
}

static bool read_exact(Reader* r, void* out, size_t size) {
    uint8_t* ptr = out;
    while (size > 0) {
        if (r->start == r->end && !fill_reader(r)) return false;
        size_t available = r->end - r->start;
        size_t n = size < available ? size : available;
        memcpy(ptr, r->data + r->start, n);
        r->start += n;
        ptr += n;
        size -= n;
    }
    return true;
}

// ---- server ----

static bool transmit_file(SOCKET sock, HANDLE file, uint64_t size) {
    for (uint64_t offset = 0; offset < size; ) {
        uint64_t remaining = size - offset;
        DWORD chunk = remaining > TRANSMIT_CHUNK_SIZE ? TRANSMIT_CHUNK_SIZE : (DWORD)remaining;

        // TransmitFile starts at the current position
        LARGE_INTEGER position;
        position.QuadPart = offset;
        if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) {
            printf("\nFailed to seek file with code %lu\n", GetLastError());
            return false;
        }
        if (!TransmitFile(sock, file, chunk, 0, NULL, NULL, 0)) {
            printf("\nTransmitFile failed with code %d\n", WSAGetLastError());
            return false;
        }
        offset += chunk;
    }
    return true;
}

static bool read_file_exact(HANDLE file, uint8_t* out, size_t size) {
    while (size > 0) {
        DWORD bytes_read;
        if (!ReadFile(file, out, (DWORD)size, &bytes_read, NULL) || bytes_read == 0) return false;
        out += bytes_read;
        size -= bytes_read;
    }
    return true;
}

static void print_transfer_progress(const char* verb, uint32_t done, uint32_t total, uint64_t bytes, uint64_t total_bytes, double start_time, const char* path) {
    char bytes_text[32], total_text[32], rate_text[32], name[48];
    double elapsed = now_seconds() - start_time;
    format_file_size(bytes_text, sizeof(bytes_text), (double)bytes);
    format_file_size(total_text, sizeof(total_text), (double)total_bytes);
    format_file_size(rate_text, sizeof(rate_text), elapsed > 0 ? bytes / elapsed : 0);
    utf8_truncate(name, sizeof(name), path, 40);
    progress_print("%s [%u/%u] %s / %s (%s/s) %s", verb, done, total, bytes_text, total_text, rate_text, name);
}

static bool send_files(SOCKET sock, const wchar_t* base_dir, const Manifest* local, const Manifest* remote) {
    uint32_t* to_send = malloc((local->file_count + 1) * sizeof(uint32_t));
    SendBuffer buffer = { .sock = sock, .data = malloc(NET_BUFFER_SIZE) };
    if (!to_send || !buffer.data) {
        printf("Failed to allocate memory for sending\n");
        free(to_send);
        free(buffer.data);
        return false;
    }

    uint32_t send_count = 0;
    uint64_t send_bytes = 0;
    for (uint32_t i = 0; i < local->file_count; i++) {
        const ManifestFile* r = &local->records[i];
        const ManifestFile* theirs = find_record(remote, r->path);
        if (!theirs || memcmp(r->checksum, theirs->checksum, HASH_SIZE) != 0) {
            to_send[send_count++] = i;
            send_bytes += r->size;
        }
    }

    char size_text[32];
    format_file_size(size_text, sizeof(size_text), (double)send_bytes);
    printf("%u files (%s) need to be sent\n", send_count, size_text);

    bool success = append_buffer(&buffer, &send_count, 4) && append_buffer(&buffer, &send_bytes, 8);

    uint32_t sent_count = 0;
    uint64_t bytes_sent = 0;
    double start_time = now_seconds();

    for (uint32_t i = 0; i < send_count && success; i++) {
        const ManifestFile* r = &local->records[to_send[i]];

        // no write sharing so the size can't change while sending
        wchar_t* full_path = join_path(base_dir, r->path, r->path_length);
        HANDLE file = full_path ? CreateFileW(full_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
        free(full_path);

        BY_HANDLE_FILE_INFORMATION info;
        if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info)) {
            printf("\nFailed to open \"%s\" with code %lu, skipping it\n", r->path, GetLastError());
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            continue;
        }

        uint64_t size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        uint64_t modified_time = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;

        uint8_t type = MSG_FILE;
        success = append_buffer(&buffer, &type, 1)
            && append_buffer(&buffer, &size, 8)
            && append_buffer(&buffer, &modified_time, 8)
            && append_buffer(&buffer, &r->path_length, 2)
            && append_buffer(&buffer, r->path, r->path_length);

        if (success) {
            if (size <= SMALL_FILE_SIZE) {
                success = reserve_buffer(&buffer, (size_t)size);
                if (success && !read_file_exact(file, buffer.data + buffer.used, (size_t)size)) {
                    // header is already queued, can't skip this file
                    printf("\nFailed to read \"%s\" with code %lu\n", r->path, GetLastError());
                    success = false;
                }
                buffer.used += (size_t)size;
            } else {
                success = flush_buffer(&buffer) && transmit_file(sock, file, size);
            }
        }
        CloseHandle(file);

        if (success) {
            sent_count++;
            bytes_sent += size;
            if (progress_due() || sent_count == send_count) {
                print_transfer_progress("Sending", sent_count, send_count, bytes_sent, send_bytes, start_time, r->path);
            }
        }
    }

    if (success) {
        uint8_t type = MSG_END;
        success = append_buffer(&buffer, &type, 1)
            && append_buffer(&buffer, &sent_count, 4)
            && flush_buffer(&buffer);
    }
    if (send_count > 0) progress_end();

    uint32_t client_results[2]; // written, failed
    if (success && recv_all(sock, client_results, sizeof(client_results))) {
        double elapsed = now_seconds() - start_time;
        format_file_size(size_text, sizeof(size_text), (double)bytes_sent);
        printf("Sent %u files (%s) in %.2fs. Client wrote %u, failed %u\n",
               sent_count, size_text, elapsed, client_results[0], client_results[1]);
        if (client_results[1] > 0) success = false;
    } else {
        success = false;
    }

    free(to_send);
    free(buffer.data);
    return success;
}

static int run_server(const wchar_t* base_dir) {
    int result = EXIT_FAILURE;
    SOCKET listen_socket = INVALID_SOCKET;
    SOCKET client_socket = INVALID_SOCKET;
    Manifest* local_manifest = NULL;
    Manifest* client_manifest = NULL;
    uint8_t* manifest_buffer = NULL;

    // dual stack, accepts IPv4 and IPv6
    listen_socket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        printf("Socket creation failed with code %d\n", WSAGetLastError());
        goto cleanup;
    }
    DWORD v6_only = 0;
    setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&v6_only, sizeof(v6_only));

    struct sockaddr_in6 address = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(PORT),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };
#ifdef LOOPBACK_ONLY
    // for testing, loopback only doesn't trigger the firewall prompt
    // ::ffff:127.0.0.1
    address.sin6_addr.s6_addr[10] = 0xff;
    address.sin6_addr.s6_addr[11] = 0xff;
    address.sin6_addr.s6_addr[12] = 127;
    address.sin6_addr.s6_addr[15] = 1;
#endif
    if (bind(listen_socket, (struct sockaddr*)&address, sizeof(address)) != 0) {
        printf("bind failed with code %d\n", WSAGetLastError());
        goto cleanup;
    }
    if (listen(listen_socket, 1) != 0) {
        printf("listen failed with code %d\n", WSAGetLastError());
        goto cleanup;
    }

    printf("Waiting for a client on port %d\n", PORT);
    struct sockaddr_storage client_address;
    int client_address_length = sizeof(client_address);
    client_socket = accept(listen_socket, (struct sockaddr*)&client_address, &client_address_length);
    if (client_socket == INVALID_SOCKET) {
        printf("Client socket accept failed with code %d\n", WSAGetLastError());
        goto cleanup;
    }
    closesocket(listen_socket);
    listen_socket = INVALID_SOCKET;

    char host[NI_MAXHOST];
    if (getnameinfo((struct sockaddr*)&client_address, client_address_length, host, sizeof(host), NULL, 0, NI_NUMERICHOST) != 0) {
        strcpy(host, "unknown address");
    }
    printf("Client connected from %s\n", host);

    Manifest* cache = load_cache(base_dir);
    local_manifest = scan_directory(base_dir, cache);
    free_manifest(cache);
    if (!local_manifest) {
        printf("Failed to scan folder\n");
        goto cleanup;
    }
    if (!save_cache(base_dir, local_manifest)) {
        printf("Warning: couldn't save the cache file, the next scan will hash everything again\n");
    }

    uint64_t manifest_size;
    if (!recv_all(client_socket, &manifest_size, sizeof(manifest_size))) goto cleanup;
    if (manifest_size > MAX_MANIFEST_SIZE) {
        printf("Client manifest is too large (%llu bytes)\n", (unsigned long long)manifest_size);
        goto cleanup;
    }

    manifest_buffer = malloc((size_t)manifest_size);
    if (!manifest_buffer) {
        printf("Failed to allocate memory for manifest\n");
        goto cleanup;
    }
    if (!recv_all(client_socket, manifest_buffer, (size_t)manifest_size)) goto cleanup;

    client_manifest = decode_manifest(manifest_buffer, (size_t)manifest_size);
    if (!client_manifest) {
        printf("Client sent an invalid manifest. Are both sides running the same version?\n");
        goto cleanup;
    }
    sort_manifest(client_manifest);

    if (send_files(client_socket, base_dir, local_manifest, client_manifest)) {
        result = EXIT_SUCCESS;
    }
    shutdown(client_socket, SD_SEND);

cleanup:
    if (client_socket != INVALID_SOCKET) closesocket(client_socket);
    if (listen_socket != INVALID_SOCKET) closesocket(listen_socket);
    free(manifest_buffer);
    free_manifest(local_manifest);
    free_manifest(client_manifest);
    return result;
}

// ---- client ----

static bool ends_with(const char* s, size_t length, const char* suffix) {
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && memcmp(s + length - suffix_length, suffix, suffix_length) == 0;
}

// paths come from the network, don't let them escape the folder
static bool is_safe_path(const char* path, size_t length) {
    if (length == 0) return false;
    if (strcmp(path, CACHE_FILE_NAME) == 0 || ends_with(path, length, TEMP_SUFFIX)) return false;

    size_t component_start = 0;
    for (size_t i = 0; i <= length; i++) {
        char c = i < length ? path[i] : '/';
        if (c == '/') {
            size_t component_length = i - component_start;
            if (component_length == 0) return false; // leading or doubled slash
            // ".", ".." and names Windows would trim
            char last = path[i - 1];
            if (last == '.' || last == ' ') return false;
            component_start = i + 1;
        } else if ((unsigned char)c < 32 || strchr("\\:*?\"<>|", c)) {
            return false;
        }
    }
    return true;
}

// create missing folders in path after start
static void create_directories(wchar_t* path, size_t start, bool include_last) {
    for (size_t i = start; path[i] != L'\0'; i++) {
        if (path[i] == L'\\') {
            path[i] = L'\0';
            CreateDirectoryW(path, NULL);
            path[i] = L'\\';
        }
    }
    if (include_last) CreateDirectoryW(path, NULL);
}

// OPEN_ALWAYS + truncate, CREATE_ALWAYS fails on hidden files
static HANDLE create_output_file(wchar_t* path, size_t base_length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
        // only create folders when missing
        create_directories(path, base_length + 1, false);
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
    if (file != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS && !SetEndOfFile(file)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

typedef enum { RECEIVE_OK, RECEIVE_FAILED, RECEIVE_CONNECTION_LOST } ReceiveResult;

// always reads the contents, even if they can't be written, so the stream stays in sync
static ReceiveResult receive_file(Reader* reader, const wchar_t* base_dir, const char* path, uint16_t path_length,
                                  uint64_t size, uint64_t modified_time, uint8_t* out_checksum, uint64_t* bytes_received) {
    bool path_ok = is_safe_path(path, path_length);
    if (!path_ok) printf("\nServer sent an unsafe path \"%s\", skipping it\n", path);

    wchar_t* full_path = path_ok ? join_path(base_dir, path, path_length) : NULL;

    // written in place, renaming a temp file is slow with Defender
    // partial files are deleted, or resent next sync since their time won't match
    HANDLE file = INVALID_HANDLE_VALUE;
    if (full_path) {
        file = create_output_file(full_path, wcslen(base_dir));
        if (file == INVALID_HANDLE_VALUE) {
            printf("\nFailed to create \"%s\" with code %lu\n", path, GetLastError());
        } else if (size > SMALL_FILE_SIZE) {
            // reduce fragmentation
            FILE_ALLOCATION_INFO allocation = { .AllocationSize.QuadPart = (LONGLONG)size };
            SetFileInformationByHandle(file, FileAllocationInfo, &allocation, sizeof(allocation));
        }
    }
    bool write_ok = file != INVALID_HANDLE_VALUE;

    // hash while receiving so the cache is updated without rereading
    BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0) != 0) hash = NULL;

    ReceiveResult result = RECEIVE_OK;
    uint64_t remaining = size;
    while (remaining > 0) {
        if (reader->start == reader->end && !fill_reader(reader)) {
            result = RECEIVE_CONNECTION_LOST;
            break;
        }
        size_t available = reader->end - reader->start;
        DWORD n = (DWORD)(remaining < available ? remaining : available);
        const uint8_t* data = reader->data + reader->start;

        if (write_ok) {
            DWORD written;
            if (!WriteFile(file, data, n, &written, NULL) || written != n) {
                printf("\nFailed to write \"%s\" with code %lu\n", path, GetLastError());
                write_ok = false;
            }
        }
        if (hash) BCryptHashData(hash, (PUCHAR)data, n, 0);

        reader->start += n;
        remaining -= n;
        *bytes_received += n;
    }

    if (result == RECEIVE_OK && write_ok) {
        // keep the server's modified time so the cache entry stays valid
        FILETIME file_time = { .dwLowDateTime = (DWORD)modified_time, .dwHighDateTime = (DWORD)(modified_time >> 32) };
        SetFileTime(file, NULL, NULL, &file_time);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        if (result != RECEIVE_OK || !write_ok) DeleteFileW(full_path);
    }

    if (hash) {
        BCryptFinishHash(hash, out_checksum, HASH_SIZE, 0);
        BCryptDestroyHash(hash);
    } else {
        memset(out_checksum, 0, HASH_SIZE);
    }

    free(full_path);
    if (result == RECEIVE_OK && !write_ok) result = RECEIVE_FAILED;
    return result;
}

static bool receive_files(SOCKET sock, const wchar_t* base_dir, Manifest* local) {
    Reader reader = { .sock = sock, .data = malloc(NET_BUFFER_SIZE) };
    char* path = malloc(UINT16_MAX + 1);
    Manifest* added = create_manifest();
    if (!reader.data || !path || !added) {
        printf("Failed to allocate memory for receiving\n");
        free(reader.data);
        free(path);
        free_manifest(added);
        return false;
    }

    bool connected = true;
    uint32_t expected_count = 0;
    uint64_t expected_bytes = 0;
    uint32_t written_count = 0;
    uint32_t failed_count = 0;
    uint64_t bytes_received = 0;
    double start_time = now_seconds();

    connected = read_exact(&reader, &expected_count, 4) && read_exact(&reader, &expected_bytes, 8);
    if (connected) {
        char size_text[32];
        format_file_size(size_text, sizeof(size_text), (double)expected_bytes);
        printf("Server is sending %u files (%s)\n", expected_count, size_text);
    }

    while (connected) {
        uint8_t type;
        if (!read_exact(&reader, &type, 1)) {
            connected = false;
            break;
        }

        if (type == MSG_END) {
            uint32_t sent_count;
            connected = read_exact(&reader, &sent_count, 4);
            break;
        }
        if (type != MSG_FILE) {
            printf("\nReceived unknown message type %u\n", type);
            connected = false;
            break;
        }

        uint64_t size, modified_time;
        uint16_t path_length;
        if (!read_exact(&reader, &size, 8) || !read_exact(&reader, &modified_time, 8)
            || !read_exact(&reader, &path_length, 2) || !read_exact(&reader, path, path_length)) {
            connected = false;
            break;
        }
        path[path_length] = '\0';

        uint8_t checksum[HASH_SIZE];
        ReceiveResult result = receive_file(&reader, base_dir, path, path_length, size, modified_time, checksum, &bytes_received);
        if (result == RECEIVE_CONNECTION_LOST) {
            connected = false;
            break;
        }

        if (result == RECEIVE_OK) {
            written_count++;
            ManifestFile* existing = find_record(local, path);
            if (existing) {
                existing->size = size;
                existing->modified_time = modified_time;
                memcpy(existing->checksum, checksum, HASH_SIZE);
            } else {
                add_record(added, size, modified_time, path, path_length, checksum);
            }
        } else {
            failed_count++;
        }

        if (progress_due()) {
            print_transfer_progress("Receiving", written_count + failed_count, expected_count, bytes_received, expected_bytes, start_time, path);
        }
    }

    if (expected_count > 0) {
        print_transfer_progress("Receiving", written_count + failed_count, expected_count, bytes_received, expected_bytes, start_time, "");
        progress_end();
    }

    // added afterwards so find_record stays sorted
    for (uint32_t i = 0; i < added->file_count; i++) {
        ManifestFile* r = &added->records[i];
        add_record(local, r->size, r->modified_time, r->path, r->path_length, r->checksum);
    }
    sort_manifest(local);
    free_manifest(added);

    if (connected) {
        uint32_t results[2] = { written_count, failed_count };
        connected = send_all(sock, results, sizeof(results));
    }

    char size_text[32];
    format_file_size(size_text, sizeof(size_text), (double)bytes_received);
    printf("Received %u files (%s) in %.2fs", written_count, size_text, now_seconds() - start_time);
    if (failed_count > 0) printf(", %u failed", failed_count);
    printf("\n");

    free(reader.data);
    free(path);
    return connected && failed_count == 0;
}

static SOCKET connect_to_server(const char* address) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP };
    struct addrinfo* addresses;
    int res = getaddrinfo(address, PORT_STRING, &hints, &addresses);
    if (res != 0) {
        printf("Couldn't resolve \"%s\" (code %d)\n", address, res);
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    int last_error = 0;
    for (struct addrinfo* a = addresses; a; a = a->ai_next) {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            continue;
        }
        if (connect(sock, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        last_error = WSAGetLastError();
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);

    if (sock == INVALID_SOCKET) printf("Failed to connect to %s with code %d\n", address, last_error);
    return sock;
}

static int run_client(const wchar_t* base_dir, const char* address) {
    int result = EXIT_FAILURE;
    Manifest* local_manifest = NULL;
    uint8_t* encoded = NULL;

    // create the destination, skipping the \\?\ prefix
    wchar_t* base_copy = _wcsdup(base_dir);
    if (base_copy) {
        create_directories(base_copy, 4, true);
        free(base_copy);
    }
    DWORD attributes = GetFileAttributesW(base_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("Couldn't create the destination folder\n");
        return EXIT_FAILURE;
    }

    SOCKET sock = connect_to_server(address);
    if (sock == INVALID_SOCKET) return EXIT_FAILURE;
    printf("Connected to server\n");

    Manifest* cache = load_cache(base_dir);
    local_manifest = scan_directory(base_dir, cache);
    free_manifest(cache);
    if (!local_manifest) {
        printf("Failed to scan folder\n");
        goto cleanup;
    }

    size_t encoded_size;
    encoded = encode_manifest(local_manifest, &encoded_size);
    if (!encoded) {
        printf("Failed to allocate memory for manifest\n");
        goto cleanup;
    }
    uint64_t manifest_size = encoded_size;
    if (!send_all(sock, &manifest_size, sizeof(manifest_size)) || !send_all(sock, encoded, encoded_size)) goto cleanup;
    free(encoded);
    encoded = NULL;

    if (receive_files(sock, base_dir, local_manifest)) result = EXIT_SUCCESS;

    // save even on failure, received files are already hashed
    if (!save_cache(base_dir, local_manifest)) {
        printf("Warning: couldn't save the cache file, the next scan will hash everything again\n");
    }
    shutdown(sock, SD_SEND);

cleanup:
    closesocket(sock);
    free(encoded);
    free_manifest(local_manifest);
    return result;
}

// ---- test ----

// scan without the cache and check encoding round trips
static int run_test(const wchar_t* base_dir) {
    Manifest* m = scan_directory(base_dir, NULL);
    if (!m) return EXIT_FAILURE;

    size_t encoded_size;
    uint8_t* encoded = encode_manifest(m, &encoded_size);
    Manifest* decoded = encoded ? decode_manifest(encoded, encoded_size) : NULL;

    bool matches = decoded && decoded->file_count == m->file_count;
    for (uint32_t i = 0; matches && i < m->file_count; i++) {
        ManifestFile* a = &m->records[i];
        ManifestFile* b = &decoded->records[i];
        matches = a->size == b->size && a->modified_time == b->modified_time
            && a->path_length == b->path_length && strcmp(a->path, b->path) == 0
            && memcmp(a->checksum, b->checksum, HASH_SIZE) == 0;
    }

    // truncated data must be rejected
    bool rejects_truncated = !encoded || encoded_size <= 9 || decode_manifest(encoded, encoded_size - 1) == NULL;

    printf("Manifest is %zu bytes, round trip %s, truncated data %s\n", encoded_size,
           matches ? "OK" : "MISMATCH", rejects_truncated ? "rejected" : "ACCEPTED");

    free(encoded);
    free_manifest(decoded);
    free_manifest(m);
    return matches && rejects_truncated ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void print_usage(const wchar_t* program) {
    printf("usage:\n"
           "  %ls PATH server          send PATH to a client\n"
           "  %ls PATH sync ADDRESS    receive files from the server at ADDRESS into PATH\n"
           "  %ls PATH test            hash PATH and check the manifest encoding\n",
           program, program, program);
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const wchar_t* command = argv[2];
    wchar_t* base_dir = make_long_path(argv[1]);
    if (!base_dir) {
        printf("Invalid path\n");
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;

    if (wcscmp(command, L"test") == 0) {
        result = run_test(base_dir);
    } else if (wcscmp(command, L"server") == 0 || wcscmp(command, L"sync") == 0) {
        WSADATA wsa_data;
        int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (res != 0) {
            printf("WSAStartup failed with code %d\n", res);
        } else if (wcscmp(command, L"server") == 0) {
            result = run_server(base_dir);
            WSACleanup();
        } else if (argc < 4) {
            print_usage(argv[0]);
            WSACleanup();
        } else {
            char* address = wide_to_utf8(argv[3], -1, NULL);
            if (address) result = run_client(base_dir, address);
            free(address);
            WSACleanup();
        }
    } else {
        print_usage(argv[0]);
    }

    free(base_dir);
    return result;
}
