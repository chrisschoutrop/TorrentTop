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

echo "=== Checking for forbidden .c or .h files ==="
# Read null-delimited results into a bash array
mapfile -d '' forbidden_files < <(find_files -name "*.c" -o -name "*.h")

if (( ${#forbidden_files[@]} > 0 )); then
    echo "ERROR: Found C-style .c or .h files:" >&2
    printf '  %s\n' "${forbidden_files[@]}" >&2
    exit 1
fi
echo "OK: No .c or .h files found."

echo "=== Running astyle on .cpp and .hpp files ==="
# Note: Adjust astyle flags (or add --options=your_astylerc) as needed
find_files -name "*.cpp" -o -name "*.hpp" | xargs -0 -r astyle $ASTYLE_SETTINGS

echo "=== Running cppcheck on .cpp and .hpp files ==="
# --error-exitcode=1 ensures the script fails if cppcheck finds an issue
find_files -name "*.cpp" -o -name "*.hpp" | xargs -0 -r cppcheck \
    --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    --std=c++23 \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=assertWithSideEffect \
    --suppress=unreadVariable

echo "=== Checking duplicated hex codes ==="
python3 check_hex_codes.py

echo "=== Checking for dangerous C functions and non-RAII patterns ==="
FORBIDDEN_REGEX='\b(strcpy|strncpy|strcat|strncat|sprintf|vsprintf|gets|scanf|fscanf|sscanf|strtok|atoi|atol|atof|system|tmpnam|tempnam|new|delete)\s*\('

if find_files -name "*.cpp" -o -name "*.hpp" | xargs -0 -r grep -En "$FORBIDDEN_REGEX"; then
    echo "ERROR: Found dangerous C functions or non-RAII manual resource management." >&2
    exit 1
fi

echo "=== Check that all are OK ==="
bash phase1/compile_run.sh
bash phase2/compile_run.sh

echo "=== End of checks ==="