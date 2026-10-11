#include "BencodeParser.hpp"

class test_Parser
{
	public:
		test_Parser()
		{
			test_integers();
			test_bytes();
			test_list();
			test_dict();
			test_parse();
			test_integer_invalid();
			test_bytes_invalid();
			test_parse_invalid();
			test_random_inputs();
		}
		static std::vector<std::byte> convert_string_to_bytes(const std::string& inputs_string)
		{
			std::vector<std::byte> bytes;
			bytes.resize(inputs_string.size());

			for(uint64_t i_char = 0; i_char < inputs_string.size(); ++i_char)
			{
				bytes[i_char] = static_cast<std::byte>(inputs_string[i_char]);
			}

			return bytes;
		}
		static std::vector<std::vector<std::byte>> convert_strings_to_bytes(const std::vector<std::string>& inputs_strings)
		{
			std::vector<std::vector<std::byte>> inputs(inputs_strings.size());
			for(uint64_t i_str = 0; i_str<inputs_strings.size(); ++i_str)
			{
				const std::string& str = inputs_strings[i_str];
				inputs[i_str] = convert_string_to_bytes(str);
			}
			return inputs;
		}

		static void test_integers()
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
		static void test_bytes()
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

		static void test_list()
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

		static void test_dict()
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

		static void test_parse()
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

		static void test_integer_invalid()
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
		static void test_bytes_invalid()
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
		static void test_parse_invalid()
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

		static void test_random_inputs()
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


};



#ifndef FUZZER
int main()
{
	std::cout << "<test>" << std::endl;
	test_Parser test;
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

	try
	{
		Parser P(input);
		BencodeValue result = P.parse();
	}
	catch(...)
	{

	}

	return 0;
}
#endif
