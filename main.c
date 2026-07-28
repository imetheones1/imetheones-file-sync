#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

#include <winsock.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include "manifest.h"

#define port 2026 // todo choose a good port

#define handle_winsock_error(res, function_name) do { if (res != 0) { printf(function_name " failed with code %d",res); WSACleanup(); return EXIT_FAILURE; }} while (0)

int receive(SOCKET sock, char* buffer, size_t amount_to_read) {
    size_t total_received = 0;

    while (total_received < amount_to_read) {
        size_t remaining = amount_to_read - total_received;
        
        // cap request size just in case
        int request_size = (remaining > 2147483647) ? 2147483647 : (int)remaining;

        int bytes_read = recv(sock, buffer + total_received, request_size, 0);

        if (bytes_read > 0) {
            total_received += bytes_read;
        } 
        else if (bytes_read == 0) {
            // connection closed sucessfully, but not enough bytes
            printf("Connection closed without reading full data\n");
            return -1; 
        } 
        else {
            // network error occured
            printf("Failed to receive data with code %d\n",WSAGetLastError());
            return -2; 
        }
    }

    return 0; 
}

int send_file(SOCKET sock, const char* filepath, size_t amount_to_send) {
    HANDLE file_handle = CreateFileA(
        filepath, 
        GENERIC_READ, 
        FILE_SHARE_READ, 
        NULL, 
        OPEN_EXISTING, 
        FILE_FLAG_SEQUENTIAL_SCAN, 
        NULL
    );

    if (file_handle == INVALID_HANDLE_VALUE) {
        printf("Failed to open file on disk: %lu\n", GetLastError());
        return -3;
    }

    BOOL success = TransmitFile(
        sock, 
        file_handle, 
        (DWORD)amount_to_send, 
        0,
        NULL,
        NULL,
        0
    );

    CloseHandle(file_handle);

    if (!success) {
        printf("TransmitFile failed with code %d\n", WSAGetLastError());
        return -1;
    }

    return 0;
}

int receive_file(SOCKET sock, const char* filepath, size_t amount_to_read) {
    if (amount_to_read == 0) return 0;

    HANDLE file_handle = CreateFileA(
        filepath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (file_handle == INVALID_HANDLE_VALUE) {
        printf("Failed to create file on disk: %lu\n", GetLastError());
        return -3;
    }

    LARGE_INTEGER li_size;
    li_size.QuadPart = amount_to_read;

    HANDLE mapping_handle = CreateFileMappingA(
        file_handle,
        NULL,
        PAGE_READWRITE,
        li_size.HighPart,
        li_size.LowPart,
        NULL
    );

    if (mapping_handle == NULL) {
        printf("Failed to create file mapping: %lu\n", GetLastError());
        CloseHandle(file_handle);
        return -3;
    }

    LPVOID map_view = MapViewOfFile(
        mapping_handle,
        FILE_MAP_WRITE,
        0,
        0,
        amount_to_read
    );

    if (map_view == NULL) {
        printf("Failed to map view of file: %lu\n", GetLastError());
        CloseHandle(mapping_handle);
        CloseHandle(file_handle);
        return -3;
    }

    char* buffer = (char*)map_view;
    size_t total_received = 0;

    while (total_received < amount_to_read) {
        size_t remaining = amount_to_read - total_received;
        
        // cap request size just in case
        int request_size = (remaining > 2147483647) ? 2147483647 : (int)remaining;

        int bytes_read = recv(sock, buffer + total_received, request_size, 0);

        if (bytes_read > 0) {
            total_received += bytes_read;
        } 
        else if (bytes_read == 0) {
            printf("Connection closed without reading full data\n");
            UnmapViewOfFile(map_view);
            CloseHandle(mapping_handle);
            CloseHandle(file_handle);
            return -1; 
        } 
        else {
            printf("Failed to receive data with code %d\n", WSAGetLastError());
            UnmapViewOfFile(map_view);
            CloseHandle(mapping_handle);
            CloseHandle(file_handle);
            return -2; 
        }
    }

    UnmapViewOfFile(map_view);
    CloseHandle(mapping_handle);
    CloseHandle(file_handle);

    return 0;
}

void create_directories(const char* file_path) {
    char temp_path[MAX_PATH];
    strncpy(temp_path, file_path, MAX_PATH);
    temp_path[MAX_PATH - 1] = '\0';

    for (char* p = temp_path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            char temp = *p;
            *p = '\0';
            
            CreateDirectoryA(temp_path, NULL); 
            
            *p = temp;
        }
    }
}

void format_file_size(char *buffer, size_t buffer_size, double bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage:\n %s PATH COMMAND ...",argv[0]);
        exit(EXIT_FAILURE);
    }

    char* path = argv[1];
    char* command = argv[2];
    
    if (strncmp(command,"test",4) == 0){
        Manifest* test_manifest = create_manifest();

        scan_directory(test_manifest,argv[1],"");

        printf("%d\n",test_manifest->file_count);
        for (uint32_t i = 0; i < test_manifest->file_count; i++) {
            printf("%d. %s\n",i+1,test_manifest->records[i].path);
        }

        size_t test_encode_out_size;
        uint8_t* test_encode = encode_manifest(test_manifest, &test_encode_out_size);
        Manifest* test_decode = decode_manifest(test_encode);

        printf("%d\n",test_decode->file_count);
        for (uint32_t i = 0; i < test_decode->file_count; i++) {
            printf("%d. %s\n",i+1,test_decode->records[i].path);
        }

        return EXIT_SUCCESS;
    }

    int res;

    WSADATA wsa_data;
    res = WSAStartup(MAKEWORD(2,2), (LPWSADATA)(&wsa_data));
    handle_winsock_error(res, "WSAStartup");

    if (strncmp(command,"server",6) == 0) {
        printf("server\n");
        SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_socket == INVALID_SOCKET) {
            printf("Socket creation failed with code %d",WSAGetLastError());
            WSACleanup();
            return EXIT_FAILURE;
        }

        struct sockaddr_in addr_in = {
            .sin_family = AF_INET,
            .sin_addr = INADDR_ANY,
            .sin_port = htons(port)
        };
        
        res = bind(server_socket, (struct sockaddr*)&addr_in, sizeof(addr_in));
        handle_winsock_error(res, "bind");

        res = listen(server_socket, SOMAXCONN);
        handle_winsock_error(res, "listen");

        SOCKET client_socket = accept(server_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
            printf("Client socket accept failed with code %d",WSAGetLastError());
            WSACleanup();
            return EXIT_FAILURE;
        }

        printf("Sucessfully established client connection\n");

        closesocket(server_socket);

        Manifest* server_manifest = create_manifest();
        scan_directory(server_manifest, path, "");

        size_t encoded_size = 0;
        res = receive(client_socket, (char*)&encoded_size, sizeof(size_t));
        if (res!=0) {
            WSACleanup();
            closesocket(client_socket);
            return EXIT_FAILURE;
        }

        printf("Manifest size: %zu\n",encoded_size);
        uint8_t* manifest_buffer = malloc(encoded_size+5);
        if (!manifest_buffer) {
            printf("failed to allocate memory for manifest\n");
            closesocket(client_socket);
            WSACleanup();
            return EXIT_FAILURE;
        }
        res = receive(client_socket, (char*)manifest_buffer, encoded_size);
        if (res!=0) {
            closesocket(client_socket);
            WSACleanup();
            return EXIT_FAILURE;
        }

        Manifest* client_manifest = decode_manifest(manifest_buffer);
        free(manifest_buffer);
        if (!client_manifest) {
            printf("Failed to decode manifest.\n");
            closesocket(client_socket);
            WSACleanup();
            return EXIT_FAILURE;
        }
        // printf("%d\n",client_manifest->file_count);
        // for (uint32_t i = 0; i < client_manifest->file_count; i++) {
        //     printf("%d. %s\n",i+1,client_manifest->records[i].path);
        // }

        for (uint32_t i = 0; i < server_manifest->file_count; i++) {
            ManifestFile* s_file = &server_manifest->records[i];
            bool needs_transfer = true;

            for (uint32_t j = 0; j < client_manifest->file_count; j++) {
                ManifestFile* c_file = &client_manifest->records[j];
                if (strncmp(s_file->path,c_file->path,MAX_PATH) == 0) {
                    if (memcmp(s_file->checksum,c_file->checksum, 32) == 0) {
                        needs_transfer = false;
                    }
                    break;
                }
            }

            if (needs_transfer) {
                char full_path[MAX_PATH];
                snprintf(full_path, MAX_PATH, "%s\\%s", path, s_file->path);

                FILE* out_file = fopen(full_path, "rb");
                if (!out_file) {
                    printf("\nFile not found: %s\n",full_path);
                    continue;
                }
                fclose(out_file);

                uint64_t file_size = s_file->size;

                char filesize_text[32];
                format_file_size(filesize_text,sizeof(filesize_text),file_size);
                
                double progress_pct = ((double)(i + 1) / server_manifest->file_count) * 100.0;
                printf("\rSending %u/%u (%.1f%%) | %s (%s)                          ", i + 1, server_manifest->file_count, progress_pct, s_file->path, filesize_text);
                fflush(stdout);

                send(client_socket, (char*)&file_size, sizeof(file_size), 0);
                send(client_socket, (char*)&s_file->path_length, sizeof(s_file->path_length), 0);
                send(client_socket, s_file->path, s_file->path_length, 0);

                send_file(client_socket,full_path,file_size);
                
                uint8_t client_result;
                receive(client_socket, (char*)&client_result, sizeof(uint8_t));
                if (client_result!=0) {
                    printf("\nclient returned failure on file %s\n", s_file->path);
                    WSACleanup();
                    return EXIT_FAILURE;
                }
            }
        }
        
        printf("\n\nAll files transferred successfully\n");
        
        uint64_t zero_size = 0;
        send(client_socket, (char*)&zero_size, sizeof(zero_size), 0);

        shutdown(client_socket, SD_SEND);

        char dummy_buffer[512];
        int bytes_received;
        do {
            bytes_received = recv(client_socket, dummy_buffer, sizeof(dummy_buffer), 0);
            if (bytes_received > 0) {
                
            } else if (bytes_received == 0) {
                printf("connection closed sucesfully\n");
            } else {
                printf("connection closed by error %d\n", WSAGetLastError());
            }
        } while (bytes_received > 0);

        closesocket(client_socket);
    } else if (strncmp(command,"sync",4) == 0) {
        printf("client\n");
        if (argc < 4) {
            printf("Usage: %s PATH sync ADDRESS\n", argv[0]);
            WSACleanup();
            return EXIT_FAILURE;
        }

        char* address = argv[3];

        SOCKET client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client_socket == INVALID_SOCKET) {
            printf("Socket creation failed with code %d",WSAGetLastError());
            WSACleanup();
            return EXIT_FAILURE;
        }

        struct sockaddr_in server_addr = { 
            .sin_family = AF_INET, 
            .sin_port = htons(port) 
        };
        inet_pton(AF_INET, address, &server_addr.sin_addr);

        int res = connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
        handle_winsock_error(res, "connect");

        printf("Sucessfully established server connection\n");

        Manifest* client_manifest = create_manifest();
        scan_directory(client_manifest, path, "");

        size_t encoded_size;
        uint8_t* encoded_buffer = encode_manifest(client_manifest, &encoded_size);
        if (encoded_size > 1024*1024*100) {
            printf("Manifest size > 100mb\n");
            WSACleanup();
            return EXIT_FAILURE;
        }
        send(client_socket, (char*)&encoded_size, sizeof(size_t), 0);
        send(client_socket, (char*)encoded_buffer, encoded_size, 0);
        free(encoded_buffer);

        size_t received_count = 0;
        uint64_t received_size;
        
        do {
            received_size = 0;
            receive(client_socket, (char*)&received_size, sizeof(received_size));
            if (received_size == 0) break;
            
            uint16_t path_length;
            receive(client_socket, (char*)&path_length, sizeof(path_length));
            if (path_length>MAX_PATH) {
                printf("\npath length exceeds limits!\n");
                continue;
            }
            
            char local_path[path_length + 1];
            receive(client_socket, local_path, path_length);
            local_path[path_length] = '\0';
            
            char filesize_text[32];
            format_file_size(filesize_text,sizeof(filesize_text),received_size);
            
            printf("\rReceiving File %zu | %s (%s)                          ", received_count + 1, local_path, filesize_text);
            fflush(stdout);
            
            char full_path[MAX_PATH];
            snprintf(full_path, MAX_PATH, "%s\\%s", path, local_path);
            create_directories(full_path);
            
            int recv_status = receive_file(client_socket, full_path, received_size);
            
            if (recv_status != 0) {
                printf("\nFailed to receive and write file %s\n", full_path);
                uint8_t result = 1; 
                send(client_socket, (char*)&result, sizeof(result), 0);
                continue;
            }

            uint8_t result = 0;
            send(client_socket, (char*)&result, sizeof(result), 0);
            received_count++;
        } while (received_size > 0);

        printf("\n\nSuccesfully received %zu files\n",received_count);

        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
    } else {
        printf("invalid usage: commands are \"server\" or \"sync\"");
    }

    WSACleanup();
}