#include "grub-assets.h"
#include "grub-assets-digest.h"
#include "hash.h"
#include <lzma.h>
#include <set>
#include <stdexcept>

// Decode only USTAR's bounded octal fields, without GNU base-256 extensions.
static uint64_t tarNumber(const unsigned char *text,size_t bytes)
{
   while (bytes && (*text==' ' || !*text)) { ++text; --bytes; }
   while (bytes && (text[bytes-1]==' ' || !text[bytes-1])) --bytes;
   uint64_t value=0;
   for (size_t i=0; i<bytes; ++i)
   {
      if (text[i]<'0' || text[i]>'7' || value>(UINT64_MAX-7)/8) throw std::runtime_error("The GRUB BIOS archive has an invalid numeric field.");
      value=value*8+text[i]-'0';
   }
   return value;
}

// Match one portable filename component instead of accepting archive paths or links.
static bool assetName(const std::string &name)
{
   if (name.empty() || name=="." || name=="..") return false;
   for (unsigned char c:name)
      if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.')) return false;
   return true;
}

// Authenticate before decoding or staging executable code from the selected source.
GrubAssets::GrubAssets(const Image &image)
{
   auto entry=std::find_if(image.entries.begin(),image.entries.end(),[](const ImageEntry &e) { return e.path==grubArchivePath && !e.directory; });
   if (entry==image.entries.end()) throw std::runtime_error("The Slax source is missing its GRUB BIOS support archive. Use a complete Slax ISO or source directory.");
   if (entry->bytes!=grubArchiveBytes || entry->bytes>32*1024*1024) throw std::runtime_error("The GRUB BIOS support archive does not match this installer. Use the installer supplied with this Slax source.");
   std::vector<unsigned char> compressed(entry->bytes);
   std::unique_ptr<FILE,int (*)(FILE *)> input(openImage(image.path),fclose);
   SourceFile source(input.get(),*entry); source.read(compressed.data(),compressed.size());
   Hash hash; hash.add(compressed.data(),compressed.size()); auto actual=hash.finish();
   if (!std::equal(actual.begin(),actual.end(),grubArchiveSha256)) throw std::runtime_error("The GRUB BIOS support archive is damaged or does not match this installer. Use the installer supplied with this Slax source.");
   if (grubArchiveRawBytes>64*1024*1024) throw std::runtime_error("The GRUB BIOS support archive is too large.");
   std::vector<unsigned char> archive(grubArchiveRawBytes);
   uint64_t memory=128*1024*1024; size_t in=0,out=0;
   auto result=lzma_stream_buffer_decode(&memory,0,nullptr,compressed.data(),&in,compressed.size(),archive.data(),&out,archive.size());
   if (result!=LZMA_OK || in!=compressed.size() || out!=archive.size()) throw std::runtime_error("The GRUB BIOS support archive cannot be decompressed.");
   std::set<std::string> names; bool ended=false;
   for (size_t at=0; at+512<=archive.size();)
   {
      auto header=archive.data()+at;
      if (std::all_of(header,header+512,[](unsigned char c) { return !c; }))
      {
         ended=archive.size()-at>=1024 && std::all_of(header,archive.data()+archive.size(),[](unsigned char c) { return !c; });
         break;
      }
      uint64_t sum=0;
      for (size_t i=0; i<512; ++i) sum+=i>=148 && i<156?' ':header[i];
      if (sum!=tarNumber(header+148,8) || memcmp(header+257,"ustar\0",6) || (header[156]!='0' && header[156])) throw std::runtime_error("The GRUB BIOS archive contains an invalid header or special file.");
      std::string name(reinterpret_cast<const char *>(header),std::find(header,header+100,0)-header);
      if (header[345]) throw std::runtime_error("The GRUB BIOS archive contains an unsupported path prefix.");
      auto slash=name.find('/');
      if (slash==std::string::npos || !assetName(name.substr(slash+1)) || !names.insert(name).second || names.size()>1024) throw std::runtime_error("The GRUB BIOS archive contains an unsafe or duplicate filename.");
      std::string group=name.substr(0,slash),file=name.substr(slash+1);
      bool common=group=="i386-pc";
      bool linuxTool=group=="linux" && (file=="grub-mkimage" || file=="grub-fstest");
      bool windowsTool=group=="windows" && (file=="grub-mkimage.exe" || file=="grub-fstest.exe");
      if (!common && !linuxTool && !windowsTool) throw std::runtime_error("The GRUB BIOS archive contains an unknown asset.");
      uint64_t bytes=tarNumber(header+124,12); at+=512;
      if (!bytes || bytes>archive.size()-at || bytes>16*1024*1024) throw std::runtime_error("The GRUB BIOS archive contains an invalid file size.");
#ifdef _WIN32
      bool native=windowsTool;
#else
      bool native=linuxTool;
#endif
      if (common || native) files.push_back({common?name:file,std::string(reinterpret_cast<const char *>(archive.data()+at),bytes),native?0700U:0600U});
      size_t padded=(bytes+511)/512*512;
      if (padded>archive.size()-at) throw std::runtime_error("The GRUB BIOS archive is truncated.");
      at+=padded;
   }
   if (!ended) throw std::runtime_error("The GRUB BIOS archive has no valid end marker.");
   for (const char *required:{"i386-pc/boot.img","i386-pc/kernel.img","i386-pc/moddep.lst"})
      if (std::none_of(files.begin(),files.end(),[&](const GrubAsset &file) { return file.name==required; })) throw std::runtime_error("The GRUB BIOS archive is incomplete.");
   tool("grub-mkimage"); tool("grub-fstest");
}

// Platform binaries are flattened only after their complete source package is verified.
const GrubAsset &GrubAssets::tool(const std::string &name) const
{
   std::string native=name;
#ifdef _WIN32
   native+=".exe";
#endif
   auto found=std::find_if(files.begin(),files.end(),[&](const GrubAsset &file) { return file.name==native; });
   if (found==files.end()) throw std::runtime_error("The Slax source has no matching native GRUB BIOS tool.");
   return *found;
}
