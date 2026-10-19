#include <iostream>
#include <fstream>
#include <algorithm>

#include "BencodeParser.hpp"


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
	std::cout << "exp_read_torrent" << std::endl;

	std::vector<std::byte> file = readFileData("archlinux-2026.10.01-x86_64.iso.torrent");

	Parser P(file);
	BencodeValue result = P.parse();
	std::cout << result << std::endl;
}