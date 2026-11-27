#include "fragment-sort.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <sys/stat.h>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using FileId = std::pair<std::uintmax_t, std::uintmax_t>;
using std::size_t;

// Look up the device and inode for a file with more than one directory entry.
static FileId file_id(const fs::path& path)
{
	struct stat info;
	if(lstat(path.c_str(), &info) != 0)
		throw fs::filesystem_error("lstat", path, {errno, std::generic_category()});
	return {info.st_dev, info.st_ino};
}

// Read only the final partial block, including the whole file if it fits.
static Bytes read_tail(const fs::path& path, size_t size)
{
	std::ifstream file(path, std::ios::binary);
	Bytes data(size);
	file.seekg(-static_cast<std::streamoff>(size), std::ios::end);
	if(!file.read(reinterpret_cast<char *>(data.data()), size))
		throw std::runtime_error("cannot read fragment of " + path.u8string());
	return data;
}

// Encode a source-relative path for the line-oriented mksquashfs -sort reader.
static std::string sort_name(const fs::path& relative)
{
	auto name = relative.generic_u8string();
	if(name.find('\n') != name.npos || (name.size() >= 2 && name.compare(name.size() - 2, 2, "/*") == 0))
		return {};
	std::string escaped;
	for(char ch : name) {
		if(std::string_view(" \t\r\v\f\\#").find(ch) != std::string_view::npos)
			escaped += '\\';
		escaped += ch;
	}
	return escaped.size() + 8 <= 16384 ? escaped : std::string();
}

// Scan files before descending into sorted subdirectories, skipping symbolic links.
static void collect(const fs::path& source, size_t block, bool tails, bool keep_data,
	std::vector<Fragment>& fragments, std::vector<std::pair<std::string, size_t>>& entries)
{
	std::vector<fs::path> dirs{source}, originals;
	std::unordered_multimap<size_t, size_t> hashes;
	std::set<FileId> links;
	size_t skipped = 0;
	while(!dirs.empty()) {
		auto directory = std::move(dirs.back());
		dirs.pop_back();
		std::vector<fs::path> files, children;
		for(const auto& entry : fs::directory_iterator(directory)) {
			auto type = entry.symlink_status().type();
			if(type == fs::file_type::directory)
				children.push_back(entry.path());
			else if(type == fs::file_type::regular)
				files.push_back(entry.path());
		}
		std::sort(children.rbegin(), children.rend());
		dirs.insert(dirs.end(), children.begin(), children.end());
		std::sort(files.begin(), files.end());
		for(const auto& path : files) {
			auto length = fs::file_size(path);
			size_t size = length % block;
			if(!size || (length >= block && !tails))
				continue;
			auto name = sort_name(path.lexically_relative(source));
			if(name.empty()) {
				++skipped;
				continue;
			}
			if(fs::hard_link_count(path) > 1 && !links.insert(file_id(path)).second)
				continue;
			auto data = read_tail(path, size);
			auto hash = std::hash<std::string_view>{}({reinterpret_cast<const char *>(data.data()), size});
			auto range = hashes.equal_range(hash);
			size_t id = fragments.size();
			for(auto it = range.first; it != range.second; ++it) {
				size_t other = it->second;
				if(fragments[other].size != size)
					continue;
				// A hash hit is only a candidate. Always compare the actual bytes.
				if(keep_data ? fragments[other].data == data : read_tail(originals[other], size) == data) {
					id = other;
					break;
				}
			}
			if(id == fragments.size()) {
				fragments.push_back({size, fragment_features(data), {}});
				if(keep_data)
					fragments.back().data = std::move(data);
				else
					originals.push_back(path);
				hashes.emplace(hash, id);
			}
			entries.emplace_back(std::move(name), id);
		}
	}
	if(skipped)
		std::cerr << skipped << " paths cannot be represented in -sort; using their default priorities\n";
}

// Accept a positive decimal count, checking overflow before using it as a size.
static size_t positive(std::string_view value)
{
	size_t number = 0;
	auto result = std::from_chars(value.data(), value.data() + value.size(), number);
	if(result.ec != std::errc() || result.ptr != value.data() + value.size() || !number)
		throw std::runtime_error("expected a positive integer, got '" + std::string(value) + "'");
	return number;
}

// Convert the block-size spellings accepted by mksquashfs to bytes.
static size_t block_size(std::string_view arg)
{
	size_t scale = 1;
	if(!arg.empty() && std::string_view("kKmM").find(arg.back()) != arg.npos) {
		scale = arg.back() == 'k' || arg.back() == 'K' ? 1024 : 1048576;
		arg.remove_suffix(1);
	}
	size_t n = positive(arg);
	if(n > 1048576 / scale || n * scale < 4096 || (n & (n - 1)))
		throw std::runtime_error("expected a power-of-two block size from 4K to 1M");
	return n * scale;
}

// Write priorities in signed 16-bit buckets, as required by mksquashfs.
int main(int argc, char **argv)
try {
	const std::vector<std::string> args(argv + 1, argv + argc);
	fs::path source, output;
	size_t block = 131072, candidates = 0;
	bool tails = false, zstd = false, positional = false;
	for(size_t i = 0; i < args.size(); ++i) {
		const auto& arg = args[i];
		if(!positional && (arg == "--help" || arg == "-h")) {
			std::cout << "Usage: fragment-sort SOURCE [-b SIZE] [-tailends] [--zstd-score] [--candidates N|all] [-o FILE]\n"
				"Write a source-relative mksquashfs -sort list to FILE or standard output.\n"
				"Block size defaults to 128K. With -tailends, use -tailends and -action 'fragment(mixed)@true' in mksquashfs.\n"
				"Candidates default to 12 with Zstandard scoring and 32 otherwise.\n";
			return 0;
		} else if(!positional && arg == "--") {
			positional = true;
		} else if(!positional && (arg == "-tailends" || arg == "--tailends")) {
			tails = true;
		} else if(!positional && arg == "--zstd-score") {
#ifdef ZSTD_SUPPORT
			zstd = true;
#else
			throw std::runtime_error("--zstd-score requires a build with ZSTD_SUPPORT=1");
#endif
		} else if(!positional && (arg == "-b" || arg == "-o" || arg == "--candidates")) {
			if(++i == args.size())
				throw std::runtime_error("missing argument for " + arg);
			if(arg == "-b")
				block = block_size(args[i]);
			else if(arg == "-o")
				output = fs::u8path(args[i]);
			else
				candidates = args[i] == "all" ? std::numeric_limits<size_t>::max() : positive(args[i]);
		} else if(!positional && arg.compare(0, 13, "--candidates=") == 0) {
			auto value = arg.substr(13);
			candidates = value == "all" ? std::numeric_limits<size_t>::max() : positive(value);
		} else if(!positional && arg.compare(0, 2, "-b") == 0) {
			block = block_size(arg.substr(arg[2] == '=' ? 3 : 2));
		} else if(!positional && !arg.empty() && arg[0] == '-') {
			throw std::runtime_error("unknown option " + arg);
		} else if(source.empty()) {
			source = fs::u8path(arg);
		} else {
			throw std::runtime_error("expected one source directory");
		}
	}
	if(source.empty())
		throw std::runtime_error("missing source directory (use --help for usage)");
	std::vector<Fragment> fragments;
	std::vector<std::pair<std::string, size_t>> entries;
	collect(fs::canonical(source), block, tails, zstd, fragments, entries);
	auto order = fragment_order(fragments, block, candidates ? candidates : (zstd ? 12 : 32), zstd);
	std::vector<int> priorities(order.size());
	auto buckets = std::min<size_t>(65536, order.size());
	for(size_t rank = 0; rank < order.size(); ++rank)
		priorities[order[rank]] = 32767 - int(std::uint64_t(rank) * buckets / order.size());
	std::ofstream file;
	if(!output.empty()) {
		file.open(output, std::ios::binary);
		if(!file)
			throw std::runtime_error("cannot open output " + output.u8string());
	}
	std::ostream& out = output.empty() ? std::cout : file;
	for(const auto& entry : entries)
		out << entry.first << ' ' << priorities[entry.second] << '\n';
	if(!out.flush())
		throw std::runtime_error("cannot write sort list");
	return 0;
} catch(const std::exception& error) {
	std::cerr << "fragment-sort: " << error.what() << '\n';
	return 1;
}
