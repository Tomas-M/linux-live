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
unsigned writeCalls=0;
bool xpHost=false;

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
}
void Disk::flush() { assert(!fsync(impl->fd)); }

struct FileTree::Impl { std::string root; };

// Mounted test filesystems are owned directories that assertions inspect independently.
FileTree::FileTree(const Partition &part,bool):impl(new Impl) { owned(part.mount); impl->root=part.mount+"/"; assert(std::filesystem::is_directory(part.mount)); }
FileTree::~FileTree()=default;
bool FileTree::exists(const std::string &path) { return std::filesystem::exists(impl->root+path); }
void FileTree::mkdir(const std::string &path) { owned(impl->root+path); if (!std::filesystem::create_directory(impl->root+path)) throw std::runtime_error("Directory exists"); }
FILE *FileTree::open(const std::string &path,bool write,uint32_t mode)
{
   owned(impl->root+path); int fd=::open((impl->root+path).c_str(),write?O_RDWR|O_CREAT|O_EXCL:O_RDONLY,mode&0777);
   if (fd<0) throw std::runtime_error("Cannot open test destination: "+path);
   return fdopen(fd,write?"w+b":"rb");
}
void FileTree::move(const std::string &from,const std::string &to) { owned(impl->root+to); if (exists(to)) throw std::runtime_error("Destination exists"); std::filesystem::rename(impl->root+from,impl->root+to); }
// Model atomic replacement only inside the owned fixture directory.
void FileTree::replace(const std::string &from,const std::string &to) { owned(impl->root+to); std::filesystem::permissions(impl->root+from,std::filesystem::status(impl->root+to).permissions()); std::filesystem::rename(impl->root+from,impl->root+to); }
std::string readBcd(const Partition &) { return ""; }
void addBcdEntry(const InstallPlan &) { throw std::runtime_error("BCD is not exposed by the Linux fixture"); }
void FileTree::remove(const std::string &path,bool) { owned(impl->root+path); std::filesystem::remove(impl->root+path); }
uint64_t FileTree::available() { return std::filesystem::space(impl->root).available; }
std::string FileTree::rootPath() { return impl->root; }
void FileTree::sync() {}
void flushFile(FILE *file) { if (fflush(file) || fsync(fileno(file))) throw std::runtime_error("Cannot flush test file"); }

// Retain the real ISO reader on regular read-only files.
FILE *openImage(const std::string &path) { return fopen(path.c_str(),"rb"); }
std::string filenameKey(const std::string &name) { char *text=g_utf8_casefold(name.c_str(),name.size()); std::string key=text; g_free(text); return key; }
std::string randomId() { static unsigned next=0; char text[33]; snprintf(text,sizeof(text),"%032x",++next); return text; }
bool administrator() { return true; }
// Exercise XP's policy without writing any real Windows disk.
bool windowsXp() { return xpHost; }
bool mbrRequired(const Device &) { return xpHost; }
void checkBiosFiles(const Device &,bool) {}
bool firmwareAvailable() { return !firmware["BootOrder"].empty(); }
std::vector<unsigned char> readFirmware(const std::string &name) { return firmware[name]; }
void writeFirmware(const std::string &name,const std::vector<unsigned char> &data)
{
   firmware[name]=data;
   if (firmwareFault && name=="BootOrder") { firmwareFault=0; throw std::runtime_error("Injected firmware failure after write"); }
}
void refreshDisk(const Device &) {}
BiosImage buildBios(const Partition &,bool) { return {std::string(512,'\0'),std::string(4096,'\0'),{}}; }
