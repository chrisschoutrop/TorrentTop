#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include <charconv>
#include <map>
#include <memory>
#include <span>
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

class Potato
{
	public:
		virtual ~Potato() = default;
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
		std::vector<std::unique_ptr<Potato>> m_data;
};
class Dict :
	public Potato
{
	public:
		std::map<std::vector<std::byte>,std::unique_ptr<Potato>> m_data;
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
		- Test if m_pos is OK
		*/
	public:
		int64_t m_pos=0;

		// std::span/view such that the Parser does not
		// copy everything, and we can view in chunks
		// ownership is with whatever passed in the input
		// span also decouples this from "vector", we can take in anything
		// as long as it's a bunch of contiguous std::bytes
		std::span<const std::byte> m_input;
		Parser(std::span<const std::byte> input): m_input(input)
		{
		}
		Potato parse()
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
		std::byte peek(const int64_t offset=0)
		{
			int64_t location=offset+m_pos;

			if(location<0 || (size_t)location>=m_input.size())
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
			int64_t start=m_pos+1; //+1 because we have to skip the 'i'
			int64_t end=start;
			int64_t count_digit_or_minus=0;

			if(peek()!=std::byte('i'))
			{
				throw std::invalid_argument("#d213b1");
			}

			for(uint64_t i=start; i<m_input.size(); ++i)
			{
				/*
				This SHOULD only be i,e,0,1,2,3,4,5,6,7,8,9,-
				*/
				std::byte current_character=m_input[i];

				// TODO: Do this with std::find and a lambda instead?
				if(current_character==std::byte('e'))
				{
					end=i;
					break;
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)) || current_character==std::byte('-'))
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

			m_pos=end+1;  // Consume the e
			Integer res;
			const char* start_ptr=reinterpret_cast<const char*>(m_input.data()+start);
			const char* end_ptr=reinterpret_cast<const char*>(m_input.data()+end);
			auto [ptr, ec] =std::from_chars(start_ptr,end_ptr,res.m_data);

			if(ec!=std::errc() || ptr !=end_ptr)
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
			int64_t start_integer=m_pos;
			int64_t end_integer=start_integer;

			for(uint64_t i=m_pos; i<m_input.size(); ++i)
			{
				/*
				Assumptions (Checks for later malformed input handling):
				    only read 0,1,2,3,4,5,6,7,8,9
				    i starts at a digit
				    There exists a :
				*/
				std::byte current_character=m_input[i];

				if(current_character==std::byte(':'))
				{
					end_integer=i;
					break;
				}
			}

			m_pos=end_integer+1;  // Consume the :
			int64_t integer_part;
			const char* start_ptr=reinterpret_cast<const char*>(m_input.data()+start_integer);
			const char* end_ptr=reinterpret_cast<const char*>(m_input.data()+end_integer);
			auto [ptr, ec]=std::from_chars(start_ptr,end_ptr,integer_part);

			if(ec!=std::errc()||ptr!=end_ptr)
			{
				throw std::runtime_error("#c9f1aa");
			}

			Bytes res;

			if(integer_part<0)
			{
				throw std::invalid_argument("#6902a2");
			}

			/*
			I suspect we can also do this with a memcpy and copy exactly integer_part bytes
			but this also smells like some horrible security problem in the making.
			We have to check the sizes.
			*/
			//if(end_copy>m_input.end()) // Doesn't work since if end_copy is past the end it's UB
			if(integer_part>static_cast<int64_t>(m_input.size())-m_pos)
			{
				throw std::invalid_argument("#405932");
			}

			res.m_data.resize(integer_part);

			auto start_copy=m_input.begin()+m_pos;
			auto end_copy=m_input.begin()+m_pos+integer_part;

			std::copy(start_copy,end_copy,res.m_data.begin());
			m_pos=m_pos+integer_part;

			return res;
		}
		List parse_list()
		{
			if(peek()!=std::byte('l'))
			{
				throw std::invalid_argument("#8b1606");
			}

			m_pos++;
			List res;

			while(peek()!=std::byte('e'))
			{
				std::byte current_character=peek();

				if(current_character==std::byte('i'))
				{
					res.m_data.emplace_back(std::make_unique<Integer>(parse_integer()));
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
				{
					res.m_data.emplace_back(std::make_unique<Bytes>(parse_bytes()));
				}
				else if(current_character==std::byte('l'))
				{
					res.m_data.emplace_back(std::make_unique<List>(parse_list()));
				}
				else if(current_character==std::byte('d'))
				{
					res.m_data.emplace_back(std::make_unique<Dict>(parse_dict()));
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
			if(peek()!=std::byte('d'))
			{
				throw std::invalid_argument("#6f44f7");
			}

			m_pos++;
			Dict res;

			while(peek()!=std::byte('e'))
			{
				Bytes key=parse_bytes();
				std::byte current_character=peek();

				if(current_character==std::byte('i'))
				{
					res.m_data.emplace(key.m_data,std::make_unique<Integer>(parse_integer()));
				}
				else if(locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
				{
					res.m_data.emplace(key.m_data,std::make_unique<Bytes>(parse_bytes()));
				}
				else if(current_character==std::byte('l'))
				{
					res.m_data.emplace(key.m_data,std::make_unique<List>(parse_list()));
				}
				else if(current_character==std::byte('d'))
				{
					res.m_data.emplace(key.m_data,std::make_unique<Dict>(parse_dict()));
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
		Parser P(inputs[i]);
		Integer result=P.parse_integer();
		assert(result.m_data==expected[i]);

		int64_t expected_pos=inputs_strings[i].size();
		assert(P.m_pos==expected_pos);
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
		Parser P(inputs[i]);
		Bytes result=P.parse_bytes();
		assert(result.m_data.size()==expected[i].size());
		assert(result.m_data==expected[i]);

		int64_t expected_pos=inputs_strings[i].size();
		assert(P.m_pos==expected_pos);
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
		std::string input_string="l7:bencodei-20ee";
		std::vector<std::byte> input=convert_string_to_bytes(input_string);

		std::vector<std::byte> expected_Bytes=convert_string_to_bytes("bencode");
		int64_t expected_Integer=-20;

		Parser P(input);
		List result=P.parse_list();
		assert(result.m_data.size()==2);
		assert(static_cast<Bytes*>(result.m_data[0].get())->m_data==expected_Bytes);
		assert(static_cast<Integer*>(result.m_data[1].get())->m_data==expected_Integer);
		int64_t expected_pos=input_string.size();
		assert(P.m_pos==expected_pos);
	}
}

void test_dict()
{
	/*
	TODO
	*/
}

int main()
{
	std::cout<<"test"<<std::endl;
	test_integers();
	test_byte_strings1();
	test_list();
	test_dict();
	return 0;
}

