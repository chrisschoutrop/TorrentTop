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
#include <random>
#include <algorithm>

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
	const Integer& get_int() const
	{
		return std::get<Integer>(data);
	}
	const Bytes& get_bytes() const
	{
		return std::get<Bytes>(data);
	}
	const List& get_list()   const
	{
		return std::get<List>(data);
	}
	const Dict& get_dict()   const
	{
		return std::get<Dict>(data);
	}

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
			[&](int64_t val)
			{
				std::cout << "Integer: " << val << "\n";
			},
			[&](const Bytes & bytes)
			{
				std::cout << "Bytes of size: " << bytes.size() << "\n";
			},
			[&](const List & list)
			{
				std::cout << "List of size: " << list.size() << "\n";

				for (const auto& elem : list) print_node(elem);
			},
			[&](const Dict & dict)
			{
				std::cout << "Dict with " << dict.size() << " keys\n";

				for (const auto& [k, v] : dict) print_node(v);
			}
		}, node.data);
	}
};

class Parser
{
	public:
		int64_t m_pos = 0;
		int64_t m_recursion_depth = 0;
		static constexpr int64_t m_max_recursion_depth = 128;
		struct DepthGuard
		{
			/*
			This saves the reference to the m_recursion_depth
			Every time we go deeper in parse_value() it gets incremented
			Every time we leave it gets decremented
			We have to use reference, since m_recursion_depth cannot be accessed
			*/
			int64_t& depth;
			DepthGuard(int64_t& d) : depth(d)
			{
				if (++depth > m_max_recursion_depth)
				{
					throw std::runtime_error("#9d10dd: max recursion depth reached in parse_value()");
				}
			}
			~DepthGuard()
			{
				--depth;
			}
		};

		/*
		std::span/view such that the Parser does not
		copy everything, and we can view in chunks
		ownership is with whatever passed in the input
		span also decouples this from "vector", we can take in anything
		as long as it's a bunch of contiguous std::bytes
		*/
		std::span<const std::byte> m_input;
		explicit Parser(std::span<const std::byte> input): m_input(input)
		{
		}
		BencodeValue parse()
		{
			m_recursion_depth = 0;
			BencodeValue result = parse_value();

			if((size_t)m_pos != m_input.size())
			{
				throw std::invalid_argument("#48a337");
			}

			return result;
		}
		BencodeValue parse_value()
		{
			DepthGuard guard(m_recursion_depth);

			std::byte current_character = peek();

			if (current_character == std::byte('i'))
			{
				return parse_integer();
			}
			else if (current_character == std::byte('l'))
			{
				return parse_list();
			}
			else if (current_character == std::byte('d'))
			{
				return parse_dict();
			}
			else if (locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
			{
				return parse_bytes();
			}
			else
			{
				throw std::invalid_argument("#fecd61");
			}
		}
		std::byte peek(const int64_t offset = 0)
		{
			int64_t location = offset + m_pos;

			if(location < 0 || (size_t)location >= m_input.size())
			{
				throw std::out_of_range("#04c706");
			}

			return m_input[location];
		}
		void consume(size_t count = 1)
		{
			if (m_pos + count > m_input.size())
			{
				throw std::out_of_range("#03c9cf");
			}

			m_pos += count;
		}
		BencodeValue parse_integer()
		{
			if(peek() != std::byte('i'))
			{
				throw std::invalid_argument("#d213b1: i not found");
			}

			consume();

			Integer res;
			res = parse_raw_integer<std::byte('e')>();

			return BencodeValue(res);
		}
		BencodeValue parse_bytes()
		{
			int64_t integer_part = parse_raw_integer<std::byte(':')>();

			if(integer_part < 0)
			{
				throw std::invalid_argument("#6902a2");
			}

			//if(end_copy>m_input.end()) // Doesn't work since if end_copy is past the end it's UB
			if(integer_part > static_cast<int64_t>(m_input.size()) - m_pos)
			{
				throw std::invalid_argument("#405932");
			}

			Bytes res(integer_part);

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
				res.emplace_back(parse_value());
			}

			m_pos++;
			return BencodeValue(res);
		}
		BencodeValue parse_dict()
		{
			if(peek() != std::byte('d'))
			{
				throw std::invalid_argument("#6f44f7");
			}

			m_pos++;
			Dict res;

			while(peek() != std::byte('e'))
			{
				Bytes key = parse_bytes().get_bytes();

				// Check if key is not duplicate
				if(res.contains(key))
				{
					throw std::invalid_argument("#a5546a: duplicate key in dict");
				}

				// Check if key is sorted
				if( res.size() > 0 && std::prev(res.end())->first >= key)
				{
					throw std::invalid_argument("#280070: keys not sorted");
				}

				res.emplace(key, parse_value());
			}

			m_pos++;

			return BencodeValue(res);
		}
	private:
		template <std::byte delimiter>
		requires (delimiter == std::byte(':') || delimiter == std::byte('e'))
		Integer parse_raw_integer()
		{
			const auto it_start = m_input.begin() + m_pos;
			const auto it_end = std::find(it_start, m_input.end(), delimiter);

			if(it_end == m_input.end())
			{
				throw std::invalid_argument("#206d05: e not found");
			}

			size_t length = std::distance(it_start, it_end);

			if(it_end == m_input.end())
			{
				throw std::invalid_argument("#5619cc: integer of length 0");
			}

			m_pos += length;
			consume(); // Consume the ending delimiter
			Integer res;
			const char* start_ptr = reinterpret_cast<const char*>(std::to_address(it_start));
			const char* end_ptr = reinterpret_cast<const char*>(std::to_address(it_end));
			auto [ptr, ec] = std::from_chars(start_ptr, end_ptr, res);

			if(ec != std::errc() || ptr != end_ptr)
			{
				throw std::runtime_error("#7defba: from_chars failed");
			}

			// Spec checks that do parse with from_chars()
			if(length > 1)
			{
				if(start_ptr[0] == '-' && start_ptr[1] == '-')
				{
					throw std::runtime_error("#148aea: double - in integer");
				}

				if(start_ptr[0] == '-' && start_ptr[1] == '0')
				{
					throw std::runtime_error("#148aea: -0 in integer");
				}

				if(start_ptr[0] == '0')
				{
					throw std::runtime_error("#6721cd: leading 0 in integer");
				}
			}

			return res;
		}
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
		assert(result.get_dict().at(expected_key).get_bytes() == expected_val);
		expected_key = convert_string_to_bytes("lang");
		expected_val = convert_string_to_bytes("en");
		assert(result.get_dict().contains(expected_key));
		assert(result.get_dict().at(expected_key).get_bytes() == expected_val);
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
		assert(result.get_dict().at(expected_key1).get_int() == -1);
		assert(result.get_dict().at(expected_key2).as_list() != nullptr);
		assert(result.get_dict().at(expected_key2).get_list()[0].get_int() == 1);
		assert(result.get_dict().at(expected_key2).get_list()[1].get_int() == 2);
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
		std::string input_string = "d1:ad3:barli1ei2ee3:bazd2:xxi277eeee";
		std::vector<std::byte> input = convert_string_to_bytes(input_string);

		Parser P(input);
		BencodeValue result = P.parse();
		assert(result.get_dict().size() == 1);
		assert(result.get_dict().contains(convert_string_to_bytes("a")));
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().contains(convert_string_to_bytes("bar")));
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().contains(convert_string_to_bytes("baz")));
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("bar")).as_list() != nullptr);
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("bar")).get_list()[0].get_int() == 1);
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("bar")).get_list()[1].get_int() == 2);
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("baz")).as_dict() != nullptr);
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("baz")).get_dict().contains(convert_string_to_bytes("xx")));
		assert(result.get_dict().at(convert_string_to_bytes("a")).get_dict().at(convert_string_to_bytes("baz")).get_dict().at(convert_string_to_bytes("xx")).get_int() == 277);
	}
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
	    i007e       leading zero in integer
	    i-0e        negative zero
	    i-3-4e      non-consecutive double -
	*/
	std::vector<std::string> inputs_strings
	{
		"i42",
		"ie",
		"i+42e",
		"i1.5e",
		"i--1e",
		"i12-34e",
		"i42x",
		"i4 2e",
		"i42Ee",
		"il42e",
		"i007e",
		"i-0e",
		"i-e",
		"i-3-4e",
	};
	std::vector<std::vector<std::byte>> inputs = convert_strings_to_bytes(inputs_strings);

	for(uint64_t i = 0; i < inputs.size(); ++i)
	{
		bool exceptionThrown = false;
		try
		{
			//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
			Parser P(inputs[i]);
			BencodeValue result = P.parse_integer();
			std::cout << "#e29e5d: " << inputs_strings[i] << std::endl;
		} catch(...)
		{
			exceptionThrown = true;
		}
		assert(exceptionThrown);
	}
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
	std::vector<std::string> inputs_strings
	{
		"4:spa",
		"a:spam",
		"-1:abc",
		":spam",
		"4:",
		"1.5:abc",
		"999:abc",
	};
	std::vector<std::vector<std::byte>> inputs = convert_strings_to_bytes(inputs_strings);

	for(uint64_t i = 0; i < inputs.size(); ++i)
	{
		bool exceptionThrown = false;
		try
		{
			//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
			Parser P(inputs[i]);
			BencodeValue result = P.parse_bytes();
			std::cout << "#46188c: " << inputs_strings[i] << std::endl;
		} catch(...)
		{
			exceptionThrown = true;
		}
		assert(exceptionThrown);
	}
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

	     i42e                       leading whitespace
	    x42e                        unknown type marker

	    00:abc                      leading zero in byte length
	*/
	std::vector<std::string> inputs_strings
	{
		"",
		"l",
		"d",
		"li1e",
		"l4:spam",
		"l4:spami1e",
		"e",
		"d3:bar",
		"d3:bar3:spam",
		"i42ee",
		"i42e42",
		"4:spame",
		"lee",
		"1:ab",
		"d3:bar3:spamee",
		"d4:spam3:bar4:lang2:ene",
		"d1:ai0e1:ai1e",
		"di1e1:ae",
		"dl4:spame1:ae",
		" i42e",
		"x42e",
		"i007e",
		"i-0e",
		"00:abc",
	};
	std::vector<std::vector<std::byte>> inputs = convert_strings_to_bytes(inputs_strings);

	for(uint64_t i = 0; i < inputs.size(); ++i)
	{
		bool exceptionThrown = false;
		try
		{
			//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
			Parser P(inputs[i]);
			BencodeValue result = P.parse();
			std::cout << "#0a9e8b: " << inputs_strings[i] << std::endl;
		} catch(...)
		{
			exceptionThrown = true;
		}
		assert(exceptionThrown);
	}
}

void test_random_inputs()
{
	/*
	Idea:
	If we have an encoder too we can encode random data
	and see if we break anything.

	For now just check that it doesn't segfault or similar
	*/
	static volatile size_t sink = 0;
	const int64_t N_test = 100;
	std::mt19937 rng(42);
	std::poisson_distribution<int64_t> dist_length(1000);
	std::uniform_int_distribution<u_int8_t> dist_bytes(0x0, 0xff);

	for(int64_t i = 0; i < N_test; ++i)
	{
		std::vector<std::byte> input(dist_length(rng));

		for (size_t j = 0; j < input.size(); j++)
		{
			input[j] = static_cast<std::byte>(dist_bytes(rng));
		}

		try
		{
			Parser P(input);
			BencodeValue result = P.parse();
			sink = sink + sizeof(result);
		}
		catch(...)
		{
			;
		}
	}
}




#ifndef FUZZER
int main()
{
	std::cout << "<test>" << std::endl;
	test_integers();
	test_bytes();
	test_list();
	test_dict();
	test_parse();
	test_integer_invalid();
	test_bytes_invalid();
	test_parse_invalid();
	test_random_inputs();
	std::cout << "</test>" << std::endl;
	return 0;
}
#else
// fuzz_target.cc
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	std::span<const std::byte> input(
	    reinterpret_cast<const std::byte*>(Data),
	    Size
	);
	Parser P(input);

	try
	{
		BencodeValue result = P.parse();
	}
	catch(...)
	{

	}

	return 0;
}
#endif
