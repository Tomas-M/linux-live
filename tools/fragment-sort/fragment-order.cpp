#include "fragment-sort.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Sample eight-byte little-endian windows at every byte offset.
std::vector<std::uint32_t> fragment_features(const std::vector<unsigned char>& data)
{
	std::unordered_set<std::uint32_t> found;
	std::uint64_t word = 0;
	for (std::size_t i = 0; i < data.size(); ++i) {
		word = (word >> 8) | (std::uint64_t(data[i]) << 56);
		if (i < 7)
			continue;
		std::uint64_t hash = word * UINT64_C(0x9e3779b97f4a7c15);
		hash ^= hash >> 32;
		hash *= UINT64_C(0xd6e8feb86659fd93);
		hash ^= hash >> 32;
		if ((hash & 511) == 0)
			found.insert(static_cast<std::uint32_t>(hash >> 9));
	}
	std::vector<std::uint32_t> signature(found.begin(), found.end());
	std::sort(signature.begin(), signature.end());
	return signature;
}

namespace {

struct Posting {
	std::vector<std::size_t> members;
	double weight = 0;
};

// Shortlist co-packable fragments sharing informative fingerprints.
std::vector<Edge> fragment_neighbors(const std::vector<Fragment>& fragments,
		std::size_t block_size, std::size_t candidates)
{
	const auto count = fragments.size();
	std::unordered_map<std::uint32_t, Posting> index;
	for (std::size_t i = 0; i < count; ++i)
		for (auto token : fragments[i].signature) {
			auto& members = index[token].members;
			if (members.size() <= 300)
				members.push_back(i);
		}
	for (auto it = index.begin(); it != index.end();) {
		const auto frequency = it->second.members.size();
		if (frequency < 2 || frequency > 300)
			it = index.erase(it);
		else {
			it->second.weight = std::log1p(static_cast<double>(count) / frequency);
			++it;
		}
	}

	std::vector<double> norms(count), scores(count);
	for (std::size_t i = 0; i < count; ++i) {
		double sum = 0;
		for (auto token : fragments[i].signature) {
			const auto it = index.find(token);
			if (it != index.end())
				sum += it->second.weight;
		}
		norms[i] = std::pow(std::max(1.0, sum), -0.26);
	}

	std::vector<Edge> edges, row;
	std::vector<std::size_t> matched;
	for (std::size_t i = 0; i < count; ++i) {
		for (auto token : fragments[i].signature) {
			const auto it = index.find(token);
			if (it == index.end())
				continue;
			for (auto j : it->second.members) {
				if (j == i || fragments[j].size > block_size - fragments[i].size)
					continue;
				if (scores[j] == 0)
					matched.push_back(j);
				scores[j] += it->second.weight;
			}
		}
		for (auto j : matched) {
			row.push_back({i, j, scores[j] * norms[j] * norms[i]});
			scores[j] = 0;
		}
		auto stronger = [](const Edge& a, const Edge& b) {
			return a.weight == b.weight ? a.right < b.right : a.weight > b.weight;
		};
		auto end = row.begin() + std::min(candidates, row.size());
		if (end == row.end())
			std::sort(row.begin(), end, stronger);
		else
			std::partial_sort(row.begin(), end, row.end(), stronger);
		edges.insert(edges.end(), row.begin(), end);
		matched.clear();
		row.clear();
	}
	return edges;
}

// Pack the largest clusters first, choosing the smallest available space that fits.
std::vector<std::size_t> pack_clusters(std::vector<std::vector<std::size_t>>& clusters,
		const std::vector<std::size_t>& sizes, std::size_t block_size)
{
	std::vector<std::size_t> roots;
	for (std::size_t i = 0; i < clusters.size(); ++i)
		if (!clusters[i].empty())
			roots.push_back(i);
	std::stable_sort(roots.begin(), roots.end(), [&](auto a, auto b) {
		return sizes[a] > sizes[b];
	});

	std::vector<std::vector<std::size_t>> bins;
	std::set<std::pair<std::size_t, std::size_t>> space;
	for (auto i : roots) {
		auto slot = space.lower_bound({sizes[i], 0});
		std::size_t bin = bins.size(), available = block_size;
		if (slot == space.end())
			bins.push_back(std::move(clusters[i]));
		else {
			available = slot->first;
			bin = slot->second;
			space.erase(slot);
			bins[bin].insert(bins[bin].end(), clusters[i].begin(), clusters[i].end());
		}
		if (available > sizes[i])
			space.emplace(available - sizes[i], bin);
	}
	std::vector<std::size_t> order;
	order.reserve(clusters.size());
	for (const auto& bin : bins)
		order.insert(order.end(), bin.begin(), bin.end());
	return order;
}

} // namespace

// Merge the strongest pairs first and orient their clusters to bring the pair closer.
std::vector<std::size_t> fragment_order(const std::vector<Fragment>& fragments,
		std::size_t block_size, std::size_t candidates, bool zstd_score)
{
	auto edges = fragment_neighbors(fragments, block_size, candidates);
	if (zstd_score && !edges.empty())
		score_pairs(edges, fragments);
	std::stable_sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
		return a.weight > b.weight;
	});

	const auto count = fragments.size();
	std::vector<std::size_t> parent(count), sizes(count);
	std::iota(parent.begin(), parent.end(), std::size_t{0});
	std::vector<std::vector<std::size_t>> clusters(count);
	for (std::size_t i = 0; i < count; ++i) {
		clusters[i].push_back(i);
		sizes[i] = fragments[i].size;
	}
	auto root = [&](std::size_t i) {
		while (parent[i] != i) {
			parent[i] = parent[parent[i]];
			i = parent[i];
		}
		return i;
	};
	for (const auto& edge : edges) {
		if (edge.weight == 0)
			break;
		const auto x = root(edge.left), y = root(edge.right);
		if (x == y || sizes[x] + sizes[y] > block_size)
			continue;
		auto& left = clusters[x];
		auto& right = clusters[y];
		const auto a = std::find(left.begin(), left.end(), edge.left);
		const auto b = std::find(right.begin(), right.end(), edge.right);
		if (a - left.begin() < left.end() - a - 1)
			std::reverse(left.begin(), left.end());
		if (b - right.begin() > right.end() - b - 1)
			std::reverse(right.begin(), right.end());
		left.insert(left.end(), right.begin(), right.end());
		std::vector<std::size_t>().swap(right);
		parent[y] = x;
		sizes[x] += sizes[y];
	}
	return pack_clusters(clusters, sizes, block_size);
}
