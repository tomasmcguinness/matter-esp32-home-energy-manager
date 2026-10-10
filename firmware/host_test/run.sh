#!/bin/sh
# Build and run the host-side unit tests (no ESP-IDF needed).
set -e
cd "$(dirname "$0")"
mkdir -p build
gcc -Wall -Wextra -Werror -I../main -o build/test_openadr_slots test_openadr_slots.c ../main/openadr_slots.c
./build/test_openadr_slots
