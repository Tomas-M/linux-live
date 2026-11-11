#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <wincrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#include <set>
#include <cstring>
#include "storage.h"
#include "disk-format.h"
#include "hash.h"
#include "windows-native.h"
#include "grub-assets.h"

// Convert UTF-8 paths at the Windows boundary without lossy code-page conversions.
std::wstring wide(const std::string &value)
{
   int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),value.size(),nullptr,0);
   if (!value.empty() && !length) throw std::runtime_error("A filename is not valid UTF-8.");
   std::wstring result(length,L'\0');
   if (length) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),value.size(),result.data(),length);
   return result;
}

// Keep native device labels and mount paths intact in the shared model.
std::string utf8(const std::wstring &value)
{
   int length=WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),nullptr,0,nullptr,nullptr);
   std::string result(length,'\0');
   if (length) WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),result.data(),length,nullptr,nullptr);
   return result;
}

// Query Windows' ISO attachment metadata without requiring VirtDisk.dll on XP.
std::string sourceImage()
{
   wchar_t program[32768],root[32768],system[MAX_PATH];
   if (!GetModuleFileNameW(nullptr,program,32768) || !GetVolumePathNameW(program,root,32768)) return "";
   if (GetDriveTypeW(root)!=DRIVE_CDROM || !GetSystemDirectoryW(system,MAX_PATH)) return "";
   HMODULE library=LoadLibraryW((std::wstring(system)+L"\\virtdisk.dll").c_str());
   if (!library) return "";
   using Query=DWORD (WINAPI *)(HANDLE,DWORD,ULONG,void *,ULONG *);
   auto query=reinterpret_cast<Query>(GetProcAddress(library,"GetStorageDependencyInformation"));
   HANDLE volume=CreateFileW(root,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
   std::string result;
   if (query && volume!=INVALID_HANDLE_VALUE)
   {
      // STORAGE_DEPENDENCY_INFO v2 layout; only ISO entries describe this executable's medium.
      struct Type { ULONG device; GUID vendor; };
      struct Entry { DWORD flags,provider; Type type; ULONG ancestor; PWSTR device,host,volume,path; };
      struct Info { ULONG version,count; Entry entries[1]; };
      std::vector<unsigned char> buffer(4096); ULONG needed=0;
      auto info=reinterpret_cast<Info *>(buffer.data()); info->version=2;
      DWORD status=query(volume,1,buffer.size(),info,&needed);
      if (status==ERROR_INSUFFICIENT_BUFFER && needed>buffer.size())
      {
         buffer.resize(needed); info=reinterpret_cast<Info *>(buffer.data()); info->version=2;
         status=query(volume,1,buffer.size(),info,&needed);
      }
      ULONG nearest=MAXDWORD;
      if (status==ERROR_SUCCESS) for (ULONG i=0; i<info->count; i++)
      {
         const auto &entry=info->entries[i];
         if (entry.type.device!=1 || entry.ancestor>=nearest) continue;
         if (!entry.host || !entry.path) continue;
         wchar_t paths[32768]; DWORD length=0; std::wstring host=entry.host,relative=entry.path;
         if (!host.empty() && host.back()!=L'\\') host+=L'\\';
         if (GetVolumePathNamesForVolumeNameW(host.c_str(),paths,32768,&length) && paths[0]) host=paths;
         if (!relative.empty() && relative.front()==L'\\' && !host.empty() && host.back()==L'\\') relative.erase(0,1);
         auto path=host+relative; DWORD attributes=GetFileAttributesW(path.c_str());
         if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
         result=utf8(path); nearest=entry.ancestor;
      }
   }
   if (volume!=INVALID_HANDLE_VALUE) CloseHandle(volume);
   FreeLibrary(library); return result;
}

// Volume-GUID paths use the extended namespace, where Windows does not normalize forward slashes.
static std::wstring nativePath(const std::string &value)
{
   auto path=wide(value); std::replace(path.begin(),path.end(),L'/',L'\\'); return path;
}

// Include Windows' concrete reason when locking, copying or boot registration fails.
static void failed(const std::string &message)
{
   DWORD code=GetLastError(); wchar_t detail[1024]={};
   FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,detail,1024,nullptr);
   throw std::runtime_error(message+": "+utf8(detail)+" (Windows error "+std::to_string(code)+")");
}

// Reject truncated or multi-disk extents instead of silently choosing the first physical disk.
static std::vector<DISK_EXTENT> extents(const std::wstring &name)
{
   std::wstring path=name; if (!path.empty() && path.back()==L'\\') path.pop_back();
   HANDLE volume=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
   if (volume==INVALID_HANDLE_VALUE) return {};
   std::vector<unsigned char> buffer(65536); DWORD returned=0;
   bool read=DeviceIoControl(volume,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,buffer.data(),buffer.size(),&returned,nullptr);
   CloseHandle(volume);
   auto data=reinterpret_cast<VOLUME_DISK_EXTENTS *>(buffer.data()); size_t offset=offsetof(VOLUME_DISK_EXTENTS,Extents);
   if (!read || returned<offset || data->NumberOfDiskExtents>(returned-offset)/sizeof(DISK_EXTENT)) return {};
   return {data->Extents,data->Extents+data->NumberOfDiskExtents};
}

// Protect every physical parent of the running OS, installer and source file.
static std::set<DWORD> fileDisks(const std::wstring &filename)
{
   wchar_t root[32768],volume[64]; std::set<DWORD> result;
   if (GetVolumePathNameW(filename.c_str(),root,32768) && GetVolumeNameForVolumeMountPointW(root,volume,64))
      for (const auto &extent:extents(volume)) result.insert(extent.DiskNumber);
   return result;
}

// Use stable printable GUIDs for partition and filesystem identity comparisons.
static std::string guidText(const GUID &id)
{
   char text[40];
   snprintf(text,sizeof(text),"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",static_cast<unsigned long>(id.Data1),id.Data2,id.Data3,id.Data4[0],id.Data4[1],id.Data4[2],id.Data4[3],id.Data4[4],id.Data4[5],id.Data4[6],id.Data4[7]);
   return text;
}

// Filesystem UUIDs use byte-order text, unlike Windows' mixed-endian GUID structure.
static std::string filesystemUuid(const unsigned char *bytes)
{
   const char *hex="0123456789abcdef"; std::string text;
   for (unsigned i=0; i<16; i++) { if (i==4 || i==6 || i==8 || i==10) text+='-'; text+=hex[bytes[i]>>4]; text+=hex[bytes[i]&15]; }
   return text;
}

// Read the raw filesystem header to detect encryption/pools and obtain GRUB's real filesystem UUID.
static void filesystemHeader(HANDLE disk,Partition &part,uint32_t sector)
{
   std::vector<unsigned char> data(sector); LARGE_INTEGER at; at.QuadPart=part.start; DWORD done=0;
   if (!SetFilePointerEx(disk,at,nullptr,FILE_BEGIN) || !ReadFile(disk,data.data(),data.size(),&done,nullptr) || done!=data.size()) return;
   const auto p=data.data();
   std::string fs=part.filesystem; for (char &c:fs) if (c>='A' && c<='Z') c+=32;
   if (!memcmp(p+3,"-FVE-FS-",8)) { part.filesystem="BitLocker"; return; }
   char uuid[24];
   if (!memcmp(p+3,"NTFS    ",8))
   {
      uint32_t high,low; memcpy(&low,p+72,4); memcpy(&high,p+76,4);
      snprintf(uuid,sizeof(uuid),"%08lX%08lX",static_cast<unsigned long>(high),static_cast<unsigned long>(low)); part.uuid=uuid;
   }
   else if (!memcmp(p+82,"FAT32   ",8))
   {
      uint32_t serial; memcpy(&serial,p+67,4);
      snprintf(uuid,sizeof(uuid),"%04lX-%04lX",static_cast<unsigned long>(serial>>16),static_cast<unsigned long>(serial&65535)); part.uuid=uuid;
   }
   else if (!memcmp(p+54,"FAT16   ",8) || !memcmp(p+3,"EXFAT   ",8))
   {
      uint32_t serial; memcpy(&serial,p+(!memcmp(p+3,"EXFAT   ",8)?100:39),4);
      snprintf(uuid,sizeof(uuid),"%04lX-%04lX",static_cast<unsigned long>(serial>>16),static_cast<unsigned long>(serial&65535)); part.uuid=uuid;
   }
   else if (fs=="xfs" && !memcmp(p,"XFSB",4)) part.uuid=filesystemUuid(p+32);
   if (fs=="ext2" || fs=="ext3" || fs=="ext4")
   {
      at.QuadPart=part.start+1024/sector*sector; size_t offset=1024%sector;
      if (SetFilePointerEx(disk,at,nullptr,FILE_BEGIN) && ReadFile(disk,data.data(),data.size(),&done,nullptr) && done==data.size())
         if (data[offset+56]==0x53 && data[offset+57]==0xef) part.uuid=filesystemUuid(data.data()+offset+104);
   }
   if (part.bytes>=65536+sector && (fs.empty() || fs=="btrfs"))
   {
      at.QuadPart=part.start+65536;
      if (SetFilePointerEx(disk,at,nullptr,FILE_BEGIN) && ReadFile(disk,data.data(),data.size(),&done,nullptr) && done==data.size() && !memcmp(data.data()+0x40,"_BHRfS_M",8))
      {
         uint64_t count=0; for (int n=7; n>=0; n--) count=count*256+data[0x88+n];
         part.filesystem=count>1?"btrfs_member":"btrfs";
         part.uuid=filesystemUuid(data.data()+0x20);
      }
   }
}

// Offer each basic volume and unpartitioned disk, including internal storage and volumes without letters.
std::vector<Device> scanDevices(const std::string &source)
{
   static const GUID diskClass={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
   std::vector<Device> result; wchar_t windows[32768],program[32768];
   GetWindowsDirectoryW(windows,32768); GetModuleFileNameW(nullptr,program,32768);
   auto system=fileDisks(windows),origin=fileDisks(program);
   if (!source.empty()) { auto disks=fileDisks(wide(source)); origin.insert(disks.begin(),disks.end()); }
   struct Volume { Partition part; std::vector<DISK_EXTENT> extents; };
   std::vector<Volume> volumes; wchar_t volumeName[64];
   HANDLE search=FindFirstVolumeW(volumeName,64);
   if (search!=INVALID_HANDLE_VALUE)
   {
      do
      {
         Volume volume; volume.extents=extents(volumeName); volume.part.mount=utf8(volumeName);
         wchar_t filesystem[64]={},label[256]={},paths[32768]={}; DWORD needed=0,flags=0;
         ULARGE_INTEGER available={},total={},free={};
         if (GetVolumeInformationW(volumeName,label,256,nullptr,nullptr,&flags,filesystem,64)) { volume.part.filesystem=utf8(filesystem); volume.part.volumeLabel=utf8(label); volume.part.readonly=flags&FILE_READ_ONLY_VOLUME; }
         if (GetDiskFreeSpaceExW(volumeName,&available,&total,&free)) { volume.part.free=available.QuadPart; volume.part.bytes=total.QuadPart; }
         else volume.part.free=UINT64_MAX;
         if (GetVolumePathNamesForVolumeNameW(volumeName,paths,32768,&needed))
         {
            for (wchar_t *path=paths; *path; path+=wcslen(path)+1)
               if (wcslen(path)==3 && path[1]==L':') { if (!volume.part.label.empty()) volume.part.label+=", "; volume.part.label+=utf8(std::wstring(path,2)); }
         }
         volumes.push_back(volume);
      } while (FindNextVolumeW(search,volumeName,64));
      FindVolumeClose(search);
   }
   HDEVINFO set=SetupDiGetClassDevsW(&diskClass,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
   if (set==INVALID_HANDLE_VALUE) return result;
   SP_DEVICE_INTERFACE_DATA deviceInterface={}; deviceInterface.cbSize=sizeof(deviceInterface);
   for (DWORD index=0; SetupDiEnumDeviceInterfaces(set,nullptr,&diskClass,index,&deviceInterface); index++)
   {
      DWORD needed=0,returned=0; SetupDiGetDeviceInterfaceDetailW(set,&deviceInterface,nullptr,0,&needed,nullptr);
      if (needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<unsigned char> buffer(needed); auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data()); detail->cbSize=sizeof(*detail);
      SP_DEVINFO_DATA information={}; information.cbSize=sizeof(information);
      if (!SetupDiGetDeviceInterfaceDetailW(set,&deviceInterface,detail,needed,nullptr,&information)) continue;
      HANDLE disk=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
      if (disk==INVALID_HANDLE_VALUE) disk=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
      if (disk==INVALID_HANDLE_VALUE) continue;
      STORAGE_PROPERTY_QUERY query={}; query.PropertyId=StorageDeviceProperty; query.QueryType=PropertyStandardQuery;
      unsigned char descriptor[8192]={},geometry[1024]={}; STORAGE_DEVICE_NUMBER number={};
      bool described=DeviceIoControl(disk,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),descriptor,sizeof(descriptor),&returned,nullptr) && returned>=sizeof(STORAGE_DEVICE_DESCRIPTOR);
      auto storage=reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(descriptor);
      bool numbered=DeviceIoControl(disk,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&returned,nullptr);
      bool sized=DeviceIoControl(disk,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,nullptr,0,geometry,sizeof(geometry),&returned,nullptr) && returned>=offsetof(DISK_GEOMETRY_EX,Data);
      if (!described || !numbered || !sized || storage->DeviceType!=0) { CloseHandle(disk); continue; }
      Device whole; auto size=reinterpret_cast<DISK_GEOMETRY_EX *>(geometry);
      whole.path="\\\\.\\PhysicalDrive"+std::to_string(number.DeviceNumber); whole.disk=utf8(detail->DevicePath);
      whole.bytes=whole.diskBytes=size->DiskSize.QuadPart; whole.sector=size->Geometry.BytesPerSector; whole.usb=storage->BusType==BusTypeUsb;
      whole.removable=storage->RemovableMedia;
      // XP headers omit the later STORAGE_BUS_TYPE values Virtual (14) and FileBackedVirtual (15).
      whole.virtualDisk=storage->BusType==14 || storage->BusType==15;
      whole.system=system.count(number.DeviceNumber);
      wchar_t instance[4096]={},name[1024]={};
      SetupDiGetDeviceInstanceIdW(set,&information,instance,4096,nullptr);
      SetupDiGetDeviceRegistryPropertyW(set,&information,SPDRP_FRIENDLYNAME,nullptr,reinterpret_cast<BYTE *>(name),sizeof(name),nullptr);
      whole.label="#"+std::to_string(number.DeviceNumber); whole.name=name[0]?utf8(name):whole.label;
      whole.diskId=utf8(instance[0]?instance:detail->DevicePath)+":"+std::to_string(whole.bytes);
      DWORD sequence=0;
      if (storage->RemovableMedia && DeviceIoControl(disk,IOCTL_STORAGE_CHECK_VERIFY2,nullptr,0,&sequence,sizeof(sequence),&returned,nullptr) && returned==sizeof(sequence)) whole.sequence=uint64_t(sequence)+1;
      whole.diskId+=":"+std::to_string(whole.sequence);
      if (system.empty()) whole.eraseBlocked="The running Windows system disk could not be identified. Erasing disks is disabled.";
      if (system.count(number.DeviceNumber)) whole.eraseBlocked="This disk contains the running Windows system. Start Slax from another disk before erasing it.";
      if (!whole.system && origin.count(number.DeviceNumber)) whole.eraseBlocked="This disk contains the ISO image or running installer. Move them to another disk before erasing it.";
      if (!DeviceIoControl(disk,IOCTL_DISK_IS_WRITABLE,nullptr,0,nullptr,0,&returned,nullptr) && GetLastError()==ERROR_WRITE_PROTECT) whole.blocked="This disk is read-only.";
      std::vector<unsigned char> layoutBytes(1024*1024);
      bool layoutRead=DeviceIoControl(disk,IOCTL_DISK_GET_DRIVE_LAYOUT_EX,nullptr,0,layoutBytes.data(),layoutBytes.size(),&returned,nullptr);
      auto layout=reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX *>(layoutBytes.data());
      if (layoutRead && returned>=offsetof(DRIVE_LAYOUT_INFORMATION_EX,PartitionEntry) && layout->PartitionCount<=(returned-offsetof(DRIVE_LAYOUT_INFORMATION_EX,PartitionEntry))/sizeof(PARTITION_INFORMATION_EX))
      {
         whole.layout=std::to_string(layout->PartitionStyle)+":"+(layout->PartitionStyle==PARTITION_STYLE_GPT?guidText(layout->Gpt.DiskId):std::to_string(layout->Mbr.Signature));
         for (DWORD p=0; p<layout->PartitionCount; p++)
         {
            const auto &entry=layout->PartitionEntry[p];
            if (!entry.PartitionNumber || !entry.PartitionLength.QuadPart) continue;
            if (entry.PartitionStyle==PARTITION_STYLE_MBR && (entry.Mbr.PartitionType==5 || entry.Mbr.PartitionType==15 || entry.Mbr.PartitionType==0x85)) continue;
            Partition part; part.start=entry.StartingOffset.QuadPart; part.bytes=entry.PartitionLength.QuadPart; part.number=entry.PartitionNumber;
            part.path=whole.path+"#"+std::to_string(part.number); part.label="P"+std::to_string(part.number);
            part.free=UINT64_MAX; part.type=entry.PartitionStyle==PARTITION_STYLE_GPT?guidText(entry.Gpt.PartitionType):std::to_string(entry.Mbr.PartitionType);
            for (const auto &volume:volumes)
            {
               if (volume.extents.size()!=1 || volume.extents[0].DiskNumber!=number.DeviceNumber || uint64_t(volume.extents[0].StartingOffset.QuadPart)!=part.start) continue;
               part.mount=volume.part.mount; part.filesystem=volume.part.filesystem; part.free=volume.part.free;
               part.volumeLabel=volume.part.volumeLabel; part.readonly=volume.part.readonly;
               if (!volume.part.label.empty()) part.label=volume.part.label;
            }
            filesystemHeader(disk,part,whole.sector);
            whole.partitions.push_back(part);
            whole.layout+=";"+std::to_string(part.number)+":"+std::to_string(part.start)+":"+std::to_string(part.bytes)+":"+part.type+":"+part.uuid;
         }
      }
      else if (GetLastError()!=ERROR_NOT_READY) whole.blocked="The complete partition layout could not be read. Run the installer as administrator.";
      whole.id=whole.diskId+":"+whole.layout; whole.detail="Disk · "+std::to_string(whole.partitions.size())+" partitions";
      if (whole.partitions.empty())
      {
         for (const auto &volume:volumes) if (volume.extents.size()==1 && volume.extents[0].DiskNumber==number.DeviceNumber && !volume.extents[0].StartingOffset.QuadPart)
         { whole.mount=volume.part.mount; whole.filesystem=volume.part.filesystem; whole.free=volume.part.free; if (!volume.part.label.empty()) whole.label=volume.part.label; }
         filesystemHeader(disk,whole,whole.sector);
      }
      if (whole.bytes) result.push_back(whole);
      for (const auto &part:whole.partitions)
      {
         if (part.bytes<1000000000 && part.label.find(':')==std::string::npos) continue;
         Device target=whole; static_cast<Partition &>(target)=part;
         target.id=whole.id+":"+std::to_string(part.number); target.name=whole.name;
         if (!part.volumeLabel.empty()) target.name+=" · "+part.volumeLabel;
         target.detail=(part.filesystem.empty()?"Unformatted":part.filesystem)+(part.free==UINT64_MAX?"":" · "+sizeText(part.free)+" free");
         filesystemHeader(disk,target,whole.sector);
         result.push_back(target);
      }
      CloseHandle(disk);
   }
   SetupDiDestroyDeviceInfoList(set);
   for (const auto &volume:volumes) if (volume.extents.size()>1)
   {
      Device target; static_cast<Partition &>(target)=volume.part;
      target.path=target.mount; target.id=target.path; target.name=target.volumeLabel.empty()?"Dynamic volume":target.volumeLabel;
      target.blocked="Spanned, striped or dynamic volumes cannot be modified safely. Select a basic partition.";
      result.push_back(target);
   }
   return result;
}

// Open the source separately for each validation or copy operation.
FILE *openImage(const std::string &path)
{
   if (path.find("\\\\.")==0) return nullptr;
   HANDLE handle=CreateFileW(wide(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
   if (handle==INVALID_HANDLE_VALUE) return nullptr;
   int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
   if (fd<0) { CloseHandle(handle); return nullptr; }
   FILE *file=_fdopen(fd,"rb"); if (!file) _close(fd); return file;
}

// Windows' invariant Unicode mapping catches aliases beyond ASCII without an external Unicode library.
std::string filenameKey(const std::string &name)
{
   auto input=wide(name); int count=LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,input.data(),input.size(),nullptr,0);
   if (!count) failed("Cannot compare an ISO filename safely");
   std::wstring output(count,L'\0'); LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,input.data(),input.size(),output.data(),output.size()); return utf8(output);
}

struct Disk::Impl
{
   HANDLE handle=INVALID_HANDLE_VALUE;
   HANDLE mutex=nullptr;
   std::vector<HANDLE> locks;
   bool claimed=false;
   std::vector<DiskRange> ranges;
   ~Impl() { for (auto lock:locks) CloseHandle(lock); if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); if (mutex) CloseHandle(mutex); }
};

// Retain this physical device from preflight through confirmation; never resolve a disk number again for writes.
Disk::Disk(const Device &expected):impl(new Impl),device(expected)
{
   auto devices=scanDevices("");
   auto found=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.id==expected.id; });
   if (found==devices.end() || found->layout!=expected.layout) throw std::runtime_error("The selected disk or partition layout changed. Select it again.");
   impl->handle=CreateFileW(wide(expected.disk).c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,nullptr);
   if (impl->handle==INVALID_HANDLE_VALUE) failed("Cannot open the physical disk; run the installer as administrator");
   unsigned char geometry[1024]={}; DWORD done=0;
   if (!DeviceIoControl(impl->handle,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,nullptr,0,geometry,sizeof(geometry),&done,nullptr)) failed("Cannot verify the approved physical disk geometry");
   auto size=reinterpret_cast<DISK_GEOMETRY_EX *>(geometry);
   if (done<offsetof(DISK_GEOMETRY_EX,Data) || uint64_t(size->DiskSize.QuadPart)!=expected.diskBytes || size->Geometry.BytesPerSector!=expected.sector) throw std::runtime_error("The approved disk geometry changed. Select the target again.");
   DWORD sequence=0;
   if (expected.sequence && (!DeviceIoControl(impl->handle,IOCTL_STORAGE_CHECK_VERIFY2,nullptr,0,&sequence,sizeof(sequence),&done,nullptr) || done!=sizeof(sequence) || uint64_t(sequence)+1!=expected.sequence)) throw std::runtime_error("The removable medium changed. Select the target again.");
}

// Closing retained handles releases every successful volume lock even after a failed operation.
Disk::~Disk()=default;

// A physical-disk mutex covers preserve installs too, where normal filesystem I/O cannot lock the system volume.
void Disk::reserve()
{
   if (impl->mutex) return;
   Hash hash; hash.add(device.diskId.data(),device.diskId.size()); auto digest=hash.finish();
   std::string name="Global\\SlaxInstallerDisk-"; const char *hex="0123456789abcdef";
   for (auto byte:digest) { name+=hex[byte>>4]; name+=hex[byte&15]; }
   HANDLE mutex=CreateMutexW(nullptr,FALSE,wide(name).c_str());
   if (!mutex) failed("Cannot reserve this physical disk for installation");
   DWORD result=WaitForSingleObject(mutex,0);
   if (result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED) { CloseHandle(mutex); throw std::runtime_error("Another Slax installer is already using this physical disk. Wait for it to finish."); }
   impl->mutex=mutex;
}

// Lock all sibling volumes before dismounting any of them or changing a single sector.
void Disk::claim(bool erase)
{
   reserve();
   if (erase && !device.eraseBlocked.empty()) throw std::runtime_error(device.eraseBlocked);
   DWORD done=0; wchar_t name[64]; HANDLE search=FindFirstVolumeW(name,64);
   STORAGE_DEVICE_NUMBER number={};
   if (!DeviceIoControl(impl->handle,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&done,nullptr)) failed("The approved disk stopped responding");
   DWORD diskNumber=number.DeviceNumber;
   if (search!=INVALID_HANDLE_VALUE)
   {
      do
      {
         auto parts=extents(name); bool affected=false;
         for (const auto &part:parts) if (part.DiskNumber==diskNumber) affected=true;
         if (!affected) continue;
         if (parts.size()!=1) { FindVolumeClose(search); throw std::runtime_error("A sibling volume spans multiple disks. No changes were made."); }
         std::wstring path=name; path.pop_back();
         HANDLE volume=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
         if (volume==INVALID_HANDLE_VALUE) { FindVolumeClose(search); failed("Cannot open a sibling volume for exclusive access"); }
         if (!DeviceIoControl(volume,FSCTL_LOCK_VOLUME,nullptr,0,nullptr,0,&done,nullptr)) { CloseHandle(volume); FindVolumeClose(search); failed("A partition is in use, contains a page file, or cannot be locked. Close applications using every partition on this disk"); }
         impl->locks.push_back(volume);
      } while (FindNextVolumeW(search,name,64));
      FindVolumeClose(search);
   }
   for (auto volume:impl->locks) if (!DeviceIoControl(volume,FSCTL_DISMOUNT_VOLUME,nullptr,0,nullptr,0,&done,nullptr)) failed("Cannot dismount an exclusively locked partition");
   unsigned char sector[4096]; read(0,sector,device.sector);
   impl->claimed=true;
}

// Windows permits raw boot writes outside mounted volume extents; never grant access to their contents.
void Disk::claimBoot(const std::vector<DiskRange> &ranges)
{
   reserve(); verify(); impl->ranges=ranges;
}

// Release all volume locks before Windows is asked to discover the finished table.
void Disk::release()
{
   impl->claimed=false;
   impl->ranges.clear();
   for (auto handle:impl->locks) CloseHandle(handle);
   impl->locks.clear();
   if (impl->mutex) { ReleaseMutex(impl->mutex); CloseHandle(impl->mutex); impl->mutex=nullptr; }
}

// Require exact reads from the originally approved native handle.
void Disk::read(uint64_t offset,void *data,size_t bytes)
{
   verify();
   if (offset>device.diskBytes || bytes>device.diskBytes-offset || bytes>MAXDWORD) throw std::runtime_error("An out-of-range disk read was refused.");
   LARGE_INTEGER at; at.QuadPart=offset; DWORD done=0;
   if (!SetFilePointerEx(impl->handle,at,nullptr,FILE_BEGIN) || !ReadFile(impl->handle,data,bytes,&done,nullptr) || done!=bytes) failed("Cannot read the selected disk completely");
}

// No raw write is possible before the complete exclusive-access phase succeeds.
void Disk::write(uint64_t offset,const void *data,size_t bytes)
{
   verify();
   bool approved=impl->claimed;
   for (const auto &range:impl->ranges) if (offset>=range.start && offset-range.start<=range.bytes && bytes<=range.bytes-(offset-range.start)) approved=true;
   if (!approved || offset>device.diskBytes || bytes>device.diskBytes-offset || bytes>MAXDWORD || offset%device.sector || bytes%device.sector) throw std::runtime_error("An unapproved, unaligned or out-of-range disk write was refused.");
   LARGE_INTEGER at; at.QuadPart=offset; DWORD done=0;
   if (!SetFilePointerEx(impl->handle,at,nullptr,FILE_BEGIN) || !WriteFile(impl->handle,data,bytes,&done,nullptr) || done!=bytes) failed("Cannot write the selected disk completely");
}

// Retain the driver's media-generation token when a removable device supplies one.
void Disk::verify()
{
   DWORD sequence=0,done=0;
   if (device.sequence && (!DeviceIoControl(impl->handle,IOCTL_STORAGE_CHECK_VERIFY2,nullptr,0,&sequence,sizeof(sequence),&done,nullptr) || done!=sizeof(sequence) || uint64_t(sequence)+1!=device.sequence)) throw std::runtime_error("The approved disk or removable medium changed; installation was stopped.");
}

// Do not report success until Windows has flushed the physical disk.
void Disk::flush() { if (!FlushFileBuffers(impl->handle)) failed("Cannot flush the physical disk"); }

struct FileTree::Impl
{
   std::wstring root;
   HANDLE handle=INVALID_HANDLE_VALUE,volume=INVALID_HANDLE_VALUE;
   ~Impl() { if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); if (volume!=INVALID_HANDLE_VALUE) CloseHandle(volume); }
};

// Lock parent directory identities against rename while rejecting junctions and reparse points.
struct Parents
{
   std::vector<HANDLE> handles;
   Parents(const std::wstring &root,const std::string &path)
   {
      try
      {
         for (size_t at=0; (at=path.find('/',at))!=std::string::npos; at++)
         {
            auto full=root+nativePath(path.substr(0,at));
            HANDLE h=CreateFileW(full.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if (h==INVALID_HANDLE_VALUE) failed("Cannot safely open a destination directory");
            BY_HANDLE_FILE_INFORMATION info;
            if (!GetFileInformationByHandle(h,&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(h); throw std::runtime_error("A destination directory is a link or mount-point substitution."); }
            handles.push_back(h);
         }
      }
      catch (...) { for (auto handle:handles) CloseHandle(handle); throw; }
   }
   ~Parents() { for (auto handle:handles) CloseHandle(handle); }
};

// Use a volume GUID root even when the EFI partition has no drive letter.
FileTree::FileTree(const Partition &partition,bool writable):impl(new Impl)
{
   if (partition.mount.empty()) throw std::runtime_error("Windows cannot mount this partition. Select a supported mounted volume or erase the whole disk.");
   impl->root=wide(partition.mount); if (impl->root.back()!=L'\\') impl->root+=L'\\';
   impl->handle=CreateFileW(impl->root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
   if (impl->handle==INVALID_HANDLE_VALUE) failed("Cannot open the selected filesystem");
   BY_HANDLE_FILE_INFORMATION info;
   if (!GetFileInformationByHandle(impl->handle,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("The destination root is a reparse point.");
   if (info.dwFileAttributes&FILE_ATTRIBUTE_ENCRYPTED) throw std::runtime_error("Windows file encryption is enabled for this target. Choose an unencrypted filesystem so Slax can read its files.");
   if (writable)
   {
      std::wstring path=impl->root; path.pop_back();
      impl->volume=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
      if (impl->volume==INVALID_HANDLE_VALUE) failed("Cannot open the destination volume for flushing");
   }
}

// Retained root handles remain valid until all filesystem operations finish.
FileTree::~FileTree()=default;
// Give the trusted verifier the pinned volume-GUID root rather than a changeable drive letter.
std::string FileTree::rootPath() { return utf8(impl->root); }

// An existing object of any kind prevents accidental replacement of an installation.
bool FileTree::exists(const std::string &path) { return GetFileAttributesW((impl->root+nativePath(path)).c_str())!=INVALID_FILE_ATTRIBUTES; }

// Inspect the retained object without accepting a junction or another special destination.
bool FileTree::isDirectory(const std::string &path)
{
   auto full=impl->root+nativePath(path);
   if (GetFileAttributesW(full.c_str())==INVALID_FILE_ATTRIBUTES)
   {
      if (GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND) return false;
      failed("Cannot inspect destination "+path);
   }
   Parents parents(impl->root,path);
   HANDLE file=CreateFileW(full.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
   if (file==INVALID_HANDLE_VALUE) failed("Cannot inspect destination "+path);
   BY_HANDLE_FILE_INFORMATION info={}; bool read=GetFileInformationByHandle(file,&info); DWORD error=GetLastError(); bool regular=GetFileType(file)==FILE_TYPE_DISK; CloseHandle(file);
   if (!read) { SetLastError(error); failed("Cannot identify destination "+path); }
   if (!regular || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("A destination is a link or special file.");
   return info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY;
}

// Compare native identities so aliases cannot make an input overwrite itself.
bool FileTree::same(const std::string &path,const std::string &external)
{
   auto full=impl->root+nativePath(path);
   if (GetFileAttributesW(full.c_str())==INVALID_FILE_ATTRIBUTES)
   {
      if (GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND) return false;
      failed("Cannot inspect destination "+path);
   }
   Parents parents(impl->root,path);
   HANDLE target=CreateFileW(full.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
   if (target==INVALID_HANDLE_VALUE) failed("Cannot identify destination "+path);
   BY_HANDLE_FILE_INFORMATION destination={},source={};
   bool read=GetFileInformationByHandle(target,&destination); DWORD error=GetLastError();
   if (!read || (destination.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
   {
      CloseHandle(target);
      if (!read) { SetLastError(error); failed("Cannot identify destination "+path); }
      throw std::runtime_error("A destination is a link or mount point.");
   }
   HANDLE input=CreateFileW(wide(external).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
   if (input==INVALID_HANDLE_VALUE) { error=GetLastError(); CloseHandle(target); SetLastError(error); failed("Cannot identify the Slax source"); }
   read=GetFileInformationByHandle(input,&source); error=GetLastError(); CloseHandle(input); CloseHandle(target);
   if (!read) { SetLastError(error); failed("Cannot identify the Slax source"); }
   return destination.dwVolumeSerialNumber==source.dwVolumeSerialNumber && destination.nFileIndexHigh==source.nFileIndexHigh && destination.nFileIndexLow==source.nFileIndexLow;
}

// Create exclusively; never reuse a directory whose ownership is unknown.
void FileTree::mkdir(const std::string &path)
{
   Parents parents(impl->root,path);
   if (!CreateDirectoryW((impl->root+nativePath(path)).c_str(),nullptr)) failed("Cannot create "+path);
}

// CREATE_NEW and reparse-point rejection prevent payload extraction from overwriting unrelated files.
FILE *FileTree::open(const std::string &path,bool write,uint32_t)
{
   Parents parents(impl->root,path);
   HANDLE h=CreateFileW((impl->root+nativePath(path)).c_str(),GENERIC_READ|(write?GENERIC_WRITE:0),FILE_SHARE_READ,nullptr,write?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr);
   if (h==INVALID_HANDLE_VALUE) failed("Cannot open destination file "+path);
   BY_HANDLE_FILE_INFORMATION info;
   if (!GetFileInformationByHandle(h,&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) { CloseHandle(h); throw std::runtime_error("A destination file is a link or directory."); }
   if (write && (info.dwFileAttributes&FILE_ATTRIBUTE_ENCRYPTED)) { CloseHandle(h); throw std::runtime_error("Windows file encryption is enabled for this destination. Choose an unencrypted target so the boot loader and Slax can read their files."); }
   if (write && (info.dwFileAttributes&FILE_ATTRIBUTE_COMPRESSED))
   {
      USHORT format=COMPRESSION_FORMAT_NONE; DWORD done=0;
      if (!DeviceIoControl(h,FSCTL_SET_COMPRESSION,&format,sizeof(format),nullptr,0,&done,nullptr)) { CloseHandle(h); failed("Cannot store boot files without NTFS compression"); }
   }
   int fd=_open_osfhandle(reinterpret_cast<intptr_t>(h),_O_BINARY|(write?_O_RDWR:_O_RDONLY));
   if (fd<0) { CloseHandle(h); throw std::runtime_error("Cannot open the destination file stream."); }
   FILE *file=_fdopen(fd,write?"w+b":"rb");
   if (!file) { _close(fd); throw std::runtime_error("Cannot create the destination file stream."); }
   return file;
}

// Publish the complete installation without replacing any destination that appeared meanwhile.
void FileTree::move(const std::string &from,const std::string &to)
{
   Parents source(impl->root,from),destination(impl->root,to);
   if (!MoveFileExW((impl->root+nativePath(from)).c_str(),(impl->root+nativePath(to)).c_str(),MOVEFILE_WRITE_THROUGH)) failed("Cannot publish the verified installation");
}

// Atomically replace an approved boot configuration while retaining its Windows attributes and ACL.
void FileTree::replace(const std::string &from,const std::string &to)
{
   Parents source(impl->root,from),destination(impl->root,to);
   auto path=impl->root+nativePath(to),backup=path+L".slax-backup-"+wide(randomId()); DWORD attributes=GetFileAttributesW(path.c_str());
   if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) throw std::runtime_error("The boot configuration is not a regular file.");
   if (!SetFileAttributesW(path.c_str(),attributes&~FILE_ATTRIBUTE_READONLY)) failed("Cannot prepare the boot configuration");
   bool replaced=ReplaceFileW(path.c_str(),(impl->root+nativePath(from)).c_str(),backup.c_str(),0,nullptr,nullptr);
   DWORD error=GetLastError();
   try
   {
      if (!replaced) { SetLastError(error); failed("Cannot atomically update the boot configuration"); }
      if (!SetFileAttributesW(path.c_str(),attributes)) failed("Cannot restore the boot configuration attributes");
      sync();
   }
   catch (...)
   {
      // ReplaceFile can remove the original name even on failure; keep its bytes recoverable.
      if (GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES)
      {
         SetFileAttributesW(path.c_str(),attributes&~FILE_ATTRIBUTE_READONLY);
         if (!MoveFileExW(backup.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("The boot configuration could not be restored. Its original bytes remain at "+utf8(backup)+".");
      }
      SetFileAttributesW(path.c_str(),attributes);
      sync();
      throw;
   }
   DeleteFileW(backup.c_str());
}

// Remove only the exact objects recorded as created by this run.
void FileTree::remove(const std::string &path,bool directory)
{
   try
   {
      Parents parents(impl->root,path);
      auto full=impl->root+nativePath(path);
      if (directory) RemoveDirectoryW(full.c_str());
      else
      {
         DWORD attributes=GetFileAttributesW(full.c_str());
         if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return;
         if (attributes&FILE_ATTRIBUTE_READONLY) SetFileAttributesW(full.c_str(),attributes&~FILE_ATTRIBUTE_READONLY);
         if (!DeleteFileW(full.c_str()) && (attributes&FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(full.c_str(),attributes);
      }
   }
   // Cleanup must retain the original error and never follow a substituted parent.
   catch (...) {}
}

// Query the actual destination volume instead of trusting a stale scan's free-space value.
uint64_t FileTree::available()
{
   ULARGE_INTEGER available;
   if (!GetDiskFreeSpaceExW(impl->root.c_str(),&available,nullptr,nullptr)) failed("Cannot determine free destination space");
   return available.QuadPart;
}

// Flush filesystem metadata before activating the new firmware boot entry.
void FileTree::sync() { if (impl->volume!=INVALID_HANDLE_VALUE && !FlushFileBuffers(impl->volume)) failed("Cannot flush the destination filesystem"); }

// Force file data through the operating-system write cache before verification.
void flushFile(FILE *file)
{
   if (fflush(file) || !FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))))) failed("Cannot flush a copied file");
}

// CryptoAPI is present on XP and avoids a dependency on modern CNG DLLs.
std::string randomId()
{
   HCRYPTPROV provider=0; unsigned char bytes[16];
   if (!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)) failed("Cannot initialize system randomness");
   bool generated=CryptGenRandom(provider,sizeof(bytes),bytes); CryptReleaseContext(provider,0);
   if (!generated) failed("Cannot generate a unique installation identity");
   const char *hex="0123456789abcdef"; std::string result;
   for (auto byte:bytes) { result+=hex[byte>>4]; result+=hex[byte&15]; }
   return result;
}

// Query the actual NT version so compatibility settings cannot select XP on modern Windows.
bool windowsXp()
{
   OSVERSIONINFOEXW version={}; version.dwOSVersionInfoSize=sizeof(version);
   using Query=LONG (WINAPI *)(OSVERSIONINFOEXW *);
   auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
   if (!query || query(&version)!=0) return false;
   bool xpVersion=version.dwMajorVersion==5 && (version.dwMinorVersion==1 || version.dwMinorVersion==2);
   return xpVersion && version.wProductType==VER_NT_WORKSTATION;
}

// Older Windows exposes only the first partition on removable media; XP also cannot use normal GPT disks.
bool mbrRequired(const Device &device)
{
   OSVERSIONINFOEXW version={}; version.dwOSVersionInfoSize=sizeof(version);
   using Query=LONG (WINAPI *)(OSVERSIONINFOEXW *);
   auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
   if (!query || query(&version)!=0) throw std::runtime_error("Cannot determine this Windows version's disk capabilities.");
   bool firstPartitionOnly=device.removable && (version.dwMajorVersion<10 || (version.dwMajorVersion==10 && version.dwBuildNumber<15063));
   SYSTEM_INFO system; GetNativeSystemInfo(&system);
   bool noGpt=version.dwMajorVersion<6 && system.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_AMD64;
   return windowsXp() || noGpt || firstPartitionOnly;
}

// Detect elevation through the current token, including ordinary XP administrator accounts.
bool administrator()
{
   SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY; PSID group=nullptr; BOOL member=FALSE;
   if (AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&group)) { CheckTokenMembership(nullptr,group,&member); FreeSid(group); }
   return member;
}

// Use Windows' own BCD editor; Sysnative avoids WOW64 filesystem redirection.
static std::wstring bcdEditor()
{
   wchar_t folder[MAX_PATH]; if (!GetWindowsDirectoryW(folder,MAX_PATH)) failed("Cannot locate Windows");
   BOOL wow64=FALSE; using Query=BOOL (WINAPI *)(HANDLE,PBOOL);
   auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"IsWow64Process"));
   if (query) query(GetCurrentProcess(),&wow64);
   auto path=std::wstring(folder)+(wow64?L"\\Sysnative\\bcdedit.exe":L"\\System32\\bcdedit.exe");
   return GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES?L"":path;
}

// Run BCDEdit directly and retain its full localized output for comparison and recovery.
static std::string bcdCommand(const Partition &partition,const std::vector<std::wstring> &arguments)
{
   auto executable=bcdEditor(); if (executable.empty()) throw std::runtime_error("This Windows session cannot edit BCD natively.");
   FileTree root(partition,false); std::wstring command=L"\""+executable+L"\" /store \""+wide(root.rootPath())+L"Boot\\BCD\"";
   for (const auto &argument:arguments) command+=L" \""+argument+L"\"";
   wchar_t temporary[32768]; DWORD count=GetTempPathW(32768,temporary);
   if (!count || count>=32768) failed("Cannot locate the BCD log directory");
   auto logPath=std::wstring(temporary)+L"slax-bcd-"+wide(randomId())+L".log";
   SECURITY_ATTRIBUTES security={sizeof(security),nullptr,TRUE};
   HANDLE log=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
   if (log==INVALID_HANDLE_VALUE) failed("Cannot create the BCD log");
   STARTUPINFOW startup={}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
   startup.hStdOutput=startup.hStdError=log; startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE); PROCESS_INFORMATION child={};
   bool started=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child); CloseHandle(log);
   if (!started) { DeleteFileW(logPath.c_str()); failed("Cannot start Windows BCDEdit"); }
   WaitForSingleObject(child.hProcess,INFINITE); DWORD code=1; GetExitCodeProcess(child.hProcess,&code); CloseHandle(child.hThread); CloseHandle(child.hProcess);
   FILE *file=_wfopen(logPath.c_str(),L"rb"); std::string output; char buffer[4096]; size_t bytes;
   if (file) { while ((bytes=fread(buffer,1,sizeof(buffer),file))) output.append(buffer,bytes); fclose(file); }
   DeleteFileW(logPath.c_str());
   if (code) throw std::runtime_error("Windows BCDEdit could not update this boot store (error "+std::to_string(code)+"). The original Windows boot entry was kept.");
   return output;
}

// Snapshot BCD without parsing translated field names or requiring WMI/PowerShell.
std::string readBcd(const Partition &partition)
{
   if (bcdEditor().empty()) return "";
   return bcdCommand(partition,{L"/enum",L"all",L"/v"});
}

// Add only one legacy boot application; preserve all existing BCD entries and the default OS.
void addBcdEntry(const InstallPlan &plan)
{
   auto id=randomId(),entry="{"+id.substr(0,8)+"-"+id.substr(8,4)+"-"+id.substr(12,4)+"-"+id.substr(16,4)+"-"+id.substr(20)+"}";
   std::wstring guid=wide(entry); FileTree target(plan.device,true);
   FILE *record=target.open(plan.recovery+"/bcd-entry.txt",true);
   std::string recovery="Slax installer BCD recovery record\nAdded entry: "+entry+"\nBoot store: "+plan.bootPartition.label+"\\Boot\\BCD\nSector file: "+plan.bootFile+"\nOriginal entries and default OS were retained.\nTo remove only this entry, run from the original Windows installation:\nbcdedit /delete "+entry+"\nThe original menu dump is in original-menu.txt.\n";
   try { if (fwrite(recovery.data(),1,recovery.size(),record)!=recovery.size()) throw std::runtime_error("Cannot save the BCD recovery record."); flushFile(record); }
   catch (...) { fclose(record); throw; }
   fclose(record); target.sync();
   bcdCommand(plan.bootPartition,{L"/create",guid,L"/d",L"Slax Linux",L"/application",L"BOOTSECTOR"});
   try
   {
      bcdCommand(plan.bootPartition,{L"/set",guid,L"device",L"boot"});
      bcdCommand(plan.bootPartition,{L"/set",guid,L"path",L"\\"+wide(plan.bootFile)});
      bcdCommand(plan.bootPartition,{L"/displayorder",guid,L"/addlast"});
      OSVERSIONINFOEXW version={}; version.dwOSVersionInfoSize=sizeof(version);
      using Query=LONG (WINAPI *)(OSVERSIONINFOEXW *); auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
      if (!query || query(&version)!=0) throw std::runtime_error("Cannot determine the Windows boot-menu capabilities.");
      if (version.dwMajorVersion>6 || (version.dwMajorVersion==6 && version.dwMinorVersion>=2)) bcdCommand(plan.bootPartition,{L"/set",L"{default}",L"bootmenupolicy",L"Legacy"});
      bcdCommand(plan.bootPartition,{L"/timeout",L"10"});
   }
   catch (...)
   {
      auto failure=std::current_exception();
      try { bcdCommand(plan.bootPartition,{L"/delete",guid}); } catch (...) { throw std::runtime_error("BCD activation failed and its new Slax entry could not be removed. Original Windows entries remain; review the saved boot-menu record."); }
      std::rethrow_exception(failure);
   }
}

// Let Mount Manager discover the completed table only after all raw volume locks are released.
void refreshDisk(const Device &device)
{
   HANDLE h=CreateFileW(wide(device.disk).c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
   if (h!=INVALID_HANDLE_VALUE) { DWORD done; DeviceIoControl(h,IOCTL_DISK_UPDATE_PROPERTIES,nullptr,0,nullptr,0,&done,nullptr); CloseHandle(h); }
}

// Enable only the privilege needed for standard UEFI boot variables.
static void firmwarePrivilege()
{
   HANDLE token; TOKEN_PRIVILEGES privilege={}; privilege.PrivilegeCount=1; privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
   if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)) failed("Cannot open the administrator token");
   bool found=LookupPrivilegeValueW(nullptr,SE_SYSTEM_ENVIRONMENT_NAME,&privilege.Privileges[0].Luid);
   bool enabled=found && AdjustTokenPrivileges(token,FALSE,&privilege,0,nullptr,nullptr) && GetLastError()!=ERROR_NOT_ALL_ASSIGNED;
   CloseHandle(token);
   if (!enabled) throw std::runtime_error("Administrator permission to update UEFI boot entries is unavailable.");
}

// Load firmware APIs dynamically so a BIOS-only XP installation can still start the application.
std::vector<unsigned char> readFirmware(const std::string &name)
{
   using Read=DWORD (WINAPI *)(LPCWSTR,LPCWSTR,PVOID,DWORD);
   auto read=reinterpret_cast<Read>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetFirmwareEnvironmentVariableW"));
   if (!read) throw std::runtime_error("This Windows version does not provide UEFI runtime services.");
   firmwarePrivilege(); std::vector<unsigned char> data(262144);
   SetLastError(ERROR_SUCCESS); DWORD bytes=read(wide(name).c_str(),L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",data.data(),data.size());
   if (!bytes && GetLastError()!=ERROR_SUCCESS && GetLastError()!=ERROR_ENVVAR_NOT_FOUND) failed("Cannot read UEFI boot variables; Windows may be running in legacy BIOS mode");
   data.resize(bytes); return data;
}

// The original firmware API uses the normal nonvolatile/boot/runtime attributes for boot variables.
void writeFirmware(const std::string &name,const std::vector<unsigned char> &data)
{
   using Write=BOOL (WINAPI *)(LPCWSTR,LPCWSTR,PVOID,DWORD);
   auto write=reinterpret_cast<Write>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetFirmwareEnvironmentVariableW"));
   if (!write) throw std::runtime_error("This Windows version cannot update UEFI boot entries.");
   firmwarePrivilege();
   if (!write(wide(name).c_str(),L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",data.empty()?nullptr:const_cast<unsigned char *>(data.data()),data.size())) failed("The firmware refused the boot entry update");
}

// Only offer automatic internal multiboot when Windows actually exposes UEFI runtime services.
bool firmwareAvailable()
{
   SYSTEM_INFO system; GetNativeSystemInfo(&system);
   if (system.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_AMD64) return false;
   try { return !readFirmware("BootOrder").empty(); } catch (...) { return false; }
}

// Keep native tool diagnostics bounded while retaining the concrete reason for a rejected boot setup.
static std::string toolLog(const std::wstring &path)
{
   FILE *file=_wfopen(path.c_str(),L"rb"); if (!file) failed("Cannot read the BIOS tool log");
   char detail[1025]={}; size_t bytes=fread(detail,1,1024,file); fclose(file); return std::string(detail,bytes);
}

// Stage on the native system volume, protecting the directory identity and every supported ACL.
struct BiosWorkspace
{
   std::wstring path;
   HANDLE handle=INVALID_HANDLE_VALUE;
   SECURITY_ATTRIBUTES security={sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
   BiosWorkspace(const wchar_t *prefix)
   {
      wchar_t windows[32768],volume[32768]; DWORD flags=0,size=GetWindowsDirectoryW(windows,32768);
      if (!size || size>=32768 || !GetVolumePathNameW(windows,volume,32768) || !GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,&flags,nullptr,0)) failed("Cannot find the system BIOS workspace");
      std::wstring temporary=std::wstring(windows)+L"\\Temp";
      path=temporary+L"\\"+prefix+wide(randomId());
      PSECURITY_DESCRIPTOR descriptor=nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr)) failed("Cannot protect the BIOS workspace");
      security.lpSecurityDescriptor=descriptor;
      if (!CreateDirectoryW(temporary.c_str(),&security) && GetLastError()!=ERROR_ALREADY_EXISTS)
      {
         DWORD error=GetLastError(); LocalFree(descriptor); SetLastError(error); failed("Cannot create the system temporary directory");
      }
      bool created=CreateDirectoryW(path.c_str(),&security); DWORD error=GetLastError();
      if (!created) { LocalFree(descriptor); SetLastError(error); failed("Cannot create a protected BIOS workspace"); }
      handle=CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
      if (handle==INVALID_HANDLE_VALUE)
      {
         error=GetLastError(); RemoveDirectoryW(path.c_str()); LocalFree(descriptor); SetLastError(error); failed("Cannot retain the protected BIOS workspace");
      }
      // FAT32-hosted Windows XP has no persistent ACLs; retain its directory identity without requiring them.
      bool protectedOwner=true;
      if (flags&FILE_PERSISTENT_ACLS)
      {
         PSID owner=nullptr,expected=nullptr; BOOL defaulted=FALSE; PSECURITY_DESCRIPTOR actual=nullptr;
         GetSecurityDescriptorOwner(descriptor,&expected,&defaulted);
         DWORD query=GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&actual);
         protectedOwner=query==ERROR_SUCCESS && EqualSid(owner,expected);
         LocalFree(actual);
      }
      BY_HANDLE_FILE_INFORMATION info;
      bool directory=GetFileInformationByHandle(handle,&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
      if (!protectedOwner || !directory)
      {
         CloseHandle(handle); RemoveDirectoryW(path.c_str()); LocalFree(descriptor);
         throw std::runtime_error("The BIOS workspace identity is not protected.");
      }
   }
   // Release the directory identity only after all child processes and staged files are gone.
   ~BiosWorkspace()
   {
      CloseHandle(handle); RemoveDirectoryW((path+L"\\i386-pc").c_str()); RemoveDirectoryW(path.c_str()); LocalFree(security.lpSecurityDescriptor);
   }
};

// Stage only authenticated assets, deleting a partial file before reporting a failed write.
static void stageBiosAsset(const std::wstring &path,const GrubAsset &asset,SECURITY_ATTRIBUTES &security)
{
   HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr); DWORD done=0;
   if (file==INVALID_HANDLE_VALUE) failed("Cannot stage the verified BIOS tools");
   bool written=WriteFile(file,asset.data.data(),asset.data.size(),&done,nullptr) && done==asset.data.size();
   DWORD error=GetLastError(); CloseHandle(file);
   if (!written) { DeleteFileW(path.c_str()); SetLastError(error); failed("Cannot write the verified BIOS tools completely"); }
}

// Build the BIOS core from the selected source's authenticated GNU tools and matching modules.
BiosImage buildBios(const Partition &target,bool gpt,const GrubAssets &assets)
{
   BiosWorkspace workspace(L"slax-install-"); const std::wstring &folder=workspace.path;
   std::vector<std::wstring> created; BiosImage result;
   try
   {
      if (!CreateDirectoryW((folder+L"\\i386-pc").c_str(),&workspace.security)) failed("Cannot create the BIOS module directory");
      for (const auto &asset:assets.entries())
      {
         std::wstring path=folder+L"\\"+wide(asset.name); stageBiosAsset(path,asset,workspace.security); created.push_back(path);
         const std::string &name=asset.name;
         if (name.size()>4 && (name.substr(name.size()-4)==".mod" || name.substr(name.size()-4)==".lst")) result.modules.push_back(generated("slax/boot/grub/"+name,asset.data));
      }
      stageBiosAsset(folder+L"\\core",{},workspace.security); created.push_back(folder+L"\\core");
      auto security=workspace.security; security.bInheritHandle=TRUE;
      HANDLE log=CreateFileW((folder+L"\\log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
      if (log==INVALID_HANDLE_VALUE) failed("Cannot create the BIOS build log");
      created.push_back(folder+L"\\log");
      std::wstring executable=folder+L"\\"+wide(assets.tool("grub-mkimage").name);
      std::wstring prefix=L"(,"+std::wstring(gpt?L"gpt":L"msdos")+std::to_wstring(target.number)+L")/slax/boot/grub";
      // Relative ASCII arguments keep GNU tools independent of the temporary path's code page.
      std::wstring command=L"grub-mkimage.exe -O i386-pc -C xz -d i386-pc -p \""+prefix+L"\" -o core biosdisk "+(gpt?std::wstring(L"part_gpt"):std::wstring(L"part_msdos"))+L" "+wide(biosFilesystem(target));
      STARTUPINFOW startup={}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES; startup.hStdOutput=startup.hStdError=log; startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
      PROCESS_INFORMATION process={};
      bool started=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,folder.c_str(),&startup,&process); CloseHandle(log);
      if (!started) failed("Cannot start the verified BIOS image builder");
      WaitForSingleObject(process.hProcess,INFINITE); DWORD code=1; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
      if (code) throw std::runtime_error("The verified GNU GRUB image builder failed. No disk changes were made.\n"+toolLog(folder+L"\\log"));
      for (int i=0; i<2; i++)
      {
         FILE *file=_wfopen((folder+(i?L"\\core":L"\\i386-pc\\boot.img")).c_str(),L"rb");
         if (!file) throw std::runtime_error("Cannot read the completed BIOS loader.");
         std::string data(1024*1024,'\0'); size_t bytes=fread(data.data(),1,data.size(),file); bool valid=!ferror(file) && feof(file); fclose(file);
         if (!valid) throw std::runtime_error("The BIOS loader is too large.");
         data.resize(bytes); (i?result.core:result.boot)=std::move(data);
      }
   }
   catch (...) { for (const auto &path:created) DeleteFileW(path.c_str()); throw; }
   for (const auto &path:created) DeleteFileW(path.c_str());
   return result;
}

// Verify the real filesystem with GRUB's own reader before offering BIOS or replacing its MBR.
void checkBiosFiles(const Device &device,bool installed,const GrubAssets &assets)
{
   FileTree target(device,false);
   BiosWorkspace workspace(L"slax-check-"); const std::wstring &folder=workspace.path;
   std::wstring executable=folder+L"\\"+wide(assets.tool("grub-fstest").name),logPath=folder+L"\\log";
   try
   {
      stageBiosAsset(executable,assets.tool("grub-fstest"),workspace.security);
      std::string volume=device.mount; if (!volume.empty() && volume.back()=='\\') volume.pop_back();
      std::wstring command=L"grub-fstest.exe \""+wide(volume)+L"\" ";
      command+=installed?L"cmp /slax/boot \""+wide(target.rootPath())+L"slax\\boot\"":L"xnu_uuid";
      auto security=workspace.security; security.bInheritHandle=TRUE;
      HANDLE log=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
      if (log==INVALID_HANDLE_VALUE) failed("Cannot open the BIOS verification log");
      STARTUPINFOW start={}; start.cb=sizeof(start); start.dwFlags=STARTF_USESTDHANDLES; start.hStdOutput=start.hStdError=log; start.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
      PROCESS_INFORMATION process={}; bool started=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,folder.c_str(),&start,&process); CloseHandle(log);
      if (!started) failed("Cannot start the BIOS filesystem verifier");
      WaitForSingleObject(process.hProcess,INFINITE); DWORD code=1; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
      if (code)
      {
         throw std::runtime_error("GRUB cannot read this filesystem or verify its boot files. Existing BIOS boot settings will be kept.\n"+toolLog(logPath));
      }
   }
   catch (...) { DeleteFileW(logPath.c_str()); DeleteFileW(executable.c_str()); throw; }
   DeleteFileW(logPath.c_str()); DeleteFileW(executable.c_str());
}
