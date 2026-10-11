#https://llvm.org/docs/LibFuzzer.html
clang -DFUZZER=1 -std=c++23 -Wall -Wextra -Wpedantic -g -O3 -fsanitize=fuzzer,address,signed-integer-overflow test_bencoding.cpp -o test_bencoding.out
mkdir fuzzer_corpus
./test_bencoding.out fuzzer_corpus -jobs=$(nproc)