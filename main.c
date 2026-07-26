#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

#include <winsock.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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

int send_file(SOCKET sock, FILE* file, size_t amount_to_send) {
    char chunk_buffer[8192]; // 8kb chunk buffer
    size_t total_sent = 0;

    while (total_sent < amount_to_send) {
        size_t remaining = amount_to_send - total_sent;
        
        size_t bytes_to_read = (remaining < sizeof(chunk_buffer)) ? remaining : sizeof(chunk_buffer);
        size_t bytes_read = fread(chunk_buffer, 1, bytes_to_read, file);
        
        if (bytes_read < bytes_to_read && ferror(file)) {
            printf("Error reading from file on disk\n");
            return -3;
        }

        size_t chunk_sent = 0;
        while (chunk_sent < bytes_read) {
            int bytes_sent = send(sock, chunk_buffer + chunk_sent, (int)(bytes_read - chunk_sent), 0);
            
            if (bytes_sent > 0) {
                chunk_sent += bytes_sent;
            } 
            else {
                printf("Failed to send data with code %d\n", WSAGetLastError());
                return -1;
            }
        }
        
        total_sent += bytes_read;
    }

    return 0;
}

int receive_file(SOCKET sock, FILE* file, size_t amount_to_read) {
    char chunk_buffer[8192]; // 8kb chunk buffer
    size_t total_received = 0;

    while (total_received < amount_to_read) {
        size_t remaining = amount_to_read - total_received;
        
        int request_size = (remaining < sizeof(chunk_buffer)) ? (int)remaining : sizeof(chunk_buffer);

        int bytes_read = recv(sock, chunk_buffer, request_size, 0);

        if (bytes_read > 0) {
            size_t bytes_written = fwrite(chunk_buffer, 1, bytes_read, file);
            if (bytes_written < (size_t)bytes_read) {
                printf("Error writing to file on disk\n");
                return -3;
            }
            
            total_received += bytes_read;
        } 
        else if (bytes_read == 0) {
            printf("Connection closed without reading full data\n");
            return -1; 
        } 
        else {
            printf("Failed to receive data with code %d\n", WSAGetLastError());
            return -2; 
        }
    }

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
        size_t encoded_size = 0;
        res = receive(client_socket, (char*)&encoded_size, sizeof(size_t));
        if (res!=0) {
            WSACleanup();
            closesocket(client_socket);
            return EXIT_FAILURE;
        }
        printf("Manifest size: %zu\n",encoded_size);
        uint8_t* manifest_buffer = malloc(encoded_size+5);
        res = receive(client_socket, (char*)manifest_buffer, encoded_size);
        if (res!=0) {
            WSACleanup();
            closesocket(client_socket);
            return EXIT_FAILURE;
        }

        Manifest* server_manifest = create_manifest();
        scan_directory(server_manifest, path, "");

        Manifest* client_manifest = decode_manifest(manifest_buffer);
        free(manifest_buffer);
        if (!client_manifest) {
            printf("Failed to decode manifest.\n");
            WSACleanup();
            closesocket(client_socket);
            return EXIT_FAILURE;
        }
        printf("%d\n",client_manifest->file_count);
        for (uint32_t i = 0; i < client_manifest->file_count; i++) {
            printf("%d. %s\n",i+1,client_manifest->records[i].path);
        }

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
                if (!out_file) continue;

                uint64_t file_size = s_file->size;

                printf("Sending file %s (size %fkb)\n",s_file->path,(float)file_size/1000);


                send(client_socket, (char*)&file_size, sizeof(file_size), 0);

                send(client_socket, (char*)&s_file->path_length, sizeof(s_file->path_length), 0);
                send(client_socket, s_file->path, s_file->path_length, 0);

                printf("Full file path: %s\n",full_path);

                send_file(client_socket,out_file,file_size);
                fclose(out_file);
                uint8_t client_result;
                receive(client_socket, (char*)&client_result, sizeof(uint8_t));
                if (client_result!=0) {
                    printf("client returned failure\n");
                    WSACleanup();
                    return EXIT_FAILURE;
                }
                printf("\n");
            }
        }
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

        uint64_t received_size;
        do {
            received_size = 0;
            receive(client_socket, (char*)&received_size, sizeof(received_size));
            if (received_size == 0) break;
            uint16_t path_length;
            receive(client_socket, (char*)&path_length, sizeof(path_length));
            if (path_length>MAX_PATH) {
                printf("path length exceeds limits!\n");
                continue;
            }
            char local_path[path_length + 1];
            receive(client_socket, local_path, path_length);
            local_path[path_length] = '\0';
            printf("\nReceiving file %s (size %.2fkb)\n", local_path, (float)received_size/1000);
            char full_path[MAX_PATH];
            snprintf(full_path, MAX_PATH, "%s\\%s", path, local_path);
            create_directories(full_path);
            FILE* in_file = fopen(full_path, "wb");
            if (!in_file) {
                printf("Failed to open %s for writing\n", full_path);
                uint8_t result = 1; 
                send(client_socket, (char*)&result, sizeof(result), 0);
                continue;
            }
            receive_file(client_socket, in_file, received_size);
            fclose(in_file);
            printf("Success\n");
            uint8_t result = 0;
            send(client_socket, (char*)&result, sizeof(result), 0);
        } while (received_size > 0);

        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
    } else {
        printf("invalid usage: commands are \"server\" or \"sync\"");
    }

    WSACleanup();
}