#https://llvm.org/docs/LibFuzzer.html
clang -DFUZZER=1 -std=c++23 -Wall -Wextra -Wpedantic -g -O1 -fsanitize=fuzzer,address,signed-integer-overflow exp_bencoding.cpp -o exp_bencoding.out
mkdir fuzzer_corpus
./exp_bencoding.out fuzzer_corpus -jobs=$(nproc)