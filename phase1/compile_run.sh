# Temporary build script, CMake later
rm exp_bencoding.out
g++ -O0 -g3 exp_bencoding.cpp -o exp_bencoding.out
./exp_bencoding.out