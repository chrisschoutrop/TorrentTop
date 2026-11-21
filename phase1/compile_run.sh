#!/usr/bin/env bash
set -euo pipefail

# Temporary build script, CMake later
GCC_FLAGS="-O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer"

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd ${SCRIPT_DIR}

targets=(
    test_BencodeParser
    exp_sha_hash
    exp_read_torrent
)

for target in "${targets[@]}"
do 
    rm -f ${target}.out  
    g++ ${GCC_FLAGS} "${target}.cpp" -o "${target}.out"
    ./"${target}.out"
done


