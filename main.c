#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <winsock.h>
#include <ws2tcpip.h>

#include "manifest.h"

#define port 2026 // todo choose a good port

#define handle_winsock_error(res, function_name) do { if (res != 0) { printf(function_name " failed with code %d",res); WSACleanup(); return EXIT_FAILURE; }} while (0)

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
        printf("server");
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
        
        res = bind(server_socket, &addr_in, sizeof(addr_in));
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
    } else if (strncmp(command,"sync",4) == 0) {
        printf("client");
        if (argc < 4) {
            printf("Usage: %s PATH sync ADDRESS");
            WSACleanup();
            return EXIT_FAILURE;
        }

        char* address = argv[3];

        // todo
    } else {
        printf("invalid usage: commands are \"server\" or \"sync\"");
    }

    WSACleanup();
}