#include <iostream>
#include <fstream>
#include <algorithm>

#include "BencodeParser.hpp"

// Boost.Hash2 headers
#include "boost/hash2/digest.hpp"
#include "boost/hash2/sha1.hpp"
#include "boost/hash2/sha2.hpp"

boost::hash2::digest<20> compute_info_hash_v1(const std::vector<std::byte>& torrent_bytes)
{
	// 1. Parse the torrent
	Parser parser(torrent_bytes);
	BencodeValue root = parser.parse();

	// 2. Root must be a dict
	if (!root.is_dict())
	{
		throw std::runtime_error("#1c231c: Torrent root is not a dict");
	}

	const Dict& top = root.get_dict();

	// 3. Look up the "info" key
	Bytes info_key;

	info_key.push_back(static_cast<std::byte>('i'));
	info_key.push_back(static_cast<std::byte>('n'));
	info_key.push_back(static_cast<std::byte>('f'));
	info_key.push_back(static_cast<std::byte>('o'));

	auto it = top.find(info_key);

	if (it == top.end())
	{
		throw std::runtime_error("#39d522: Missing \"info\" key in torrent");
	}

	// 4. The "info" value's .raw span IS the exact byte range to hash
	const std::span<const std::byte>& info_raw = it->second.raw;

	// 5. Hash it (SHA-1 for v1 torrents)
	boost::hash2::sha1_160 hasher;
	hasher.update(info_raw.data(), info_raw.size());
	boost::hash2::digest<20> sha1_digest = hasher.result();

	return sha1_digest;
}

std::vector<std::byte> readFileData(const std::string& name)
{
	//https://www.coniferproductions.com/posts/2022/10/25/reading-binary-files-cpp/

	std::ifstream inputFile(name, std::ios_base::binary);

	// Determine the length of the file by seeking
	// to the end of the file, reading the value of the
	// position indicator, and then seeking back to the beginning.
	inputFile.seekg(0, std::ios_base::end);
	auto length = inputFile.tellg();
	inputFile.seekg(0, std::ios_base::beg);

	// Make a buffer of the exact size of the file and read the data into it.
	std::vector<std::byte> buffer(length);
	inputFile.read(reinterpret_cast<char*>(buffer.data()), length);

	inputFile.close();
	return buffer;
}

int main()
{
	std::cout << "exp_read_torrent.cpp" << std::endl;

	std::vector<std::byte> torrent_bytes = readFileData("archlinux-2026.10.01-x86_64.iso.torrent");

	//Parser P(torrent_bytes);
	//BencodeValue result = P.parse();
	//std::cout << result << std::endl;
	boost::hash2::digest<20> info_hash = compute_info_hash_v1(torrent_bytes);
	assert(to_string(info_hash) == "9b521219bc48550140a666f74a29048ed78e933b");

	/*
	From Torrent decoder:
	We expect this:
	Name    archlinux-2026.10.01-x86_64.iso
	Info Hash   9b521219bc48550140a666f74a29048ed78e933b
	Created On  Oct 01, 2026, 04:51 PM GMT+2
	Created By  mktorrent 1.1
	Comment Arch Linux 2026.10.01 <https://archlinux.org>
	Piece Length    512.00 KB
	Last Piece Length   512.00 KB
	Trackers

	Files
	    archlinux-2026.10.01-x86_64.iso

	We have to hash this part to get the info hash:
	https://stackoverflow.com/questions/28348678/what-exactly-is-the-info-hash-in-a-torrent-file

	*/
}