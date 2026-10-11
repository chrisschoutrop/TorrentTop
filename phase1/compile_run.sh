# Temporary build script, CMake later
astyle_settings="--style=allman --pad-oper --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
astyle --style=allman -n *.cpp $astyle_settings
astyle --style=allman -n *.hpp $astyle_settings
#cppcheck --enable=all --suppress=missingIncludeSystem --check-level=exhaustive *.cpp
#cppcheck --enable=all --suppress=missingIncludeSystem --check-level=exhaustive *.hpp
rm test_BencodeParser.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic test_BencodeParser.cpp -o test_BencodeParser.out
./test_BencodeParser.out