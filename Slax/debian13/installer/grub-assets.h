#pragma once
#include "installer.h"

inline constexpr const char *grubArchivePath="slax/boot/grub/bios/tools.tar.xz";

struct GrubAsset { std::string name,data; uint32_t mode=0644; };

// Keep the verified BIOS support bytes pinned across planning and installation.
class GrubAssets
{
   std::vector<GrubAsset> files;
public:
   explicit GrubAssets(const Image &image);
   const std::vector<GrubAsset> &entries() const { return files; }
   const GrubAsset &tool(const std::string &name) const;
};
