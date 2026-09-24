#include <iostream>
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include <charconv>
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
    /*
    TODO:
    - Input should be a vector of bytes, not strings?
    - Parse basic inputs
    - Parse complex inputs
    - Handle malformed inputs
        - What happens if the input string contains an 'i' but never reaches an 'e'?
        - Overly long integers?
        - Mismatch in length & actual length in bytes
    - Clean this up once it works correctly
    - Test if pos is OK
    */
public:
    int64_t pos;
    Parser() {
        pos=0;

    }
    int64_t parse_integer(const std::vector<std::byte>& input) {
        /*
        Example I/O (substring starting from "pos"):
            "i0e"       ->  0
            "i42e"      ->  42
            "i-42e"     ->  -42
        For now parses a SINGLE integer, e.g. NOT i0ei42e
        */
        /*
        Idea:
        - We find the 'i'
        - Skip everything until we hit 'e'
        - Get the string view of whatever is in between, pass to std::to_int() or whatever
        */
        int64_t start=pos+1; //+1 because we have to skip the 'i'
        int64_t end=0;
        int64_t count_digit_or_minus=0;
        if(input[pos]!=std::byte('i')) {
            throw std::invalid_argument("#d213b1");
        }
        for(uint64_t i=start; i<input.size(); ++i) {
            /*
            This SHOULD only be i,e,0,1,2,3,4,5,6,7,8,9,-
            */
            std::byte current_character=input[i];
            if(current_character==std::byte('e')) {
                end=i;
                break;
            } else if(isdigit(std::to_integer<uint8_t>(current_character)) || current_character==std::byte('-')) {
                count_digit_or_minus++;
                continue;
            } else {
                throw std::invalid_argument("#c2c85a");
            }
        }
        if(count_digit_or_minus==0) {
            throw std::invalid_argument("#286991");
        }
        pos=end+1;  // Consume the e
        int64_t res;
        std::from_chars(reinterpret_cast<const char*>(input.data()+start),reinterpret_cast<const char*>(input.data()+end),res);
        return res;
    }
    std::vector<std::byte> parse_byte(const std::vector<std::byte>& input) {
        /*
        Example I/O (substring starting from "pos"):
            "0:",           ->  ""
            "7:bencode",    ->  "bencode"
            "1:\0x27",      ->  "\0x27"
            "10:horseHorse" ->  "horseHorse"

        Note:
            We don't have to loop through all the bytes,
            assuming the length is OK.

        Idea:
            We have to find the part before the ":"
            Convert the part before : to integer
            Copy integer many symbols to output

        TODO:
            - Combine parse_integer's integer reading part with
                what we do here for integer part into a function?
        */
        int64_t start_integer=pos;
        int64_t end_integer=0;
        for(uint64_t i=pos; i<input.size(); ++i) {
            /*
            Assumptions (Checks for later malformed input handling):
                only read 0,1,2,3,4,5,6,7,8,9
                i starts at a digit
                There exists a :
            */
            std::byte current_character=input[i];
            if(current_character==std::byte(':')) {
                end_integer=i;
                break;
            }
        }
        pos=end_integer+1;  // Consume the :
        int64_t integer_part;
        std::from_chars(reinterpret_cast<const char*>(input.data()+start_integer),reinterpret_cast<const char*>(input.data()+end_integer),integer_part);

        std::vector<std::byte> res(integer_part);
        /*
        I suspect we can also do this with a memcpy and copy exactly integer_part bytes
        but this also smells like some horrible security problem in the making.
        We have to check the sizes.
        */
        auto start_copy=input.begin()+pos;
        auto end_copy=input.begin()+pos+integer_part;
        if(end_copy>input.end()) {
            throw std::invalid_argument("#405932");
        }
        if(pos<0) {
            throw std::invalid_argument("#4de083");
        }
        std::copy(start_copy,end_copy,res.begin());
        pos=pos+integer_part;


        return res;
    }
};

std::vector<std::vector<std::byte>> convert_strings_to_bytes(const std::vector<std::string>& inputs_strings) {
    std::vector<std::vector<std::byte>> inputs(inputs_strings.size());
    for(uint64_t i_str=0; i_str<inputs_strings.size(); ++i_str) {
        const std::string& str=inputs_strings[i_str];
        inputs[i_str].resize(str.size());
        for(uint64_t i_char=0; i_char<str.size(); ++i_char) {
            inputs[i_str][i_char]=static_cast<std::byte>(str[i_char]);
        }
    }
    return inputs;
}

void test_integers() {
    std::vector<std::string> inputs_strings{
        "i0e",
        "i42e",
        "i-42e"
    };
    std::vector<std::vector<std::byte>> inputs=convert_strings_to_bytes(inputs_strings);

    std::vector<int64_t> expected{
        0,
        42,
        -42
    };
    for(uint64_t i=0; i<expected.size(); ++i) {
        Parser P;
        int64_t result=P.parse_integer(inputs[i]);
        assert(result==expected[i]);

        int64_t expected_pos=inputs_strings[i].size();
        assert(P.pos==expected_pos);
    }
}
void test_byte_strings1() {
    std::vector<std::string> inputs_strings{
        "0:",
        "7:bencode",
        "1:\x27",
        "10:horseHorse"
    };
    std::vector<std::string> expected_strings{
        "",
        "bencode",
        "\x27",
        "horseHorse"
    };
    std::vector<std::vector<std::byte>> inputs=convert_strings_to_bytes(inputs_strings);
    std::vector<std::vector<std::byte>> expected=convert_strings_to_bytes(expected_strings);

    for(uint64_t i=0; i<expected.size(); ++i) {
        Parser P;
        std::vector<std::byte> result=P.parse_byte(inputs[i]);
        assert(result.size()==expected[i].size());
        assert(result==expected[i]);

        int64_t expected_pos=inputs_strings[i].size();
        assert(P.pos==expected_pos);
    }
}
void test_byte_strings2() {
    /*
    TODO:
    - Test for inputs with non-printable characters
    - Can probably make this MCT test too
    */
}

int main() {
    std::cout<<"test"<<std::endl;
    test_integers();
    test_byte_strings1();
    return 0;
}

