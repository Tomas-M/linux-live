#pragma once
#include "installer.h"
#include <stdexcept>

using Progress=std::function<void(const std::string &,int)>;
class Disk;
struct BootLayout;
struct BiosImage;

struct DiskRange { uint64_t start=0,bytes=0; };

struct InstallPlan
{
   Device device;
   Image image;
   Partition efi;
   bool erase=false,bios=false,registerBoot=false,fallback=false;
   uint64_t bootBytes=0;
   std::string confirmation,windowsLoader,fallbackDigest,previousBios,bootWarning,efiFirst;
   Partition bootPartition;
   std::string bootConfig,bootBefore,bootFile;
   std::string snapshot;
   std::shared_ptr<Disk> disk;
   std::shared_ptr<BootLayout> bootLayout;
   std::shared_ptr<BiosImage> biosImage;
   std::vector<unsigned char> bootOrder;
   std::vector<std::pair<std::string,std::string>> efiLoaders;
   uint16_t bootNumber=0;
};

// Native handles own the exact approved disk and any exclusive filesystem locks.
class Disk
{
   struct Impl;
   std::unique_ptr<Impl> impl;
public:
   Device device;
   Disk(const Device &expected);
   ~Disk();
   void verify();
   void reserve();
   void claim(bool erase);
   void claimBoot(const std::vector<DiskRange> &ranges);
   void release();
   void read(uint64_t offset,void *data,size_t bytes);
   void write(uint64_t offset,const void *data,size_t bytes);
   void flush();
};

// A rooted native directory handle prevents copies from following mount-point or link substitutions.
class FileTree
{
   struct Impl;
   std::unique_ptr<Impl> impl;
public:
   FileTree(const Partition &partition,bool writable);
   ~FileTree();
   bool exists(const std::string &path);
   void mkdir(const std::string &path);
   FILE *open(const std::string &path,bool write,uint32_t mode=0644);
   void move(const std::string &from,const std::string &to);
   void replace(const std::string &from,const std::string &to);
   void remove(const std::string &path,bool directory);
   uint64_t available();
   std::string rootPath();
   void sync();
};

void seekImage(FILE *file,uint64_t offset);
void flushFile(FILE *file);
std::string randomId();
bool administrator();
bool windowsXp();
bool mbrRequired(const Device &device);
void checkBiosFiles(const Device &device,bool installed);
bool firmwareAvailable();
void registerFirmware(const InstallPlan &plan,const std::string &path,const std::string &journal);
void readEfiLoaders(InstallPlan &plan);
std::vector<unsigned char> readFirmware(const std::string &name);
void writeFirmware(const std::string &name,const std::vector<unsigned char> &data);
void refreshDisk(const Device &device);
std::string basicEligibility(const Device &device,const Image &image,bool erase);
InstallPlan prepareInstall(Device device,const Image &image,bool erase);
void install(const InstallPlan &plan,const Progress &progress);
void planBootMenu(InstallPlan &plan);
void publishBootMenu(const InstallPlan &plan,const std::string &vendor);
bool grubBootMenu(const InstallPlan &plan);
bool grubEfiMenu(const InstallPlan &plan);
std::string ntldrMenu(const std::string &text,const std::string &file);
std::string readBcd(const Partition &partition);
void addBcdEntry(const InstallPlan &plan);
