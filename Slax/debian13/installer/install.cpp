#include "disk-format.h"
#include "hash.h"
#include "grub-assets.h"
#include <cstring>
#include <map>
#include <set>
#include <filesystem>

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

// Only readable data-only disks may discard an old BIOS loader; unknown systems stay protected.
static bool dataOnly(const Device &device,bool updatingSlax)
{
   if (device.system || device.partitions.empty()) return false;
   const std::set<std::string> systemFiles={"boot","etc","windows","winnt","system","ntldr","bootmgr","io.sys","kernel.sys","netbsd","bsd","os2krnl"};
   try
   {
      for (const auto &part:device.partitions)
      {
         auto type=lower(part.type);
         bool bootArea=type=="21686148-6449-6e6f-744e-656564454649" || type=="0xda";
         if (bootArea && part.filesystem.empty()) continue;
         if (part.filesystem.empty() || lower(part.filesystem)=="btrfs") return false;
         FileTree tree(part,false);
         for (const auto &entry:std::filesystem::directory_iterator(std::filesystem::u8path(tree.rootPath())))
         {
            auto name=lower(entry.path().filename().u8string());
            if (systemFiles.count(name) || entry.is_symlink()) return false;
            if (name=="slax" && tree.exists("slax/boot/vmlinuz") && (!updatingSlax || part.start!=device.start)) return false;
            if (name!="efi") continue;
            for (const auto &vendor:std::filesystem::directory_iterator(entry.path()))
            {
               auto name=lower(vendor.path().filename().u8string());
               if (vendor.is_symlink() || (name!="slax" && name!="boot")) return false;
               if (name!="boot") continue;
               std::unique_ptr<FILE,int (*)(FILE *)> file(tree.open("EFI/BOOT/grub.cfg",false),fclose);
               std::string config(1024,'\0'); config.resize(fread(config.data(),1,config.size(),file.get()));
               if (ferror(file.get()) || config.find("\nconfigfile /EFI/Slax/")==std::string::npos) return false;
            }
         }
      }
   }
   catch (const std::exception &) { return false; }
   return true;
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
   bool fatTarget=erase || fat(device);
   if (fatTarget && std::any_of(image.entries.begin(),image.entries.end(),[](const ImageEntry &entry) { return !entry.directory && entry.bytes>UINT32_MAX; })) return "This ISO contains a file larger than FAT32's 4 GiB file limit. Select an existing NTFS or Linux filesystem; it cannot be installed to a new FAT32 layout.";
   if (device.sector!=512 && device.sector!=4096) return "This disk uses an unsupported logical sector size; only 512-byte and 4096-byte sectors are supported.";
   if (erase)
   {
      if (!device.eraseBlocked.empty()) return device.eraseBlocked;
      bool pool=member(device) || std::any_of(device.partitions.begin(),device.partitions.end(),[](const Partition &part) { return member(part); });
      if (pool) return "This disk contains a RAID, LVM or storage-pool member. Erasing it could also destroy data on other disks. Remove it safely using that storage system's management tools first.";
      if (device.diskBytes<1000000000) return "The whole disk must have at least 1 GB of capacity.";
      if (mbrRequired(device) && device.sector!=512) return "This Windows version needs an MBR layout with 512-byte logical sectors to keep the Slax partition accessible. Use Slax or newer Windows for this disk.";
      if (mbrRequired(device) && device.diskBytes/device.sector>uint64_t(UINT32_MAX)+1) return "This Windows version cannot expose a full-capacity Slax partition on this disk using its compatible MBR layout (maximum 2 TiB). Use Slax or newer Windows.";
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

// Saved profiles and this disk's recovery records must never come from a different installation.
static bool retainedPath(const std::string &path)
{
   auto key=filenameKey(path);
   bool session=key=="slax/changes" || key.find("slax/changes/")==0;
   bool recovery=key=="slax/boot/grub/boot-backup" || key.find("slax/boot/grub/boot-backup/")==0;
   return session || recovery;
}

// Detect aliases by native file identity, not by the spelling of the source path.
static void checkSourceTarget(FileTree &target,const Image &image)
{
   for (const auto &entry:image.entries)
   {
      if (!target.exists(entry.path)) continue;
      if (target.isDirectory(entry.path)!=entry.directory) throw std::runtime_error("A source file conflicts with an existing directory, or a source directory conflicts with an existing file: "+entry.path+". Move that item before installing.");
      auto source=entry.source.empty()?image.path:entry.source;
      if (target.same(entry.path,source)) throw std::runtime_error("The Slax source and destination are the same files. Choose a different source ISO or Slax directory.");
   }
}

// Produce a concrete plan while holding the original disk object open across the native confirmation.
InstallPlan prepareInstall(Device device,const Image &source,bool erase)
{
   if (!device.blocked.empty()) throw std::runtime_error(device.blocked);
   if (!source.error.empty()) throw std::runtime_error(source.error);
   if (!administrator()) throw std::runtime_error("Run the installer as administrator on Windows, or as root in Slax, before installing.");
   auto grub=device.sector==512?std::make_shared<GrubAssets>(source):nullptr;
   Image image=source; bool overwrite=false;
   if (!erase)
   {
      FileTree target(device,false); overwrite=target.exists("slax");
      if (overwrite)
      {
         if (!target.isDirectory("slax")) throw std::runtime_error("The destination already has a file named slax. Move it before installing.");
         image.entries.erase(std::remove_if(image.entries.begin(),image.entries.end(),[](const ImageEntry &entry) { return retainedPath(entry.path); }),image.entries.end());
         image.payload=0; for (const auto &entry:image.entries) if (!entry.directory) image.payload+=entry.bytes;
      }
      checkSourceTarget(target,image);
   }
   auto reason=basicEligibility(device,image,erase);
   if (!reason.empty()) throw std::runtime_error(reason);
   InstallPlan plan; plan.device=device; plan.image=image; plan.erase=erase; plan.overwrite=overwrite;
   plan.grub=std::move(grub);
   bool replaceBios=false;
   plan.disk=std::make_shared<Disk>(device); plan.snapshot=partitionSnapshot(*plan.disk,!erase);
   if (!erase && lower(device.filesystem)=="ntfs")
   {
      std::vector<unsigned char> boot(device.sector); plan.disk->read(device.start,boot.data(),boot.size());
      if (memcmp(boot.data()+3,"NTFS    ",8)) throw std::runtime_error("This NTFS volume is encrypted or cannot be read directly from the disk. Slax needs an unencrypted filesystem, even when Windows has unlocked the volume. Choose another target.");
   }
   for (const auto &entry:image.entries)
   {
      bool menu=entry.path.find("slax/boot/grub/")==0 && entry.path.find("slax/boot/grub/windows/")!=0 && entry.path.find("slax/boot/grub/net/")!=0;
      if (entry.path.find("slax/boot/grub/bios/")==0 || entry.path.find("slax/boot/grub/i386-pc/")==0) menu=false;
      bool kernel=entry.path=="slax/boot/vmlinuz" || entry.path=="slax/boot/initrfs.img";
      bool loader=entry.path.find("slax/boot/EFI/BOOT/")==0;
      if (!entry.directory && (menu || kernel || loader)) plan.bootBytes+=entry.bytes+32768;
   }
   plan.bootBytes+=4*1024*1024;
   if (erase)
   {
      auto layout=newLayout(device,mbrRequired(device)); fatGeometry(layout.efi.bytes,device.sector); fatGeometry(layout.data.bytes,device.sector);
      plan.bios=device.sector==512; plan.fallback=true;
      if (plan.bios) plan.biosImage=std::make_shared<BiosImage>(buildBios(layout.data,!layout.primary.empty(),*plan.grub));
   }
   else
   {
      FileTree target(device,false);
      if (target.available()<required(image)) throw std::runtime_error("There is not enough free space in the selected filesystem. Free some space or choose another target.");
      // An inaccessible/full ESP must not rule out another valid ESP or a BIOS-only installation.
      std::string efiFailure;
      for (const auto &part:device.partitions)
      {
         if (!efi(part) || !fat(part) || part.readonly) continue;
         try
         {
            FileTree boot(part,false);
            if (boot.available()<plan.bootBytes) { efiFailure="The existing EFI partition does not have enough free space."; continue; }
            bool windows=boot.exists("EFI/Microsoft/Boot/bootmgfw.efi");
            if (!plan.efi.bytes || windows)
            {
               plan.efi=part;
               if (windows) { plan.windowsLoader="/EFI/Microsoft/Boot/bootmgfw.efi"; break; }
            }
         }
         catch (const std::exception &error) { efiFailure=error.what(); }
      }
      // The small embedded core can read FAT, NTFS or Linux data without an EFI partition.
      std::vector<unsigned char> header(device.sector); plan.disk->read(device.sector,header.data(),header.size());
      bool gpt=!memcmp(header.data(),"EFI PART",8);
      if (device.number && device.sector==512)
      {
         try { checkBiosFiles(device,false,*plan.grub); plan.biosImage=std::make_shared<BiosImage>(buildBios(device,gpt,*plan.grub)); }
         catch (const std::exception &error) { plan.bootWarning=error.what(); }
      }
      uint64_t coreBytes=plan.biosImage?plan.biosImage->core.size():0;
      replaceBios=dataOnly(device,overwrite);
      plan.bootLayout=std::make_shared<BootLayout>(planBootLayout(*plan.disk,plan.biosImage.get(),plan.bootBytes,!plan.efi.bytes && device.number,replaceBios));
      plan.bios=plan.biosImage && plan.bootLayout->bios.bytes;
      if (plan.biosImage && !plan.bios) plan.bootWarning="The BIOS loader needs "+std::to_string((coreBytes+1023)/1024)+" KiB of safe empty space outside the filesystems. None of the available areas is large enough.";
      if (!plan.efi.bytes && plan.bootLayout->efi.bytes) plan.efi=plan.bootLayout->efi;
      if (!plan.efi.bytes && fat(device))
      {
         if (!target.exists("EFI/BOOT") && target.available()>=required(image)+plan.bootBytes) plan.efi=static_cast<const Partition &>(device);
         else if (overwrite && target.available()>=required(image)+plan.bootBytes)
         {
            plan.efi=static_cast<const Partition &>(device); readSlaxEfi(plan);
            if (plan.efiFallback.empty()) { plan.efi={}; plan.efiVendors.clear(); }
         }
         else efiFailure="The data filesystem already has an EFI fallback loader, or lacks room for the additional EFI files.";
      }
      if (plan.efiVendors.empty()) readSlaxEfi(plan);
      if (!plan.efiVendors.empty())
      {
         plan.bootBytes*=plan.efiVendors.size(); FileTree boot(plan.efi,false);
         if (boot.available()<plan.bootBytes) throw std::runtime_error("The EFI partition does not have enough free space to stage the updated Slax boot files.");
      }
      if (plan.bios)
      {
         const auto &mbr=plan.bootLayout->originalMbr;
         bool code=std::any_of(mbr.begin(),mbr.begin()+440,[](unsigned char c) { return c!=0; });
         // A byte-identical core already targets this Slax directory, not another operating system.
         bool sameCore=std::any_of(plan.bootLayout->biosBefore.begin(),plan.bootLayout->biosBefore.end(),[](unsigned char c) { return c!=0; });
         if (code && !sameCore && !replaceBios)
         {
            bool windows=target.exists("ntldr") || target.exists("bootmgr") || target.exists("NTLDR") || target.exists("BOOTMGR") || target.exists("Windows") || target.exists("WINDOWS");
            if (gpt) plan.previousBios="Previous BIOS boot loader";
            else plan.previousBios=windows?"Windows (BIOS)":"Previous operating system (BIOS)";
         }
      }
      // Keep unknown fallback files intact; a matching Microsoft copy can be backed up atomically.
      plan.fallback=plan.efi.bytes && (device.usb || !efi(plan.efi) || plan.bootLayout->efi.bytes);
      if (plan.fallback && !plan.bootLayout->efi.bytes)
      {
         FileTree esp(plan.efi,false);
         if (esp.exists("EFI/BOOT"))
         {
            bool windowsCopy=!plan.windowsLoader.empty() && esp.exists("EFI/BOOT/BOOTX64.EFI");
            if (!plan.efiFallback.empty() || (windowsCopy && bootDigest(esp,"EFI/BOOT/BOOTX64.EFI")==bootDigest(esp,"EFI/Microsoft/Boot/bootmgfw.efi"))) plan.fallbackDigest=bootDigest(esp,"EFI/BOOT/BOOTX64.EFI");
            else { plan.fallback=false; plan.bootWarning="The existing EFI fallback loader will be kept. Select Slax Linux in the firmware boot menu."; }
         }
      }
      bool wantEntry=plan.efi.number && (!device.usb || device.system || !plan.fallback);
      plan.registerBoot=wantEntry && firmwareAvailable();
      if (plan.registerBoot)
      {
         plan.bootOrder=readFirmware("BootOrder");
         if (plan.bootOrder.empty() || plan.bootOrder.size()%2) throw std::runtime_error("The existing UEFI boot order is invalid. Repair it before installing; no boot settings will be changed.");
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
         readEfiLoaders(plan);
         if (!plan.efiBootEntry.empty()) plan.registerBoot=false;
      }
      planBootMenu(plan);
      if (grubBootMenu(plan))
      {
         bool newEfi=plan.bootLayout->efi.bytes!=0;
         plan.bootLayout=std::make_shared<BootLayout>(planBootLayout(*plan.disk,nullptr,plan.bootBytes,newEfi,false));
         if (newEfi) plan.efi=plan.bootLayout->efi;
         plan.bios=bool(plan.biosImage);
      }
      else if (!plan.bios) plan.biosImage.reset();
      if (!plan.bios && !plan.efi.bytes)
         throw std::runtime_error("Slax cannot be made bootable on this target. "+plan.bootWarning+"\n"+efiFailure+" UEFI needs a writable FAT EFI partition or free space for one. Choose another target, free unallocated disk space, or erase the whole disk after backing it up.");
      if (!plan.bios && !plan.fallback && !plan.registerBoot && plan.efiBootEntry.empty() && plan.bootConfig.empty())
         throw std::runtime_error("Slax files can be copied, but this session cannot add a usable boot entry while keeping the existing loader. Start the installer in 64-bit UEFI mode, or prepare a separate disk. Existing boot files will not be replaced.");
      if (wantEntry && !plan.registerBoot && plan.efiBootEntry.empty() && plan.bios && plan.bootConfig.empty()) plan.bootWarning="This session runs without writable UEFI firmware. BIOS boot will be set up; existing UEFI boot settings will stay unchanged.";
      if (!plan.bios) plan.bootWarning+=" BIOS boot is unavailable because this disk has no safe empty embedding area"+std::string(device.sector==4096?" and uses native 4096-byte sectors.":".");
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
   plan.confirmation="Install Slax to "+device.label+" — "+device.name+" ("+sizeText(erase?device.diskBytes:device.bytes)+")\n\n";
   if (erase)
   {
      plan.confirmation+="Everything on this physical disk will be permanently deleted, including these partitions:\n";
      if (device.partitions.empty()) plan.confirmation+="  "+device.path+" — entire disk\n";
      for (const auto &part:device.partitions) plan.confirmation+="  "+part.label+" — "+sizeText(part.bytes)+" · "+(part.filesystem.empty()?"unformatted/unknown":part.filesystem)+"\n";
      plan.confirmation+="\nCreate a large FAT32 partition for the slax folder and a small, separate EFI partition for starting the computer. ";
      if (mbrRequired(device)) plan.confirmation+="Use MBR with the data partition first so this Windows version can still open and use it. ";
      else plan.confirmation+="Use GPT, with reserved BIOS boot space for older computers. ";
   }
   else
   {
      plan.confirmation+="Keep your files and existing partitions. ";
      plan.confirmation+=overwrite?"Replace matching files in the existing slax folder. Keep saved profiles and files not present in the source.\n\n":"Copy Slax into a new slax folder on "+device.label+".\n\n";
      if (!plan.bootConfig.empty()) plan.confirmation+="Add Slax Linux to the existing "+std::string(plan.bootConfig=="boot.ini"?"Windows XP":plan.bootConfig=="Boot/BCD"?"Windows":"GRUB")+" boot menu. Keep the original operating system as the default and preserve its boot code. ";
      else if (plan.bios) plan.confirmation+="Set up BIOS boot (for older computers). "+std::string(plan.previousBios.empty()?"":"Show a text system-selection menu first, with the original system as the default. ");
      if (plan.bios && replaceBios) plan.confirmation+="Any existing BIOS boot loader on this disk will be replaced. ";
      bool newBios=plan.bootLayout->bios.number && std::none_of(device.partitions.begin(),device.partitions.end(),[&](const Partition &part) { return part.start==plan.bootLayout->bios.start; });
      if (newBios) plan.confirmation+="Create a small BIOS boot partition in unused space, keeping the sizes of your existing partitions. ";
      if (plan.bootLayout->efi.bytes) plan.confirmation+="Create a small EFI partition in currently unused space; no existing partition will be resized. ";
      else if (plan.efi.bytes) plan.confirmation+=efi(plan.efi)?"Put signed UEFI boot files in the existing EFI partition, separate from your data. ":"Put signed UEFI boot files on the same FAT filesystem; no separate EFI partition can be created safely. ";
      if (!plan.windowsLoader.empty() && plan.bootConfig.empty()) plan.confirmation+="Keep Windows Boot Manager and show it first in a text system-selection menu. ";
      if (!plan.efiLoaders.empty() && plan.bootConfig.empty()) plan.confirmation+="Retain the other system loaders as choices in this outer menu. ";
      if (plan.registerBoot) plan.confirmation+=plan.efiFirst.empty()?"Add Slax Linux as another UEFI choice, keeping the current firmware default. ":"Start this selector through UEFI, retaining all previous firmware choices. ";
      if (!plan.fallbackDigest.empty()) plan.confirmation+="Keep a backup of the existing EFI fallback boot files. ";
   }
   bool uefi=erase || plan.efi.bytes;
   plan.confirmation+="\n\nSelecting Slax Linux starts its normal logo and countdown; Escape opens the Slax options. ";
   if (uefi) plan.confirmation+="Signed UEFI files support Secure Boot on compatible 64-bit computers. ";
   if (!plan.bios) plan.confirmation+="This installation uses UEFI boot only. ";
   else if (!uefi) plan.confirmation+="This installation uses BIOS boot only; it cannot start with Secure Boot. ";
   if (!plan.bootWarning.empty()) plan.confirmation+="\n\n"+plan.bootWarning;
   return plan;
}

// Read small boot text files from their already-validated ISO extents.
static std::string contents(FILE *source,const ImageEntry &entry)
{
   if (entry.bytes>8*1024*1024) throw std::runtime_error("A boot configuration file is unexpectedly large.");
   std::string text(entry.bytes,'\0'); SourceFile input(source,entry); input.read(text.data(),text.size());
   return text;
}

// Rewrite only the copied ESP boot assets so signed GRUB reads its kernel from FAT, even with NTFS data.
static void replace(std::string &text,const std::string &from,const std::string &to)
{
   size_t at=0;
   while ((at=text.find(from,at))!=std::string::npos) { text.replace(at,from.size(),to); at+=to.size(); }
}

// Keep system selection in a plain outer menu; Slax retains its own countdown and Escape menu.
static std::string menu(const std::string &boot,const std::string &uuid,const InstallPlan &plan,bool uefi=false)
{
   bool previous=uefi?(!plan.windowsLoader.empty() || !plan.efiLoaders.empty()):!plan.previousBios.empty();
   bool native=uefi?grubEfiMenu(plan):!plan.bootConfig.empty();
   if (!previous || native) return "configfile "+boot+"/grub.cfg\n";
   unsigned first=0;
   if (uefi && !plan.efiFirst.empty() && filenameKey(plan.efiFirst)!=filenameKey(plan.windowsLoader))
      for (size_t i=0; i<plan.efiLoaders.size(); i++) if (plan.efiLoaders[i].second==plan.efiFirst) first=i+!plan.windowsLoader.empty();
   std::string text="terminal_output console\nunset theme\nset default="+std::to_string(first)+"\nset timeout=10\nset timeout_style=menu\n";
   if (!plan.windowsLoader.empty()) text+="if [ \"$grub_platform\" = efi ]; then\nmenuentry \"Windows Boot Manager\" {\n   search --no-floppy --fs-uuid --set=windows "+plan.efi.uuid+"\n   chainloader ($windows)"+plan.windowsLoader+"\n}\nfi\n";
   if (!plan.previousBios.empty()) text+="if [ \"$grub_platform\" = pc ]; then\nmenuentry \""+plan.previousBios+"\" {\n   search --no-floppy --fs-uuid --set=root "+uuid+"\n   drivemap -s (hd0) $root\n   chainloader /"+plan.recovery+"/original-mbr.bin\n}\nfi\n";
   for (const auto &loader:plan.efiLoaders)
   {
      auto quote=[](const std::string &value)
      {
         std::string text="\"";
         for (unsigned char c:value) { if (c<32) continue; if (c=='\\' || c=='\"' || c=='$' || c=='`') text+='\\'; text+=c; }
         return text+'\"';
      };
      text+="if [ \"$grub_platform\" = efi ]; then\nmenuentry "+quote(loader.first)+" {\n   search --no-floppy --fs-uuid --set=root "+plan.efi.uuid+"\n   chainloader "+quote(loader.second)+"\n}\nfi\n";
   }
   text+="menuentry \"Slax Linux\" {\n   search --no-floppy --fs-uuid --set=root "+uuid+"\n   configfile "+boot+"/grub.cfg\n   terminal_output console\n}\n";
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
      bool asset=entry.path.find("slax/boot/grub/")==0 && entry.path.find("slax/boot/grub/windows/")!=0 && entry.path.find("slax/boot/grub/net/")!=0 && entry.path.find("slax/boot/grub/i386-pc/")!=0 && entry.path.find("slax/boot/grub/boot-backup/")!=0;
      if (entry.path.find("slax/boot/grub/bios/")==0) asset=false;
      bool kernel=entry.path=="slax/boot/vmlinuz" || entry.path=="slax/boot/initrfs.img";
      bool config=entry.path=="slax/boot/grub.cfg";
      if (asset && (entry.path.find(".img")!=std::string::npos || entry.path.find("usb-space.tar")!=std::string::npos)) continue;
      if (asset || kernel || config)
      {
         Content file={entry,""}; file.entry.path=vendor+"/boot/"+entry.path.substr(10);
         if (config || (asset && entry.path.size()>4 && (entry.path.substr(entry.path.size()-4)==".cfg" || entry.path.substr(entry.path.size()-4)==".txt")))
         {
            auto text=contents(source,entry); replace(text,"/slax/boot","/"+vendor+"/boot");
            if (config) replace(text,"probe --set=bootuuid --fs-uuid ($root)","set bootuuid="+uuid);
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
   files.push_back(generated(vendor+"/boot/grub/menu.cfg",menu("/"+vendor+"/boot",plan.efi.uuid.empty()?uuid:plan.efi.uuid,plan,true)));
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
         FILE *output=tree.open(entry.path,true,entry.mode); created.push_back({entry.path,false});
         try
         {
            Hash expected;
            SourceFile input(source,entry);
            for (uint64_t copied=0; copied<entry.bytes;)
            {
               size_t count=std::min<uint64_t>(sizeof(buffer),entry.bytes-copied);
               if (entry.offset==UINT64_MAX) memcpy(buffer,file.data.data()+copied,count);
               else input.read(buffer,count);
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

// Verify the complete replacement payload before journalling and publishing any existing files.
FileMerge::FileMerge(FileTree &target,std::vector<Content> files,FILE *source,const Progress &progress):tree(target),workspace(".slax-install-"+randomId())
{
   files=ordered(std::move(files)); entries.reserve(files.size());
   for (const auto &file:files)
   {
      const auto &entry=file.entry;
      if (tree.exists(entry.path) && tree.isDirectory(entry.path)!=entry.directory) throw std::runtime_error("A file and directory conflict at the destination: "+entry.path);
      entries.push_back({entry.path,workspace+"/new/"+entry.path,workspace+"/old/"+entry.path,entry.directory});
   }
   tree.mkdir(workspace);
   try
   {
      for (size_t i=0; i<files.size(); i++) files[i].entry.path=entries[i].staged;
      copyTree(tree,ordered(std::move(files)),source,[&](auto text,int percent) { replace(text,workspace+"/new/",""); progress(text,percent); });
      tree.mkdir(workspace+"/old");
      for (const auto &entry:entries) if (entry.directory) tree.mkdir(entry.backup);
      for (auto &entry:entries)
      {
         progress("Updating "+entry.path,75);
         bool exists=tree.exists(entry.path);
         if (exists && tree.isDirectory(entry.path)!=entry.directory) throw std::runtime_error("The destination changed while preparing the installation: "+entry.path);
         if (entry.directory)
         {
            if (!exists) { tree.mkdir(entry.path); entry.created=true; }
            continue;
         }
         // A native rename can succeed before its flush fails; retain the intent for recovery.
         if (exists) { entry.oldMoved=true; tree.move(entry.path,entry.backup); }
         entry.newMoved=true; tree.move(entry.staged,entry.path);
      }
      tree.sync();
   }
   catch (const std::exception &error)
   {
      std::string reason=error.what();
      try { rollback(); }
      catch (const std::exception &recovery) { throw std::runtime_error(reason+"\n"+recovery.what()); }
      throw;
   }
}

// Remove only this operation's recorded staging objects, never recursively walking target content.
void FileMerge::clean()
{
   for (auto entry=entries.rbegin(); entry!=entries.rend(); ++entry)
   {
      tree.remove(entry->staged,entry->directory);
      if (entry->oldMoved || entry->directory) tree.remove(entry->backup,entry->directory);
   }
   tree.remove(workspace+"/new",true); tree.remove(workspace+"/old",true); tree.remove(workspace,true); tree.sync();
   workspace.clear(); entries.clear();
}

// Boot activation has succeeded; the old matching files no longer need recovery copies.
void FileMerge::commit()
{
   if (!workspace.empty()) clean();
}

// Restore every replaced file before discarding backups, including partially completed native renames.
void FileMerge::rollback()
{
   if (workspace.empty()) return;
   std::string failure;
   for (auto entry=entries.rbegin(); entry!=entries.rend(); ++entry)
   {
      try
      {
         if (entry->directory)
         {
            if (entry->created) tree.remove(entry->path,true);
            continue;
         }
         if (entry->newMoved && !tree.exists(entry->staged))
         {
            tree.remove(entry->path,false);
            if (tree.exists(entry->path)) throw std::runtime_error("Cannot remove the replacement file "+entry->path);
         }
         entry->newMoved=false;
         if (entry->oldMoved && tree.exists(entry->backup)) tree.move(entry->backup,entry->path);
         entry->oldMoved=false;
      }
      catch (const std::exception &error) { if (failure.empty()) failure=error.what(); }
   }
   try { tree.sync(); } catch (const std::exception &error) { if (failure.empty()) failure=error.what(); }
   if (!failure.empty()) throw std::runtime_error("Some original files could not be restored. Keep the recovery directory "+workspace+" on this drive. "+failure);
   clean();
}

// Reopen the source for copying and revalidate the approved destination before writing.
void install(const InstallPlan &approved,const Progress &progress)
{
   auto plan=approved;
   plan.recovery="slax/boot/grub/boot-backup"+std::string(plan.overwrite?"/update-"+randomId():"");
   using File=std::unique_ptr<FILE,int (*)(FILE *)>;
   File source(openImage(plan.image.path),fclose);
   if (!source && !std::filesystem::is_directory(std::filesystem::u8path(plan.image.path))) throw std::runtime_error("The selected Slax source cannot be opened. No changes were made.");
   if (partitionSnapshot(*plan.disk,!plan.erase)!=plan.snapshot) throw std::runtime_error("The approved partition table changed. No changes were made.");
   auto reason=basicEligibility(plan.device,plan.image,plan.erase); if (!reason.empty()) throw std::runtime_error(reason);
   progress("Preparing verified boot files…",1);
   std::string vendor=plan.efiVendors.empty()?"EFI/Slax/"+randomId():plan.efiVendors.front();
   Layout layout;
   if (plan.erase) layout=newLayout(plan.device,mbrRequired(plan.device));
   std::string uuid=plan.erase?layout.data.uuid:plan.device.uuid;
   BiosImage bios;
   if (plan.bios) bios=*plan.biosImage;
   std::vector<Content> payload;
   for (const auto &entry:plan.image.entries) payload.push_back({entry,""});
   if (!plan.erase) payload.push_back(generated(plan.recovery,"",true));
   if (plan.bios)
   {
      payload.insert(payload.end(),bios.modules.begin(),bios.modules.end());
      payload.push_back(generated("slax/boot/grub/grub.cfg",plan.biosMenu.empty()?menu("/slax/boot",uuid,plan):plan.biosMenu));
   }
   if (plan.bootLayout && (plan.bios || !plan.bootLayout->patches.empty()))
   {
      std::string record="Slax boot recovery backup. Files contain original bytes from the physical disk.\nDisk: "+plan.device.disk+"\nLogical sector size: "+std::to_string(plan.device.sector)+"\n";
      payload.push_back(generated(plan.recovery+"/original-mbr.bin",plan.bootLayout->originalMbr));
      if (plan.bios)
      {
         payload.push_back(generated(plan.recovery+"/bios-core.bin",plan.bootLayout->biosBefore));
         record+="bios-core.bin: byte offset "+std::to_string(plan.bootLayout->bios.start)+", length "+std::to_string(plan.bootLayout->biosBefore.size())+"\n";
      }
      for (const auto &patch:plan.bootLayout->patches)
      {
         auto name=std::to_string(patch.start)+".bin"; record+=name+": byte offset "+std::to_string(patch.start)+", length "+std::to_string(patch.before.size())+"\n";
         payload.push_back(generated(plan.recovery+"/"+name,patch.before));
      }
      record+="original-mbr.bin: byte offset 0. Restore boot metadata only from a separate recovery session, on this exact disk. Existing partitions must retain their original positions.\n";
      payload.push_back(generated(plan.recovery+"/README.txt",record));
   }
   auto boot=plan.erase || plan.efi.bytes?bootFiles(plan,source.get(),vendor,uuid,plan.fallback):std::vector<Content>{};
   for (const auto &existing:plan.efiVendors) if (existing!=vendor)
   {
      auto files=bootFiles(plan,source.get(),existing,uuid,false); boot.insert(boot.end(),files.begin(),files.end());
   }
   std::string fallbackStage=vendor+"/fallback-"+randomId(),fallbackBackup=vendor+"/original-fallback-"+randomId();
   bool replaceFallback=!plan.fallbackDigest.empty() && plan.efiFallback.empty();
   if (replaceFallback)
      for (auto &file:boot) if (file.entry.path.find("EFI/BOOT")==0) file.entry.path=fallbackStage+file.entry.path.substr(8);
   boot=ordered(std::move(boot));
   bool changed=false,activated=false;
   std::unique_ptr<FileTree> mergedTarget,mergedEfi;
   std::unique_ptr<FileMerge> dataMerge,efiMerge;
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
         progress("Writing BIOS and UEFI boot records…",94);
         if (plan.bios) writeBios(*plan.disk,layout.bios.start,layout.bios.bytes,bios,layout.mbr);
         writeLayout(*plan.disk,layout);
      }
      else
      {
         mergedTarget=std::make_unique<FileTree>(plan.device,true);
         auto &target=*mergedTarget;
         if (target.exists("slax")!=plan.overwrite || target.available()<required(plan.image)) throw std::runtime_error("The destination changed or no longer has enough free space. Review the installation plan again.");
         checkSourceTarget(target,plan.image);
         if (!plan.biosMenu.empty())
         {
            File previous(target.open("slax/boot/grub/grub.cfg",false),fclose);
            std::string text(plan.biosMenu.size()+1,'\0'); text.resize(fread(text.data(),1,text.size(),previous.get()));
            if (ferror(previous.get()) || text!=plan.biosMenu) throw std::runtime_error("The existing BIOS selection menu changed after confirmation. Review the installation plan again.");
         }
         if (plan.overwrite)
         {
            changed=true;
            dataMerge=std::make_unique<FileMerge>(target,std::move(payload),source.get(),[&](const auto &text,int percent) { plan.disk->verify(); progress(text,5+percent); });
         }
         else
         {
            std::string stage=".slax-install-"+randomId();
            for (auto &file:payload) file.entry.path=stage+file.entry.path.substr(4);
            changed=true;
            copyTree(target,ordered(std::move(payload)),source.get(),[&](auto text,int percent) { plan.disk->verify(); replace(text,stage,"slax"); progress(text,5+percent); });
            target.move(stage,"slax"); target.sync();
            mergedTarget.reset();
         }
         if (plan.bios) checkBiosFiles(plan.device,true,*plan.grub);
         if (plan.efi.bytes && !plan.bootLayout->efi.bytes)
         {
            mergedEfi=std::make_unique<FileTree>(plan.efi,true); auto &esp=*mergedEfi;
            if (plan.fallback && !efi(plan.efi) && plan.fallbackDigest.empty() && esp.exists("EFI/BOOT")) throw std::runtime_error("An EFI fallback loader appeared after confirmation. It will not be overwritten.");
            if (!plan.fallbackDigest.empty() && bootDigest(esp,"EFI/BOOT/BOOTX64.EFI")!=plan.fallbackDigest) throw std::runtime_error("The existing EFI fallback changed after confirmation; it will not be replaced.");
            if (plan.fallback && plan.fallbackDigest.empty() && esp.exists("EFI/BOOT")) throw std::runtime_error("An EFI fallback loader appeared after confirmation; it will not be replaced.");
            if (esp.available()<plan.bootBytes) throw std::runtime_error("The EFI filesystem no longer has enough free space. The copied Slax files remain, and the original boot loader has not been replaced.");
            if (!plan.efiVendors.empty())
            {
               efiMerge=std::make_unique<FileMerge>(esp,boot,source.get(),[&](const auto &,int percent) { plan.disk->verify(); progress("Copying UEFI boot files…",80+percent/6); });
               activated=true;
            }
            else copyTree(esp,boot,source.get(),[&](const auto &,int percent) { plan.disk->verify(); progress("Copying UEFI boot files…",80+percent/6); });
            esp.sync();
            if (replaceFallback)
            {
               activated=true; esp.move("EFI/BOOT",fallbackBackup);
               try { esp.move(fallbackStage,"EFI/BOOT"); esp.sync(); }
               catch (...) { esp.move(fallbackBackup,"EFI/BOOT"); esp.sync(); throw; }
            }
            if (!efiMerge) mergedEfi.reset();
         }
         if (plan.bios || !plan.bootLayout->patches.empty())
         {
            progress("Preparing BIOS and EFI boot records…",94);
            plan.disk->claimBoot(plan.bootLayout->ranges);
            if (partitionSnapshot(*plan.disk)!=plan.snapshot) throw std::runtime_error("The partition table changed before boot setup. The original boot code was not replaced.");
            verifyBootLayout(*plan.disk,*plan.bootLayout);
            if (plan.bootLayout->efi.bytes)
            {
               writeFat(*plan.disk,plan.efi,boot,source.get(),[&](const auto &,int percent) { progress("Preparing the new EFI partition…",85+percent/10); });
            }
            commitBootLayout(*plan.disk,*plan.bootLayout,plan.bios && !grubBootMenu(plan)?&bios:nullptr,!plan.bootConfig.empty());
            activated=true;
         }
      }
      plan.disk->verify(); plan.disk->flush();
      if (!plan.bootConfig.empty())
      {
         progress("Adding Slax to the original boot menu…",97);
         activated=true;
         publishBootMenu(plan,vendor);
      }
      if (plan.registerBoot)
      {
         progress("Adding Slax multiboot while preserving existing entries…",97);
         auto active=plan; active.snapshot=partitionSnapshot(*plan.disk);
         activated=true;
         registerFirmware(active,"\\"+vendor+"\\BOOTX64.EFI",plan.recovery+"/boot-order-backup.txt");
      }
      if (efiMerge) efiMerge->commit();
      if (dataMerge) dataMerge->commit();
      efiMerge.reset(); dataMerge.reset(); mergedEfi.reset(); mergedTarget.reset();
      plan.disk->release(); refreshDisk(plan.device); progress("Installation verified and complete.",100);
   }
   catch (const std::exception &error)
   {
      std::string recovery;
      if (!activated)
      {
         try { if (efiMerge) efiMerge->rollback(); } catch (const std::exception &failure) { recovery+="\n"+std::string(failure.what()); }
         try { if (dataMerge) dataMerge->rollback(); } catch (const std::exception &failure) { recovery+="\n"+std::string(failure.what()); }
      }
      efiMerge.reset(); dataMerge.reset(); mergedEfi.reset(); mergedTarget.reset();
      plan.disk->release();
      throw std::runtime_error(std::string(error.what())+recovery+(changed?(plan.erase?"\n\nThe approved disk may already have been repartitioned or partially overwritten. Installation did not finish.":"\n\nSome Slax files or boot settings may already have been written. Installation did not finish."):"\n\nNo installation data was written."));
   }
}
