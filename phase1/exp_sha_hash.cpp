
// ~/dependencies/boost_1_92_0/
// 1.92.0 adds variant support
#include <iostream>
#include <vector>
#include <cstddef>
#include <iomanip>
#include <span>

// Boost.Hash2 headers
#include "boost/hash2/sha1.hpp"
#include "boost/hash2/sha2.hpp"

int main()
{
	// Example piece data in std::vector<std::byte>
	std::vector<std::byte> piece_data =
	{
		std::byte{'T'}, std::byte{'o'}, std::byte{'r'}, std::byte{'r'},
		std::byte{'e'}, std::byte{'n'}, std::byte{'t'}, std::byte{'T'},
		std::byte{'o'}, std::byte{'p'}
	};

	// Construct a span over the raw contiguous bytes
	std::span<const std::byte> data_span{piece_data.data(), piece_data.size()};

	// -------------------------------------------------------------
	// 1. SHA-1 (BitTorrent v1 Piece / Info-Hash Verification)
	// -------------------------------------------------------------
	boost::hash2::sha1_160 sha1_hasher;

	// Hash2 accepts raw pointer + byte count or a byte span
	sha1_hasher.update(data_span.data(), data_span.size());

	// obtain result as std::array<unsigned char, 20>
	auto sha1_digest = sha1_hasher.result();

	// -------------------------------------------------------------
	// 2. SHA-256 (BitTorrent v2 Piece / Merkle Root Verification)
	// -------------------------------------------------------------
	boost::hash2::sha2_256 sha256_hasher;

	sha256_hasher.update(data_span.data(), data_span.size());

	// obtain result as std::array<unsigned char, 32>
	auto sha256_digest = sha256_hasher.result();


	std::cout << "sha1_digest:   " << sha1_digest << std::endl;
	std::cout << "sha256_digest: " << sha256_digest << std::endl;

	assert(to_string(sha1_digest) == "05e41604b98e52b941e23d093ee5006bb12d5c36");
	assert(to_string(sha256_digest) == "a41d3b6a2e60c5758f14b0edeb9ddbae8a116c42871232e8d544599a4cbfafa9");
	return 0;
}