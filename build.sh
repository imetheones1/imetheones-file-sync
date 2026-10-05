#!/bin/sh
# Builds filesync.exe with MinGW gcc (e.g. from MSYS2)
gcc -std=c11 -O2 -Wall -Wextra -municode -o filesync.exe main.c manifest.c util.c -lws2_32 -lmswsock -lbcrypt "$@"
