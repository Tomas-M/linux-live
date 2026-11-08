#include "disk-format.h"
#include "hash.h"
#include <cstring>
#include <map>
#include <set>

// Compare filesystem/type identifiers without changing international user-facing labels.
static std::string lower(std::string text)
{
   for (char &c:text) if (c>='A' && c<='Z') c+=32;
   return text;
}

// Existing FAT volumes can host removable UEFI loaders without a separate boot filesystem.
static bool fat(const Partition &part)
{
   auto fs=lower(part.filesystem); return fs=="vfat" || fs=="fat32" || fs=="fat16" || fs=="fat";
}

// Identify only genuine ESP partition types; never silently retype an existing data partition.
static bool efi(const Partition &part)
{
   auto type=lower(part.type); return type=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b" || type=="0xef" || type=="239";
}

// Erasing one member of layered storage can destroy data on other physical disks too.
static bool member(const Partition &part)
{
   const std::set<std::string> filesystems={"linux_raid_member","lvm2_member","zfs_member","bcache","isw_raid_member","ddf_raid_member","mpath_member","btrfs_member"};
   const std::set<std::string> types={"42","142","191","253","0x42","0x8e","0xbf","0xfd","a19d880f-05fc-4d3b-a006-743f0f84911e","e6d6d379-f507-44c2-a23c-238f2a3df928","e75caf8f-f680-4cee-afa3-b001e56efc2d","5808c8aa-7e8f-42e0-85d2-e1e90434cfb3","af9b60a0-1431-4f62-bc68-3311714a69ad","6a898cc3-1dd2-11b2-99a6-080020736631","6a85cf4d-1dd2-11b2-99a6-080020736631"};
   return filesystems.count(lower(part.filesystem)) || types.count(lower(part.type));
}

// Reserve payload allocation slack and working metadata, instead of accepting an exact-byte fit.
static uint64_t required(const Image &image)
{
   return image.payload+std::max<uint64_t>(16*1024*1024,image.entries.size()*32768ULL);
}

// Explain all known unsupported configurations before enabling the destructive confirmation.
std::string basicEligibility(const Device &device,const Image &image,bool erase)
{
   if (!device.blocked.empty()) return device.blocked;
   if (!image.error.empty()) return image.error;
   if (device.sector!=512 && device.sector!=4096) return "This disk uses an unsupported logical sector size; only 512-byte and 4096-byte sectors are supported.";
   if (erase)
   {
      if (!device.eraseBlocked.empty()) return device.eraseBlocked;
      bool pool=member(device) || std::any_of(device.partitions.begin(),device.partitions.end(),[](const Partition &part) { return member(part); });
      if (pool) return "This disk contains a RAID, LVM or storage-pool member. Erasing it could also destroy data on other disks. Remove it safely using that storage system's management tools first.";
      if (device.diskBytes<1000000000) return "The whole disk must have at least 1 GB of capacity.";
      if (windowsXp() && device.sector!=512) return "Windows XP requires 512-byte logical sectors for this BIOS/UEFI MBR installation. Use Slax or newer Windows for this disk.";
      if (windowsXp() && device.diskBytes/device.sector>uint64_t(UINT32_MAX)+1) return "Windows XP's MBR layout cannot use a disk larger than 2 TiB at full capacity. Use Slax or newer Windows for this disk.";
      uint64_t reserve=(device.sector==4096?514ULL:258ULL)*1024*1024+1024*1024;
      if (device.diskBytes<=reserve || device.diskBytes-reserve<required(image)) return "This disk is too small for Slax, its EFI partition and filesystem overhead.";
      if ((device.diskBytes-reserve)/device.sector>UINT32_MAX) return "One full-capacity FAT32 data partition would exceed FAT32's sector-addressing limit. Use a smaller disk or an existing smaller partition.";
      return "";
   }
   if (!device.number && !device.partitions.empty()) return "Installing to this whole disk requires Erase entire disk, which replaces its complete partition layout.";
   if (device.bytes<1000000000) return "The selected filesystem must have at least 1 GB of total capacity.";
   if (device.readonly) return "The selected filesystem is read-only. Mount it read-write, choose another target, or erase the whole disk.";
   auto fs=lower(device.filesystem);
   if (member(device)) return "This partition belongs to RAID, LVM or a storage pool. Select a supported basic filesystem; its storage metadata will not be changed.";
   if (fs=="bitlocker" || fs=="crypto_luks") return "The selected partition is encrypted. Use an unencrypted filesystem, or erase a different disk. Existing encrypted system boot will not be changed.";
   const std::set<std::string> supported={"vfat","fat","fat16","fat32","ntfs","exfat","ext2","ext3","ext4","btrfs","xfs"};
   if (!supported.count(fs)) return "This filesystem is unformatted or unsupported for a Slax installation. Select Erase entire disk to create EFI and FAT32 data partitions.";
   if (std::find(image.filesystems.begin(),image.filesystems.end(),fs)==image.filesystems.end()) return "The selected Slax ISO does not advertise boot support for "+device.filesystem+". Choose a newer ISO, use a supported filesystem, or erase the whole disk to create FAT32 partitions.";
   if (device.uuid.empty()) return "The filesystem has no readable stable UUID. Run the installer with administrator/root permissions, repair the filesystem, or erase the whole disk.";
   for (unsigned char c:device.uuid) if (!(c>='0' && c<='9') && !(c>='a' && c<='f') && !(c>='A' && c<='F') && c!='-') return "The filesystem identifier cannot safely be used by the Slax boot loader.";
   bool hasEfi=std::any_of(device.partitions.begin(),device.partitions.end(),[](const Partition &p) { return efi(p) && fat(p); });
   if (!fat(device) && !hasEfi) return "This filesystem needs an existing FAT EFI System Partition on the same disk for UEFI boot. Select another partition, or erase the whole disk to create the required EFI and FAT32 data partitions.";
   if (device.free!=UINT64_MAX && device.free<required(image)) return "There is not enough free space for Slax and filesystem overhead. Free space, choose another target, or erase the whole disk.";
   return "";
}

// Compute a small-file hash through the same source/destination streams used for payload verification.
static std::array<unsigned char,32> digest(FILE *file,uint64_t bytes)
{
   Hash hash; unsigned char buffer[65536];
   while (bytes)
   {
      size_t count=std::min<uint64_t>(sizeof(buffer),bytes);
      if (fread(buffer,1,count,file)!=count) throw std::runtime_error("A source or destination file could not be read completely.");
      hash.add(buffer,count); bytes-=count;
   }
   return hash.finish();
}

// Compare a known existing fallback with Windows Boot Manager before permitting an atomic backup.
static std::string bootDigest(FileTree &tree,const std::string &path)
{
   FILE *file=tree.open(path,false); Hash hash; unsigned char buffer[65536]; size_t bytes; uint64_t total=0;
   try
   {
      while ((bytes=fread(buffer,1,sizeof(buffer),file))) { hash.add(buffer,bytes); total+=bytes; if (total>32*1024*1024) throw std::runtime_error("An existing EFI boot loader is unexpectedly large."); }
      if (ferror(file)) throw std::runtime_error("An existing EFI boot loader could not be read completely.");
   }
   catch (...) { fclose(file); throw; }
   fclose(file); auto value=hash.finish(); return {reinterpret_cast<const char *>(value.data()),value.size()};
}

// Produce a concrete plan while holding the original disk object open across the native confirmation.
InstallPlan prepareInstall(Device device,const Image &image,bool erase)
{
   auto reason=basicEligibility(device,image,erase);
   if (!reason.empty()) throw std::runtime_error(reason);
   if (!administrator()) throw std::runtime_error("Run the installer as administrator on Windows, or as root in Slax, before installing.");
   InstallPlan plan; plan.device=device; plan.image=image; plan.erase=erase;
   plan.disk=std::make_shared<Disk>(device); plan.snapshot=partitionSnapshot(*plan.disk,!erase);
   for (const auto &entry:image.entries)
   {
      bool menu=entry.path.find("slax/boot/grub/")==0 && entry.path.find("slax/boot/grub/windows/")!=0 && entry.path.find("slax/boot/grub/net/")!=0;
      bool kernel=entry.path=="slax/boot/vmlinuz" || entry.path=="slax/boot/initrfs.img";
      bool loader=entry.path.find("slax/boot/EFI/BOOT/")==0;
      if (!entry.directory && (menu || kernel || loader)) plan.bootBytes+=entry.bytes+32768;
   }
   plan.bootBytes+=4*1024*1024;
   if (erase)
   {
      auto layout=newLayout(device,windowsXp()); fatGeometry(layout.efi.bytes,device.sector); fatGeometry(layout.data.bytes,device.sector);
      plan.bios=device.sector==512;
   }
   else
   {
      FileTree target(device,false);
      if (target.exists("slax")) throw std::runtime_error("A slax file or directory already exists on this filesystem. It may contain saved sessions. Move or rename it before installing; it will not be overwritten.");
      if (target.available()<required(image)) throw std::runtime_error("There is not enough actual free space in the selected filesystem.");
      std::vector<Partition> candidates;
      for (const auto &part:device.partitions) if (efi(part) && fat(part) && !part.readonly) candidates.push_back(part);
      if (!candidates.empty())
      {
         bool selected=false;
         for (const auto &part:candidates)
         {
            FileTree boot(part,false);
            if (boot.available()<plan.bootBytes) continue;
            bool windows=boot.exists("EFI/Microsoft/Boot/bootmgfw.efi");
            if (!selected || windows) { plan.efi=part; selected=true; if (windows) { plan.windowsLoader="/EFI/Microsoft/Boot/bootmgfw.efi"; break; } }
         }
         if (!selected) throw std::runtime_error("No existing EFI System Partition has enough free space for Slax's signed loaders, kernel and boot menu. Free EFI space or erase the whole disk.");
      }
      else
      {
         if (!fat(device)) throw std::runtime_error("No usable EFI System Partition is available on this disk. Erase the whole disk to create one.");
         plan.efi=static_cast<const Partition &>(device);
         if (target.exists("EFI/BOOT")) throw std::runtime_error("This filesystem already contains an EFI fallback boot loader. It will not be overwritten. Use a separate EFI System Partition or erase the whole disk.");
         if (target.available()<required(image)+plan.bootBytes) throw std::runtime_error("There is not enough free space for both the Slax payload and EFI boot files.");
         if (target.exists("bootmgr") || target.exists("ntldr") || target.exists("BOOTMGR") || target.exists("NTLDR")) throw std::runtime_error("This FAT filesystem contains an existing Windows BIOS loader. Its boot path will not be replaced automatically. Choose a target with an EFI System Partition or prepare a separate disk.");
         bool simple=device.partitions.size()<=1 && device.start>=1024*1024 && device.sector==512;
         if (simple && device.eraseBlocked.empty())
         {
            // Confirm the actual sole primary FAT partition; cached table names cannot authorize gap writes.
            std::vector<unsigned char> mbr(device.sector*2); plan.disk->read(0,mbr.data(),mbr.size());
            bool matches=false; unsigned used=0;
            for (unsigned i=0; i<4; i++)
            {
               auto entry=mbr.data()+446+i*16; unsigned type=entry[4];
               if (!type || !disk32(entry+12)) continue;
               used++;
               bool fatType=type==4 || type==6 || type==0x0b || type==0x0c || type==0x0e;
               bool geometry=uint64_t(disk32(entry+8))*device.sector==device.start && uint64_t(disk32(entry+12))*device.sector==device.bytes;
               if (fatType && geometry && device.number==i+1) matches=true;
            }
            bool signature=mbr[510]==0x55 && mbr[511]==0xaa;
            bool dos=memcmp(mbr.data()+device.sector,"EFI PART",8)!=0;
            bool solePartition=used==1 && matches;
            plan.bios=dos && solePartition && signature;
         }
      }
      if (device.usb && efi(plan.efi))
      {
         FileTree esp(plan.efi,false);
         if (esp.exists("EFI/BOOT"))
         {
            if (plan.windowsLoader.empty() || !esp.exists("EFI/BOOT/BOOTX64.EFI")) throw std::runtime_error("This removable disk already has an unknown EFI fallback loader. It will not be replaced. Choose another target or erase the whole disk.");
            auto original=bootDigest(esp,"EFI/BOOT/BOOTX64.EFI");
            if (original!=bootDigest(esp,"EFI/Microsoft/Boot/bootmgfw.efi")) throw std::runtime_error("The existing removable EFI fallback is not an identical Windows Boot Manager. It will not be replaced. Use another target or erase the whole disk.");
            plan.fallbackDigest=original;
         }
      }
      // Internal or Windows-system multiboot uses a new vendor path and preserves every original boot entry.
      plan.registerBoot=(!device.usb || device.system) && efi(plan.efi);
      if (plan.registerBoot)
      {
         if (!firmwareAvailable()) throw std::runtime_error("Automatic internal-disk multiboot requires a 64-bit UEFI-booted session with writable firmware variables. Start Windows or Slax in 64-bit UEFI mode, or install to a separate removable disk.");
         plan.bootOrder=readFirmware("BootOrder");
         if (plan.bootOrder.empty() || plan.bootOrder.size()%2) throw std::runtime_error("The existing UEFI boot order is invalid. No boot settings will be changed.");
         std::set<uint16_t> referenced;
         for (size_t at=0; at<plan.bootOrder.size(); at+=2) referenced.insert(plan.bootOrder[at]|plan.bootOrder[at+1]<<8);
         for (const char *variable:{"BootCurrent","BootNext"})
         {
            auto value=readFirmware(variable); if (value.size()==2) referenced.insert(value[0]|value[1]<<8);
         }
         bool found=false;
         for (uint32_t n=0; n<65535; n++)
         {
            char name[16]; snprintf(name,sizeof(name),"Boot%04X",n);
            if (!referenced.count(n) && readFirmware(name).empty()) { plan.bootNumber=n; found=true; break; }
         }
         if (!found) throw std::runtime_error("The firmware has no free UEFI boot entry slots.");
      }
   }
   if (!erase) for (const auto &other:scanDevices(image.path))
   {
      auto duplicate=[&](const Partition &part)
      {
         bool sameTarget=other.diskId==device.diskId && part.start==device.start;
         bool sameEfi=other.diskId==device.diskId && part.start==plan.efi.start;
         return (!sameTarget && part.uuid==device.uuid) || (!sameEfi && !plan.efi.uuid.empty() && part.uuid==plan.efi.uuid);
      };
      if (duplicate(other) || std::any_of(other.partitions.begin(),other.partitions.end(),duplicate)) throw std::runtime_error("Another connected filesystem has the same UUID as the target or EFI partition. Disconnect the duplicate or assign it a unique filesystem UUID before installing; boot would otherwise be ambiguous.");
   }
   plan.confirmation="Install Slax to "+device.name+" ("+device.label+")\n"+device.path+" · "+sizeText(erase?device.diskBytes:device.bytes)+"\n\n";
   if (erase)
   {
      plan.confirmation+="ERASE THE ENTIRE PHYSICAL DISK, including every partition below:\n";
      if (device.partitions.empty()) plan.confirmation+="  "+device.path+" — entire disk; any existing contents will be lost\n";
      for (const auto &part:device.partitions) plan.confirmation+="  "+part.label+" — "+sizeText(part.bytes)+" · "+(part.filesystem.empty()?"unformatted/unknown":part.filesystem)+"\n";
      if (windowsXp()) plan.confirmation+="\nCreate MBR with an active FAT32 Slax data partition first and a small FAT32 EFI partition second. Install GRUB BIOS boot code in the MBR and reserved space before the first partition. The medium will support BIOS and UEFI boot.";
      else plan.confirmation+="\nCreate GPT, a 1 MiB BIOS boot partition, a FAT32 EFI partition and one FAT32 Slax data partition using the remaining capacity.";
      plan.confirmation+=" All existing data on this physical disk will be lost.";
   }
   else
   {
      plan.confirmation+="Keep the existing partitions and files. Create a new slax directory and separate Slax EFI boot files.";
      if (!plan.windowsLoader.empty()) plan.confirmation+="\nPreserve Windows Boot Manager and add Windows to the Slax menu.";
      if (!plan.fallbackDigest.empty()) plan.confirmation+="\nBack up the identical Windows removable fallback directory, then publish Slax there. The Microsoft boot loader and its firmware entry will be retained.";
      if (plan.registerBoot) plan.confirmation+="\nAdd Slax first in the UEFI boot order, retaining all original boot entries. The full Slax menu will be visible with a 10-second timeout.";
      if (plan.bios) plan.confirmation+="\nInstall GRUB BIOS boot code in the MBR and the unused space before the first partition. The partition table and files will be kept.";
   }
   if (!plan.bios) plan.confirmation+="\n\nThis installation supports UEFI boot only. "+std::string(device.sector==4096?"The disk uses native 4096-byte sectors; the BIOS loader requires 512-byte sectors.":"Existing BIOS boot code will not be replaced in this configuration.");
   return plan;
}

// Read small boot text files from their already-validated ISO extents.
static std::string contents(FILE *source,const ImageEntry &entry)
{
   if (entry.bytes>8*1024*1024) throw std::runtime_error("A boot configuration file is unexpectedly large.");
   std::string text(entry.bytes,'\0'); seekImage(source,entry.offset);
   if (fread(text.data(),1,text.size(),source)!=text.size()) throw std::runtime_error("Cannot read a boot configuration file from the ISO.");
   return text;
}

// Rewrite only the copied ESP boot assets so signed GRUB reads its kernel from FAT, even with NTFS data.
static void replace(std::string &text,const std::string &from,const std::string &to)
{
   size_t at=0;
   while ((at=text.find(from,at))!=std::string::npos) { text.replace(at,from.size(),to); at+=to.size(); }
}

// Keep a visible countdown in the full menu for both BIOS and UEFI installations.
static std::string menuTheme(std::string text,bool windows)
{
   if (windows) replace(text,"height = 64","height = 80");
   text+="\n+ label { id = \"__timeout__\" left = 0 top = \"100%-52\" width = 100% height = 16 text = \"Booting Slax in %d seconds\" font = \"Unifont Regular 16\" color = \"#FFFFFF\" align = \"center\" }\n";
   return text;
}

// Show the full Slax menu, with a visible timeout and an intact Windows chainloader when present.
static std::string menu(const std::string &boot,const std::string &uuid,const InstallPlan &plan)
{
   std::string text="set default=0\nset timeout=10\nset timeout_style=menu\nset gfxmode=auto\nset gfxpayload=keep\nset bootuuid="+uuid+"\nexport bootuuid\nsource "+boot+"/grub/graphics.cfg\nset theme="+boot+"/grub/theme.txt\nexport theme\nsource "+boot+"/grub/entries.cfg\n";
   if (!plan.windowsLoader.empty()) text+="menuentry \"Windows Boot Manager\" {\n   search --no-floppy --fs-uuid --set=windows "+plan.efi.uuid+"\n   chainloader ($windows)"+plan.windowsLoader+"\n}\n";
   return text;
}

// Sort parent directories before their children for both native filesystems and the raw FAT writer.
static std::vector<Content> ordered(std::vector<Content> files)
{
   std::map<std::string,Content> result;
   for (auto &file:files)
   {
      auto path=file.entry.path;
      for (auto slash=path.find('/'); slash!=std::string::npos; slash=path.find('/',slash+1))
      {
         auto parent=path.substr(0,slash); if (!result.count(parent)) result[parent]=generated(parent,"",true);
      }
      result[path]=std::move(file);
   }
   std::vector<Content> sorted; for (auto &item:result) sorted.push_back(std::move(item.second)); return sorted;
}

// Assemble an isolated vendor directory; no Microsoft or other vendor path is overwritten.
static std::vector<Content> bootFiles(const InstallPlan &plan,FILE *source,const std::string &vendor,const std::string &uuid,bool fallback)
{
   std::vector<Content> files;
   for (const auto &entry:plan.image.entries)
   {
      if (entry.directory) continue;
      bool asset=entry.path.find("slax/boot/grub/")==0 && entry.path.find("slax/boot/grub/windows/")!=0 && entry.path.find("slax/boot/grub/net/")!=0;
      bool kernel=entry.path=="slax/boot/vmlinuz" || entry.path=="slax/boot/initrfs.img";
      if (asset && (entry.path.find(".img")!=std::string::npos || entry.path.find("usb-space.tar")!=std::string::npos)) continue;
      if (asset || kernel)
      {
         Content file={entry,""}; file.entry.path=vendor+"/boot/"+entry.path.substr(10);
         if (asset && (entry.path.size()>4 && (entry.path.substr(entry.path.size()-4)==".cfg" || entry.path.substr(entry.path.size()-4)==".txt")))
         {
            auto text=contents(source,entry); replace(text,"/slax/boot","/"+vendor+"/boot");
            if (entry.path=="slax/boot/grub/theme.txt")
            {
               text=menuTheme(text,!plan.windowsLoader.empty());
            }
            file=generated(file.entry.path,text);
         }
         files.push_back(std::move(file));
      }
      if (entry.path.find("slax/boot/EFI/BOOT/")==0)
      {
         auto name=entry.path.substr(19);
         if (lower(name)=="grub.cfg") continue;
         Content file={entry,""}; file.entry.path=vendor+"/"+name; files.push_back(file);
         if (fallback) { file.entry.path="EFI/BOOT/"+name; files.push_back(file); }
      }
   }
   files.push_back(generated(vendor+"/boot/grub/menu.cfg",menu("/"+vendor+"/boot",uuid,plan)));
   std::string bootstrap="set slax_path=$cmdpath\nif [ -n \"$fw_path\" ]; then set slax_path=$fw_path; fi\nif regexp --set=1:slax_efi '^\\(([^)]+)\\)' \"$slax_path\"; then set root=$slax_efi; fi\nset prefix=($root)/"+vendor+"/boot/grub\nconfigfile /"+vendor+"/boot/grub/menu.cfg\n";
   files.push_back(generated(vendor+"/grub.cfg",bootstrap));
   if (fallback) files.push_back(generated("EFI/BOOT/grub.cfg",bootstrap));
   return ordered(std::move(files));
}

// Stream, flush and SHA-256 read-back each file before publishing the directory.
static void copyTree(FileTree &tree,const std::vector<Content> &files,FILE *source,const Progress &progress)
{
   uint64_t total=0,completed=0; for (const auto &file:files) if (!file.entry.directory) total+=file.entry.bytes;
   std::vector<std::pair<std::string,bool>> created;
   try
   {
      unsigned char buffer[1024*1024];
      for (const auto &file:files)
      {
         const auto &entry=file.entry;
         if (entry.directory)
         {
            if (!tree.exists(entry.path)) { tree.mkdir(entry.path); created.push_back({entry.path,true}); }
            continue;
         }
         FILE *output=tree.open(entry.path,true); created.push_back({entry.path,false});
         try
         {
            Hash expected;
            if (entry.offset!=UINT64_MAX) seekImage(source,entry.offset);
            for (uint64_t copied=0; copied<entry.bytes;)
            {
               size_t count=std::min<uint64_t>(sizeof(buffer),entry.bytes-copied);
               if (entry.offset==UINT64_MAX) memcpy(buffer,file.data.data()+copied,count);
               else if (fread(buffer,1,count,source)!=count) throw std::runtime_error("The source ISO stopped responding during copying.");
               expected.add(buffer,count);
               if (fwrite(buffer,1,count,output)!=count) throw std::runtime_error("The destination is full or stopped accepting file writes.");
               copied+=count; progress("Copying "+entry.path,total?int((completed+copied)*75/total):75);
            }
            flushFile(output); seekImage(output,0);
            if (digest(output,entry.bytes)!=expected.finish()) throw std::runtime_error("Read-back verification failed for "+entry.path+". The medium may be damaged.");
         }
         catch (...) { fclose(output); throw; }
         fclose(output); completed+=entry.bytes;
      }
      tree.sync();
   }
   catch (...)
   {
      for (auto it=created.rbegin(); it!=created.rend(); it++) tree.remove(it->first,it->second);
      throw;
   }
}

// Perform real writes only after revalidating the source, approved metadata and retained device handle.
void install(const InstallPlan &plan,const Progress &progress)
{
   using File=std::unique_ptr<FILE,int (*)(FILE *)>;
   File source(openImage(plan.image.path),fclose);
   if (!source || imageIdentity(source.get())!=plan.image.identity) throw std::runtime_error("The selected ISO changed or cannot be opened. No changes were made.");
   if (partitionSnapshot(*plan.disk,!plan.erase)!=plan.snapshot) throw std::runtime_error("The approved partition table changed. No changes were made.");
   auto reason=basicEligibility(plan.device,plan.image,plan.erase); if (!reason.empty()) throw std::runtime_error(reason);
   progress("Preparing verified boot files…",1);
   std::string vendor="EFI/Slax/"+randomId();
   Layout layout;
   if (plan.erase) layout=newLayout(plan.device,windowsXp());
   std::string uuid=plan.erase?layout.data.uuid:plan.device.uuid;
   BiosImage bios;
   if (plan.bios) bios=buildBios(plan.image,source.get(),uuid);
   std::vector<Content> payload;
   for (const auto &entry:plan.image.entries) payload.push_back({entry,""});
   for (auto &file:payload) if (file.entry.path=="slax/boot/grub.cfg") file=generated(file.entry.path,menu("/slax/boot",uuid,plan));
   for (auto &file:payload) if (file.entry.path=="slax/boot/grub/theme.txt") file=generated(file.entry.path,menuTheme(contents(source.get(),file.entry),!plan.windowsLoader.empty()));
   auto boot=bootFiles(plan,source.get(),vendor,uuid,plan.erase || plan.device.usb || !efi(plan.efi));
   if (!plan.fallbackDigest.empty())
      for (auto &file:boot) if (file.entry.path.find("EFI/BOOT")==0) file.entry.path=vendor+"/fallback"+file.entry.path.substr(8);
   boot=ordered(std::move(boot));
   if (imageIdentity(source.get())!=plan.image.identity) throw std::runtime_error("The ISO changed while preparing the installation. No changes were made.");
   bool changed=false;
   try
   {
      plan.disk->reserve();
      if (plan.erase)
      {
         plan.disk->claim(true);
         if (partitionSnapshot(*plan.disk,false)!=plan.snapshot) throw std::runtime_error("The approved disk contents changed before erasure. No changes were made.");
         changed=true;
         writeFat(*plan.disk,layout.data,ordered(std::move(payload)),source.get(),[&](const auto &text,int percent) { progress(text,5+percent); });
         writeFat(*plan.disk,layout.efi,boot,source.get(),[&](const auto &text,int percent) { progress(text,80+percent/6); });
         if (imageIdentity(source.get())!=plan.image.identity) throw std::runtime_error("The ISO changed during copying; boot activation was stopped.");
         progress("Writing BIOS and UEFI boot records…",94);
         if (plan.bios) writeBios(*plan.disk,layout.bios.start,layout.bios.bytes,bios,layout.mbr);
         writeLayout(*plan.disk,layout);
      }
      else
      {
         std::string stage=".slax-install-"+randomId();
         {
            FileTree target(plan.device,true);
            if (target.exists("slax") || target.available()<required(plan.image)) throw std::runtime_error("The destination changed or no longer has enough free space. No files were overwritten.");
            for (auto &file:payload) file.entry.path=stage+file.entry.path.substr(4);
            changed=true;
            copyTree(target,ordered(std::move(payload)),source.get(),[&](auto text,int percent) { plan.disk->verify(); replace(text,stage,"slax"); progress(text,5+percent); });
            if (imageIdentity(source.get())!=plan.image.identity) throw std::runtime_error("The ISO changed during copying; the new slax directory was not published.");
            target.move(stage,"slax"); target.sync();
         }
         {
            FileTree esp(plan.efi,true);
            if (!efi(plan.efi) && esp.exists("EFI/BOOT")) throw std::runtime_error("An EFI fallback loader appeared after confirmation. It will not be overwritten.");
            if (!plan.fallbackDigest.empty() && bootDigest(esp,"EFI/BOOT/BOOTX64.EFI")!=plan.fallbackDigest) throw std::runtime_error("The existing EFI fallback changed after confirmation; it will not be replaced.");
            if (plan.device.usb && plan.fallbackDigest.empty() && esp.exists("EFI/BOOT")) throw std::runtime_error("An EFI fallback loader appeared after confirmation; it will not be replaced.");
            if (esp.available()<plan.bootBytes) throw std::runtime_error("The EFI filesystem no longer has enough free space. The copied Slax files remain, and the original boot loader has not been replaced.");
            copyTree(esp,boot,source.get(),[&](const auto &,int percent) { plan.disk->verify(); progress("Copying UEFI boot files…",80+percent/6); });
            esp.sync();
            if (!plan.fallbackDigest.empty())
            {
               esp.move("EFI/BOOT",vendor+"/original-fallback");
               try { esp.move(vendor+"/fallback","EFI/BOOT"); esp.sync(); }
               catch (...) { esp.move(vendor+"/original-fallback","EFI/BOOT"); esp.sync(); throw; }
            }
         }
         if (plan.bios)
         {
            progress("Installing GRUB BIOS boot code…",94);
            plan.disk->claim(false);
            if (partitionSnapshot(*plan.disk)!=plan.snapshot) throw std::runtime_error("The partition table changed before BIOS setup. The original boot code was not replaced.");
            std::vector<unsigned char> original(std::min<uint64_t>(plan.device.start,1024*1024)),mbr(plan.device.sector);
            plan.disk->read(0,original.data(),original.size()); std::copy(original.begin(),original.begin()+mbr.size(),mbr.begin());
            try { writeBios(*plan.disk,plan.device.sector,original.size()-plan.device.sector,bios,mbr); plan.disk->write(0,mbr.data(),mbr.size()); plan.disk->flush(); }
            catch (...) { plan.disk->write(0,original.data(),original.size()); plan.disk->flush(); throw; }
         }
      }
      if (imageIdentity(source.get())!=plan.image.identity) throw std::runtime_error("The ISO changed during installation. The copied files were individually verified, but installation cannot be confirmed.");
      plan.disk->verify(); plan.disk->flush();
      if (plan.registerBoot)
      {
         progress("Adding Slax multiboot while preserving existing entries…",97);
         registerFirmware(plan,"\\"+vendor+"\\BOOTX64.EFI",vendor+"/boot-order-backup.txt");
      }
      plan.disk->release(); refreshDisk(plan.device); progress("Installation verified and complete.",100);
   }
   catch (const std::exception &error)
   {
      plan.disk->release();
      throw std::runtime_error(std::string(error.what())+(changed?(plan.erase?"\n\nThe approved disk may already have been repartitioned or partially overwritten. Installation did not finish.":"\n\nSome verified Slax files may remain on the target. Existing files were not overwritten. Installation did not finish."):"\n\nNo installation data was written."));
   }
}
