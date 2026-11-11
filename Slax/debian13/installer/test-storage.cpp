// Test-only boundaries: every write is confined to an owned Linux temporary directory.
#include "disk-format.h"
#include <glib.h>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <cassert>
#include <map>

std::string fixtureRoot;
std::map<std::string,std::vector<unsigned char>> firmware;
int firmwareFault=0,writeFault=-1;
int corruptWrite=-1;
size_t corruptWriteByte=0;
unsigned writeCalls=0;
bool xpHost=false;
std::string bcdStore,bcdDirty;
bool bcdSyncFailure=false;
int fileMoveFault=-1,fileMoveSyncFault=-1,fileSyncFault=-1;

// Check ownership before any test implementation opens a file for writing.
static void owned(const std::string &path) { assert(path.find(fixtureRoot+"/")==0); }

struct Disk::Impl { int fd=-1; bool claimed=false; std::vector<DiskRange> ranges; ~Impl() { if (fd>=0) close(fd); } };

// Accept regular sparse fixture files only, never host block devices.
Disk::Disk(const Device &expected):impl(new Impl),device(expected)
{
   owned(expected.disk); impl->fd=::open(expected.disk.c_str(),O_RDWR|O_NOFOLLOW); assert(impl->fd>=0);
   struct stat info; assert(!fstat(impl->fd,&info) && S_ISREG(info.st_mode) && uint64_t(info.st_size)==expected.diskBytes);
}
Disk::~Disk()=default;
// Owned regular files have no removable-media generation.
void Disk::verify() {}
void Disk::reserve() { if (flock(impl->fd,LOCK_EX|LOCK_NB)) throw std::runtime_error("Another installer is using this disk"); }
void Disk::claim(bool erase) { if (erase && !device.eraseBlocked.empty()) throw std::runtime_error(device.eraseBlocked); reserve(); impl->claimed=true; }
void Disk::claimBoot(const std::vector<DiskRange> &ranges) { reserve(); impl->ranges=ranges; }
void Disk::release() { impl->claimed=false; impl->ranges.clear(); flock(impl->fd,LOCK_UN); }
void Disk::read(uint64_t offset,void *data,size_t bytes) { assert(offset+bytes<=device.diskBytes); if (pread(impl->fd,data,bytes,offset)!=ssize_t(bytes)) throw std::runtime_error("Injected disk read failure"); }
void Disk::write(uint64_t offset,const void *data,size_t bytes)
{
   bool approved=impl->claimed;
   for (const auto &range:impl->ranges) if (offset>=range.start && offset-range.start<=range.bytes && bytes<=range.bytes-(offset-range.start)) approved=true;
   if (!approved) throw std::runtime_error("An unapproved boot write was refused");
   assert(offset+bytes<=device.diskBytes && offset%device.sector==0 && bytes%device.sector==0);
   if (writeFault==0) { writeFault=-1; throw std::runtime_error("Injected disk write failure"); }
   if (writeFault>0) writeFault--;
   writeCalls++; if (pwrite(impl->fd,data,bytes,offset)!=ssize_t(bytes)) throw std::runtime_error("Disk write failed");
   // Model a device that acknowledges a write but stores a corrupted byte.
   if (corruptWrite==0)
   {
      size_t byte=std::min(corruptWriteByte,bytes-1); unsigned char value=static_cast<const unsigned char *>(data)[byte]^1;
      assert(pwrite(impl->fd,&value,1,offset+byte)==1); corruptWrite=-1;
   }
   else if (corruptWrite>0) corruptWrite--;
}
void Disk::flush() { assert(!fsync(impl->fd)); }

struct FileTree::Impl { std::string root; };

// Mounted test filesystems are owned directories that assertions inspect independently.
FileTree::FileTree(const Partition &part,bool):impl(new Impl) { owned(part.mount); impl->root=part.mount+"/"; assert(std::filesystem::is_directory(part.mount)); }
FileTree::~FileTree()=default;
bool FileTree::exists(const std::string &path) { return std::filesystem::exists(impl->root+path); }
// Match native alias detection and reject conflicting or linked destination objects.
bool FileTree::isDirectory(const std::string &path)
{
   auto status=std::filesystem::symlink_status(impl->root+path);
   if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) throw std::runtime_error("Unsupported destination object");
   return std::filesystem::is_directory(status);
}
bool FileTree::same(const std::string &path,const std::string &external)
{
   if (!exists(path)) return false;
   return std::filesystem::equivalent(impl->root+path,external);
}
void FileTree::mkdir(const std::string &path) { owned(impl->root+path); if (!std::filesystem::create_directory(impl->root+path)) throw std::runtime_error("Directory exists"); }
FILE *FileTree::open(const std::string &path,bool write,uint32_t mode)
{
   owned(impl->root+path); int fd=::open((impl->root+path).c_str(),write?O_RDWR|O_CREAT|O_EXCL:O_RDONLY,mode&0777);
   if (fd<0) throw std::runtime_error("Cannot open test destination: "+path);
   return fdopen(fd,write?"w+b":"rb");
}
void FileTree::move(const std::string &from,const std::string &to)
{
   owned(impl->root+to);
   if (fileMoveFault==0) { fileMoveFault=-1; throw std::runtime_error("Injected rename failure"); }
   if (fileMoveFault>0) fileMoveFault--;
   if (exists(to)) throw std::runtime_error("Destination exists");
   std::filesystem::rename(impl->root+from,impl->root+to);
   if (fileMoveSyncFault==0) { fileMoveSyncFault=-1; throw std::runtime_error("Injected post-rename flush failure"); }
   if (fileMoveSyncFault>0) fileMoveSyncFault--;
}
// Model atomic replacement only inside the owned fixture directory.
void FileTree::replace(const std::string &from,const std::string &to) { owned(impl->root+to); std::filesystem::permissions(impl->root+from,std::filesystem::status(impl->root+to).permissions()); std::filesystem::rename(impl->root+from,impl->root+to); }
std::string readBcd(const Partition &) { return bcdStore; }
// Model BCDEdit's separate filesystem writes until its boot volume is explicitly flushed.
void addBcdEntry(const InstallPlan &plan) { owned(plan.bootPartition.mount); bcdDirty=plan.bootPartition.mount+"/"; }
void FileTree::remove(const std::string &path,bool) { owned(impl->root+path); std::filesystem::remove(impl->root+path); }
uint64_t FileTree::available() { return std::filesystem::space(impl->root).available; }
std::string FileTree::rootPath() { return impl->root; }
void FileTree::sync()
{
   if (fileSyncFault==0) { fileSyncFault=-1; throw std::runtime_error("Injected filesystem flush failure"); }
   if (fileSyncFault>0) fileSyncFault--;
   if (bcdDirty!=impl->root) return;
   if (bcdSyncFailure) throw std::runtime_error("Injected BCD volume flush failure");
   bcdDirty.clear();
}
void flushFile(FILE *file) { if (fflush(file) || fsync(fileno(file))) throw std::runtime_error("Cannot flush test file"); }

// Retain the real ISO reader on regular read-only files.
FILE *openImage(const std::string &path) { return fopen(path.c_str(),"rb"); }
std::string filenameKey(const std::string &name) { char *text=g_utf8_casefold(name.c_str(),name.size()); std::string key=text; g_free(text); return key; }
std::string randomId() { static unsigned next=0; char text[33]; snprintf(text,sizeof(text),"%032x",++next); return text; }
bool administrator() { return true; }
// Exercise XP's policy without writing any real Windows disk.
bool windowsXp() { return xpHost; }
bool mbrRequired(const Device &) { return xpHost; }
void checkBiosFiles(const Device &,bool,const GrubAssets &) {}
bool firmwareAvailable() { return !firmware["BootOrder"].empty(); }
std::vector<unsigned char> readFirmware(const std::string &name) { auto found=firmware.find(name); return found==firmware.end()?std::vector<unsigned char>{}:found->second; }
void writeFirmware(const std::string &name,const std::vector<unsigned char> &data)
{
   firmware[name]=data;
   if (firmwareFault && name=="BootOrder") { firmwareFault=0; throw std::runtime_error("Injected firmware failure after write"); }
}
void refreshDisk(const Device &) {}
BiosImage buildBios(const Partition &,bool,const GrubAssets &) { return {std::string(512,'\0'),std::string(4096,'\0'),{}}; }
