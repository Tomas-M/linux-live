#include "installer.h"
#include "grub-assets.h"
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <filesystem>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

static const char *requiredFiles[]={"slax/boot/vmlinuz","slax/boot/initrfs.img","slax/modules/01-core.sb","slax/boot/EFI/BOOT/BOOTX64.EFI","slax/boot/EFI/BOOT/grubx64.efi","slax/boot/grub/entries.cfg",grubArchivePath};

// Decode ISO9660's little-endian half without alignment assumptions.
static uint32_t number(const unsigned char *p)
{
   return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
}

// Use 64-bit image offsets on both supported operating systems.
void seekImage(FILE *file,uint64_t offset)
{
#ifdef _WIN32
   if (_fseeki64(file,offset,SEEK_SET)) throw std::runtime_error("Cannot seek in the source ISO.");
#else
   if (fseeko(file,offset,SEEK_SET)) throw std::runtime_error("Cannot seek in the source ISO.");
#endif
}

// Bound every read by the real file, including Rock Ridge continuation areas.
static std::vector<unsigned char> read(FILE *file,uint64_t offset,uint64_t bytes,uint64_t size)
{
   if (offset>size || bytes>size-offset || bytes>32*1024*1024) throw std::runtime_error("An ISO directory lies outside the image.");
   std::vector<unsigned char> data(bytes); seekImage(file,offset);
   if (fread(data.data(),1,bytes,file)!=bytes) throw std::runtime_error("The source ISO could not be read completely.");
   return data;
}

// Read Rock Ridge names and reject links or relocation rather than copying ambiguous paths.
static void extensions(FILE *file,uint64_t size,const unsigned char *data,size_t bytes,std::string &name,uint32_t &mode,int depth=0)
{
   if (depth>4) throw std::runtime_error("The ISO has recursive Rock Ridge continuation records.");
   for (size_t at=0; at+4<=bytes;)
   {
      const auto p=data+at; unsigned length=p[2];
      if (length<4 || at+length>bytes) throw std::runtime_error("The ISO contains an invalid Rock Ridge record.");
      if (!memcmp(p,"ST",2)) break;
      if (!memcmp(p,"SL",2) || !memcmp(p,"RE",2) || !memcmp(p,"CL",2)) throw std::runtime_error("This ISO contains links or relocated directories in slax; use an unmodified Slax ISO.");
      if (!memcmp(p,"PX",2) && length>=12) mode=number(p+4);
      if (!memcmp(p,"NM",2) && length>=5)
      {
         if (p[4]&~1) throw std::runtime_error("The ISO contains an unsupported Rock Ridge filename.");
         name.append(reinterpret_cast<const char *>(p+5),length-5);
      }
      if (!memcmp(p,"CE",2) && length==28)
      {
         auto continuation=read(file,uint64_t(number(p+4))*2048+number(p+12),number(p+20),size);
         extensions(file,size,continuation.data(),continuation.size(),name,mode,depth+1);
      }
      at+=length;
   }
}

// Permit portable UTF-8 components while excluding traversal, Windows aliases and special files.
static bool component(const std::string &name)
{
   if (name.empty() || name=="." || name==".." || name.size()>255 || name.back()=='.' || name.back()==' ') return false;
   for (unsigned char c:name) if (c<32 || c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|') return false;
   auto stem=name.substr(0,name.find('.'));
   for (char &c:stem) if (c>='a' && c<='z') c-=32;
   if (stem=="CON" || stem=="PRN" || stem=="AUX" || stem=="NUL") return false;
   if (stem.size()==4 && (stem.substr(0,3)=="COM" || stem.substr(0,3)=="LPT") && stem[3]>='1' && stem[3]<='9') return false;
   return true;
}

// Walk only /slax, keeping original Rock Ridge names and checking every extent and destination.
static void directory(FILE *file,Image &image,uint32_t sector,uint32_t bytes,const std::string &parent,std::set<uint32_t> &visited,unsigned depth)
{
   if (depth>24 || !visited.insert(sector).second || image.entries.size()>32768) throw std::runtime_error("The ISO contains a cyclic or excessively large directory tree.");
   auto data=read(file,uint64_t(sector)*2048,bytes,image.bytes);
   std::set<std::string> names;
   for (size_t at=0; at<data.size();)
   {
      auto p=data.data()+at;
      if (!p[0]) { at=(at/2048+1)*2048; continue; }
      if (data.size()-at<34) throw std::runtime_error("The ISO contains a truncated directory record.");
      unsigned length=p[0],nameBytes=p[32];
      unsigned extension=33+nameBytes+(nameBytes%2==0);
      if (length<34 || at+length>data.size() || extension>length) throw std::runtime_error("The ISO contains a truncated directory record.");
      at+=length;
      if (nameBytes==1 && p[33]<=1) continue;
      std::string name(reinterpret_cast<const char *>(p+33),nameBytes),rock;
      uint32_t mode=0644; extensions(file,image.bytes,p+extension,length-extension,rock,mode);
      if (!rock.empty()) name=rock;
      else
      {
         auto version=name.find(';'); if (version!=std::string::npos) name.resize(version);
         if (!name.empty() && name.back()=='.') name.pop_back();
         for (char &c:name) if (c>='A' && c<='Z') c+=32;
      }
      if (parent.empty() && name!="slax") continue;
      if (!component(name)) throw std::runtime_error("The ISO contains an unsafe or non-portable filename.");
      std::string key=filenameKey(name);
      if (!names.insert(key).second) throw std::runtime_error("The ISO contains duplicate destination filenames.");
      uint64_t offset=uint64_t(number(p+2))*2048,size=number(p+10);
      if (offset>image.bytes || size>image.bytes-offset || p[26] || p[27] || (p[25]&0x80)) throw std::runtime_error("The ISO contains an unsupported or out-of-range file extent.");
      ImageEntry item={parent.empty()?name:parent+"/"+name,offset,size,bool(p[25]&2)};
      uint32_t type=mode&0170000;
      if (type && type!=(item.directory?0040000:0100000)) throw std::runtime_error("The ISO contains an unsupported special file or inconsistent directory type.");
      item.mode=mode&0777;
      image.entries.push_back(item);
      if (item.directory) directory(file,image,number(p+2),size,item.path,visited,depth+1);
      else
      {
         if (size>0xffffffffULL || image.payload>image.bytes*4-size) throw std::runtime_error("The ISO payload is too large for FAT32.");
         image.payload+=size;
      }
   }
}

// Read either an ISO extent or an ordinary file from an inventoried Slax directory.
SourceFile::SourceFile(FILE *image,const ImageEntry &entry)
{
   if (entry.offset==UINT64_MAX) return;
   owned=!entry.source.empty(); file=owned?openImage(entry.source):image;
   if (!file) throw std::runtime_error("A Slax source file cannot be opened.");
   if (!owned) seekImage(file,entry.offset);
}

// Image streams belong to the installation; directory-file streams belong to this reader.
SourceFile::~SourceFile() { if (owned) fclose(file); }

// A short source read must stop installation before boot activation.
void SourceFile::read(void *data,size_t bytes)
{
   if (fread(data,1,bytes,file)!=bytes) throw std::runtime_error("The Slax source could not be read completely.");
}

// Accept the Slax directory itself or its parent, preserving the ISO reader's portable-path rules.
static void directorySource(Image &image)
{
   namespace fs=std::filesystem;
   auto root=fs::canonical(fs::u8path(image.path));
   bool complete=true;
   for (const char *required:requiredFiles)
   {
      auto file=root/(required+5);
      if (!fs::is_regular_file(file) || !fs::file_size(file)) complete=false;
   }
   if (!complete && fs::is_directory(root/"slax")) root=fs::canonical(root/"slax");
   for (const char *required:requiredFiles)
      if (!fs::is_regular_file(root/(required+5))) throw std::runtime_error("Missing required file: "+std::string(required)+".");
   image.path=root.u8string(); image.name=root.filename().u8string();
   image.entries.push_back({"slax",0,0,true});
   std::set<std::string> names;
   for (auto item=fs::recursive_directory_iterator(root); item!=fs::recursive_directory_iterator(); ++item)
   {
      auto status=item->symlink_status(); bool folder=fs::is_directory(status);
      if (!folder && !fs::is_regular_file(status)) throw std::runtime_error("The Slax directory contains links or special files.");
      if (item.depth()>24 || image.entries.size()>32768) throw std::runtime_error("The Slax directory contains too many files or nested directories.");
      auto relative=item->path().lexically_relative(root);
      for (const auto &part:relative) if (!component(part.u8string())) throw std::runtime_error("The Slax directory contains an unsafe or non-portable filename.");
      auto path="slax/"+relative.generic_u8string();
      if (!names.insert(filenameKey(path)).second) throw std::runtime_error("The Slax directory contains duplicate destination filenames.");
      ImageEntry entry={path,0,folder?0:item->file_size(),folder};
      entry.mode=uint32_t(status.permissions())&0777; entry.source=item->path().u8string();
      image.payload+=entry.bytes; image.entries.push_back(std::move(entry));
   }
   image.bytes=image.payload;
}

// Validate and inventory the actual payload before any destination is opened for writing.
Image inspectImage(const std::string &path)
{
   Image image; image.path=path; image.name=path.substr(path.find_last_of("/\\")+1);
   FILE *file=nullptr;
   try
   {
      if (std::filesystem::is_directory(std::filesystem::u8path(path))) directorySource(image);
      else
      {
         file=openImage(path);
         if (!file) throw std::runtime_error("The ISO file cannot be opened.");
#ifdef _WIN32
         if (_fseeki64(file,0,SEEK_END)) throw std::runtime_error("Cannot determine the ISO size.");
         image.bytes=_ftelli64(file);
#else
         // Optical block devices report their capacity through the kernel, not the file's seek endpoint.
         if (ioctl(fileno(file),BLKGETSIZE64,&image.bytes))
         {
            if (fseeko(file,0,SEEK_END)) throw std::runtime_error("Cannot determine the ISO size.");
            image.bytes=ftello(file);
         }
#endif
         auto p=read(file,16*2048,2048,image.bytes);
         bool valid=p[0]==1 && !memcmp(p.data()+1,"CD001",5) && p[6]==1 && p[128]==0 && p[129]==8 && p[156]>=34;
         if (!valid || uint64_t(number(p.data()+80))*2048>image.bytes) throw std::runtime_error("This is not a valid ISO9660 Slax image.");
         std::set<uint32_t> visited;
         directory(file,image,number(p.data()+158),number(p.data()+166),"",visited,0);
      }
      for (const auto &entry:image.entries) if (entry.path=="slax/boot/grub/filesystems.txt" && !entry.directory)
      {
         if (entry.bytes>1024) throw std::runtime_error("The ISO filesystem capability list is invalid.");
         std::string list(entry.bytes,'\0'); SourceFile source(file,entry); source.read(list.data(),list.size());
         for (const char *name:{"btrfs","exfat"}) if (("\n"+list+"\n").find("\n"+std::string(name)+"\n")!=std::string::npos) image.filesystems.push_back(name);
      }
      for (const char *required:requiredFiles)
      {
         auto found=std::find_if(image.entries.begin(),image.entries.end(),[&](const ImageEntry &e) { return e.path==required && !e.directory && e.bytes>0; });
         if (found==image.entries.end()) throw std::runtime_error("Missing required file: "+std::string(required)+".");
      }
   }
   catch (const std::exception &error) { image.error=error.what(); image.entries.clear(); }
   if (file) fclose(file);
   return image;
}
