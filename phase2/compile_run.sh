# Temporary build script, CMake later
astyle_settings="--style=allman --pad-oper --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
astyle --style=allman -n *.cpp $astyle_settings
astyle --style=allman -n *.hpp $astyle_settings
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=missingInclude  --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.cpp
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=missingInclude --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.hpp

rm phase2.out

g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic phase2.cpp -o phase2.out

./phase2.out