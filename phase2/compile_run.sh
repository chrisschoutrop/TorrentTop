# Temporary build script, CMake later
astyle_settings="--style=allman --pad-oper --suffix=none --indent=tab --indent-classes --lineend=linux --mode=c --convert-tabs --break-blocks"
astyle --style=allman -n *.cpp $astyle_settings
astyle --style=allman -n *.hpp $astyle_settings
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=missingInclude  --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.cpp
cppcheck --enable=all --suppress=unmatchedSuppression --suppress=missingIncludeSystem --suppress=missingInclude --suppress=unusedFunction --suppress=assertWithSideEffect --check-level=exhaustive *.hpp

rm phase2.out
rm test_HttpTransport.out
rm test_TrackerClient.out
rm exp_boost_asio.out
rm exp_announce.out
  
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion  phase2.cpp -o phase2.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion  test_HttpTransport.cpp -o test_HttpTransport.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion  test_TrackerClient.cpp -o test_TrackerClient.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion  exp_boost_asio.cpp -o exp_boost_asio.out
g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion  exp_announce.cpp -o exp_announce.out
  
./phase2.out
./test_HttpTransport.out
./test_TrackerClient.out
./exp_boost_asio.out
./exp_announce.out
