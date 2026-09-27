# Temporary build script, CMake later
astyle_settings="--style=allman --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
astyle --style=allman -n *.cpp $astyle_settings
astyle --style=allman -n *.hpp $astyle_settings
rm exp_bencoding.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_bencoding.cpp -o exp_bencoding.out
./exp_bencoding.out