#include "fragment-sort.hpp"

#include <stdexcept>

#ifdef ZSTD_SUPPORT
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <tuple>
#include <zstd.h>

// Report compression errors before their size_t results can become graph weights.
static std::size_t checked(std::size_t result)
{
	if (ZSTD_isError(result))
		throw std::runtime_error(ZSTD_getErrorName(result));
	return result;
}

// Score each undirected pair once, reusing the larger fragment's dictionary.
void score_pairs(std::vector<Edge>& edges, const std::vector<Fragment>& fragments)
{
	if (edges.empty())
		return;
	for (auto& edge : edges) {
		auto a = fragments[edge.left].size, b = fragments[edge.right].size;
		if (a < b || (a == b && edge.left > edge.right))
			std::swap(edge.left, edge.right);
	}
	auto by_pair = [](const Edge& a, const Edge& b) {
		return std::tie(a.left, a.right) < std::tie(b.left, b.right);
	};
	std::sort(edges.begin(), edges.end(), by_pair);
	edges.erase(std::unique(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
		return a.left == b.left && a.right == b.right;
	}), edges.end());

	constexpr int level = 5;
	std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> ctx(ZSTD_createCCtx(), ZSTD_freeCCtx);
	if (!ctx)
		throw std::bad_alloc();
	std::vector<unsigned char> output;
	std::vector<std::size_t> sizes;
	sizes.reserve(fragments.size());
	for (const auto& fragment : fragments) {
		const auto& data = fragment.data;
		auto bound = checked(ZSTD_compressBound(data.size()));
		if (bound > output.size())
			output.resize(bound);
		sizes.push_back(checked(ZSTD_compressCCtx(ctx.get(), output.data(), output.size(),
			data.data(), data.size(), level)));
	}

	std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> dict(nullptr, ZSTD_freeCDict);
	std::size_t previous = fragments.size();
	bool raw_prefix = false;
	for (auto& edge : edges) {
		const auto& donor = fragments[edge.left].data;
		const auto& peer = fragments[edge.right].data;
		if (previous != edge.left) {
			// createCDict auto-detects formatted dictionary headers. File bytes
			// starting with that magic must go through libzstd's raw prefix API.
			raw_prefix = donor.size() >= 4 && std::memcmp(donor.data(), "\x37\xa4\x30\xec", 4) == 0;
			if (raw_prefix) {
				checked(ZSTD_CCtx_reset(ctx.get(), ZSTD_reset_session_and_parameters));
				checked(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, level));
			} else {
				dict.reset(ZSTD_createCDict(donor.data(), donor.size(), level));
				if (!dict)
					throw std::bad_alloc();
			}
			previous = edge.left;
		}
		std::size_t size;
		if (raw_prefix) {
			checked(ZSTD_CCtx_refPrefix(ctx.get(), donor.data(), donor.size()));
			size = checked(ZSTD_compress2(ctx.get(), output.data(), output.size(),
				peer.data(), peer.size()));
		} else
			size = checked(ZSTD_compress_usingCDict(ctx.get(), output.data(), output.size(),
				peer.data(), peer.size(), dict.get()));
		const auto plain = sizes[edge.right];
		const double product = static_cast<double>(plain) * sizes[edge.left];
		edge.weight = size < plain ? (plain - size) / std::pow(product, 0.19) : 0.0;
		if (edge.left > edge.right)
			std::swap(edge.left, edge.right);
	}
	// The stable merge step resolves equal scores in canonical pair order.
	std::sort(edges.begin(), edges.end(), by_pair);
}
#else
// Keep fingerprint-only builds independent of libzstd.
void score_pairs(std::vector<Edge>&, const std::vector<Fragment>&)
{
	throw std::runtime_error("--zstd-score requires a build with ZSTD_SUPPORT=1");
}
#endif
