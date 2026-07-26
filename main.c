#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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
            return EXIT_FAILURE;
        }
        printf("Manifest size: %zu\n",encoded_size);
        uint8_t* manifest_buffer = malloc(encoded_size+5);
        res = receive(client_socket, (char*)manifest_buffer, encoded_size);
        if (res!=0) {
            WSACleanup();
            return EXIT_FAILURE;
        }
        Manifest* client_manifest = decode_manifest(manifest_buffer);
        free(manifest_buffer);
        if (!client_manifest) {
            printf("Failed to decode manifest.\n");
            WSACleanup();
            return EXIT_FAILURE;
        }
        printf("%d\n",client_manifest->file_count);
        for (uint32_t i = 0; i < client_manifest->file_count; i++) {
            printf("%d. %s\n",i+1,client_manifest->records[i].path);
        }
        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
    } else if (strncmp(command,"sync",4) == 0) {
        printf("client\n");
        if (argc < 4) {
            printf("Usage: %s PATH sync ADDRESS");
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
        scan_directory(client_manifest,path,"");

        size_t encoded_size;
        uint8_t* encoded_buffer = encode_manifest(client_manifest, &encoded_size);
        send(client_socket, (char*)&encoded_size, sizeof(size_t), 0);
        send(client_socket, (char*)encoded_buffer, encoded_size, 0);

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
    } else {
        printf("invalid usage: commands are \"server\" or \"sync\"");
    }

    WSACleanup();
}