#include "storage.h"
#include "disk-format.h"
#include "grub-assets.h"
#include <libudev.h>
#include <glib.h>
#include <blkid/blkid.h>
#include <dirent.h>
#include <fcntl.h>
#include <mntent.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <spawn.h>
#include <linux/fs.h>
#include <linux/cdrom.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <fstream>
#include <sstream>
#include <filesystem>

// Older build headers omit this standard ioctl; runtime use still requires diskseq.
#ifndef BLKGETDISKSEQ
#define BLKGETDISKSEQ _IOR(0x12,128,__u64)
#endif

// Preserve the native error as part of every actionable storage failure.
static void failed(const std::string &message) { throw std::runtime_error(message+": "+strerror(errno)); }

// Resolve a source file's mounted filesystem even through symlinks and bind mounts.
static std::string mountedSource(const char *source)
{
   struct stat executable,backing; struct statfs filesystem;
   if (stat(source,&executable) || statfs(source,&filesystem)) return "";
   if (filesystem.f_type!=ISOFS_SUPER_MAGIC && filesystem.f_type!=UDF_SUPER_MAGIC) return "";
   std::ifstream file("/sys/dev/block/"+std::to_string(major(executable.st_dev))+":"+std::to_string(minor(executable.st_dev))+"/loop/backing_file");
   std::string path; std::getline(file,path);
   if (!stat(path.c_str(),&backing) && S_ISREG(backing.st_mode)) return path;
   path.clear();
   auto context=udev_new(); if (!context) return "";
   auto device=udev_device_new_from_devnum(context,'b',executable.st_dev);
   if (device)
   {
      auto node=udev_device_get_devnode(device),optical=udev_device_get_property_value(device,"ID_CDROM");
      if (node && optical && !strcmp(optical,"1")) path=node;
      udev_device_unref(device);
   }
   udev_unref(context);
   return path;
}

// Installed desktop launchers use the active Slax data, including a complete copy-to-RAM tree.
std::string sourceImage()
{
   auto source=mountedSource("/proc/self/exe");
   if (!source.empty()) return source;
   for (const char *path:{"/run/initramfs/memory/toram","/run/initramfs/memory/iso/slax","/run/initramfs/memory/data/slax"})
   {
      struct stat boot;
      if (!stat((std::string(path)+"/boot/vmlinuz").c_str(),&boot) && S_ISREG(boot.st_mode)) return path;
   }
   return "";
}

// Read optional udev strings without substituting an unstable physical identity.
static std::string property(udev_device *device,const char *key,bool attribute=false)
{
   const char *value=attribute?udev_device_get_sysattr_value(device,key):udev_device_get_property_value(device,key);
   return value?value:"";
}

struct Mounted { dev_t number,source; std::string path; bool root,readonly; };

// Decode mountinfo's octal escaping without altering international path bytes.
static std::string mountPath(const std::string &path)
{
   std::string decoded;
   for (size_t at=0; at<path.size(); at++)
   {
      bool escaped=path[at]=='\\' && at+3<path.size();
      if (escaped) for (size_t n=1; n<=3; n++) escaped=escaped && path[at+n]>='0' && path[at+n]<='7';
      if (escaped) { decoded+=char(strtoul(path.substr(at+1,3).c_str(),nullptr,8)); at+=3; }
      else decoded+=path[at];
   }
   return decoded;
}

// Mountinfo exposes filesystem roots, unlike /proc/mounts which cannot distinguish bind subdirectories.
static std::vector<Mounted> mountedFilesystems()
{
   std::ifstream file("/proc/self/mountinfo"); std::string line; std::vector<Mounted> result;
   while (std::getline(file,line))
   {
      std::istringstream row(line); std::string id,parent,device,root,path,options;
      if (!(row>>id>>parent>>device>>root>>path>>options)) continue;
      auto colon=device.find(':'); if (colon==std::string::npos) continue;
      dev_t number=makedev(strtoul(device.substr(0,colon).c_str(),nullptr,10),strtoul(device.substr(colon+1).c_str(),nullptr,10));
      std::string field,type,source;
      while (row>>field && field!="-") {}
      row>>type>>source; struct stat block;
      dev_t physical=number;
      if (!stat(mountPath(source).c_str(),&block) && S_ISBLK(block.st_mode)) physical=block.st_rdev;
      result.push_back({number,physical,mountPath(path),root=="/",(","+options+",").find(",ro,")!=std::string::npos});
   }
   return result;
}

// Format the kernel's filesystem identity independently of Btrfs subvolume device numbers.
static std::string btrfsUuid(const btrfs_ioctl_fs_info_args &info)
{
   const char *hex="0123456789abcdef"; std::string uuid;
   for (unsigned n=0; n<16; n++)
   {
      if (n==4 || n==6 || n==8 || n==10) uuid+='-';
      uuid+=hex[info.fsid[n]>>4]; uuid+=hex[info.fsid[n]&15];
   }
   return uuid;
}

static void protectPath(udev *context,const std::string &path,std::set<std::string> &paths);

// Follow mapper, RAID and loop backing devices when protecting a whole disk from erasure.
static void protect(udev *context,dev_t number,std::set<std::string> &paths)
{
   auto device=udev_device_new_from_devnum(context,'b',number);
   if (!device) return;
   std::string path=udev_device_get_syspath(device);
   if (!paths.insert(path).second) { udev_device_unref(device); return; }
   auto parent=udev_device_get_parent_with_subsystem_devtype(device,"block","disk");
   if (parent) paths.insert(udev_device_get_syspath(parent));
   auto directory=opendir((path+"/slaves").c_str());
   if (directory)
   {
      while (auto entry=readdir(directory))
      {
         if (entry->d_name[0]=='.') continue;
         auto child=udev_device_new_from_subsystem_sysname(context,"block",entry->d_name);
         if (child) { protect(context,udev_device_get_devnum(child),paths); udev_device_unref(child); }
      }
      closedir(directory);
   }
   auto backing=property(device,"loop/backing_file",true);
   if (!backing.empty()) protectPath(context,backing,paths);
   udev_device_unref(device);
}

// Btrfs assigns synthetic st_dev values per subvolume; its kernel FSID identifies all backing devices.
static void protectPath(udev *context,const std::string &path,std::set<std::string> &paths)
{
   struct stat info;
   if (stat(path.c_str(),&info)) return;
   protect(context,info.st_dev,paths);
   for (const auto &mount:mountedFilesystems())
   {
      struct stat root;
      if (mount.number==info.st_dev || (!stat(mount.path.c_str(),&root) && root.st_dev==info.st_dev)) protect(context,mount.source,paths);
   }
   int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC); btrfs_ioctl_fs_info_args fs={};
   bool btrfs=fd>=0 && !ioctl(fd,BTRFS_IOC_FS_INFO,&fs); if (fd>=0) close(fd);
   if (!btrfs) return;
   std::string directory="/sys/fs/btrfs/"+btrfsUuid(fs)+"/devices"; DIR *devices=opendir(directory.c_str());
   if (!devices) return;
   while (auto entry=readdir(devices))
   {
      if (entry->d_name[0]=='.') continue;
      char *resolved=realpath((directory+"/"+entry->d_name).c_str(),nullptr);
      auto block=resolved?udev_device_new_from_syspath(context,resolved):nullptr; free(resolved);
      if (block) { protect(context,udev_device_get_devnum(block),paths); udev_device_unref(block); }
   }
   closedir(devices);
}

// Obtain filesystem identities from libblkid even when an ordinary data partition is unmounted.
static void filesystem(Partition &part)
{
   auto probe=blkid_new_probe_from_filename(part.path.c_str());
   if (!probe) return;
   part.filesystem.clear(); part.uuid.clear(); part.volumeLabel.clear();
   blkid_probe_set_partitions_flags(probe,BLKID_PARTS_ENTRY_DETAILS);
   if (!blkid_do_safeprobe(probe))
   {
      const char *value=nullptr;
      if (!blkid_probe_lookup_value(probe,"TYPE",&value,nullptr)) part.filesystem=value;
      if (!blkid_probe_lookup_value(probe,"UUID",&value,nullptr)) part.uuid=value;
      if (!blkid_probe_lookup_value(probe,"LABEL",&value,nullptr)) part.volumeLabel=value;
      if (!blkid_probe_lookup_value(probe,"PART_ENTRY_TYPE",&value,nullptr)) part.type=value;
   }
   blkid_free_probe(probe);
   if (part.filesystem=="btrfs")
   {
      unsigned char header[160]={}; int fd=open(part.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
      ssize_t bytes=fd<0?-1:pread(fd,header,sizeof(header),65536); if (fd>=0) close(fd);
      uint64_t count=0; for (int n=7; n>=0; n--) count=count*256+header[0x88+n];
      if (bytes==sizeof(header) && !memcmp(header+0x40,"_BHRfS_M",8) && count>1) part.filesystem="btrfs_member";
   }
}

// Inventory physical disks and each partition separately, retaining every sibling in erase warnings.
std::vector<Device> scanDevices(const std::string &source)
{
   std::vector<Device> result;
   auto context=udev_new();
   if (!context) return result;
   std::set<std::string> system,origin;
   auto mounts=mountedFilesystems();
   for (const auto &path:{source,std::string("/proc/self/exe")}) if (!path.empty()) protectPath(context,path,origin);
   for (const auto &mount:mounts)
   {
      const auto &path=mount.path;
      bool active=path=="/" || path=="/boot" || path=="/boot/efi" || path=="/usr" || path=="/var" || path=="/home";
      bool live=path.find("/run/initramfs")==0 || path.find("/memory")==0;
      if (active || live) protect(context,mount.source,system);
   }
   auto enumeration=udev_enumerate_new(context);
   udev_enumerate_add_match_subsystem(enumeration,"block"); udev_enumerate_scan_devices(enumeration);
   std::vector<std::string> paths;
   udev_list_entry *entry;
   udev_list_entry_foreach(entry,udev_enumerate_get_list_entry(enumeration)) paths.push_back(udev_list_entry_get_name(entry));
   for (const auto &path:paths)
   {
      auto disk=udev_device_new_from_syspath(context,path.c_str());
      if (!disk) continue;
      std::string name=udev_device_get_sysname(disk);
      bool virtualDisk=name.find("loop")==0 || name.find("zram")==0 || name.find("ram")==0 || name.find("fd")==0;
#ifdef INSTALLER_TEST_STORAGE
      if (name.find("loop")==0) virtualDisk=false;
#endif
      bool physical=property(disk,"DEVTYPE")=="disk" && property(disk,"ID_CDROM")!="1" && !virtualDisk;
      if (!physical || !udev_device_get_devnode(disk)) { udev_device_unref(disk); continue; }
      Device whole;
      whole.path=whole.disk=udev_device_get_devnode(disk);
      whole.bytes=whole.diskBytes=strtoull(property(disk,"size",true).c_str(),nullptr,10)*512;
      whole.sector=strtoul(property(disk,"queue/logical_block_size",true).c_str(),nullptr,10);
      if (!whole.sector) whole.sector=512;
      whole.diskId=path+":"+property(disk,"USEC_INITIALIZED")+":"+property(disk,"ID_SERIAL")+":"+std::to_string(udev_device_get_devnum(disk))+":"+std::to_string(whole.bytes);
      whole.sequence=strtoull(property(disk,"diskseq",true).c_str(),nullptr,10);
      whole.diskId+=":"+std::to_string(whole.sequence);
      whole.id=whole.diskId;
      whole.name=property(disk,"ID_MODEL_FROM_DATABASE");
      if (whole.name.empty()) whole.name=property(disk,"ID_MODEL");
      if (whole.name.empty()) whole.name=name;
      std::replace(whole.name.begin(),whole.name.end(),'_',' ');
      whole.label=name;
      whole.usb=udev_device_get_parent_with_subsystem_devtype(disk,"usb","usb_device")!=nullptr;
      bool layered=name.find("dm-")==0 || name.find("md")==0 || name.find("loop")==0;
      bool virtio=udev_device_get_parent_with_subsystem_devtype(disk,"virtio",nullptr)!=nullptr || name.find("vd")==0;
      whole.virtualDisk=layered || virtio;
      whole.system=system.count(path);
      whole.readonly=property(disk,"ro",true)=="1";
      if (whole.readonly) whole.blocked="This disk is read-only.";
      if (name.find("dm-")==0 || name.find("md")==0) whole.blocked="Layered RAID or device-mapper storage cannot be modified safely by this installer. Select a basic disk or partition.";
      if (system.count(path)) whole.eraseBlocked="This disk contains the running system or live session. Start Slax from another disk before erasing it.";
      if (!whole.system && origin.count(path)) whole.eraseBlocked="This disk contains the ISO image or running installer. Move them to another disk before erasing it.";
      for (const auto &childPath:paths)
      {
         auto child=udev_device_new_from_syspath(context,childPath.c_str());
         if (!child) continue;
         auto parent=udev_device_get_parent_with_subsystem_devtype(child,"block","disk");
         bool partition=property(child,"DEVTYPE")=="partition" && udev_device_get_devnode(child);
         bool belongs=parent && path==udev_device_get_syspath(parent);
         if (partition && belongs)
         {
            Partition part;
            part.path=udev_device_get_devnode(child); part.label=udev_device_get_sysname(child);
            part.number=strtoul(property(child,"partition",true).c_str(),nullptr,10);
            part.start=strtoull(property(child,"start",true).c_str(),nullptr,10)*512;
            part.bytes=strtoull(property(child,"size",true).c_str(),nullptr,10)*512;
            part.type=property(child,"ID_PART_ENTRY_TYPE");
            part.filesystem=property(child,"ID_FS_TYPE"); part.uuid=property(child,"ID_FS_UUID");
            part.readonly=property(child,"ro",true)=="1";
            filesystem(part);
            part.free=UINT64_MAX;
            for (const auto &mount:mounts) if (mount.root && mount.source==udev_device_get_devnum(child))
            {
               part.mount=mount.path; part.readonly=part.readonly || mount.readonly;
               struct statvfs space;
               if (!statvfs(mount.path.c_str(),&space)) part.free=uint64_t(space.f_bavail)*space.f_frsize;
            }
            whole.partitions.push_back(part);
         }
         udev_device_unref(child);
      }
      std::sort(whole.partitions.begin(),whole.partitions.end(),[](const Partition &a,const Partition &b) { return a.start<b.start; });
      whole.layout=property(disk,"ID_PART_TABLE_TYPE")+":"+property(disk,"ID_PART_TABLE_UUID");
      for (const auto &part:whole.partitions) whole.layout+=";"+std::to_string(part.number)+":"+std::to_string(part.start)+":"+std::to_string(part.bytes)+":"+part.uuid+":"+part.type;
      whole.free=UINT64_MAX;
      if (whole.partitions.empty())
      {
         filesystem(whole);
         for (const auto &mount:mounts) if (mount.root && mount.source==udev_device_get_devnum(disk)) { whole.mount=mount.path; whole.readonly=whole.readonly || mount.readonly; }
      }
      whole.detail="Disk · "+std::to_string(whole.partitions.size())+" partitions";
      if (whole.bytes) result.push_back(whole);
      for (const auto &part:whole.partitions)
      {
         if (part.bytes<1000000000) continue;
         Device target=whole; static_cast<Partition &>(target)=part;
         target.id=whole.diskId+":"+std::to_string(part.start)+":"+std::to_string(part.bytes)+":"+part.uuid;
         target.name=whole.name+" · "+(part.volumeLabel.empty()?part.label:part.volumeLabel);
         target.detail=(part.filesystem.empty()?"Unformatted":part.filesystem)+(part.free==UINT64_MAX?" · not mounted":" · "+sizeText(part.free)+" free");
         result.push_back(target);
      }
      udev_device_unref(disk);
   }
   udev_enumerate_unref(enumeration); udev_unref(context);
   return result;
}

// Inspect types without waiting on FIFOs; accept files or optical aliases but reject other raw devices.
FILE *openImage(const std::string &path)
{
   auto resolved=realpath(path.c_str(),nullptr); if (!resolved) return nullptr;
   int fd=open(resolved,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW); free(resolved);
   if (fd<0) return nullptr;
   struct stat info;
   if (fstat(fd,&info)) { close(fd); return nullptr; }
   bool optical=S_ISBLK(info.st_mode) && ioctl(fd,CDROM_GET_CAPABILITY)>=0;
   if (!S_ISREG(info.st_mode) && !optical) { close(fd); return nullptr; }
   if (fcntl(fd,F_SETFL,0)) { close(fd); return nullptr; }
   FILE *file=fdopen(fd,"rb"); if (!file) close(fd); return file;
}

// Reject invalid UTF-8 and Unicode case aliases before any FAT or Windows destination is written.
std::string filenameKey(const std::string &name)
{
   if (!g_utf8_validate(name.data(),name.size(),nullptr)) throw std::runtime_error("The ISO contains a filename that is not valid UTF-8.");
   char *folded=g_utf8_casefold(name.c_str(),name.size()); std::string result=folded; g_free(folded); return result;
}

struct Disk::Impl { int fd=-1,writable=-1,exclusive=-1; bool claimed=false; std::vector<DiskRange> ranges; };

// Reopen the exact physical device, refusing stale identities before taking an exclusive write claim.
Disk::Disk(const Device &expected):impl(new Impl),device(expected)
{
   auto current=scanDevices("");
   auto found=std::find_if(current.begin(),current.end(),[&](const Device &d) { return d.id==expected.id; });
   if (found==current.end() || found->layout!=expected.layout || found->diskId!=expected.diskId) throw std::runtime_error("The selected disk or partition layout changed. Select it again.");
   impl->fd=open(expected.disk.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
   if (impl->fd<0) failed("Cannot open the selected physical disk; run the installer as root");
   struct stat s; uint64_t bytes=0; int sector=0;
   bool valid=!fstat(impl->fd,&s) && S_ISBLK(s.st_mode) && !ioctl(impl->fd,BLKGETSIZE64,&bytes) && !ioctl(impl->fd,BLKSSZGET,&sector);
   if (!valid || bytes!=expected.diskBytes || uint32_t(sector)!=expected.sector) { close(impl->fd); impl->fd=-1; throw std::runtime_error("The disk geometry changed or is not a physical block device."); }
   uint64_t sequence=0;
   if (expected.sequence && (ioctl(impl->fd,BLKGETDISKSEQ,&sequence) || sequence!=expected.sequence)) { close(impl->fd); impl->fd=-1; throw std::runtime_error("The removable medium changed. Select the target again."); }
}

// Release the kernel's exclusive disk claim only after all writes and verification finish.
Disk::~Disk() { release(); if (impl->fd>=0) close(impl->fd); }

// Planning stays read-only: closing a writable block handle would trigger a competing udev probe.
void Disk::reserve()
{
   if (impl->writable>=0) return;
   if (flock(impl->fd,LOCK_EX|LOCK_NB)) failed("Another disk operation is already using this physical disk");
   verify();
   impl->writable=open(device.disk.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
   if (impl->writable<0) failed("Cannot open the selected physical disk for writing");
   struct stat original,current; uint64_t sequence=0;
   bool sameNode=!fstat(impl->fd,&original) && !fstat(impl->writable,&current) && original.st_dev==current.st_dev && original.st_ino==current.st_ino && original.st_rdev==current.st_rdev;
   bool sameMedium=!device.sequence || (!ioctl(impl->writable,BLKGETDISKSEQ,&sequence) && sequence==device.sequence);
   if (!sameNode || !sameMedium) throw std::runtime_error("The approved disk or removable medium changed; installation was stopped.");
}

// Claim the whole device only after approval, while retaining the originally approved disk handle.
void Disk::claim(bool erase)
{
   auto current=scanDevices("");
   auto found=std::find_if(current.begin(),current.end(),[&](const Device &d) { return d.id==device.id; });
   if (found==current.end() || found->layout!=device.layout) throw std::runtime_error("The disk changed before exclusive access could be obtained.");
   if (erase && !device.eraseBlocked.empty()) throw std::runtime_error(device.eraseBlocked);
   reserve();
   std::set<dev_t> volumes;
   struct stat s;
   if (!stat(device.disk.c_str(),&s)) volumes.insert(s.st_rdev);
   for (const auto &part:device.partitions) if (!stat(part.path.c_str(),&s)) volumes.insert(s.st_rdev);
   std::vector<std::string> mounts;
   for (const auto &mount:mountedFilesystems()) if (volumes.count(mount.source)) mounts.push_back(mount.path);
   std::sort(mounts.begin(),mounts.end(),[](const auto &a,const auto &b) { return a.size()>b.size(); });
   for (const auto &mount:mounts) if (umount(mount.c_str())) failed("A partition is in use and could not be unmounted; close applications using "+mount);
   impl->exclusive=open(device.disk.c_str(),O_RDWR|O_EXCL|O_CLOEXEC|O_NOFOLLOW);
   if (impl->exclusive<0) failed("The disk is mounted or in use. Unmount all its partitions and close applications before retrying");
   unsigned char sector[4096]; read(0,sector,device.sector);
   impl->claimed=true;
}

// Preserve installs write only approved boot metadata and unused ranges, without unmounting live filesystems.
void Disk::claimBoot(const std::vector<DiskRange> &ranges)
{
   reserve(); verify(); impl->ranges=ranges;
}

// Release the exclusive claim before asking the kernel to discover the completed filesystems.
void Disk::release()
{
   impl->ranges.clear();
   impl->claimed=false;
   if (impl->exclusive>=0) { close(impl->exclusive); impl->exclusive=-1; }
   // Keep udev excluded until every writable block handle has closed.
   if (impl->writable>=0) { close(impl->writable); impl->writable=-1; }
   flock(impl->fd,LOCK_UN);
}

// Short disk reads are failures, never padding or successful verification.
void Disk::read(uint64_t offset,void *data,size_t bytes)
{
   verify();
   if (offset>device.diskBytes || bytes>device.diskBytes-offset) throw std::runtime_error("A disk read is outside the approved device.");
   if (pread(impl->fd,data,bytes,offset)!=ssize_t(bytes)) failed("Cannot read the selected disk completely");
}

// Every write is bounded by the retained device handle and sector geometry.
void Disk::write(uint64_t offset,const void *data,size_t bytes)
{
   bool approved=impl->claimed;
   for (const auto &range:impl->ranges) if (offset>=range.start && offset-range.start<=range.bytes && bytes<=range.bytes-(offset-range.start)) approved=true;
   verify();
   if (!approved || offset>device.diskBytes || bytes>device.diskBytes-offset || offset%device.sector || bytes%device.sector) throw std::runtime_error("An unapproved, unaligned or out-of-range disk write was refused.");
   if (pwrite(impl->writable,data,bytes,offset)!=ssize_t(bytes)) failed("Cannot write the selected disk completely");
}

// The kernel generation counter distinguishes card-reader media replacements from the same device node.
void Disk::verify()
{
   uint64_t sequence=0;
   if (device.sequence && (ioctl(impl->fd,BLKGETDISKSEQ,&sequence) || sequence!=device.sequence)) throw std::runtime_error("The approved disk or removable medium changed; installation was stopped.");
}

// Report device flush failures instead of showing installation success prematurely.
void Disk::flush() { if (fsync(impl->writable)) failed("Cannot flush the target disk"); }

struct FileTree::Impl
{
   int fd=-1; std::string mount; bool owned=false;
   ~Impl() { if (fd>=0) close(fd); if (owned) { umount(mount.c_str()); rmdir(mount.c_str()); } }
};

// Walk directory components relative to held descriptors, never following a symlink.
static int parent(int root,const std::string &path,std::string &leaf,bool allowMissing=false)
{
   int fd=dup(root); size_t start=0;
   while (true)
   {
      auto slash=path.find('/',start);
      if (slash==std::string::npos) { leaf=path.substr(start); return fd; }
      auto name=path.substr(start,slash-start); int next=openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); close(fd);
      if (next<0 && allowMissing && errno==ENOENT) return -1;
      if (next<0) failed("Cannot safely open the destination directory");
      fd=next; start=slash+1;
   }
}

// Probe an unmounted volume read-only first; writable mounts happen only after confirmation.
FileTree::FileTree(const Partition &partition,bool writable):impl(new Impl)
{
   // Verify Windows hibernation through a read-only handle before any writable NTFS mount.
   if (writable && partition.filesystem=="ntfs") { FileTree probe(partition,false); }
   impl->mount=partition.mount;
   struct stat block;
   if (stat(partition.path.c_str(),&block) || !S_ISBLK(block.st_mode)) throw std::runtime_error("The destination partition is no longer available.");
   bool mounted=false; std::string currentRoot;
   for (const auto &mount:mountedFilesystems()) if (mount.root && mount.source==block.st_rdev)
   {
      if (currentRoot.empty()) currentRoot=mount.path;
      if (mount.path==impl->mount) { mounted=true; break; }
   }
   // Another retained journal may already own a private writable mount of this filesystem.
   if (!mounted) impl->mount=currentRoot;
   if (impl->mount.empty())
   {
      char directory[]="/tmp/slax-install-mount.XXXXXX";
      if (!mkdtemp(directory)) failed("Cannot create a private mount directory");
      impl->mount=directory;
      std::string type=partition.filesystem=="ntfs"?"ntfs3":partition.filesystem;
      const char *options=nullptr;
      if (!writable && (type=="ext4" || type=="ext3")) options="noload";
      if (!writable && type=="xfs") options="norecovery";
      if (!writable && type=="btrfs") options="rescue=nologreplay";
      if (mount(partition.path.c_str(),directory,type.c_str(),MS_NODEV|MS_NOSUID|(writable?0:MS_RDONLY),options))
      {
         int saved=errno; rmdir(directory); errno=saved;
         failed("Cannot mount "+partition.path+" safely; a hibernated, damaged or unsupported filesystem must be repaired first");
      }
      impl->owned=true;
   }
   impl->fd=::open(impl->mount.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
   if (impl->fd<0) failed("Cannot open the selected filesystem");
   struct stat s;
   if (fstat(impl->fd,&s)) failed("Cannot identify the destination filesystem");
   btrfs_ioctl_fs_info_args fs={};
   bool btrfs=partition.filesystem=="btrfs" && !ioctl(impl->fd,BTRFS_IOC_FS_INFO,&fs);
   bool btrfsMatches=btrfs && fs.num_devices==1 && !g_ascii_strcasecmp(btrfsUuid(fs).c_str(),partition.uuid.c_str());
   bool belongs=false;
   for (const auto &mount:mountedFilesystems()) if (mount.path==impl->mount && mount.source==block.st_rdev && (mount.number==s.st_dev || btrfsMatches)) belongs=true;
   if (!belongs) throw std::runtime_error("The destination mount does not belong to the approved partition.");
   struct statvfs space;
   if (fstatvfs(impl->fd,&space) || (writable && (space.f_flag&ST_RDONLY))) throw std::runtime_error("The destination filesystem is read-only.");
   if (partition.filesystem=="ntfs")
   {
      DIR *directory=fdopendir(dup(impl->fd)); if (!directory) failed("Cannot inspect Windows hibernation state");
      std::string name; errno=0;
      while (auto entry=readdir(directory)) if (!g_ascii_strcasecmp(entry->d_name,"hiberfil.sys")) name=entry->d_name;
      int error=errno;
      closedir(directory);
      if (error) { errno=error; failed("Cannot verify Windows hibernation state"); }
      if (!name.empty())
      {
         int file=openat(impl->fd,name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC); unsigned char header[4096]={}; struct stat info;
         bool regular=file>=0 && !fstat(file,&info) && S_ISREG(info.st_mode);
         ssize_t bytes=regular?read(file,header,sizeof(header)):-1; if (file>=0) close(file);
         bool inactive=bytes==sizeof(header) && std::all_of(header,header+sizeof(header),[](unsigned char byte) { return byte==0; });
         if (!inactive) throw std::runtime_error("Windows is hibernated or its hibernation state cannot be verified. Resume Windows and shut it down fully, without hibernation or Fast Startup, before installing from Linux. No Windows files were changed.");
      }
   }
}

// Unmount only private mounts created by this operation, keeping user mounts untouched.
FileTree::~FileTree()=default;
// Let the trusted verifier read through the pinned root, even if its mount name changes.
std::string FileTree::rootPath() { return "/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(impl->fd)+"/"; }

// Existing files and links both count as occupied destinations.
bool FileTree::exists(const std::string &path)
{
   struct stat info; return !fstatat(impl->fd,path.c_str(),&info,AT_SYMLINK_NOFOLLOW);
}

// Classify only ordinary objects below pinned parents, without opening special files for I/O.
bool FileTree::isDirectory(const std::string &path)
{
   std::string leaf; int directory=parent(impl->fd,path,leaf);
   int fd=openat(directory,leaf.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC),error=errno; close(directory); errno=error;
   if (fd<0) failed("Cannot inspect the destination entry "+path);
   struct stat info; int result=fstat(fd,&info); error=errno; close(fd); errno=error;
   if (result) failed("Cannot identify the destination entry "+path);
   if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) throw std::runtime_error("The destination contains a link or special file: "+path);
   return S_ISDIR(info.st_mode);
}

// Compare held native identities so source symlinks, hard links and bind mounts cannot disguise self-copying.
bool FileTree::same(const std::string &path,const std::string &external)
{
   std::string leaf; int directory=parent(impl->fd,path,leaf,true);
   if (directory<0) return false;
   int target=openat(directory,leaf.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC),error=errno; close(directory); errno=error;
   if (target<0) { if (errno==ENOENT) return false; failed("Cannot identify the destination entry "+path); }
   try
   {
      struct stat a,b;
      if (fstat(target,&a)) failed("Cannot identify the destination entry "+path);
      if (!S_ISREG(a.st_mode) && !S_ISDIR(a.st_mode)) throw std::runtime_error("The destination contains a link or special file: "+path);
      int source=::open(external.c_str(),O_PATH|O_CLOEXEC);
      if (source<0) failed("Cannot identify the source path");
      int result=fstat(source,&b); error=errno; close(source); errno=error;
      if (result) failed("Cannot identify the source path");
      // Validated optical sources are devices and cannot alias an ordinary destination entry.
      if (S_ISBLK(b.st_mode)) { close(target); return false; }
      if (!S_ISREG(b.st_mode) && !S_ISDIR(b.st_mode)) throw std::runtime_error("The source is not a regular file or directory.");
      close(target); return a.st_dev==b.st_dev && a.st_ino==b.st_ino;
   }
   catch (...) { close(target); throw; }
}

// New directories are created exclusively below a safely held parent.
void FileTree::mkdir(const std::string &path)
{
   std::string leaf; int fd=parent(impl->fd,path,leaf); int result=mkdirat(fd,leaf.c_str(),0755); close(fd);
   if (result) failed("Cannot create "+path);
}

// Never overwrite a file while extracting a new installation.
FILE *FileTree::open(const std::string &path,bool write,uint32_t mode)
{
   std::string leaf; int directory=parent(impl->fd,path,leaf);
   int fd=openat(directory,leaf.c_str(),(write?O_RDWR|O_CREAT|O_EXCL:O_RDONLY)|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,mode&0777); close(directory);
   if (fd<0) failed("Cannot open destination file "+path);
   struct stat info;
   if (fstat(fd,&info) || !S_ISREG(info.st_mode)) { close(fd); throw std::runtime_error("The destination is not a regular file: "+path); }
   FILE *file=fdopen(fd,write?"w+b":"rb");
   if (!file) { close(fd); failed("Cannot open a destination stream"); }
   return file;
}

// Publish a verified tree atomically without replacing any pre-existing installation.
void FileTree::move(const std::string &from,const std::string &to)
{
   std::string source,destination; int a=parent(impl->fd,from,source),b;
   try { b=parent(impl->fd,to,destination); } catch (...) { close(a); throw; }
   int result=syscall(SYS_renameat2,a,source.c_str(),b,destination.c_str(),RENAME_NOREPLACE),error=errno; close(a); close(b); errno=error;
   if (result) failed("Cannot publish the verified installation");
   sync();
}

// Publish an approved boot configuration atomically, preserving its owner and access mode.
void FileTree::replace(const std::string &from,const std::string &to)
{
   std::string source,destination; int a=parent(impl->fd,from,source),b; struct stat info;
   try { b=parent(impl->fd,to,destination); } catch (...) { close(a); throw; }
   int file=openat(a,source.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
   bool valid=file>=0 && !fstat(file,&info) && S_ISREG(info.st_mode);
   if (valid) valid=!fstatat(b,destination.c_str(),&info,AT_SYMLINK_NOFOLLOW) && S_ISREG(info.st_mode);
   if (valid) valid=!fchown(file,info.st_uid,info.st_gid) && !fchmod(file,info.st_mode&07777) && !fsync(file);
   if (file>=0) close(file);
   int result=valid?renameat(a,source.c_str(),b,destination.c_str()):-1; close(a); close(b);
   if (result) throw std::runtime_error("Cannot atomically update the boot configuration.");
   sync();
}

// Native BCD editing belongs to Windows; Linux can retain it behind the text system selector.
std::string readBcd(const Partition &) { return ""; }
void addBcdEntry(const InstallPlan &) { throw std::runtime_error("Native Windows BCD editing is unavailable in this session."); }

// Rollback removes only the explicitly recorded files and empty directories created by this run.
void FileTree::remove(const std::string &path,bool directory)
{
   try
   {
      std::string leaf; int fd=parent(impl->fd,path,leaf); unlinkat(fd,leaf.c_str(),directory?AT_REMOVEDIR:0); close(fd);
   }
   catch (...) {}
}

// Space is measured on the held filesystem, after any private mount has been established.
uint64_t FileTree::available()
{
   struct statvfs space;
   if (fstatvfs(impl->fd,&space)) failed("Cannot determine free destination space");
   return uint64_t(space.f_bavail)*space.f_frsize;
}

// Flush filesystem metadata before making the new boot entry active.
void FileTree::sync() { if (syncfs(impl->fd)) failed("Cannot flush the destination filesystem"); }

// Flush individual payload files before their read-back verification.
void flushFile(FILE *file) { if (fflush(file) || fsync(fileno(file))) failed("Cannot flush a copied file"); }

// Use kernel randomness for partition GUIDs, staging directories and filesystem serials.
std::string randomId()
{
   unsigned char bytes[16];
   if (getrandom(bytes,sizeof(bytes),0)!=sizeof(bytes)) failed("Cannot generate a unique installation identity");
   const char *hex="0123456789abcdef"; std::string result;
   for (auto byte:bytes) { result+=hex[byte>>4]; result+=hex[byte&15]; }
   return result;
}

// Disk writes require root; discovery and image validation remain available without it.
bool administrator() { return geteuid()==0; }

// Linux retains the standard GPT erase layout.
bool windowsXp() { return false; }
bool mbrRequired(const Device &) { return false; }

// Ask the kernel to expose the finished table only after raw writers release their claims.
void refreshDisk(const Device &device)
{
   int fd=open(device.disk.c_str(),O_RDONLY|O_CLOEXEC);
   if (fd>=0) { ioctl(fd,BLKRRPART); close(fd); }
}

// Read standard UEFI boot variables through efivarfs, keeping their attributes outside the payload.
std::vector<unsigned char> readFirmware(const std::string &name)
{
   std::string path="/sys/firmware/efi/efivars/"+name+"-8be4df61-93ca-11d2-aa0d-00e098032b8c";
   int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
   if (fd<0) { if (errno==ENOENT) return {}; failed("Cannot read UEFI boot variables"); }
   std::vector<unsigned char> data(262148); ssize_t size=read(fd,data.data(),data.size()); close(fd);
   if (size<4 || size==ssize_t(data.size())) throw std::runtime_error("The UEFI boot variable has an invalid size.");
   return {data.begin()+4,data.begin()+size};
}

// Write only standard Boot#### and BootOrder variables, with normal nonvolatile runtime attributes.
void writeFirmware(const std::string &name,const std::vector<unsigned char> &data)
{
   std::string path="/sys/firmware/efi/efivars/"+name+"-8be4df61-93ca-11d2-aa0d-00e098032b8c";
   if (data.empty()) { if (unlink(path.c_str()) && errno!=ENOENT) failed("Cannot remove the new UEFI boot variable"); return; }
   int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0644);
   if (fd<0) failed("Cannot update UEFI boot variables");
   std::vector<unsigned char> value={7,0,0,0}; value.insert(value.end(),data.begin(),data.end());
   ssize_t written=write(fd,value.data(),value.size()); int saved=errno; close(fd); errno=saved;
   if (written!=ssize_t(value.size())) failed("The firmware did not accept the complete boot variable");
}

// An internal multiboot installation requires real UEFI runtime services in this session.
bool firmwareAvailable()
{
   std::ifstream file("/sys/firmware/efi/fw_platform_size"); int bits=0; file>>bits;
   if (bits!=64) return false;
   try { return !readFirmware("BootOrder").empty(); } catch (...) { return false; }
}

// Materialize the selected source's verified tools and modules in an owner-private directory.
struct GrubWorkspace
{
   std::string folder;
   explicit GrubWorkspace(const GrubAssets &assets)
   {
      auto temporary=std::filesystem::canonical(std::filesystem::temp_directory_path());
      // Protect the workspace name too: a writable ancestor could replace its private directory.
      for (auto parent=temporary;;parent=parent.parent_path())
      {
         struct stat info;
         if (stat(parent.c_str(),&info)) failed("Cannot inspect the BIOS tools temporary directory");
         bool writable=info.st_mode&(S_IWGRP|S_IWOTH);
         if (info.st_uid!=geteuid() || (writable && !(info.st_mode&S_ISVTX)))
            throw std::runtime_error("The BIOS tools need a protected temporary directory. Set TMPDIR to a root-owned directory that permits execution, such as /tmp or /var/tmp.");
         if (parent==parent.root_path()) break;
      }
      auto pattern=(temporary/"slax-install-grub.XXXXXX").string();
      std::vector<char> name(pattern.begin(),pattern.end()); name.push_back(0);
      if (!mkdtemp(name.data())) failed("Cannot create the BIOS tools workspace");
      folder=name.data();
      try
      {
         for (const auto &asset:assets.entries())
         {
            auto path=std::filesystem::path(folder)/asset.name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path,std::ios::binary); file.write(asset.data.data(),asset.data.size()); file.close();
            if (!file) throw std::runtime_error("Cannot write a verified BIOS tool or module.");
            if (chmod(path.c_str(),asset.mode&0111?0700:0600)) failed("Cannot set BIOS tool permissions");
         }
      }
      catch (...) { std::error_code error; std::filesystem::remove_all(folder,error); throw; }
   }
   // Cleanup must not hide an installation or verification error.
   ~GrubWorkspace() { std::error_code error; std::filesystem::remove_all(folder,error); }
};

// Run the verified absolute helper without a shell, preserving bounded native diagnostics.
static void runGrub(const std::string &executable,std::vector<std::string> arguments,const std::string &failure)
{
   std::unique_ptr<FILE,int (*)(FILE *)> log(tmpfile(),fclose);
   if (!log) failed("Cannot create the BIOS tools log");
   arguments.insert(arguments.begin(),executable);
   std::vector<char *> argv; for (auto &argument:arguments) argv.push_back(argument.data()); argv.push_back(nullptr);
   posix_spawn_file_actions_t actions;
   int error=posix_spawn_file_actions_init(&actions);
   if (error) { errno=error; failed("Cannot prepare the BIOS tool"); }
   error=posix_spawn_file_actions_adddup2(&actions,fileno(log.get()),STDOUT_FILENO);
   if (!error) error=posix_spawn_file_actions_adddup2(&actions,fileno(log.get()),STDERR_FILENO);
   if (!error && fileno(log.get())>STDERR_FILENO) error=posix_spawn_file_actions_addclose(&actions,fileno(log.get()));
   pid_t child=0;
   if (!error) error=posix_spawn(&child,executable.c_str(),&actions,nullptr,argv.data(),environ);
   posix_spawn_file_actions_destroy(&actions);
   if (error==EACCES) throw std::runtime_error("The BIOS tools cannot execute in this temporary directory. Set TMPDIR to a directory that permits program execution.");
   if (error) { errno=error; failed("Cannot start the source BIOS tool"); }
   int status=0; while (waitpid(child,&status,0)<0) if (errno!=EINTR) failed("Cannot wait for the BIOS tool");
   if (!WIFEXITED(status) || WEXITSTATUS(status))
   {
      char detail[1025]={}; rewind(log.get()); size_t bytes=fread(detail,1,1024,log.get());
      throw std::runtime_error(failure+"\n"+std::string(detail,bytes));
   }
}

// Embed only the disk/filesystem reader; matching menu modules live in the new Slax directory.
BiosImage buildBios(const Partition &target,bool gpt,const GrubAssets &assets)
{
   GrubWorkspace work(assets); BiosImage result;
   std::string prefix="(,"+std::string(gpt?"gpt":"msdos")+std::to_string(target.number)+")/slax/boot/grub";
   runGrub(work.folder+"/"+assets.tool("grub-mkimage").name,
      {"-d",work.folder+"/i386-pc","-O","i386-pc","-C","xz","-p",prefix,"-o",work.folder+"/core","biosdisk",gpt?"part_gpt":"part_msdos",biosFilesystem(target)},
      "The source GRUB tools could not build the BIOS loader.");
   FILE *file=fopen((work.folder+"/core").c_str(),"rb");
   if (!file) failed("Cannot read the BIOS loader");
   result.core.resize(1024*1024); size_t bytes=fread(result.core.data(),1,result.core.size(),file); bool valid=!ferror(file) && feof(file); fclose(file);
   if (!valid) throw std::runtime_error("The BIOS loader is too large.");
   result.core.resize(bytes);
   for (const auto &asset:assets.entries())
   {
      if (asset.name=="i386-pc/boot.img") result.boot=asset.data;
      bool module=asset.name.size()>4 && (asset.name.substr(asset.name.size()-4)==".mod" || asset.name.substr(asset.name.size()-4)==".lst");
      if (asset.name.find("i386-pc/")==0 && module) result.modules.push_back(generated("slax/boot/grub/"+asset.name,asset.data));
   }
   return result;
}

// Use the source's matching GRUB reader for filesystem features and copied boot-file comparisons.
void checkBiosFiles(const Device &device,bool installed,const GrubAssets &assets)
{
   FileTree target(device,false); GrubWorkspace work(assets);
   std::vector<std::string> arguments={device.path};
   if (installed) arguments.insert(arguments.end(),{"cmp","/slax/boot",target.rootPath()+"slax/boot"});
   else arguments.push_back("xnu_uuid");
   runGrub(work.folder+"/"+assets.tool("grub-fstest").name,std::move(arguments),
      "GRUB cannot read this filesystem or verify its boot files. Existing BIOS boot settings will be kept.");
}
