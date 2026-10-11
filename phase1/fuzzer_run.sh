#https://llvm.org/docs/LibFuzzer.html
clang -DFUZZER=1 -std=c++23 -Wall -Wextra -Wpedantic -g -O3 -fsanitize=fuzzer,address,signed-integer-overflow test_BencodeParser.cpp -o test_BencodeParser.out
mkdir fuzzer_corpus
./test_BencodeParser.out fuzzer_corpus -jobs=$(nproc)