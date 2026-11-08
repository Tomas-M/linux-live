#include "disk-format.h"
#include "hash.h"
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
extern unsigned writeCalls;
extern bool xpHost;
static std::vector<Device> hardware;
static Action native=None;
static bool approve=false;

// Stub only GUI and discovery; planning, extraction, FAT, GPT and firmware code stays real.
std::vector<Device> scanDevices(const std::string &) { return hardware; }
std::string chooseImage() { return ""; }
bool confirmInstall(const std::string &text,bool) { assert(text.find("Install Slax")!=std::string::npos); return approve; }
static std::string dialogError;
void showInstallError(const std::string &message) { dialogError=message; }
void invalidate() {}
void windowAction(Action action) { native=action; }
int runWindow() { return 0; }

// Read independent evidence about preserved operating-system files.
static std::string text(const std::string &path)
{
   std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}

// Create an owned sparse disk with valid redundant GPT and separate mounted-filesystem fixtures.
static Device fixture(unsigned n,bool usb=false,uint32_t sector=512)
{
   Device d; d.disk=fixtureRoot+"/disk"+std::to_string(n)+".img"; d.diskBytes=2ULL*1024*1024*1024; d.bytes=d.diskBytes;
   d.sector=sector;
   d.id=d.diskId="fixture-"+std::to_string(n); d.path=d.disk; d.name="Owned test disk 東京"; d.label="Disk "+std::to_string(n); d.usb=usb;
   int fd=open(d.disk.c_str(),O_RDWR|O_CREAT|O_EXCL,0600); assert(fd>=0 && !ftruncate(fd,d.diskBytes)); close(fd);
   auto layout=newLayout(d); Disk disk(d); disk.claim(true); writeLayout(disk,layout);
   std::vector<unsigned char> ntfs(sector); memcpy(ntfs.data()+3,"NTFS    ",8); disk.write(layout.data.start,ntfs.data(),ntfs.size()); disk.release();
   layout.efi.mount=fixtureRoot+"/efi"+std::to_string(n); layout.data.mount=fixtureRoot+"/data"+std::to_string(n);
   std::filesystem::create_directories(layout.efi.mount+"/EFI/Microsoft/Boot"); std::filesystem::create_directory(layout.data.mount);
   std::ofstream(layout.efi.mount+"/EFI/Microsoft/Boot/bootmgfw.efi")<<"Windows boot loader must remain byte-for-byte intact";
   layout.data.label="D:"; layout.data.filesystem="NTFS"; layout.data.uuid="1122334455667788";
   layout.efi.type="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"; layout.efi.filesystem="FAT32";
   d.partitions={layout.bios,layout.efi,layout.data}; d.layout="gpt-owned-fixture";
   static_cast<Partition &>(d)=layout.data; d.path=d.disk+"#3"; d.free=UINT64_MAX;
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
   parent.partitions={formattedPartition,unformattedPartition}; Installer hidden; hidden.page=1; hidden.iso=image; hidden.selected="part-4";
   hardware={parent,row(formattedPartition),row(unformattedPartition)}; deliver(hidden);
   assert(hidden.selected.empty() && !hidden.canContinue() && hidden.message().empty()); hardware.clear();
   Hash hash; hash.add("abc",3);
   const unsigned char expected[]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
   assert(!memcmp(hash.finish().data(),expected,32)); assert(crc32("123456789",9)==0xcbf43926);
   firmware={{"BootOrder",{0,0,1,0}},{"Boot0000",{1,2,3,4}},{"Boot0001",{5,6,7,8}}};
   auto windowsEntry=firmware["Boot0000"],otherEntry=firmware["Boot0001"];

   auto disk=fixture(1); assert(basicEligibility(disk,image,false).empty());
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
   option.insert(option.end(),entries.begin()+128+16,entries.begin()+128+32); option.insert(option.end(),{2,2,4,4}); append(4+(loader.size()+1)*2,2);
   for (char16_t c:loader) append(c,2);
   append(0,2); option.insert(option.end(),{0x7f,0xff,4,0});
   firmware["Boot0005"]=option; firmware["BootOrder"]={0,0,1,0,5,0};
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
   option.insert(option.end(),entries.begin()+128+16,entries.begin()+128+32); option.insert(option.end(),{2,2,4,4}); append(4+(loader.size()+1)*2,2);
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
   auto cached=fixture(8,true); cached.filesystem="FAT32"; cached.layout="dos"; cached.partitions={static_cast<const Partition &>(cached)};
   assert(prepareInstall(cached,image,false).bios);
   cached.start=1024*1024; cached.bytes=cached.diskBytes-cached.start; cached.number=1; cached.partitions={static_cast<const Partition &>(cached)};
   std::vector<unsigned char> mbr(1024); mbr[450]=0x0c; mbr[510]=0x55; mbr[511]=0xaa;
   for (int b=0; b<4; b++) { mbr[454+b]=(cached.start/512)>>(b*8); mbr[458+b]=(cached.bytes/512)>>(b*8); }
   { Disk physical(cached); physical.claim(true); physical.write(0,mbr.data(),mbr.size()); physical.release(); }
   assert(!prepareInstall(cached,image,false).bios); // Stale nonzero boot-gap metadata is never overwritten.
   mbr.resize(1024*1024);
   { Disk physical(cached); physical.claim(true); physical.write(0,mbr.data(),mbr.size()); physical.release(); }
   assert(prepareInstall(cached,image,false).bios);

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
   assert((std::filesystem::status(legacy.mount+"/slax/slax-install-linux").permissions()&std::filesystem::perms::owner_exec)!=std::filesystem::perms::none);
   std::string activeMbr(512,'\0'); legacyPlan.disk->read(0,activeMbr.data(),512);
   assert(activeMbr.substr(440)==originalMbr.substr(440));

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
   Installer modal; modal.iso=image; modal.page=1; modal.devices=hardware; modal.activate(Select,unsupported.id);
   assert(modal.canContinue() && modal.message().empty()); modal.activate(Next); unsigned modalWrites=writeCalls; deliver(modal);
   assert(dialogError==unsupported.blocked && modal.message().empty() && modal.error.empty() && writeCalls==modalWrites); hardware.clear();

   // Whole-disk selection is derived from the latest click and restores a single row when disabled.
   Installer selection; selection.page=1; selection.iso=image;
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
   assert(fails([&] { prepareInstall(disk,image,false); }).find("already exists")!=std::string::npos);

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

   // Automatic validation advances once without selecting a target or starting writes.
   Installer automatic; before=writeCalls; automatic.setImagePath(image.path,true);
   assert(automatic.pending==Next && automatic.source==image.path && !automatic.canContinue());
   deliver(automatic);
   assert(automatic.page==1 && automatic.selected.empty() && !automatic.plan && writeCalls==before);
   automatic.activate(Back); deliver(automatic);
   assert(automatic.page==0 && automatic.pending==None && automatic.source.empty() && automatic.iso.path==image.path);
   Installer invalidSource; invalidSource.setImagePath(fixtureRoot+"/missing-source.iso",true); deliver(invalidSource);
   assert(invalidSource.page==0 && !invalidSource.error.empty() && invalidSource.pending==None && writeCalls==before);
   Installer noSource; noSource.setImagePath("",true); assert(noSource.page==0 && noSource.pending==None);

   // Typed and pasted paths are not validated until Next, including a retry at the same path.
   Installer input; input.setImagePath(fixtureRoot+"/missing-東京-İstanbul.iso");
   assert(input.source.empty() && input.message().empty() && input.canContinue());
   input.activate(Next); deliver(input);
   assert(input.page==0 && !input.error.empty() && input.source.empty() && input.canContinue());
   auto feedback=input.message(); assert(std::count(feedback.begin(),feedback.end(),'\n')==2 && feedback.find("\nReason: ")!=std::string::npos);
   input.setImagePath("\""+image.path+"\""); assert(input.error.empty() && input.source.empty());
   input.activate(Next); deliver(input); assert(input.page==1 && input.iso.path==image.path && input.iso.error.empty());
   input.activate(Back); input.setImagePath(fixtureRoot+"/different.iso");
   assert(input.page==0 && input.iso.entries.empty() && input.message().empty() && input.source.empty());

   auto drive=fixture(5,true); hardware={drive}; Installer wizard; wizard.iso=image;
   wizard.activate(Next); wizard.scannedRevision=wizard.revision-1; wizard.scanReady=true; wizard.tick(); assert(wizard.page==0);
   deliver(wizard); assert(wizard.page==1 && wizard.pending==None); wizard.activate(Select,drive.id); wizard.activate(Erase);
   wizard.activate(Next); assert(wizard.displayPage()==2 && wizard.indeterminate() && !wizard.progressText().empty());
   before=writeCalls; deliver(wizard); assert(wizard.page==1 && wizard.displayPage()==1 && !wizard.plan && writeCalls==before);
   approve=true; wizard.activate(Next); deliver(wizard); assert(wizard.pending==Confirm);
   hardware.clear(); deliver(wizard); assert(wizard.page==1 && !dialogError.empty() && wizard.message().empty() && writeCalls==before);
   hardware={drive}; deliver(wizard); wizard.activate(Select,drive.id); wizard.activate(Next); deliver(wizard);
   assert(wizard.displayPage()==2 && wizard.pending==Confirm);
   // A slow scanner must not block the GUI handoff or allow writes before it finishes.
   std::atomic<bool> releaseScan{false},scanFinished{false};
   wizard.scanner=std::thread([&]
   {
      for (int i=0; i<200 && !releaseScan; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      scanFinished=true;
   });
   deliver(wizard); assert(!scanFinished && writeCalls==before); releaseScan=true;
   if (wizard.page!=2) fprintf(stderr,"Wizard page %d, pending %d: %s\n",wizard.page,int(wizard.pending),wizard.error.c_str());
   assert(wizard.page==2);
   wizard.activate(Close); assert(native==None);
   for (int i=0; i<1000 && wizard.page==2; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); wizard.tick(); }
   wizard.stop(); assert(wizard.page==3 && wizard.error.empty() && wizard.percent==100); wizard.activate(Next); assert(native==Close);
   puts("Installer: ISO, SHA-256, GPT/FAT32 writes, existing files, Windows multiboot, firmware rollback, disconnection and confirmation checks passed.");
}
