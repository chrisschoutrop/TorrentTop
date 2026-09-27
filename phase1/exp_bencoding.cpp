#include <iostream>
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include <charconv>
#include <list>
#include <map>
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

Handling recursive mess:
- We can handle inputs that are only bytes or integers with
    std::vector<std::variant<int64_t,std::vector<std::byte>>>
- Problem happens when we have to store lists/dicts, which can recursively
    contain more lists/dicts.
- I'm fully expecting that there is some <100 line solution to parse bencode out there
    but that's not the point of this project.
- ? *Potato where *Potato points to any of:
    - int64_t
    - std::vector<std::byte>
    - std::list<*Potato>
    - std::map<std::vector<std::byte>>,*Potato>
- I vaguely remember this from a hackerrank problem long ago;
    class Potato{
    };
    class Integer : public Potato{
        int64_t m_data;
    };
    class Bytes : public Potato{
        std::vector<std::byte> m_data;
    };
    class List : public Potato{
        std::list<*Potato> m_data;
    };
    class Dict : public Potato{
        std::map<std::vector<std::byte>>,*Potato> m_data
    };
    Where we could make a std::vector<*Potato> which could contain any of the sub-potatoes.
- Note: in bencode, the "base objects" are either int64_t or std::vector<std::byte>.
    So Lists and Dicts MUST contain a std::vector<*Potato> as their m_data "contents" (+keys for dict).
- How I think this helps is that we can now go through the input left-to-right, then
    if we encounter an integer, spawn an Integer subpotato, throw that into the std::vector<*Potato>
- Note: std::list is probably terrible performance, but keeping it in the notes to help my mental model
- We should also use something other than raw pointers in final version
- I think this concept could work

Loot from poking someone about the problem:
Often one includes something to know what the type of an AST node is:
At least if you don't use native rtti
struct AstNode {
    enum Type { ... } type;
    ...
- RTTI = run-time type identification, then we don't have to rely on static_cast
    https://en.wikipedia.org/wiki/Run-time_type_information
*/
class Potato
{
};
class Integer : public Potato
{
public:
    int64_t m_data;
};
class Bytes : public Potato
{
public:
    std::vector<std::byte> m_data;
};
class List : public Potato
{
public:
    std::vector<Potato*> m_data;
};
class Dict : public Potato
{
public:
    std::map<std::vector<std::byte>,Potato*> m_data;
};
class Parser
{
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
    Parser()
    {
        pos=0;

    }
    Integer parse_integer(const std::vector<std::byte>& input)
    {
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
        if(input[pos]!=std::byte('i'))
        {
            throw std::invalid_argument("#d213b1");
        }
        for(uint64_t i=start; i<input.size(); ++i)
        {
            /*
            This SHOULD only be i,e,0,1,2,3,4,5,6,7,8,9,-
            */
            std::byte current_character=input[i];
            if(current_character==std::byte('e'))
            {
                end=i;
                break;
            }
            else if(isdigit(std::to_integer<uint8_t>(current_character)) || current_character==std::byte('-'))
            {
                count_digit_or_minus++;
                continue;
            }
            else
            {
                throw std::invalid_argument("#c2c85a");
            }
        }
        if(count_digit_or_minus==0)
        {
            throw std::invalid_argument("#286991");
        }
        pos=end+1;  // Consume the e
        Integer res;
        std::from_chars(reinterpret_cast<const char*>(input.data()+start),reinterpret_cast<const char*>(input.data()+end),res.m_data);
        return res;
    }
    Bytes parse_bytes(const std::vector<std::byte>& input)
    {
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
        for(uint64_t i=pos; i<input.size(); ++i)
        {
            /*
            Assumptions (Checks for later malformed input handling):
                only read 0,1,2,3,4,5,6,7,8,9
                i starts at a digit
                There exists a :
            */
            std::byte current_character=input[i];
            if(current_character==std::byte(':'))
            {
                end_integer=i;
                break;
            }
        }
        pos=end_integer+1;  // Consume the :
        int64_t integer_part;
        std::from_chars(reinterpret_cast<const char*>(input.data()+start_integer),reinterpret_cast<const char*>(input.data()+end_integer),integer_part);

        Bytes res;
        if(integer_part<0)
        {
            throw std::invalid_argument("#6902a2");
        }
        res.m_data.resize(integer_part);
        //std::vector<std::byte> res(integer_part);
        /*
        I suspect we can also do this with a memcpy and copy exactly integer_part bytes
        but this also smells like some horrible security problem in the making.
        We have to check the sizes.
        */
        auto start_copy=input.begin()+pos;
        auto end_copy=input.begin()+pos+integer_part;
        if(end_copy>input.end())
        {
            throw std::invalid_argument("#405932");
        }
        if(pos<0)
        {
            throw std::invalid_argument("#4de083");
        }
        std::copy(start_copy,end_copy,res.m_data.begin());
        pos=pos+integer_part;

        return res;
    }
    List parse_list(const std::vector<std::byte>& input)
    {
        if(input[pos]!=std::byte('l'))
        {
            throw std::invalid_argument("#8b1606");
        }
        pos++;
        List res;
        while(input[pos]!=std::byte('e'))
        {
            std::byte current_character=input[pos];
            if(current_character==std::byte('i'))
            {
                Integer* new_element=new Integer;
                *new_element=parse_integer(input);
                res.m_data.push_back(new_element);
            }
            else if(isdigit(std::to_integer<uint8_t>(current_character)))
            {
                Bytes* new_element=new Bytes;
                *new_element=parse_bytes(input);
                res.m_data.push_back(new_element);
            }
            else if(current_character==std::byte('l'))
            {
                List* new_element=new List;
                *new_element=parse_list(input);
                res.m_data.push_back(new_element);
            }
            else if(current_character==std::byte('l'))
            {
                Dict* new_element=new Dict;
                *new_element=parse_dict(input);
                res.m_data.push_back(new_element);
            }
            else
            {
                throw std::invalid_argument("#51e02a");
            }
        }

        if(input[pos]!=std::byte('e'))
        {
            throw std::invalid_argument("#b14a04");
        }
        pos++;
        return res;
    }
    Dict parse_dict(const std::vector<std::byte>& input)
    {
        if(input[pos]!=std::byte('d'))
        {
            throw std::invalid_argument("#6f44f7");
        }
        pos++;
        Dict res;
        std::byte current_character=input[pos];

        while(input[pos]!=std::byte('e'))
        {
            Bytes key=parse_bytes(input);

            if(current_character==std::byte('i'))
            {
                Integer* new_element=new Integer;
                *new_element=parse_integer(input);
                res.m_data.insert({key.m_data,new_element});
            }
            else if(isdigit(std::to_integer<uint8_t>(current_character)) || current_character==std::byte('-'))
            {
                Bytes* new_element=new Bytes;
                *new_element=parse_bytes(input);
                res.m_data.insert({key.m_data,new_element});
            }
            else if(current_character==std::byte('l'))
            {
                List* new_element=new List;
                *new_element=parse_list(input);
                res.m_data.insert({key.m_data,new_element});
            }
            else if(current_character==std::byte('l'))
            {
                Dict* new_element=new Dict;
                *new_element=parse_dict(input);
                res.m_data.insert({key.m_data,new_element});
            }
            else
            {
                throw std::invalid_argument("#51e02a");
            }
            pos++;
        }
        if(input[pos]!=std::byte('e'))
        {
            throw std::invalid_argument("#fe2d56");
        }
        pos++;

        return res;
    }
};

std::vector<std::byte> convert_string_to_bytes(const std::string& inputs_string)
{
    std::vector<std::byte> inputs;
    inputs.resize(inputs_string.size());
    for(uint64_t i_char=0; i_char<inputs_string.size(); ++i_char)
    {
        inputs[i_char]=static_cast<std::byte>(inputs_string[i_char]);
    }
    return inputs;
}
std::vector<std::vector<std::byte>> convert_strings_to_bytes(const std::vector<std::string>& inputs_strings)
{
    std::vector<std::vector<std::byte>> inputs(inputs_strings.size());
    for(uint64_t i_str=0; i_str<inputs_strings.size(); ++i_str)
    {
        const std::string& str=inputs_strings[i_str];
        inputs[i_str]=convert_string_to_bytes(str);
    }
    return inputs;
}

void test_integers()
{
    std::vector<std::string> inputs_strings
    {
        "i0e",
        "i42e",
        "i-42e"
    };
    std::vector<std::vector<std::byte>> inputs=convert_strings_to_bytes(inputs_strings);

    std::vector<int64_t> expected
    {
        0,
        42,
        -42
    };
    for(uint64_t i=0; i<expected.size(); ++i)
    {
        Parser P;
        Integer result=P.parse_integer(inputs[i]);
        assert(result.m_data==expected[i]);

        int64_t expected_pos=inputs_strings[i].size();
        assert(P.pos==expected_pos);
    }
}
void test_byte_strings1()
{
    std::vector<std::string> inputs_strings
    {
        "0:",
        "7:bencode",
        "1:\x27",
        "10:horseHorse"
    };
    std::vector<std::string> expected_strings
    {
        "",
        "bencode",
        "\x27",
        "horseHorse"
    };
    std::vector<std::vector<std::byte>> inputs=convert_strings_to_bytes(inputs_strings);
    std::vector<std::vector<std::byte>> expected=convert_strings_to_bytes(expected_strings);

    for(uint64_t i=0; i<expected.size(); ++i)
    {
        Parser P;
        Bytes result=P.parse_bytes(inputs[i]);
        assert(result.m_data.size()==expected[i].size());
        assert(result.m_data==expected[i]);

        int64_t expected_pos=inputs_strings[i].size();
        assert(P.pos==expected_pos);
    }
}
void test_byte_strings2()
{
    /*
    TODO:
    - Test for inputs with non-printable characters
    - Can probably make this MCT test too
    */
}
void test_list()
{
    {
        std::vector<std::byte> input=convert_string_to_bytes("l7:bencodei-20ee");

        List res;
        Bytes* r1=new Bytes;
        r1->m_data=convert_string_to_bytes("bencode");
        res.m_data.push_back(r1);
        Integer* r2=new Integer;
        r2->m_data=-20;
        res.m_data.push_back(r2);

        Parser P;
        List result=P.parse_list(input);
        assert(result.m_data.size()==2);
        assert(static_cast<Bytes*>(result.m_data[0])->m_data==r1->m_data);
        assert(static_cast<Integer*>(result.m_data[1])->m_data==r2->m_data);
    }
}

int main()
{
    std::cout<<"test"<<std::endl;
    test_integers();
    test_byte_strings1();
    test_list();
    return 0;
}

