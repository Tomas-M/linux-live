#ifndef FRAGMENT_SORT_HPP
#define FRAGMENT_SORT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

struct Fragment {
	std::size_t size;
	std::vector<std::uint32_t> signature;
	std::vector<unsigned char> data;
};

struct Edge {
	std::size_t left, right;
	double weight;
};

std::vector<std::uint32_t> fragment_features(const std::vector<unsigned char>& data);
std::vector<std::size_t> fragment_order(const std::vector<Fragment>& fragments,
	std::size_t block_size, std::size_t candidates, bool zstd_score);
void score_pairs(std::vector<Edge>& edges, const std::vector<Fragment>& fragments);

#endif
