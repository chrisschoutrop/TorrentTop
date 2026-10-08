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

// Modern idea using std::variant
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

	// Getters returning std::optional or pointers
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

	// TODO: Check if it exists with is_* ?
	// Currently there can be a nullptr dereference
	Integer get_int() const
	{
		return *std::get_if<Integer>(&data);
	}
	Bytes get_bytes() const
	{
		return *std::get_if<Bytes>(&data);
	}
	List get_list()   const
	{
		return *std::get_if<List>(&data);
	}
	Dict get_dict()   const
	{
		return *std::get_if<Dict>(&data);
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

class Parser
{
		/*
		TODO:
		- Handle malformed inputs
		    - What happens if the input string contains an 'i' but never reaches an 'e'?
		    - Overly long integers?
		    - Mismatch in length & actual length in bytes
		- Clean this up once it works correctly
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
			std::byte current_character = peek();

			if(current_character == std::byte('i'))
			{
				return parse_integer();
			}
			else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
			{
				return parse_bytes();
			}
			else if(current_character == std::byte('l'))
			{
				return parse_list();
			}
			else if(current_character == std::byte('d'))
			{
				return parse_dict();
			}
			else
			{
				throw std::invalid_argument("#34bc24");
			}

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
		BencodeValue parse_integer()
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

			return BencodeValue(res);
		}
		BencodeValue parse_bytes()
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

			return BencodeValue(res);
		}
		BencodeValue parse_list()
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
			return BencodeValue(res);
		}
		BencodeValue parse_dict()
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
				Bytes key = *parse_bytes().as_bytes();
				std::byte current_character = peek();

				if(current_character == std::byte('i'))
				{
					res.emplace(key, *parse_integer().as_int());
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
				{
					res.emplace(key, *parse_bytes().as_bytes());
				}
				else if(current_character == std::byte('l'))
				{
					res.emplace(key, *parse_list().as_list());
				}
				else if(current_character == std::byte('d'))
				{
					res.emplace(key, *parse_dict().as_dict());
				}
				else
				{
					throw std::invalid_argument("#51e02a");
				}
			}

			m_pos++;

			return BencodeValue(res);
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
		BencodeValue result = P.parse_integer();
		assert(result.get_int() == expected[i]);

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
		BencodeValue result = P.parse_bytes();
		assert(result.get_bytes().size() == expected[i].size());
		assert(result.get_bytes() == expected[i]);

		int64_t expected_pos = inputs_strings[i].size();
		assert(P.m_pos == expected_pos);
	}
}

void test_list()
{
	/*
	Test inputs:
	li1ei2ee        ["bencode",-20]
	le              []
	li1ei2ee        [1, 2]
	lli1ei2eei3ee   [[1, 2], 3] (nested)
	*/
	{
		std::string input_string = "l7:bencodei-20ee";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_list();

		assert(result.get_list().size() == 2);

		assert(result.get_list()[0].get_bytes() == convert_string_to_bytes("bencode"));
		assert(result.get_list()[1].get_int() == -20);
	}
	{
		std::string input_string = "le";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_list();

		assert(result.get_list().size() == 0);
	}
	{
		std::string input_string = "li1ei2ee";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_list();

		assert(result.get_list().size() == 2);

		assert(*result.get_list()[0].as_int() == 1);
		assert(*result.get_list()[1].as_int() == 2);
	}
	{
		std::string input_string = "lli1ei2eei3ee";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_list();

		assert(result.get_list().size() == 2);
		const List sublist = result.get_list()[0].get_list();
		assert(sublist.size() == 2);
		assert(sublist[0].get_int() == 1);
		assert(sublist[1].get_int() == 2);
		assert(result.get_list()[1].get_int() == 3);
	}
}

void test_dict()
{
	/*
	TODO
	- dict in dict testcase?
	- every time we do get_dict() this copies a Dict, but as_dict has a lot
	    of * in using it. Maybe it's nice to have something that returns a const&
	    instead of const* so we get the best of both.

	Ideas for inputs:
	    de  {}
	    d3:bar4:spam4:lang2:ene     {"bar": "spam", "lang": "en"}
	    d3:fooi-1e4:spamli1ei2eee   {"foo": -1, "spam": [1, 2]}
	*/
	{
		std::string input_string = "de";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_dict();
		assert(result.get_dict().size() == 0);
	}
	{
		std::string input_string = "d3:bar4:spam4:lang2:ene";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_dict();

		assert(result.get_dict().size() == 2);
		Bytes expected_key;
		Bytes expected_val;
		expected_key = convert_string_to_bytes("bar");
		expected_val = convert_string_to_bytes("spam");
		assert(result.get_dict().contains(expected_key));
		assert(result.get_dict()[expected_key].get_bytes() == expected_val);
		expected_key = convert_string_to_bytes("lang");
		expected_val = convert_string_to_bytes("en");
		assert(result.get_dict().contains(expected_key));
		assert(result.get_dict()[expected_key].get_bytes() == expected_val);
	}
	{
		std::string input_string = "d3:fooi-1e4:spamli1ei2eee";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse_dict();
		assert(result.get_dict().size() == 2);
		Bytes expected_key1, expected_key2;
		expected_key1 = convert_string_to_bytes("foo");
		expected_key2 = convert_string_to_bytes("spam");
		assert(result.get_dict().contains(expected_key1));
		assert(result.get_dict().contains(expected_key2));
		assert(result.get_dict()[expected_key1].get_int() == -1);
		assert(result.get_dict()[expected_key2].as_list() != nullptr);
		assert(result.get_dict()[expected_key2].get_list()[0].get_int() == 1);
		assert(result.get_dict()[expected_key2].get_list()[1].get_int() == 2);
	}
}

void test_parse()
{
	/*
	TODO

	Ideas for inputs:
	    d1:ad3:barli1ei2ee3:bazd2:xxi0eeee      {"a": {"bar": [1, 2], "baz": {"xx": 277}}}
	*/
	{
		std::string input_string = "d1:ad3:barli1ei2ee3:bazd2:xxi277eeee ";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse();
		assert(result.get_dict().size() == 1);
		assert(result.get_dict().contains(convert_string_to_bytes("a")));
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict().contains(convert_string_to_bytes("bar")));
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict().contains(convert_string_to_bytes("baz")));
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("bar")].as_list() != nullptr);
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("bar")].get_list()[0].get_int() == 1);
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("bar")].get_list()[1].get_int() == 2);
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("baz")].as_dict() != nullptr);
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("baz")].get_dict().contains(convert_string_to_bytes("xx")));
		assert(result.get_dict()[convert_string_to_bytes("a")].get_dict()[convert_string_to_bytes("baz")].get_dict()[convert_string_to_bytes("xx")].get_int() == 277);
	}
}

void test_parse_MCT()
{
	/*
	Idea:
	If we have an encoder too we can encode random data
	and see if we break anything.
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
	test_parse();
	return 0;
}

