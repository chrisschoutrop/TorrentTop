#!/usr/bin/env bash
set -euo pipefail

# CONSTANTS
ASTYLE_SETTINGS="--style=allman --pad-oper --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
# Directories to ignore
PRUNE_DIRS=( -name ".git" -o -name "build" -o -name "cmake-build-*" -o -name "third_party" -o -name "external" )

# Helper function to find source files while skipping pruned directories
find_files() {
    find . \( "${PRUNE_DIRS[@]}" \) -prune -o -type f \( "$@" \) -print0
}

echo "=== 1. Checking for forbidden .c or .h files ==="
# Read null-delimited results into a bash array
mapfile -d '' forbidden_files < <(find_files -name "*.c" -o -name "*.h")

if (( ${#forbidden_files[@]} > 0 )); then
    echo "ERROR: Found C-style .c or .h files:" >&2
    printf '  %s\n' "${forbidden_files[@]}" >&2
    exit 1
fi
echo "OK: No .c or .h files found."

echo "=== 2. Running astyle on .cpp and .hpp files ==="
# Note: Adjust astyle flags (or add --options=your_astylerc) as needed
find_files -name "*.cpp" -o -name "*.hpp" | xargs -0 -r astyle $ASTYLE_SETTINGS

echo "=== 3. Running cppcheck on .cpp and .hpp files ==="
# --error-exitcode=1 ensures the script fails if cppcheck finds an issue
find_files -name "*.cpp" -o -name "*.hpp" | xargs -0 -r cppcheck \
    --enable=warning,style,performance,portability \
    --std=c++20 \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=assertWithSideEffect \
    --suppress=unreadVariable

echo "=== 4. Checking duplicated hex codes ==="
python3 check_hex_codes.py

echo "=== End of checks ==="