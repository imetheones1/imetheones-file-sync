#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "manifest.h"

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
}