#include "disk-format.h"
#include "hash.h"
#include "grub-assets.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <fcntl.h>
#include <unistd.h>

extern std::string fixtureRoot;
extern std::map<std::string,std::vector<unsigned char>> firmware;
extern int firmwareFault,writeFault;
extern int fileMoveFault,fileMoveSyncFault,fileSyncFault;
extern int corruptWrite;
extern size_t corruptWriteByte;
extern unsigned writeCalls;
extern bool xpHost;
extern std::string bcdStore,bcdDirty;
extern bool bcdSyncFailure;
static std::vector<Device> hardware;
static Action native=None;
static bool approve=false;
static Installer *dialogWizard=nullptr;

// Stub only GUI and discovery; planning, extraction, FAT, GPT and firmware code stays real.
std::vector<Device> scanDevices(const std::string &) { return hardware; }
std::string chooseImage(bool) { return ""; }
bool confirmInstall(const std::string &text,bool)
{
   assert(text.find("Install Slax")!=std::string::npos);
   if (dialogWizard)
   {
      assert(dialogWizard->page==TargetPage && dialogWizard->confirming && !dialogWizard->busy());
      assert(dialogWizard->heading()=="Choose a target drive" && !dialogWizard->canContinue());
   }
   return approve;
}
static std::string dialogError;
void showInstallError(const std::string &message)
{
   if (dialogWizard)
   {
      assert(dialogWizard->page<=TargetPage && dialogWizard->confirming && !dialogWizard->busy());
      assert(dialogWizard->message().empty() && !dialogWizard->canContinue());
   }
   dialogError=message;
}
void invalidate() {}
void windowAction(Action action) { native=action; }
int runWindow() { return 0; }

// Read independent evidence about preserved operating-system files.
static std::string text(const std::string &path)
{
   std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}

// Write portable little-endian fields in the legacy GPT fixture.
static void putLittle(std::vector<unsigned char> &data,size_t offset,uint64_t value,unsigned bytes)
{
   for (unsigned i=0; i<bytes; i++) data[offset+i]=value>>(i*8);
}

// Create an owned sparse disk with valid redundant GPT and separate mounted-filesystem fixtures.
static Device fixture(unsigned n,bool usb=false,uint32_t sector=512)
{
   Device d; d.disk=fixtureRoot+"/disk"+std::to_string(n)+".img"; d.diskBytes=2ULL*1024*1024*1024; d.bytes=d.diskBytes;
   d.sector=sector;
   d.id=d.diskId="fixture-"+std::to_string(n); d.path=d.disk; d.name="Owned test disk 東京"; d.label="Disk "+std::to_string(n); d.usb=usb;
   int fd=open(d.disk.c_str(),O_RDWR|O_CREAT|O_EXCL,0600); assert(fd>=0 && !ftruncate(fd,d.diskBytes)); close(fd);
   auto layout=newLayout(d);
   assert(!layout.bios.number && layout.efi.number==1 && layout.data.number==2);
   assert(layout.bios.start+layout.bios.bytes==layout.efi.start && disk64(layout.primary.data()+40)*sector==layout.efi.start);
   assert(std::all_of(layout.table.begin()+256,layout.table.end(),[](unsigned char c) { return c==0; }));
   // Preserve-mode fixtures retain an existing BIOS partition inside their older usable range.
   layout.bios.number=3;
   const unsigned char biosType[]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};
   memcpy(layout.table.data()+256,biosType,16); memcpy(layout.table.data()+272,layout.primary.data()+56,16);
   putLittle(layout.table,288,layout.bios.start/sector,8); putLittle(layout.table,296,(layout.bios.start+layout.bios.bytes)/sector-1,8);
   for (auto header:{&layout.primary,&layout.backup})
   {
      putLittle(*header,40,layout.bios.start/sector,8); putLittle(*header,88,crc32(layout.table.data(),layout.table.size()),4);
      putLittle(*header,16,0,4); putLittle(*header,16,crc32(header->data(),92),4);
   }
   Disk disk(d); disk.claim(true); writeLayout(disk,layout);
   std::vector<unsigned char> ntfs(sector); memcpy(ntfs.data()+3,"NTFS    ",8); disk.write(layout.data.start,ntfs.data(),ntfs.size()); disk.release();
   layout.efi.mount=fixtureRoot+"/efi"+std::to_string(n); layout.data.mount=fixtureRoot+"/data"+std::to_string(n);
   std::filesystem::create_directories(layout.efi.mount+"/EFI/Microsoft/Boot"); std::filesystem::create_directory(layout.data.mount);
   std::ofstream(layout.efi.mount+"/EFI/Microsoft/Boot/bootmgfw.efi")<<"Windows boot loader must remain byte-for-byte intact";
   layout.data.label="D:"; layout.data.filesystem="NTFS"; layout.data.uuid="1122334455667788";
   layout.efi.type="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"; layout.efi.filesystem="FAT32";
   d.partitions={layout.bios,layout.efi,layout.data}; d.layout="gpt-owned-fixture";
   static_cast<Partition &>(d)=layout.data; d.path=d.disk+"#"+std::to_string(d.number); d.free=UINT64_MAX;
   return d;
}

// Model a live Windows BIOS disk, optionally with room for an additional EFI partition.
static Device biosFixture(unsigned n,bool freeEfi=false)
{
   auto d=fixture(n); d.number=1; d.start=63*512; d.bytes=d.diskBytes-d.start-(freeEfi?256*1024*1024:0);
   d.partitions={static_cast<const Partition &>(d)}; d.system=true; d.eraseBlocked="The running system is protected";
   std::vector<unsigned char> area(1024*1024); area[0]=0x5a; area[440]=0x71; area[446]=0x80; area[450]=7; area[510]=0x55; area[511]=0xaa;
   for (int b=0; b<4; b++) { area[454+b]=(d.start/512)>>(b*8); area[458+b]=(d.bytes/512)>>(b*8); }
   int fd=open(d.disk.c_str(),O_RDWR); assert(fd>=0 && pwrite(fd,area.data(),area.size(),0)==ssize_t(area.size())); close(fd);
   std::vector<unsigned char> ntfs(512); memcpy(ntfs.data()+3,"NTFS    ",8);
   fd=open(d.disk.c_str(),O_RDWR); assert(fd>=0 && pwrite(fd,ntfs.data(),ntfs.size(),d.start)==ssize_t(ntfs.size())); close(fd);
   std::ofstream(d.mount+"/ntldr")<<"Owned Windows loader sentinel";
   return d;
}

// A rejected operation must provide a useful explanation.
template<class Operation> static std::string fails(Operation operation)
{
   try { operation(); } catch (const std::exception &error) { assert(strlen(error.what())>8); return error.what(); }
   assert(false && "An unsafe operation unexpectedly succeeded"); return "";
}

// Exercise the wizard's real versioned handoff without automating any host disk-writing GUI.
static void deliver(Installer &wizard)
{
   wizard.scanned=hardware; wizard.scannedIso=inspectImage(wizard.iso.path); wizard.scanError.clear(); wizard.scannedPlan.reset();
   if (!wizard.requestId.empty())
   {
      auto found=std::find_if(hardware.begin(),hardware.end(),[&](const Device &d) { return d.id==wizard.requestId; });
      if (found==hardware.end()) wizard.scanError="Target disconnected";
      else try { wizard.scannedPlan=std::make_shared<InstallPlan>(prepareInstall(*found,wizard.scannedIso,wizard.requestErase)); } catch (const std::exception &e) { wizard.scanError=e.what(); }
   }
   wizard.scannedRevision=wizard.revision; wizard.scanReady=true; wizard.tick();
}

// Test destructive paths exclusively on owned sparse files and fake firmware variables.
int main(int argc,char **argv)
{
   assert(argc==4); fixtureRoot=argv[3];
   auto real=inspectImage(argv[1]); assert(real.error.empty() && real.entries.size()>20 && real.payload>0);
   auto image=inspectImage(argv[2]); assert(image.error.empty() && image.entries.size()>6);
   assert(!inspectImage("/missing.iso").error.empty() && !inspectImage(argv[0]).error.empty());
   // Reject short records and missing filename padding before reading their fields or extensions.
   for (unsigned bytes:{1,16,32,35})
   {
      std::vector<unsigned char> malformed(20*2048); size_t descriptor=16*2048,root=18*2048;
      memcpy(malformed.data()+descriptor,"\1CD001\1",7); malformed[descriptor+129]=8; malformed[descriptor+156]=34;
      putLittle(malformed,descriptor+80,20,4); putLittle(malformed,descriptor+158,18,4); putLittle(malformed,descriptor+166,bytes,4);
      malformed[root]=bytes;
      if (bytes==35) { malformed[root+32]=2; malformed[root+33]='a'; malformed[root+34]='b'; }
      auto path=fixtureRoot+"/malformed-directory.iso";
      std::ofstream(path,std::ios::binary).write(reinterpret_cast<const char *>(malformed.data()),malformed.size());
      assert(inspectImage(path).error=="The ISO contains a truncated directory record.");
   }
   // A directory and its parent resolve to the same complete Slax source, including Unicode files.
   auto folder=inspectImage(fixtureRoot+"/tree/slax"),parentFolder=inspectImage(fixtureRoot+"/tree");
   assert(folder.error.empty() && parentFolder.error.empty() && folder.path==parentFolder.path);
   assert(folder.payload==image.payload && folder.entries.size()==image.entries.size());
   std::filesystem::create_directory(fixtureRoot+"/tree/boot"); std::ofstream(fixtureRoot+"/tree/boot/vmlinuz")<<"Another operating system's kernel";
   assert(inspectImage(fixtureRoot+"/tree").path==folder.path);
   std::filesystem::create_directory_symlink(folder.path,fixtureRoot+"/source alias 東京");
   assert(inspectImage(fixtureRoot+"/source alias 東京").path==folder.path);
   std::filesystem::create_directory(fixtureRoot+"/parent alias");
   std::filesystem::create_directory_symlink(folder.path,fixtureRoot+"/parent alias/slax");
   assert(inspectImage(fixtureRoot+"/parent alias").path==folder.path);
   std::filesystem::create_symlink("modules/01-core.sb",folder.path+"/unsafe-link");
   assert(!inspectImage(folder.path).error.empty()); std::filesystem::remove(folder.path+"/unsafe-link");
   // Stage complete updates, preserve unrelated files and sessions, and recover partial native renames.
   Partition mergePartition; mergePartition.mount=fixtureRoot+"/merge";
   std::filesystem::create_directories(mergePartition.mount+"/slax/changes");
   std::ofstream(mergePartition.mount+"/slax/current")<<"original";
   std::ofstream(mergePartition.mount+"/slax/extra")<<"user file";
   std::ofstream(mergePartition.mount+"/slax/changes/session.dat")<<"saved session";
   FileTree mergeTree(mergePartition,true);
   std::vector<Content> update={generated("slax/current","updated"),generated("slax/nested/new","new file")};
   update[0].entry.mode=0755;
   auto unchanged=[&]
   {
      assert(text(mergePartition.mount+"/slax/current")=="original");
      assert(text(mergePartition.mount+"/slax/extra")=="user file" && text(mergePartition.mount+"/slax/changes/session.dat")=="saved session");
      assert(!mergeTree.exists("slax/nested"));
      for (const auto &entry:std::filesystem::directory_iterator(mergePartition.mount)) assert(entry.path().filename()=="slax");
   };
   {
      FileMerge merge(mergeTree,update,nullptr,[&](const auto &status,int) { if (status.find("Copying ")==0) assert(text(mergePartition.mount+"/slax/current")=="original"); });
      assert(text(mergePartition.mount+"/slax/current")=="updated" && text(mergePartition.mount+"/slax/nested/new")=="new file");
      assert((std::filesystem::status(mergePartition.mount+"/slax/current").permissions()&std::filesystem::perms::owner_exec)!=std::filesystem::perms::none);
      merge.rollback(); unchanged();
   }
   for (int fault=0; fault<5; fault++)
   {
      fileMoveFault=fault<2?fault:-1; fileMoveSyncFault=fault==2?0:(fault==3?1:-1); fileSyncFault=fault==4?1:-1;
      assert(fails([&] { FileMerge merge(mergeTree,update,nullptr,[](const auto &,int) {}); }).find("Injected")!=std::string::npos);
      fileMoveFault=fileMoveSyncFault=fileSyncFault=-1; unchanged();
   }
   {
      FileMerge merge(mergeTree,update,nullptr,[](const auto &,int) {}); fileMoveFault=0;
      assert(fails([&] { merge.rollback(); }).find("recovery directory .slax-install-")!=std::string::npos);
      merge.rollback(); unchanged();
   }
   auto unreadable=update; unreadable[1].entry.offset=0; unreadable[1].entry.source=fixtureRoot+"/missing-source-file";
   assert(fails([&] { FileMerge merge(mergeTree,unreadable,nullptr,[](const auto &,int) {}); }).find("source")!=std::string::npos); unchanged();
   assert(fails([&] { FileMerge merge(mergeTree,{generated("slax/current/child","conflict")},nullptr,[](const auto &,int) {}); }).find("conflict")!=std::string::npos); unchanged();
   {
      FileMerge merge(mergeTree,update,nullptr,[](const auto &,int) {}); merge.commit();
      assert(text(mergePartition.mount+"/slax/current")=="updated" && text(mergePartition.mount+"/slax/changes/session.dat")=="saved session");
      assert(text(mergePartition.mount+"/slax/extra")=="user file");
      for (const auto &entry:std::filesystem::directory_iterator(mergePartition.mount)) assert(entry.path().filename()=="slax");
   }
   // ISO and directory sources authenticate identical helpers before any disk access.
   GrubAssets isoGrub(image),folderGrub(folder);
   assert(isoGrub.entries().size()==folderGrub.entries().size());
   assert(isoGrub.tool("grub-mkimage").data==folderGrub.tool("grub-mkimage").data);
   assert(isoGrub.tool("grub-fstest").data==folderGrub.tool("grub-fstest").data);
   auto archivePath=folder.path+"/boot/grub/bios/tools.tar.xz",archive=text(archivePath);
   std::ofstream(archivePath,std::ios::binary).write("!",1);
   assert(fails([&] { GrubAssets damaged(inspectImage(folder.path)); }).find("does not match")!=std::string::npos);
   std::ofstream(archivePath,std::ios::binary).write(archive.data(),archive.size());
   auto corrupt=archive; corrupt[corrupt.size()/2]^=1;
   std::ofstream(archivePath,std::ios::binary).write(corrupt.data(),corrupt.size());
   unsigned writesBeforeArchive=writeCalls;
   Device unopened; unopened.bytes=unopened.diskBytes=2ULL*1024*1024*1024; unopened.sector=512;
   assert(fails([&] { prepareInstall(unopened,inspectImage(folder.path),true); }).find("damaged")!=std::string::npos);
   assert(writeCalls==writesBeforeArchive);
   std::filesystem::remove(archivePath); assert(!inspectImage(folder.path).error.empty());
   std::ofstream(archivePath,std::ios::binary).write(archive.data(),archive.size());
   // Target filtering preserves erase metadata and distinguishes the exact decimal 1 GB boundary.
   Device parent; parent.id="parent"; parent.bytes=parent.diskBytes=10000000000;
   Partition tinyPartition,boundaryPartition,formattedPartition,unformattedPartition;
   tinyPartition.number=1; tinyPartition.bytes=999999999; tinyPartition.label="D:"; tinyPartition.filesystem="FAT32";
   boundaryPartition=tinyPartition; boundaryPartition.number=2; boundaryPartition.bytes=1000000000;
   formattedPartition=tinyPartition; formattedPartition.number=3; formattedPartition.bytes=1000000001; formattedPartition.filesystem="NTFS";
   unformattedPartition.number=4; unformattedPartition.bytes=2000000000;
   parent.partitions={tinyPartition,boundaryPartition,formattedPartition,unformattedPartition};
   auto row=[&](const Partition &part) { Device target=parent; static_cast<Partition &>(target)=part; target.id="part-"+std::to_string(part.number); return target; };
   auto menu=visibleTargets({parent,row(tinyPartition),row(boundaryPartition),row(formattedPartition),row(unformattedPartition)});
   assert(menu.size()==1 && menu[0].number==3 && menu[0].partitions.size()==4);
   parent.partitions={boundaryPartition,unformattedPartition}; menu=visibleTargets({parent,row(boundaryPartition),row(unformattedPartition)});
   assert(menu.size()==1 && menu[0].number==4);
   parent.partitions={tinyPartition,boundaryPartition}; menu=visibleTargets({parent,row(tinyPartition),row(boundaryPartition)});
   assert(menu.size()==1 && !menu[0].number && menu[0].partitions.size()==2);
   parent.partitions.clear(); assert(visibleTargets({parent}).size()==1);
   parent.filesystem="FAT32"; assert(visibleTargets({parent}).size()==1);
   // Linux orders complete paths regardless of discovery order or display labels.
   std::vector<Device> unordered(4,parent);
   const char *paths[]={"/dev/sdb2","/dev/nvme0n1p2","/dev/sdb1","/dev/sda"};
   for (size_t i=0; i<unordered.size(); i++) unordered[i].path=paths[i];
   menu=visibleTargets(unordered);
   assert(menu[0].path=="/dev/nvme0n1p2" && menu[1].path=="/dev/sda" && menu[2].path=="/dev/sdb1" && menu[3].path=="/dev/sdb2");
   parent.partitions={formattedPartition,unformattedPartition}; Installer hidden; hidden.page=TargetPage; hidden.iso=image; hidden.selected="part-4";
   hardware={parent,row(formattedPartition),row(unformattedPartition)}; deliver(hidden);
   assert(hidden.selected.empty() && !hidden.canContinue() && hidden.message().empty()); hardware.clear();
   Hash hash; hash.add("abc",3);
   const unsigned char expected[]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
   assert(!memcmp(hash.finish().data(),expected,32)); assert(crc32("123456789",9)==0xcbf43926);
   firmware={{"BootOrder",{0,0,1,0}},{"Boot0000",{1,2,3,4}},{"Boot0001",{5,6,7,8}}};
   auto windowsEntry=firmware["Boot0000"],otherEntry=firmware["Boot0001"];

   auto disk=fixture(1); assert(basicEligibility(disk,image,false).empty());
   // Exercise both directory-copy and raw FAT writers, retaining original paths for renamed EFI payloads.
   auto beforeFolderFirmware=firmware;
   auto folderTarget=fixture(28); install(prepareInstall(folderTarget,folder,false),[](const auto &,int) {});
   assert(text(folderTarget.mount+"/slax/boot/vmlinuz")==text(folder.path+"/boot/vmlinuz"));
   assert(text(folderTarget.mount+"/slax/modules/module-東京-İstanbul.sb")=="Unicode payload\n");
   assert(text(folderTarget.mount+"/slax/boot/grub/bios/tools.tar.xz")==archive);
   bool copiedKernel=false;
   for (const auto &entry:std::filesystem::recursive_directory_iterator(folderTarget.partitions[1].mount))
   {
      assert(entry.path().filename()!="tools.tar.xz");
      if (entry.path().filename()=="vmlinuz") { assert(text(entry.path())==text(folder.path+"/boot/vmlinuz")); copiedKernel=true; }
   }
   assert(copiedKernel);
   auto folderErase=fixture(29,true); install(prepareInstall(folderErase,parentFolder,true),[](const auto &,int) {});
   firmware=beforeFolderFirmware;
   auto system=disk; system.eraseBlocked="The running system is protected";
   assert(!basicEligibility(system,image,true).empty() && basicEligibility(system,image,false).empty());
   auto encrypted=disk; encrypted.filesystem="BitLocker";
   assert(!basicEligibility(encrypted,image,false).empty() && basicEligibility(encrypted,image,true).empty());
   auto noEfi=disk; noEfi.partitions.clear(); assert(basicEligibility(noEfi,image,false).empty());
   auto biosOnly=prepareInstall(noEfi,image,false); assert(biosOnly.bios && !biosOnly.efi.bytes && !biosOnly.registerBoot);
   auto small=disk; small.bytes=999999999; assert(!basicEligibility(small,image,false).empty());
   auto largeFile=image; largeFile.entries.push_back({"slax/modules/large.sb",0,uint64_t(UINT32_MAX)+1,false});
   assert(basicEligibility(disk,largeFile,true).find("4 GiB")!=std::string::npos && basicEligibility(disk,largeFile,false).empty());

   // Decode an existing same-ESP UEFI Linux entry; unrelated, malformed and option-bearing entries stay in firmware.
   std::filesystem::create_directories(disk.partitions[1].mount+"/EFI/OtherLinux");
   std::ofstream(disk.partitions[1].mount+"/EFI/OtherLinux/shimx64.efi")<<"Owned original Linux loader";
   std::u16string description=u"Linux $test 東京",loader=u"\\EFI\\OtherLinux\\shimx64.efi";
   std::vector<unsigned char> option;
   auto append=[&](uint64_t value,unsigned bytes) { for (unsigned i=0; i<bytes; i++) option.push_back(value>>(i*8)); };
   append(1,4); append(42+4+(loader.size()+1)*2+4,2);
   for (char16_t c:description) append(c,2);
   append(0,2);
   option.insert(option.end(),{4,1,42,0}); append(disk.partitions[1].number,4); append(disk.partitions[1].start/512,8); append(disk.partitions[1].bytes/512,8);
   std::vector<unsigned char> entries(512); { Disk raw(disk); raw.read(1024,entries.data(),entries.size()); }
   auto efiGuid=entries.begin()+(disk.partitions[1].number-1)*128+16;
   option.insert(option.end(),efiGuid,efiGuid+16); option.insert(option.end(),{2,2,4,4}); append(4+(loader.size()+1)*2,2);
   for (char16_t c:loader) append(c,2);
   append(0,2); option.insert(option.end(),{0x7f,0xff,4,0});
   firmware["Boot0005"]=option; firmware["BootOrder"]={0,0,1,0,5,0};
   auto originalLinuxOption=option;
   auto linuxEntry=prepareInstall(disk,image,false); assert(linuxEntry.efiLoaders.size()==1 && linuxEntry.efiLoaders[0].first=="Linux $test 東京");
   assert(linuxEntry.efiLoaders[0].second=="/EFI/OtherLinux/shimx64.efi");
   firmware["BootOrder"]={5,0,0,0,1,0};
   assert(prepareInstall(disk,image,false).efiFirst=="/EFI/OtherLinux/shimx64.efi");
   firmware["Boot0005"].push_back(1); assert(prepareInstall(disk,image,false).efiLoaders.empty());
   assert(prepareInstall(disk,image,false).efiFirst.empty());
   // Windows' standard binary load options do not prevent preserving its default boot path.
   option.clear(); loader=u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
   append(1,4); append(42+4+(loader.size()+1)*2+4,2); append('W',2); append(0,2);
   option.insert(option.end(),{4,1,42,0}); append(disk.partitions[1].number,4); append(disk.partitions[1].start/512,8); append(disk.partitions[1].bytes/512,8);
   option.insert(option.end(),efiGuid,efiGuid+16); option.insert(option.end(),{2,2,4,4}); append(4+(loader.size()+1)*2,2);
   for (char16_t c:loader) append(c,2);
   append(0,2); option.insert(option.end(),{0x7f,0xff,4,0,1,2,3,4}); firmware["Boot0005"]=option;
   assert(prepareInstall(disk,image,false).efiFirst=="/EFI/Microsoft/Boot/bootmgfw.efi");
   firmware.erase("Boot0005"); firmware["BootOrder"]={0,0,1,0};
   firmware.erase("Boot0000"); firmware["BootCurrent"]={2,0}; firmware["BootNext"]={3,0};
   assert(prepareInstall(disk,image,false).bootNumber==4);
   firmware["Boot0000"]=windowsEntry; firmware.erase("BootCurrent"); firmware.erase("BootNext");
   auto pool=disk; pool.partitions[2].filesystem="btrfs_member"; assert(!basicEligibility(pool,image,true).empty());
   auto btrfs=disk; btrfs.filesystem="btrfs"; assert(basicEligibility(btrfs,image,false).find("ISO")!=std::string::npos);
   auto capable=image; capable.filesystems.push_back("btrfs"); assert(basicEligibility(btrfs,capable,false).empty());
   auto duplicate=disk; duplicate.diskId="duplicate-media"; hardware={duplicate};
   assert(fails([&] { prepareInstall(disk,image,false); }).find("same UUID")!=std::string::npos); hardware.clear();
   auto large=fatGeometry(128ULL*1024*1024*1024,512); assert(large.cluster==64 && large.clusters>=65525);
   assert(!fails([] { fatGeometry(3ULL*1024*1024*1024*1024,512); }).empty());
   // Silent corruption in either FAT, either boot sector or FSInfo must stop formatting.
   auto fatTarget=fixture(72,true); auto fatPartition=newLayout(fatTarget).efi;
   auto fatLayout=fatGeometry(fatPartition.bytes,fatTarget.sector);
   int metadataWrites=((32ULL+2ULL*fatLayout.fat)*fatTarget.sector+1048575)/1048576;
   {
      Disk raw(fatTarget); raw.claim(true);
      for (int record=0; record<6; record++)
      {
         corruptWrite=metadataWrites+record; corruptWriteByte=0;
         assert(fails([&] { writeFat(raw,fatPartition,{generated("payload.bin","Verified payload")},nullptr,[](const auto &,int) {}); }).find("FAT32 metadata verification failed")!=std::string::npos);
      }
      // Unallocated FAT entries must be zero too, including bytes outside copied file chains.
      corruptWrite=0; corruptWriteByte=1048575;
      assert(fails([&] { writeFat(raw,fatPartition,{generated("payload.bin","Verified payload")},nullptr,[](const auto &,int) {}); }).find("FAT32 metadata verification failed")!=std::string::npos);
      // Legal long names must stay distinct from synthetic DOS aliases in the same directory.
      writeFat(raw,fatPartition,{generated("F0000002","first file"),generated("other.txt","second file")},nullptr,[](const auto &,int) {});
      std::vector<unsigned char> directory(fatTarget.sector);
      raw.read(fatPartition.start+(32ULL+2ULL*fatLayout.fat)*fatTarget.sector,directory.data(),directory.size());
      std::string firstAlias(reinterpret_cast<const char *>(directory.data()+64),8),secondAlias(reinterpret_cast<const char *>(directory.data()+128),8);
      assert(firstAlias!=secondAlias && firstAlias!="F0000002" && secondAlias!="F0000002");
      for (size_t entry:{0u,64u,128u}) for (size_t offset:{16u,18u,24u})
      {
         uint16_t date=directory[entry+offset]|directory[entry+offset+1]<<8;
         assert((date&31)>=1 && (date&31)<=31 && ((date>>5)&15)>=1 && ((date>>5)&15)<=12);
      }
      raw.release();
   }
   auto cached=fixture(8,true); cached.filesystem="FAT32"; cached.layout="dos"; cached.partitions={static_cast<const Partition &>(cached)};
   assert(prepareInstall(cached,image,false).bios);
   cached.start=1024*1024; cached.bytes=cached.diskBytes-cached.start; cached.number=1; cached.partitions={static_cast<const Partition &>(cached)};
   std::vector<unsigned char> mbr(1024); mbr[450]=0x0c; mbr[510]=0x55; mbr[511]=0xaa;
   for (int b=0; b<4; b++) { mbr[454+b]=(cached.start/512)>>(b*8); mbr[458+b]=(cached.bytes/512)>>(b*8); }
   { Disk physical(cached); physical.claim(true); physical.write(0,mbr.data(),mbr.size()); physical.release(); }
   std::ofstream(cached.mount+"/bootmgr")<<"Existing Windows loader";
   assert(!prepareInstall(cached,image,false).bios); // Occupied embedding bytes remain protected when an OS exists.
   std::filesystem::remove(cached.mount+"/bootmgr");
   auto replaceGap=prepareInstall(cached,image,false);
   assert(replaceGap.bios && replaceGap.confirmation.find("BIOS boot loader on this disk will be replaced")!=std::string::npos);
   mbr.resize(1024*1024);
   { Disk physical(cached); physical.claim(true); physical.write(0,mbr.data(),mbr.size()); physical.release(); }
   assert(prepareInstall(cached,image,false).bios);

   // Newly erased GPT disks reserve BIOS bytes before FirstUsableLBA without a BIOS partition.
   auto beforeReinstallFirmware=firmware;
   auto reinstall=fixture(50,true); auto reservedLayout=newLayout(reinstall);
   reinstall.partitions.erase(reinstall.partitions.begin());
   auto reinstallCore=buildBios(reinstall,true,isoGrub);
   {
      Disk raw(reinstall); raw.claim(true); writeLayout(raw,reservedLayout); raw.release();
   }
   auto emptyReserved=prepareInstall(reinstall,image,false);
   assert(emptyReserved.bios && emptyReserved.bootLayout->bios.start==reservedLayout.bios.start && !emptyReserved.bootLayout->bios.number);
   assert(emptyReserved.bootLayout->patches.empty()); emptyReserved.disk.reset();
   std::vector<unsigned char> reservedBefore(1024*1024),reservedAfter(reservedBefore.size());
   {
      Disk raw(reinstall); raw.claim(true);
      writeBios(raw,reservedLayout.bios.start,reservedLayout.bios.bytes,reinstallCore,reservedLayout.mbr);
      raw.write(0,reservedLayout.mbr.data(),reservedLayout.mbr.size()); raw.read(0,reservedBefore.data(),reservedBefore.size()); raw.release();
   }
   auto repeatPlan=prepareInstall(reinstall,image,false);
   assert(repeatPlan.bios && repeatPlan.previousBios.empty() && repeatPlan.bootLayout->patches.empty());
   assert(repeatPlan.bootLayout->bios.start==reservedLayout.bios.start && !repeatPlan.bootLayout->bios.number);
   assert(std::any_of(repeatPlan.bootLayout->biosBefore.begin(),repeatPlan.bootLayout->biosBefore.end(),[](char c) { return c!=0; }));
   // Reusing an identical core still requires exact approval-time revalidation.
   auto changed=reservedBefore; changed[reservedLayout.bios.start+600]^=1;
   {
      Disk raw(reinstall); raw.claim(true); raw.write(0,changed.data(),changed.size()); raw.release();
   }
   assert(!fails([&] { verifyBootLayout(*repeatPlan.disk,*repeatPlan.bootLayout); }).empty());
   assert(!prepareInstall(reinstall,image,false).bios);
   {
      Disk raw(reinstall); raw.claim(true); raw.write(0,reservedBefore.data(),reservedBefore.size()); raw.release();
   }
   install(repeatPlan,[](const auto &,int) {});
   repeatPlan.disk->read(0,reservedAfter.data(),reservedAfter.size()); assert(reservedAfter==reservedBefore);
   std::filesystem::remove_all(reinstall.mount+"/slax");
   auto repeated=prepareInstall(reinstall,image,false); assert(repeated.bios && repeated.previousBios.empty());
   install(repeated,[](const auto &,int) {});
   repeated.disk->read(0,reservedAfter.data(),reservedAfter.size()); assert(reservedAfter==reservedBefore);
   // Data-only disks can replace an unrelated loader, with a warning and durable original bytes.
   std::filesystem::remove_all(reinstall.mount+"/slax");
   std::filesystem::remove_all(reinstall.partitions[0].mount+"/EFI/Microsoft");
   std::ofstream(reinstall.mount+"/KEEP.txt")<<"Existing user data";
   auto foreign=reservedBefore; memcpy(foreign.data(),"Foreign BIOS loader",19); foreign[reservedLayout.bios.start+600]^=1;
   {
      Disk raw(reinstall); raw.claim(true); raw.write(0,foreign.data(),foreign.size()); raw.release();
   }
   std::filesystem::create_directory(reinstall.mount+"/Windows");
   assert(!prepareInstall(reinstall,image,false).bios);
   std::filesystem::remove(reinstall.mount+"/Windows");
   std::filesystem::create_directories(reinstall.partitions[0].mount+"/EFI/UnknownOS");
   assert(!prepareInstall(reinstall,image,false).bios);
   std::filesystem::remove(reinstall.partitions[0].mount+"/EFI/UnknownOS");
   auto unknownSibling=reinstall; Partition unknown; unknown.number=4; unknown.bytes=1024*1024;
   unknownSibling.partitions.push_back(unknown); assert(!prepareInstall(unknownSibling,image,false).bios);
   auto replacePlan=prepareInstall(reinstall,image,false);
   assert(replacePlan.bios && replacePlan.previousBios.empty() && replacePlan.bootLayout->patches.empty());
   assert(replacePlan.confirmation.find("Any existing BIOS boot loader on this disk will be replaced.")!=std::string::npos);
   // Approval must pin occupied BIOS bytes too, even when replacement is allowed.
   hardware={reinstall}; Installer replacement; replacement.iso=image; replacement.page=TargetPage; replacement.devices=hardware;
   replacement.activate(Select,reinstall.id); approve=true; replacement.activate(Next); deliver(replacement); assert(replacement.pending==Confirm);
   changed=foreign; changed[reservedLayout.bios.start+601]^=1;
   {
      Disk raw(reinstall); raw.claim(true); raw.write(0,changed.data(),changed.size()); raw.release();
   }
   unsigned beforeReplace=writeCalls; deliver(replacement);
   assert(replacement.page==TargetPage && dialogError.find("changed after confirmation")!=std::string::npos && writeCalls==beforeReplace);
   approve=false; hardware.clear();
   {
      Disk raw(reinstall); raw.claim(true); raw.write(0,foreign.data(),foreign.size()); raw.release();
   }
   install(replacePlan,[](const auto &,int) {});
   assert(text(reinstall.mount+"/KEEP.txt")=="Existing user data");
   assert(text(reinstall.mount+"/slax/boot/grub/boot-backup/bios-core.bin")==replacePlan.bootLayout->biosBefore);
   assert(text(reinstall.mount+"/slax/boot/grub/boot-backup/original-mbr.bin")==replacePlan.bootLayout->originalMbr);
   firmware=beforeReinstallFirmware;

   // BIOS-only NTFS installs preserve MBR partition/signature bytes and offer the original loader.
   auto legacy=biosFixture(9); auto legacyPlan=prepareInstall(legacy,image,false);
   assert(legacyPlan.bios && !legacyPlan.efi.bytes && legacyPlan.previousBios=="Windows (BIOS)");
   assert(legacyPlan.bootLayout->bios.start==512 && legacyPlan.bootLayout->biosBefore.size()==4096);
   auto originalMbr=legacyPlan.bootLayout->originalMbr;
   fails([&] { legacyPlan.disk->write(legacy.start,originalMbr.data(),512); });
   install(legacyPlan,[](const auto &,int) {});
   assert(text(legacy.mount+"/slax/boot/grub/boot-backup/original-mbr.bin")==originalMbr);
   assert(text(legacy.mount+"/slax/boot/grub/grub.cfg").find("Windows (BIOS)")!=std::string::npos);
   assert(text(legacy.mount+"/ntldr")=="Owned Windows loader sentinel");
   assert((std::filesystem::status(legacy.mount+"/slax/installer/slax-install-linux").permissions()&std::filesystem::perms::owner_exec)!=std::filesystem::perms::none);
   std::string activeMbr(512,'\0'); legacyPlan.disk->read(0,activeMbr.data(),512);
   assert(activeMbr.substr(440)==originalMbr.substr(440));
   // Reusing Slax's core must not drop the original Windows selector or chainload Slax recursively.
   auto originalSelector=text(legacy.mount+"/slax/boot/grub/grub.cfg");
   auto selectorPlan=prepareInstall(legacy,image,false);
   assert(selectorPlan.overwrite && selectorPlan.bios && selectorPlan.biosMenu==originalSelector && selectorPlan.bootConfig.empty());
   install(selectorPlan,[](const auto &,int) {});
   assert(text(legacy.mount+"/slax/boot/grub/grub.cfg")==originalSelector);
   assert(text(legacy.mount+"/slax/boot/grub/boot-backup/original-mbr.bin")==originalMbr);

   // NTLDR keeps the original boot code and defaults, adding only a verified sector-file entry.
   auto nt=biosFixture(40);
   std::string ini="[boot loader]\r\ntimeout=0\r\ndefault=multi(0)disk(0)rdisk(0)partition(1)\\WINDOWS\r\n[operating systems]\r\nmulti(0)disk(0)rdisk(0)partition(1)\\WINDOWS=\"Windows XP 東京\" /fastdetect\r\n";
   std::ofstream(nt.mount+"/boot.ini",std::ios::binary)<<ini;
   auto ntPlan=prepareInstall(nt,image,false); assert(ntPlan.bootConfig=="boot.ini" && ntPlan.bootFile=="slaxboot.bin");
   auto ntMbr=ntPlan.bootLayout->originalMbr; install(ntPlan,[](const auto &,int) {});
   ntPlan.disk->read(0,activeMbr.data(),512); assert(activeMbr.substr(0,440)==ntMbr.substr(0,440));
   auto ntMenu=text(nt.mount+"/boot.ini"),sectorFile=text(nt.mount+"/slaxboot.bin");
   assert(ntMenu.find("timeout=10\r\n")!=std::string::npos && ntMenu.find("Windows XP 東京")!=std::string::npos);
   assert(ntMenu.find("C:\\slaxboot.bin=\"Slax Linux\"")!=std::string::npos && sectorFile.size()==512);
   assert(disk64(reinterpret_cast<const unsigned char *>(sectorFile.data())+0x5c)==ntPlan.bootLayout->bios.start/512);
   assert(text(nt.mount+"/slax/boot/grub/grub.cfg")=="configfile /slax/boot/grub.cfg\n");
   std::string unicode="\xff\xfe"; for (unsigned char c:ini) { unicode+=char(c); unicode+='\0'; }
   auto wideIni=ntldrMenu(unicode,"slax0001.bin"); assert(wideIni.substr(0,2)==unicode.substr(0,2) && wideIni.size()%2==0);
   assert(!fails([&] { ntldrMenu("[operating systems]\n", "slaxboot.bin"); }).empty());
   // Reinstalling through NTLDR reuses its exact core and sector file without duplicating the menu.
   std::ofstream(nt.mount+"/slax/changes/session.dat")<<"Saved native-menu session";
   for (int update=0; update<2; update++)
   {
      auto repeatNt=prepareInstall(nt,image,false);
      assert(repeatNt.overwrite && repeatNt.bios && repeatNt.bootFile=="slaxboot.bin" && repeatNt.bootFileBefore==sectorFile);
      install(repeatNt,[](const auto &,int) {});
      assert(text(nt.mount+"/boot.ini")==ntMenu && text(nt.mount+"/slaxboot.bin")==sectorFile);
      assert(text(nt.mount+"/slax/changes/session.dat")=="Saved native-menu session");
   }

   // A successful BCDEdit exit is not completion until its volume flush also succeeds.
   bcdStore="Owned existing BCD menu";
   for (bool fail:{false,true})
   {
      auto bcd=biosFixture(fail?71:70);
      std::filesystem::remove(bcd.mount+"/ntldr"); std::filesystem::create_directory(bcd.mount+"/Boot");
      std::ofstream(bcd.mount+"/bootmgr")<<"Owned Windows loader";
      std::ofstream(bcd.mount+"/Boot/BCD")<<bcdStore;
      auto bcdPlan=prepareInstall(bcd,image,false); assert(bcdPlan.bootConfig=="Boot/BCD");
      bcdSyncFailure=fail;
      if (fail) assert(fails([&] { install(bcdPlan,[](const auto &,int) {}); }).find("BCD volume flush failure")!=std::string::npos);
      else { install(bcdPlan,[](const auto &,int) {}); assert(bcdDirty.empty()); }
      assert(std::filesystem::exists(bcd.mount+"/"+bcdPlan.bootFile));
      bcdSyncFailure=false; bcdDirty.clear();
   }
   bcdStore.clear();

   // Localized BCD labels and earlier firmware references must not create a second Slax entry.
   auto repeatBcdDisk=biosFixture(75); std::filesystem::remove(repeatBcdDisk.mount+"/ntldr");
   std::filesystem::create_directory(repeatBcdDisk.mount+"/Boot");
   std::ofstream(repeatBcdDisk.mount+"/bootmgr")<<"Owned Windows loader";
   bcdStore="Owned original BCD"; std::ofstream(repeatBcdDisk.mount+"/Boot/BCD")<<bcdStore;
   auto initialBcd=prepareInstall(repeatBcdDisk,image,false); install(initialBcd,[](const auto &,int) {});
   bcdStore="Firmware\r\nidentifikátor {11111111-1111-1111-1111-111111111111}\r\npořadí {9dea862c-5cdd-4e70-acc1-f32b344d4795}\r\n\r\n"
      "Windows\r\nidentifikátor {9dea862c-5cdd-4e70-acc1-f32b344d4795}\r\npořadí {22222222-2222-2222-2222-222222222222}\r\n\r\n"
      "Slax\r\nidentifikátor {22222222-2222-2222-2222-222222222222}\r\npopis Slax Linux\r\ncesta \\"+initialBcd.bootFile+"\r\n";
   auto nativeBcd=bcdStore,oldSector=text(repeatBcdDisk.mount+"/"+initialBcd.bootFile);
   auto updateBcd=prepareInstall(repeatBcdDisk,image,false);
   assert(updateBcd.overwrite && updateBcd.bios && updateBcd.bootFileBefore==oldSector);
   install(updateBcd,[](const auto &,int) {}); assert(bcdStore==nativeBcd && text(repeatBcdDisk.mount+"/"+initialBcd.bootFile)==oldSector);
   bcdStore.clear();

   // A standard existing GRUB hook needs no MBR/core writes and preserves its original custom entries.
   auto linux=biosFixture(41); { Disk d(linux); d.claimBoot({{0,512}}); d.read(0,activeMbr.data(),512); memcpy(activeMbr.data(),"GRUB",4); d.write(0,activeMbr.data(),512); d.release(); }
   std::filesystem::create_directories(linux.mount+"/boot/grub");
   std::ofstream(linux.mount+"/boot/grub/grub.cfg")<<"set default=0\nsource ${config_directory}/custom.cfg\n";
   std::string custom="menuentry 'Existing Linux' { echo existing; }\n";
   std::ofstream(linux.mount+"/boot/grub/custom.cfg")<<custom;
   auto linuxPlan=prepareInstall(linux,image,false); assert(grubBootMenu(linuxPlan) && linuxPlan.bootLayout->bios.bytes==0);
   unsigned nativeWrites=writeCalls; install(linuxPlan,[](const auto &,int) {}); assert(writeCalls==nativeWrites);
   auto customAfter=text(linux.mount+"/boot/grub/custom.cfg");
   assert(customAfter.find(custom)==0 && customAfter.find("menuentry 'Slax Linux'")!=std::string::npos);
   for (int update=0; update<2; update++)
   {
      auto repeatGrub=prepareInstall(linux,image,false); assert(repeatGrub.overwrite && grubBootMenu(repeatGrub));
      install(repeatGrub,[](const auto &,int) {});
      assert(text(linux.mount+"/boot/grub/custom.cfg")==customAfter);
   }

   // A preserve install can add EFI in empty space without moving the NTFS data partition.
   auto room=biosFixture(10,true); room.usb=true; room.system=false; auto roomPlan=prepareInstall(room,image,false);
   assert(roomPlan.bios && roomPlan.bootLayout->efi.bytes>=128*1024*1024 && roomPlan.efi.number==2);
   assert(roomPlan.efi.start>=room.start+room.bytes && roomPlan.device.start==room.start);
   install(roomPlan,[](const auto &,int) {});
   assert(text(room.mount+"/ntldr")=="Owned Windows loader sentinel");
   assert(!partitionSnapshot(*roomPlan.disk).empty());

   // A failed MBR activation restores the original metadata and keeps a durable recovery backup.
   auto bootFailure=biosFixture(11); auto failedBootPlan=prepareInstall(bootFailure,image,false);
   assert(fails([&] { install(failedBootPlan,[](const auto &message,int) { if (message=="Preparing BIOS and EFI boot records…") writeFault=1; }); }).find("Injected disk write")!=std::string::npos);
   failedBootPlan.disk->read(0,activeMbr.data(),512); assert(activeMbr==failedBootPlan.bootLayout->originalMbr);

   // Occupied old embedding bytes stay intact; a new MBR boot partition uses a zero-filled tail extent.
   auto occupied=biosFixture(12); occupied.bytes-=8*1024*1024; occupied.partitions={static_cast<const Partition &>(occupied)};
   std::vector<unsigned char> oldGap(62*512,0x93),recordMbr(512);
   {
      Disk raw(occupied); raw.claim(false); raw.read(0,recordMbr.data(),512);
      for (int b=0; b<4; b++) recordMbr[458+b]=(occupied.bytes/512)>>(b*8);
      raw.write(0,recordMbr.data(),512); raw.write(512,oldGap.data(),oldGap.size()); raw.release();
   }
   auto occupiedPlan=prepareInstall(occupied,image,false);
   assert(occupiedPlan.bios && occupiedPlan.bootLayout->bios.number==2 && occupiedPlan.bootLayout->bios.start>=occupied.start+occupied.bytes);
   install(occupiedPlan,[](const auto &,int) {});
   std::vector<unsigned char> keptGap(oldGap.size()); occupiedPlan.disk->read(512,keptGap.data(),keptGap.size()); assert(keptGap==oldGap);
   occupiedPlan.disk->read(0,recordMbr.data(),512); assert(recordMbr[466]==0xda && recordMbr[462]==0);

   // An unlocked Windows volume can still be encrypted on disk and unreadable to Slax.
   auto unlocked=fixture(13);
   {
      Disk raw(unlocked); raw.claim(false); std::vector<unsigned char> boot(512); memcpy(boot.data()+3,"-FVE-FS-",8);
      raw.write(unlocked.start,boot.data(),boot.size()); raw.release();
   }
   assert(fails([&] { prepareInstall(unlocked,image,false); }).find("encrypted")!=std::string::npos);

   auto unsupported=fixture(14); unsupported.blocked="Owned read-only fixture"; hardware={unsupported};
   Installer modal; modal.iso=image; modal.page=TargetPage; modal.devices=hardware; modal.activate(Select,unsupported.id);
   dialogWizard=&modal;
   assert(modal.canContinue() && modal.message().empty()); modal.activate(Next); unsigned modalWrites=writeCalls;
   assert(modal.page==TargetPage && modal.message().empty() && modal.progressText()=="Preparing installation plan…" && !modal.indeterminate()); deliver(modal);
   assert(modal.page==TargetPage && dialogError==unsupported.blocked && modal.message().empty() && modal.error.empty() && writeCalls==modalWrites);
   dialogWizard=nullptr; hardware.clear();

   // A whole-disk row can disappear during discovery without invalidating its inventory object.
   auto hiddenWhole=fixture(73,true); static_cast<Partition &>(hiddenWhole)=Partition{};
   hiddenWhole.path=hiddenWhole.disk; hiddenWhole.bytes=hiddenWhole.diskBytes; hiddenWhole.id+="-whole";
   auto initiallyBare=hiddenWhole; initiallyBare.partitions.clear();
   Installer lostSelection; lostSelection.iso=image; lostSelection.page=TargetPage; lostSelection.devices={initiallyBare};
   lostSelection.activate(Select,initiallyBare.id); lostSelection.activate(Erase); lostSelection.activate(Next);
   hardware={hiddenWhole}; dialogWizard=&lostSelection; dialogError.clear(); unsigned beforeHidden=writeCalls;
   deliver(lostSelection);
   assert(lostSelection.page==TargetPage && lostSelection.selected.empty() && dialogError.find("Select the target again")!=std::string::npos && writeCalls==beforeHidden);
   dialogWizard=nullptr; hardware.clear();

   // Whole-disk selection is derived from the latest click and restores a single row when disabled.
   Installer selection; selection.page=TargetPage; selection.iso=image;
   auto sibling=legacy; sibling.id="sibling"; auto other=legacy; other.id=other.diskId="another-disk";
   selection.devices={legacy,sibling,other}; selection.activate(Erase);
   assert(!selection.isSelected(legacy) && !selection.isSelected(sibling));
   selection.activate(Select,sibling.id); assert(selection.isSelected(legacy) && selection.isSelected(sibling) && !selection.isSelected(other));
   selection.activate(Erase); assert(!selection.isSelected(legacy) && selection.isSelected(sibling));
   selection.activate(Erase); selection.activate(Select,other.id); assert(!selection.isSelected(sibling) && selection.isSelected(other));
   selection.activate(Erase); assert(selection.selected==other.id && selection.isSelected(other) && selection.message().empty());

   auto plan=prepareInstall(disk,image,false); assert(plan.registerBoot && !plan.windowsLoader.empty());
   auto original=text(disk.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi");
   int percent=0;
   install(plan,[&](const std::string &status,int progress) { assert(!status.empty() && progress>=percent && progress<=100); percent=progress; });
   assert(percent==100 && std::filesystem::exists(disk.mount+"/slax/boot/vmlinuz"));
   assert(text(disk.mount+"/slax/boot/grub.cfg").find("Windows Boot Manager")==std::string::npos);
   assert(text(disk.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi")==original);
   assert(firmware["Boot0000"]==windowsEntry && firmware["Boot0001"]==otherEntry);
   assert(plan.efiFirst.empty() && firmware["BootOrder"]==std::vector<unsigned char>({0,0,1,0,2,0}));
   // Refresh every existing Slax EFI vendor without duplicating firmware entries or losing sessions.
   auto installedFirmware=firmware;
   std::ofstream(disk.mount+"/slax/changes/session.dat")<<"Saved EFI session";
   std::ofstream(disk.mount+"/slax/modules/user.sb")<<"User module";
   auto originalBackup=text(disk.mount+"/slax/boot/grub/boot-backup/original-mbr.bin");
   for (int update=0; update<2; update++)
   {
      auto kernel="Updated kernel "+std::to_string(update),initrd="Matching initramfs "+std::to_string(update);
      std::ofstream(folder.path+"/boot/vmlinuz")<<kernel; std::ofstream(folder.path+"/boot/initrfs.img")<<initrd;
      auto updated=inspectImage(folder.path); assert(updated.error.empty());
      auto repeatEfi=prepareInstall(disk,updated,false);
      assert(repeatEfi.overwrite && repeatEfi.bios && !repeatEfi.registerBoot && repeatEfi.efiBootEntry=="Boot0002" && repeatEfi.efiVendors.size()==1);
      install(repeatEfi,[](const auto &,int) {});
      assert(firmware==installedFirmware);
      assert(text(disk.mount+"/slax/boot/vmlinuz")==kernel && text(disk.mount+"/slax/boot/initrfs.img")==initrd);
      assert(text(disk.partitions[1].mount+"/"+repeatEfi.efiVendors[0]+"/boot/vmlinuz")==kernel);
      assert(text(disk.partitions[1].mount+"/"+repeatEfi.efiVendors[0]+"/boot/initrfs.img")==initrd);
      assert(text(disk.mount+"/slax/changes/session.dat")=="Saved EFI session" && text(disk.mount+"/slax/modules/user.sb")=="User module");
      assert(text(disk.mount+"/slax/boot/grub/boot-backup/original-mbr.bin")==originalBackup);
      assert(std::distance(std::filesystem::directory_iterator(disk.partitions[1].mount+"/EFI/Slax"),std::filesystem::directory_iterator())==1);
   }
   // A Slax wrapper at the front must retain the original Linux default behind it.
   firmware["Boot0005"]=originalLinuxOption; firmware["BootOrder"]={2,0,5,0,0,0,1,0};
   auto selectorFirmware=firmware; auto selectorUpdate=prepareInstall(disk,inspectImage(folder.path),false);
   assert(!selectorUpdate.registerBoot && selectorUpdate.efiFirst=="/EFI/OtherLinux/shimx64.efi");
   install(selectorUpdate,[](const auto &,int) {}); assert(firmware==selectorFirmware);
   assert(text(disk.partitions[1].mount+"/"+selectorUpdate.efiVendors[0]+"/boot/grub/menu.cfg").find("set default=1\n")!=std::string::npos);
   firmware=installedFirmware;

   // A removable Slax fallback keeps its reachable vendor current without adding firmware entries.
   auto removable=fixture(74,true); std::filesystem::remove_all(removable.partitions[1].mount+"/EFI/Microsoft");
   auto removablePlan=prepareInstall(removable,image,false); assert(removablePlan.fallback && !removablePlan.registerBoot);
   install(removablePlan,[](const auto &,int) {});
   std::ofstream(removable.partitions[1].mount+"/EFI/BOOT/user.txt")<<"User EFI file";
   for (int update=0; update<2; update++)
   {
      auto repeatUsb=prepareInstall(removable,inspectImage(folder.path),false);
      assert(repeatUsb.overwrite && repeatUsb.fallback && !repeatUsb.registerBoot && !repeatUsb.efiFallback.empty() && !repeatUsb.fallbackDigest.empty());
      install(repeatUsb,[](const auto &,int) {}); assert(firmware==installedFirmware);
      auto prefix=removable.partitions[1].mount+"/"+repeatUsb.efiFallback;
      assert(text(prefix+"/boot/vmlinuz")==text(folder.path+"/boot/vmlinuz"));
      assert(text(prefix+"/boot/initrfs.img")==text(folder.path+"/boot/initrfs.img"));
      assert(text(removable.partitions[1].mount+"/EFI/BOOT/grub.cfg")==text(prefix+"/grub.cfg"));
      assert(text(removable.partitions[1].mount+"/EFI/BOOT/user.txt")=="User EFI file");
   }
   // Matching leftover configuration must not authorize replacement of an unrelated EFI loader.
   auto fallbackPath=removable.partitions[1].mount+"/EFI/BOOT/BOOTX64.EFI",fallbackLoader=text(fallbackPath);
   std::ofstream(fallbackPath)<<"A different vendor's fallback loader";
   auto foreignFallback=prepareInstall(removable,inspectImage(folder.path),false);
   assert(foreignFallback.efiFallback.empty() && !foreignFallback.fallback && text(fallbackPath)=="A different vendor's fallback loader");
   std::ofstream(fallbackPath)<<fallbackLoader;

   // XP cannot mount a removable disk's second ESP; its exact Slax MBR/core still remains reusable.
   auto xpRepeat=fixture(76,true); auto xpLayout=newLayout(xpRepeat,true); auto xpMount=xpRepeat.mount;
   xpLayout.data.mount=xpMount; xpLayout.data.filesystem="FAT32";
   xpRepeat.partitions={xpLayout.data,xpLayout.efi}; xpRepeat.partitions[1].filesystem.clear();
   static_cast<Partition &>(xpRepeat)=xpLayout.data; xpRepeat.path=xpRepeat.disk+"#1"; xpRepeat.free=UINT64_MAX;
   std::filesystem::copy(std::filesystem::u8path(folder.path),std::filesystem::u8path(xpMount+"/slax"),std::filesystem::copy_options::recursive);
   // An older or modified ordinary bootstrap is overwritten without having to match the new source.
   assert(text(xpMount+"/slax/boot/grub/grub.cfg").find("set prefix=")!=std::string::npos);
   std::ofstream(xpMount+"/slax/boot/grub/grub.cfg",std::ios::app)<<"# Older local boot configuration\nset old_setting=yes\n";
   auto xpCore=buildBios(xpRepeat,false,isoGrub); std::vector<unsigned char> xpBefore(1024*1024),xpAfter(xpBefore.size());
   {
      Disk raw(xpRepeat); raw.claim(true); writeBios(raw,xpLayout.bios.start,xpLayout.bios.bytes,xpCore,xpLayout.mbr); writeLayout(raw,xpLayout);
      raw.read(0,xpBefore.data(),xpBefore.size()); raw.release();
   }
   for (int update=0; update<2; update++)
   {
      auto repeat=prepareInstall(xpRepeat,image,false);
      assert(repeat.overwrite && repeat.bios && repeat.biosMenu.empty() && repeat.bootLayout->bios.start==xpLayout.bios.start && !repeat.bootLayout->biosBefore.empty());
      install(repeat,[](const auto &,int) {});
      assert(text(xpMount+"/slax/boot/grub/grub.cfg")=="configfile /slax/boot/grub.cfg\n");
      repeat.disk->read(0,xpAfter.data(),xpAfter.size()); assert(xpAfter==xpBefore);
   }

   auto rollback=fixture(2); auto order=firmware["BootOrder"]; auto rollbackPlan=prepareInstall(rollback,image,false); firmwareFault=1;
   assert(fails([&] { install(rollbackPlan,[](const auto &,int) {}); }).find("firmware failure")!=std::string::npos);
   assert(firmware["BootOrder"]==order && firmware["Boot0000"]==windowsEntry);
   assert(text(rollback.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi")==original);

   auto blank=fixture(3,true); auto erase=prepareInstall(blank,image,true); assert(erase.confirmation.find("D:")!=std::string::npos && erase.bios);
   auto unavailable=erase; unavailable.image.path=fixtureRoot+"/missing-source.iso"; unsigned before=writeCalls;
   assert(fails([&] { install(unavailable,[](const auto &,int) {}); }).find("No changes")!=std::string::npos && writeCalls==before);
   install(erase,[](const auto &,int) {}); assert(!partitionSnapshot(*erase.disk).empty());
   auto bad=fixture(4,true); auto badPlan=prepareInstall(bad,image,true); writeFault=1;
   assert(fails([&] { install(badPlan,[](const auto &,int) {}); }).find("did not finish")!=std::string::npos); writeFault=-1;
   auto native4k=fixture(6,true,4096); auto plan4k=prepareInstall(native4k,image,true); assert(!plan4k.bios);
   install(plan4k,[](const auto &,int) {}); assert(!partitionSnapshot(*plan4k.disk).empty());

   // XP converts an existing GPT fixture to MBR while keeping its data partition first.
   auto xp=fixture(7,true); xpHost=true; auto xpPlan=prepareInstall(xp,image,true);
   assert(xpPlan.bios && xpPlan.confirmation.find("Use MBR")!=std::string::npos && xpPlan.confirmation.find("data partition first")!=std::string::npos);
   auto huge=xp; huge.diskBytes=(uint64_t(UINT32_MAX)+2)*512;
   assert(basicEligibility(huge,image,true).find("2 TiB")!=std::string::npos);
   assert(basicEligibility(native4k,image,true).find("512-byte")!=std::string::npos);
   assert(fails([&] { newLayout(huge,true); }).find("2 TiB")!=std::string::npos);
   assert(fails([&] { newLayout(native4k,true); }).find("512-byte")!=std::string::npos);
   auto limit=xp; limit.diskBytes=(uint64_t(UINT32_MAX)+1)*512;
   auto maximum=newLayout(limit,true); fatGeometry(maximum.data.bytes,512);
   assert(maximum.efi.start+maximum.efi.bytes+33*512==limit.diskBytes);
   install(xpPlan,[](const auto &,int) {});
   std::vector<unsigned char> record(512),empty(512),core(512);
   xpPlan.disk->read(0,record.data(),record.size());
   assert(record[510]==0x55 && record[511]==0xaa && disk32(record.data()+440)!=0);
   assert(record[446]==0x80 && record[450]==0x0c && record[462]==0 && record[466]==0xef);
   uint64_t dataStart=disk32(record.data()+454),dataCount=disk32(record.data()+458),efiStart=disk32(record.data()+470),efiCount=disk32(record.data()+474);
   assert(dataStart==2048 && dataStart+dataCount==efiStart && dataCount>efiCount && efiCount*512>=256*1024*1024);
   assert((efiStart+efiCount+33)*512==xp.diskBytes && disk64(record.data()+0x5c)==34);
   xpPlan.disk->read(512,record.data(),record.size()); assert(record==empty);
   xpPlan.disk->read(xp.diskBytes-512,record.data(),record.size()); assert(record==empty);
   xpPlan.disk->read(34*512,core.data(),core.size()); assert(core!=empty);
   assert(!partitionSnapshot(*xpPlan.disk).empty()); xpHost=false;
   assert(newLayout(xp).primary.size()==512 && prepareInstall(xp,image,true).confirmation.find("Use GPT")!=std::string::npos);

   // Background validation must never dismiss Welcome, show errors over it or start writes.
   Installer welcome; before=writeCalls; dialogError.clear();
   assert(welcome.page==WelcomePage && welcome.canContinue() && welcome.heading()=="Welcome to Slax Installer");
   welcome.setImagePath(image.path,true); assert(welcome.canContinue() && !welcome.busy()); deliver(welcome);
   assert(welcome.page==WelcomePage && welcome.message().empty() && dialogError.empty() && writeCalls==before);
   welcome.activate(Next); assert(welcome.page==SourcePage && !welcome.message().empty());
   welcome.activate(Back); assert(welcome.page==WelcomePage && welcome.iso.path==image.path);
   welcome.activate(Next); welcome.activate(Next); welcome.activate(Back); deliver(welcome);
   assert(welcome.page==WelcomePage && welcome.pending==None && writeCalls==before);
   welcome.activate(Next); assert(welcome.page==SourcePage && welcome.canContinue());
   Installer missingWelcome; missingWelcome.setImagePath(fixtureRoot+"/missing-welcome.iso",true); deliver(missingWelcome);
   assert(missingWelcome.page==WelcomePage && missingWelcome.canContinue() && dialogError.empty());

   // Automatic validation fills source metadata but only an explicit Next advances.
   Installer automatic; automatic.activate(Next); before=writeCalls; automatic.setImagePath(image.path,true);
   assert(automatic.pending==CheckSource && automatic.source==image.path && !automatic.canContinue() && automatic.busy());
   assert(automatic.message().empty() && automatic.progressText()=="Checking source…");
   deliver(automatic);
   assert(automatic.page==SourcePage && !automatic.iso.entries.empty() && automatic.canContinue() && !automatic.message().empty());
   assert(automatic.selected.empty() && !automatic.plan && writeCalls==before && !automatic.busy());
   automatic.activate(Next); assert(automatic.page==SourcePage && automatic.pending==Next); deliver(automatic);
   assert(automatic.page==TargetPage && automatic.pending==None && writeCalls==before);
   automatic.activate(Back); deliver(automatic);
   assert(automatic.page==SourcePage && automatic.pending==None && automatic.source.empty() && automatic.iso.path==image.path);
   Installer invalidSource; invalidSource.activate(Next); dialogWizard=&invalidSource; dialogError.clear();
   invalidSource.setImagePath(fixtureRoot+"/missing-source.iso",true); deliver(invalidSource);
   assert(invalidSource.page==SourcePage && !invalidSource.iso.error.empty() && invalidSource.error.empty() && invalidSource.pending==None && writeCalls==before);
   assert(dialogError.empty()); invalidSource.activate(Next); deliver(invalidSource);
   assert(dialogError.find(invalidSource.iso.error)!=std::string::npos && invalidSource.message().empty());
   dialogError.clear(); deliver(invalidSource); assert(dialogError.empty()); dialogWizard=nullptr;
   Installer noSource; noSource.activate(Next); noSource.setImagePath("",true); assert(noSource.page==SourcePage && noSource.pending==None && !noSource.canContinue());

   // Edits schedule checks, keep Next disabled and silently discard obsolete results.
   Installer input; input.activate(Next); input.setImagePath(fixtureRoot+"/missing-東京-İstanbul.iso");
   assert(input.pending==CheckSource && input.message().empty() && !input.canContinue());
   assert(input.sourceCheckAfter>std::chrono::steady_clock::now());
   dialogWizard=&input; dialogError.clear(); input.activate(Next); assert(input.pending==CheckSource); deliver(input);
   assert(input.page==SourcePage && !input.iso.error.empty() && input.error.empty() && input.source.empty() && input.canContinue());
   assert(input.message().empty() && dialogError.empty());
   dialogError.clear(); input.activate(Next); deliver(input);
   assert(input.page==SourcePage && !dialogError.empty() && input.message().empty() && writeCalls==before);
   dialogWizard=nullptr;
   input.setImagePath(fixtureRoot+"/old-source.iso"); auto obsolete=input.revision;
   input.setImagePath(image.path,true); assert(input.revision>obsolete && input.pending==CheckSource && !input.canContinue());
   input.scannedIso=image; input.scannedRevision=obsolete; input.scanReady=true; input.tick();
   assert(input.pending==CheckSource && input.message().empty() && !input.canContinue());
   deliver(input); assert(!input.message().empty() && input.canContinue() && input.page==SourcePage);
   input.setImagePath("\""+image.path+"\""); assert(input.error.empty() && input.message().empty() && !input.canContinue()); deliver(input);
   input.activate(Next); deliver(input); assert(input.page==TargetPage && input.iso.path==image.path && input.iso.error.empty());
   input.activate(Back); input.setImagePath(fixtureRoot+"/different.iso");
   assert(input.page==SourcePage && input.iso.entries.empty() && input.message().empty() && input.pending==CheckSource && !input.canContinue());
   input.setImagePath(""); assert(input.pending==None && input.source.empty() && !input.canContinue());

   auto drive=fixture(5,true); hardware={drive}; Installer wizard; wizard.activate(Next); wizard.iso=image;
   dialogWizard=&wizard;
   wizard.activate(Next); wizard.scannedRevision=wizard.revision-1; wizard.scanReady=true; wizard.tick(); assert(wizard.page==SourcePage);
   deliver(wizard); assert(wizard.page==TargetPage && wizard.pending==None); wizard.activate(Select,drive.id); wizard.activate(Erase);
   wizard.activate(Next); assert(wizard.page==TargetPage && !wizard.indeterminate() && wizard.message().empty() && wizard.progressText()=="Preparing installation plan…" && wizard.busy());
   assert(wizard.heading()=="Choose a target drive" && !wizard.canContinue());
   before=writeCalls; deliver(wizard); assert(wizard.page==TargetPage && !wizard.plan && writeCalls==before && !wizard.busy());
   approve=true; wizard.activate(Next); deliver(wizard); assert(wizard.pending==Confirm);
   hardware.clear(); deliver(wizard); assert(wizard.page==TargetPage && !dialogError.empty() && wizard.message().empty() && writeCalls==before);
   hardware={drive}; deliver(wizard); wizard.activate(Select,drive.id); wizard.activate(Next); deliver(wizard);
   assert(wizard.page==TargetPage && wizard.pending==Confirm && wizard.message().empty() && wizard.progressText()=="Rechecking plan…" && writeCalls==before);
   // A slow scanner must not block the GUI handoff or allow writes before it finishes.
   std::atomic<bool> releaseScan{false},scanFinished{false};
   wizard.scanner=std::thread([&]
   {
      for (int i=0; i<200 && !releaseScan; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      scanFinished=true;
   });
   deliver(wizard); assert(!scanFinished && writeCalls==before); releaseScan=true;
   if (wizard.page!=InstallationPage) fprintf(stderr,"Wizard page %d, pending %d: %s\n",wizard.page,int(wizard.pending),wizard.error.c_str());
   assert(wizard.page==InstallationPage && wizard.busy());
   wizard.activate(Close); assert(native==None);
   for (int i=0; i<1000 && wizard.page==InstallationPage; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); wizard.tick(); }
   wizard.stop(); assert(wizard.page==FinishPage && wizard.error.empty() && wizard.percent==100 && !wizard.busy()); wizard.activate(Next); assert(native==Close);
   dialogWizard=nullptr;
   puts("Installer: ISO, SHA-256, GPT/FAT32 writes, existing files, Windows multiboot, firmware rollback, disconnection and confirmation checks passed.");
}
