#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include <charconv>
#include <map>
#include <memory>
#include <span>
#include <variant>
#include <optional>
/*
Notes from Dave 27-Sep-2026:
[X] Why isn't input just a field in the parser?
[X] Use std::span instead of const std::vector&.
[X] Add a peek(offset = 0) method instead of input.at(pos).
[ ] You can just use std::find to locate the next 'e'. -> I think the loops do more
    than just finding, can maybe do with lambda
[X] Why not use a switch to call the correct variant and put that in a top-level parse function?
[P] Exceptions :( -> Planned: Error handling later in something outside of this to catch all throws from the Parser.
*/
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
bool locale_proof_isdigit(const uint8_t ch)
{
	/*
	From: https://en.cppreference.com/cpp/string/byte/isdigit
	    isdigit and isxdigit are the only standard narrow character classification
	    functions that are not affected by the currently installed C locale.
	    although some implementations (e.g. Microsoft in 1252 codepage) may classify
	    additional single-byte characters as digits.
	*/
	return (ch >= '0' && ch <= '9');
}

// Old version using inheritance
// class Potato
// {
//  public:
//      virtual ~Potato() = default;
// };
// class Integer : public Potato
// {
//  public:
//      int64_t m_data;
// };
// class Bytes : public Potato
// {
//  public:
//      std::vector<std::byte> m_data;
// };
// class List : public Potato
// {
//  public:
//      std::vector<std::unique_ptr<Potato>> m_data;
// };
// class Dict :
//  public Potato
// {
//  public:
//      std::map<std::vector<std::byte>, std::unique_ptr<Potato >> m_data;
// };
// Modern idea using std::variant, to be investigated
// We can then access with std::holds_alternative, std::get, std::get_if
struct BencodeValue;
using Integer = int64_t;
using Bytes   = std::vector<std::byte>;
using List    = std::vector<BencodeValue>;
using Dict    = std::map<Bytes, BencodeValue>;

struct BencodeValue
{
	using VariantType = std::variant<Integer, Bytes, List, Dict>;
	VariantType data;

	BencodeValue() = default;
	BencodeValue(VariantType v) : data(std::move(v)) {}

	// Checkers
	bool is_int()   const
	{
		return std::holds_alternative<Integer>(data);
	}
	bool is_bytes() const
	{
		return std::holds_alternative<Bytes>(data);
	}
	bool is_list()  const
	{
		return std::holds_alternative<List>(data);
	}
	bool is_dict()  const
	{
		return std::holds_alternative<Dict>(data);
	}

	// Safe getters returning std::optional or pointers
	const Integer* as_int() const
	{
		return std::get_if<Integer>(&data);
	}
	const Bytes* as_bytes() const
	{
		return std::get_if<Bytes>(&data);
	}
	const List* as_list()   const
	{
		return std::get_if<List>(&data);
	}
	const Dict* as_dict()   const
	{
		return std::get_if<Dict>(&data);
	}

	// String helper (convenient for torrent keys and string values)
	std::optional<std::string> as_string() const
	{
		if (auto * b = as_bytes())
		{
			return std::string(reinterpret_cast<const char*>(b->data()), b->size());
		}

		return std::nullopt;
	}
};

// Helper struct for inline pattern matching
// https://www.cppstories.com/2019/02/2lines3featuresoverload.html/
template<class... Ts> struct overloaded : Ts...
{
	using Ts::operator()...;
};
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

void print_node(const BencodeValue& node)
{
	std::visit(overloaded
	{
		[](int64_t val)
		{
			std::cout << "Integer: " << val << "\n";
		},
		[](const Bytes & bytes)
		{
			std::cout << "Bytes of size: " << bytes.size() << "\n";
		},
		[](const List & list)
		{
			std::cout << "List of size: " << list.size() << "\n";

			for (const auto& elem : list) print_node(elem);
		},
		[](const Dict & dict)
		{
			std::cout << "Dict with " << dict.size() << " keys\n";

			for (const auto& [k, v] : dict) print_node(v);
		}
	}, node.data);
}

/*
Modern idea using std::variant, to be investigated
We can then access with std::holds_alternative, std::get, std::get_if
struct BencodeValue;
using Integer = int64_t;
using Bytes   = std::vector<std::byte>;
using List    = std::vector<BencodeValue>;
using Dict    = std::map<Bytes, BencodeValue>;
struct BencodeValue {
    using VariantType = std::variant<
        Integer,
        Bytes,
        List,
        Dict
    >;

    VariantType data;

    BencodeValue() = default;
    BencodeValue(VariantType v) : data(std::move(v)) {}
};

// Helper struct for inline pattern matching
// https://www.cppstories.com/2019/02/2lines3featuresoverload.html/
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

void print_node(const BencodeValue& node) {
    std::visit(overloaded {
        [](int64_t val) {
            std::cout << "Integer: " << val << "\n";
        },
        [](const Bytes& bytes) {
            std::cout << "Bytes of size: " << bytes.size() << "\n";
        },
        [](const List& list) {
            std::cout << "List of size: " << list.size() << "\n";
            for (const auto& elem : list) print_node(elem);
        },
        [](const Dict& dict) {
            std::cout << "Dict with " << dict.size() << " keys\n";
            for (const auto& [k, v] : dict) print_node(v);
        }
    }, node.data);
}

struct BencodeValue {
    using VariantType = std::variant<Integer, Bytes, List, Dict>;
    VariantType data;

    BencodeValue() = default;
    BencodeValue(VariantType v) : data(std::move(v)) {}

    // Checkers
    bool is_int()   const { return std::holds_alternative<Integer>(data); }
    bool is_bytes() const { return std::holds_alternative<Bytes>(data); }
    bool is_list()  const { return std::holds_alternative<List>(data); }
    bool is_dict()  const { return std::holds_alternative<Dict>(data); }

    // Safe getters returning std::optional or pointers
    const Integer* as_int() const { return std::get_if<Integer>(&data); }
    const Bytes* as_bytes() const { return std::get_if<Bytes>(&data); }
    const List* as_list()   const { return std::get_if<List>(&data); }
    const Dict* as_dict()   const { return std::get_if<Dict>(&data); }

    // String helper (convenient for torrent keys and string values)
    std::optional<std::string> as_string() const {
        if (auto* b = as_bytes()) {
            return std::string(reinterpret_cast<const char*>(b->data()), b->size());
        }
        return std::nullopt;
    }
};

void test_list() {
    std::string input_string = "l7:bencodei-20ee";
    std::vector<std::byte> input = convert_string_to_bytes(input_string);

    Parser P(input);
    BencodeValue result = P.parse_list();

    // Clean, safe access using helper getters or std::get
    const List& list = std::get<List>(result.data);
    assert(list.size() == 2);

    assert(list[0].as_string() == "bencode");
    assert(*list[1].as_int() == -20);
}
*/

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
		- Test if m_pos is OK
		*/
	public:
		int64_t m_pos = 0;

		// std::span/view such that the Parser does not
		// copy everything, and we can view in chunks
		// ownership is with whatever passed in the input
		// span also decouples this from "vector", we can take in anything
		// as long as it's a bunch of contiguous std::bytes
		std::span<const std::byte> m_input;
		Parser(std::span<const std::byte> input): m_input(input)
		{
		}
		BencodeValue parse()
		{
			// TODO, switch to call correct function for parsing entire bencoded input
			/*
			TODO Qwen warned for this problem:
			    The one thing to fix before you implement parse(): Potato parse() returns
			    by value, and its first real line — return parse_list(); — will slice.
			    Copying a List into a Potato keeps only the base subobject;
			    the vector<unique_ptr<Potato>> (i.e. the entire parsed tree) is destroyed.
			    It compiles clean, no warning, silent data loss — the classic
			    polymorphic-return trap. The top-level parse must return ownership,
			    the same way List/Dict already store children:
			    std::unique_ptr<Potato> parse();
			    The body is the same if/else chain you already wrote in parse_list (a real
			    switch works too — it needs an integer type, so
			    switch(std::to_integer<unsigned char>(peek())) with the digit case in default).
			    Small related point: the return {} stub compiles (a bare Potato),
			    but it's a silent "unimplemented"; throw std::logic_error("not implemented");
			    fails loudly instead — and add <stdexcept> if it doesn't come in transitively.
			*/
			return {};
		}
		std::byte peek(const int64_t offset = 0)
		{
			int64_t location = offset + m_pos;

			if(location < 0 || (size_t)location >= m_input.size())
			{
				throw std::invalid_argument("#04c706");
			}

			return m_input[location];
		}
		Integer parse_integer()
		{
			/*
			Example I/O (substring starting from "m_pos"):
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
			/*
			Malformed input check ideas
			"i42" (no e)
			"42" (no :) in parse_bytes
			"i-e"
			"i042e" (leading zero)  should parse fine as 42, but bencode says invalid
			*/
			int64_t start = m_pos + 1; //+1 because we have to skip the 'i'
			int64_t end = start;
			int64_t count_digit_or_minus = 0;

			if(peek() != std::byte('i'))
			{
				throw std::invalid_argument("#d213b1");
			}

			for(uint64_t i = start; i < m_input.size(); ++i)
			{
				/*
				This SHOULD only be i,e,0,1,2,3,4,5,6,7,8,9,-
				*/
				std::byte current_character = m_input[i];

				// TODO: Do this with std::find and a lambda instead?
				if(current_character == std::byte('e'))
				{
					end = i;
					break;
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)) || current_character == std::byte('-'))
				{
					count_digit_or_minus++;
					continue;
				}
				else
				{
					throw std::invalid_argument("#c2c85a");
				}
			}

			if(count_digit_or_minus == 0)
			{
				throw std::invalid_argument("#286991");
			}

			m_pos = end + 1; // Consume the e
			Integer res;
			const char* start_ptr = reinterpret_cast<const char*>(m_input.data() + start);
			const char* end_ptr = reinterpret_cast<const char*>(m_input.data() + end);
			auto [ptr, ec] = std::from_chars(start_ptr, end_ptr, res);

			if(ec != std::errc() || ptr != end_ptr)
			{
				throw std::runtime_error("#7defba");
			}

			return res;
		}
		Bytes parse_bytes()
		{
			/*
			Example I/O (substring starting from "m_pos"):
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
			int64_t start_integer = m_pos;
			int64_t end_integer = start_integer;

			for(uint64_t i = m_pos; i < m_input.size(); ++i)
			{
				/*
				Assumptions (Checks for later malformed input handling):
				    only read 0,1,2,3,4,5,6,7,8,9
				    i starts at a digit
				    There exists a :
				*/
				std::byte current_character = m_input[i];

				if(current_character == std::byte(':'))
				{
					end_integer = i;
					break;
				}
			}

			m_pos = end_integer + 1; // Consume the :
			int64_t integer_part;
			const char* start_ptr = reinterpret_cast<const char*>(m_input.data() + start_integer);
			const char* end_ptr = reinterpret_cast<const char*>(m_input.data() + end_integer);
			auto [ptr, ec] = std::from_chars(start_ptr, end_ptr, integer_part);

			if(ec != std::errc() || ptr != end_ptr)
			{
				throw std::runtime_error("#c9f1aa");
			}

			Bytes res;

			if(integer_part < 0)
			{
				throw std::invalid_argument("#6902a2");
			}

			/*
			I suspect we can also do this with a memcpy and copy exactly integer_part bytes
			but this also smells like some horrible security problem in the making.
			We have to check the sizes.
			*/
			//if(end_copy>m_input.end()) // Doesn't work since if end_copy is past the end it's UB
			if(integer_part > static_cast<int64_t>(m_input.size()) - m_pos)
			{
				throw std::invalid_argument("#405932");
			}

			res.resize(integer_part);

			auto start_copy = m_input.begin() + m_pos;
			auto end_copy = m_input.begin() + m_pos + integer_part;

			std::copy(start_copy, end_copy, res.begin());
			m_pos = m_pos + integer_part;

			return res;
		}
		List parse_list()
		{
			if(peek() != std::byte('l'))
			{
				throw std::invalid_argument("#8b1606");
			}

			m_pos++;
			List res;

			while(peek() != std::byte('e'))
			{
				std::byte current_character = peek();

				if(current_character == std::byte('i'))
				{
					res.emplace_back(parse_integer());
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
				{
					res.emplace_back(parse_bytes());
				}
				else if(current_character == std::byte('l'))
				{
					res.emplace_back(parse_list());
				}
				else if(current_character == std::byte('d'))
				{
					res.emplace_back(parse_dict());
				}
				else
				{
					throw std::invalid_argument("#51e02a");
				}
			}

			m_pos++;
			return res;
		}
		Dict parse_dict()
		{
			/*
			TODO:
			- std::map silently reorders unsorted keys. BEP3 requires dictionary keys in ascending byte order,
			    so d3:woof3:dog3:cow3:mooe is spec-invalid but your parser accepts it.
			- Duplicate keys are silently dropped: map::emplace is a no-op for an existing key (the freshly-built
			    value is created and immediately destroyed). You'll want a reject, not a drop.
			*/
			if(peek() != std::byte('d'))
			{
				throw std::invalid_argument("#6f44f7");
			}

			m_pos++;
			Dict res;

			while(peek() != std::byte('e'))
			{
				Bytes key = parse_bytes();
				std::byte current_character = peek();

				if(current_character == std::byte('i'))
				{
					res.emplace(key, parse_integer());
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
				{
					res.emplace(key, parse_bytes());
				}
				else if(current_character == std::byte('l'))
				{
					res.emplace(key, parse_list());
				}
				else if(current_character == std::byte('d'))
				{
					res.emplace(key, parse_dict());
				}
				else
				{
					throw std::invalid_argument("#51e02a");
				}
			}

			m_pos++;

			return res;
		}
};

std::vector<std::byte> convert_string_to_bytes(const std::string& inputs_string)
{
	std::vector<std::byte> inputs;
	inputs.resize(inputs_string.size());

	for(uint64_t i_char = 0; i_char < inputs_string.size(); ++i_char)
	{
		inputs[i_char] = static_cast<std::byte>(inputs_string[i_char]);
	}

	return inputs;
}
std::vector<std::vector<std::byte>> convert_strings_to_bytes(const std::vector<std::string>& inputs_strings)
{
	std::vector<std::vector<std::byte>> inputs(inputs_strings.size());
	for(uint64_t i_str = 0; i_str<inputs_strings.size(); ++i_str)
	{
		const std::string& str = inputs_strings[i_str];
		inputs[i_str] = convert_string_to_bytes(str);
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
	std::vector<std::vector<std::byte>> inputs = convert_strings_to_bytes(inputs_strings);

	std::vector<int64_t> expected
	{
		0,
		42,
		-42
	};

	for(uint64_t i = 0; i < expected.size(); ++i)
	{
		Parser P(inputs[i]);
		Integer result = P.parse_integer();
		assert(result == expected[i]);

		int64_t expected_pos = inputs_strings[i].size();
		assert(P.m_pos == expected_pos);
	}
}
void test_bytes()
{
	/*
	TODO:
	\0
	*/
	std::vector<std::string> inputs_strings
	{
		"0:",
		"7:bencode",
		"1:\x27",
		"10:horseHorse",
		"13:hello world!!"
	};
	std::vector<std::string> expected_strings
	{
		"",
		"bencode",
		"\x27",
		"horseHorse",
		"hello world!!"
	};
	std::vector<std::vector<std::byte>> inputs = convert_strings_to_bytes(inputs_strings);
	std::vector<std::vector<std::byte>> expected = convert_strings_to_bytes(expected_strings);

	for(uint64_t i = 0; i < expected.size(); ++i)
	{
		Parser P(inputs[i]);
		Bytes result = P.parse_bytes();
		assert(result.size() == expected[i].size());
		assert(result == expected[i]);

		int64_t expected_pos = inputs_strings[i].size();
		assert(P.m_pos == expected_pos);
	}
}
void test_list()
{
	/*
	Test inputs:
	le              []
	li1ei2ee        [1, 2]
	l4:spami42ee    ["spam", 42]
	lli1ei2eei3ee   [[1, 2], 3] (nested)
	*/
	// {
	//  std::string input_string = "l7:bencodei-20ee";
	//  std::vector<std::byte> input = convert_string_to_bytes(input_string);

	//  std::vector<std::byte> expected_Bytes = convert_string_to_bytes("bencode");
	//  int64_t expected_Integer = -20;

	//  Parser P(input);
	//  List result = P.parse_list();
	//  assert(result.m_data.size() == 2);
	//  assert(static_cast<Bytes*>(result.m_data[0].get())->m_data == expected_Bytes);
	//  assert(static_cast<Integer*>(result.m_data[1].get())->m_data == expected_Integer);
	//  int64_t expected_pos = input_string.size();
	//  assert(P.m_pos == expected_pos);
	// }

	//TODO
}

void test_dict()
{
	/*
	TODO

	Ideas for inputs:
	    de  {}
	    d3:bar4:spam4:lang2:ene     {"bar": "spam", "lang": "en"}
	    d3:cow3:moo4:spam3:bare     {"cow": "moo", "spam": "bar"}
	    d3:fooi-1e4:spamli1ei2eee   {"foo": -1, "spam": [1, 2]}
	*/
}

void test_parse()
{
	/*
	TODO

	Ideas for inputs:
	    d1:ad3:barli1ei2ee3:bazd2:xxi0eeee      {"a": {"bar": [1, 2], "baz": {"xx": 0}}}
	    d4:colsl4:spam4:eggs5:applee3:numi42ee  {"cols": ["spam", "eggs", "apple"], "num": 42}
	    d7:content6:banana4:name6:bananee       {"content": "banana", "name": "banane"}
	*/
}

void test_integer_invalid()
{
	/*
	    i42         missing terminator e
	    ie          no digits
	    i+42e       plus sign not allowed
	    i1.5e       not an integer
	    i--1e       double minus
	    i12-34e     minus in the middle
	    i42x        wrong terminator
	    i4 2e       whitespace inside
	    i42Ee       uppercase E
	    il42e       non-digit after i
	*/
}
void test_bytes_invalid()
{
	/*
	    4:spa       data shorter than declared length
	    a:spam      non-numeric length
	    -1:abc      negative length
	    :spam       empty length
	    4:          length declared, no data
	    1.5:abc     decimal length
	    999:abc     length exceeds available data

	*/
}
void test_parse_invalid()
{
	/*
	    (empty string)              no value at all
	    l                           list with no terminator
	    d                           dict with no terminator
	    li1e                        list missing closing e
	    l4:spam                     list missing closing e
	    l4:spami1e                  outer list unterminated
	    e                           stray terminator used as a value
	    d3:bar                      key with no value, unterminated
	    d3:bar3:spam                dict missing closing e

	    i42ee                       extra e after complete integer
	    i42e42                      junk after complete integer
	    4:spame                     junk after complete byte string
	    lee                         extra e after complete list
	    1:ab                        length says 1, extra b left over
	    d3:bar3:spamee              extra e after complete dict
	    d4:spam3:bar4:lang2:ene     keys not sorted (spam before bar)

	    d1:ai0e1:ai1e               duplicate key a
	    di1e1:ae                    non-byte-string key (integer)
	    dl4:spame1:ae               non-byte-string key (list)

	    i42e                        leading whitespace
	    x42e                        unknown type marker

	    i007e                       leading zero in integer
	    i-0e                        negative zero
	    00:abc                      leading zero in byte length
	*/
}

int main()
{
	std::cout << "test" << std::endl;
	test_integers();
	test_bytes();
	test_list();
	test_dict();
	return 0;
}

