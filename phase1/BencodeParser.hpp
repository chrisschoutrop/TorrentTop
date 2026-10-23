#ifndef BENCODE_PARSER_HPP
#define BENCODE_PARSER_HPP

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
#include <stdexcept>


// Modern idea using std::variant
// We can then access with std::holds_alternative, std::get, std::get_if
struct BencodeValue;
using Integer = int64_t;
using Bytes   = std::vector<std::byte>;
using List    = std::vector<BencodeValue>;
using Dict    = std::map<Bytes, BencodeValue>;

struct BencodeValue
{
	using VariantType = std::variant<std::monostate, Integer, Bytes, List, Dict>;
	VariantType data;

	std::span<const std::byte> raw{};

	/*
	Avoiding empty can be done with setting this =delete
	in that case we can remove the std::monostate as std::variant
	by default constructs whatever is first we need it with =default.
	Having an empty constructor is nice to avoid some code duplication.
	*/
	BencodeValue() = default;
	explicit BencodeValue(VariantType v) : data(std::move(v)) {}

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

	friend std::ostream& operator<< (std::ostream& stream, const BencodeValue& node)
	{
		std::visit(overloaded
		{
			[&](std::monostate)
			{
				stream << "Empty: " << "\n";
			},
			[&](int64_t val)
			{
				stream << "Integer: " << val << "\n";
			},
			[&](const Bytes & bytes)
			{
				stream << "Bytes of size: " << bytes.size() << "\n";
			},
			[&](const List & list)
			{
				stream << "List of size: " << list.size() << "\n";

				for (const auto& elem : list)
				{
					stream << elem;
				}
			},
			[&](const Dict & dict)
			{
				stream << "Dict with " << dict.size() << " keys\n";

				for (const auto& [k, v] : dict)
				{
					stream << v;
				}
			}
		}, node.data);
		return stream;
	}
};

class Parser
{
		friend class test_Parser;
	public:
	private:
		int64_t m_pos = 0;
		std::span<const std::byte> m_input;

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
			explicit DepthGuard(int64_t& d) : depth(d)
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
	public:
		explicit Parser(std::span<const std::byte> input): m_input(input)
		{
			/*
			std::span/view such that the Parser does not
			copy everything, and we can view in chunks
			ownership is with whatever passed in the input
			span also decouples this from "vector", we can take in anything
			as long as it's a bunch of contiguous std::bytes
			*/
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
	private:
		BencodeValue parse_value()
		{
			DepthGuard guard(m_recursion_depth);
			size_t start_pos = m_pos;

			std::byte current_character = peek();
			BencodeValue result;

			if (current_character == std::byte('i'))
			{
				result = parse_integer();
			}
			else if (current_character == std::byte('l'))
			{
				result = parse_list();
			}
			else if (current_character == std::byte('d'))
			{
				result = parse_dict();
			}
			else if (locale_proof_isdigit(std::to_integer<uint8_t>(current_character)))
			{
				result = parse_bytes();
			}
			else
			{
				throw std::invalid_argument("#fecd61");
			}

			result.raw = m_input.subspan(start_pos, m_pos - start_pos);
			return result;
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

			if(length == 0)
			{
				throw std::invalid_argument("#5619cc: integer of length 0");
			}

			m_pos += length;
			consume(); // Consume the ending delimiter
			Integer res;
			// std::byte is unsigned char; from_chars needs char*
			const char* start_ptr = reinterpret_cast<const char*>(std::to_address(it_start));
			const char* end_ptr = reinterpret_cast<const char*>(std::to_address(it_end));
			auto [ptr, ec] = std::from_chars(start_ptr, end_ptr, res, 10);

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
		static bool locale_proof_isdigit(const uint8_t ch)
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
		std::byte peek(const int64_t offset = 0) const
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
};

#endif