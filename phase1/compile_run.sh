# Temporary build script, CMake later
astyle -n *.cpp
astyle -n *.hpp
rm exp_bencoding.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_bencoding.cpp -o exp_bencoding.out
./exp_bencoding.out