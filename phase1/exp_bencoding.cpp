#include <iostream>
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
/*
Testcases from https://en.wikipedia.org/wiki/Bencode
Idea of this file is to make a working experiment for bencode
then move this into a proper boost test+construction later when we
have figured out how to structure everything.
*/

void test_integers() {
    std::vector<std::string> inputs{
        "i0e",
        "i42e",
        "i-42e"
    };
    std::vector<int64_t> expected{
        0,
        42,
        -42
    };
}

int main() {
    std::cout<<"test"<<std::endl;
    return 0;
}

