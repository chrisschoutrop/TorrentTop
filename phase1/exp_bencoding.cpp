#include <iostream>
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
/*
Testcases from
https://en.wikipedia.org/wiki/Bencode
https://www.bittorrent.org/beps/bep_0003.html

Idea of this file is to make a working experiment for bencode
then move this into a proper boost test+construction later when we
have figured out how to structure everything.

Problem:
- I don't really know what this is supposed to yield eventually.
- I suspect we have to do something with std::variant, but the
    dict in dict seems to mess with the types
- I vaguely know there is something with LL1 grammars and recursive
    decent parsers that may be useful here.
    The "recursive" makes sense since we can have dicts in dicts.
- I don't know what a "grammar" is.
- I don't know what the output has to be; how to do dicts of lists/dicts recursively.

Way forward (what locally seems OK):
- Implement something that can parse basic things like int/byte string
    that way we can at least get a feel for LL1 parsing (per-element decisions)
- How I think this works is we can use a/the stack, whenever we encounter something
    like "i" we call parse_integer().
*/
class Parser {
public:
    int64_t pos;
    Parser() {
        pos=0;

    }
    int parse_integer(const std::string& input) {
        /*
        Example I/O:
            "i0e"       ->  0
            "i42e"      ->  42
            "i-42e"     ->  -42
        */

        return 234234;
    }
};


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
    std::vector<int64_t> results;
    for(const auto& input:inputs) {
        Parser P;
        results.push_back(P.parse_integer(input));
    }
    assert(results.size()==expected.size());
    for(int64_t i=0; i<expected.size(); ++i) {
        assert(results[i]==expected[i]);
    }
}

int main() {
    std::cout<<"test"<<std::endl;
    test_integers();
    return 0;
}

