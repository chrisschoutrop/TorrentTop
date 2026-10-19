# Temporary build script, CMake later
astyle_settings="--style=allman --pad-oper --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
astyle --style=allman -n *.cpp $astyle_settings
astyle --style=allman -n *.hpp $astyle_settings
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.cpp
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.hpp

#rm test_BencodeParser.out
#rm exp_read_torrent.out
rm exp_sha_hash.out

#g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic test_BencodeParser.cpp -o test_BencodeParser.out
#g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_read_torrent.cpp -o exp_read_torrent.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_sha_hash.cpp -o exp_sha_hash.out

#./test_BencodeParser.out
#./exp_read_torrent.out
./exp_sha_hash.out