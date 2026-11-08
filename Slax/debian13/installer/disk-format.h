#pragma once
#include "storage.h"

struct Content
{
   ImageEntry entry;
   std::string data;
};

struct Layout
{
   Partition bios,efi,data;
   std::vector<unsigned char> mbr,primary,backup,table;
};

struct BiosImage { std::string boot,core; };
struct FatGeometry { uint32_t sectors,cluster,fat,clusters; };

uint32_t disk32(const unsigned char *p);
uint64_t disk64(const unsigned char *p);
uint32_t crc32(const void *data,size_t bytes);
std::string partitionSnapshot(Disk &disk,bool strict=true);
Layout newLayout(const Device &device,bool mbr=false);
FatGeometry fatGeometry(uint64_t bytes,uint32_t sector);
void writeLayout(Disk &disk,const Layout &layout);
void writeFat(Disk &disk,const Partition &partition,const std::vector<Content> &files,FILE *source,const Progress &progress);
void writeBios(Disk &disk,uint64_t start,uint64_t capacity,const BiosImage &image,std::vector<unsigned char> &mbr);
BiosImage buildBios(const Image &image,FILE *source,const std::string &uuid);
Content generated(const std::string &path,const std::string &text,bool directory=false);
