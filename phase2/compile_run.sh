#!/usr/bin/env bash
set -euo pipefail

# Temporary build script, CMake later
GCC_FLAGS="-O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer"

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd ${SCRIPT_DIR}

targets=(
    phase2
    test_HttpTransport
    test_TrackerClient
    exp_boost_asio
    exp_announce
)

for target in "${targets[@]}"
do 
    rm -f ${target}.out  
    g++ ${GCC_FLAGS} "${target}.cpp" -o "${target}.out"
    ./"${target}.out"
done