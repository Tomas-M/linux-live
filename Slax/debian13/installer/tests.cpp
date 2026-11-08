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
   auto layout=newLayout(d); Disk disk(d); disk.claim(true); writeLayout(disk,layout); disk.release();
   layout.efi.mount=fixtureRoot+"/efi"+std::to_string(n); layout.data.mount=fixtureRoot+"/data"+std::to_string(n);
   std::filesystem::create_directories(layout.efi.mount+"/EFI/Microsoft/Boot"); std::filesystem::create_directory(layout.data.mount);
   std::ofstream(layout.efi.mount+"/EFI/Microsoft/Boot/bootmgfw.efi")<<"Windows boot loader must remain byte-for-byte intact";
   layout.data.label="D:"; layout.data.filesystem="NTFS"; layout.data.uuid="1122334455667788";
   layout.efi.type="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"; layout.efi.filesystem="FAT32";
   d.partitions={layout.bios,layout.efi,layout.data}; d.layout="gpt-owned-fixture";
   static_cast<Partition &>(d)=layout.data; d.path=d.disk+"#3"; d.free=UINT64_MAX;
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
   assert(hidden.selected.empty() && !hidden.canContinue() && !hidden.error.empty()); hardware.clear();
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
   auto noEfi=disk; noEfi.partitions.clear(); assert(!basicEligibility(noEfi,image,false).empty());
   auto small=disk; small.bytes=999999999; assert(!basicEligibility(small,image,false).empty());
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
   assert(!prepareInstall(cached,image,false).bios);
   cached.start=1024*1024; cached.bytes=cached.diskBytes-cached.start; cached.number=1; cached.partitions={static_cast<const Partition &>(cached)};
   std::vector<unsigned char> mbr(1024); mbr[450]=0x0c; mbr[510]=0x55; mbr[511]=0xaa;
   for (int b=0; b<4; b++) { mbr[454+b]=(cached.start/512)>>(b*8); mbr[458+b]=(cached.bytes/512)>>(b*8); }
   { Disk physical(cached); physical.claim(true); physical.write(0,mbr.data(),mbr.size()); physical.release(); }
   assert(prepareInstall(cached,image,false).bios);
   auto plan=prepareInstall(disk,image,false); assert(plan.registerBoot && !plan.windowsLoader.empty());
   auto original=text(disk.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi");
   int percent=0;
   install(plan,[&](const std::string &status,int progress) { assert(!status.empty() && progress>=percent && progress<=100); percent=progress; });
   assert(percent==100 && std::filesystem::exists(disk.mount+"/slax/boot/vmlinuz"));
   assert(text(disk.mount+"/slax/boot/grub.cfg").find("Windows Boot Manager")!=std::string::npos);
   assert(text(disk.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi")==original);
   assert(firmware["Boot0000"]==windowsEntry && firmware["Boot0001"]==otherEntry);
   assert(firmware["BootOrder"]==std::vector<unsigned char>({2,0,0,0,1,0}));
   assert(fails([&] { prepareInstall(disk,image,false); }).find("already exists")!=std::string::npos);

   auto rollback=fixture(2); auto order=firmware["BootOrder"]; auto rollbackPlan=prepareInstall(rollback,image,false); firmwareFault=1;
   assert(fails([&] { install(rollbackPlan,[](const auto &,int) {}); }).find("firmware failure")!=std::string::npos);
   assert(firmware["BootOrder"]==order && firmware["Boot0000"]==windowsEntry);
   assert(text(rollback.partitions[1].mount+"/EFI/Microsoft/Boot/bootmgfw.efi")==original);

   auto blank=fixture(3,true); auto erase=prepareInstall(blank,image,true); assert(erase.confirmation.find("D:")!=std::string::npos && erase.bios);
   auto changed=erase; changed.image.identity="changed"; unsigned before=writeCalls;
   assert(fails([&] { install(changed,[](const auto &,int) {}); }).find("No changes")!=std::string::npos && writeCalls==before);
   install(erase,[](const auto &,int) {}); assert(!partitionSnapshot(*erase.disk).empty());
   auto bad=fixture(4,true); auto badPlan=prepareInstall(bad,image,true); writeFault=1;
   assert(fails([&] { install(badPlan,[](const auto &,int) {}); }).find("did not finish")!=std::string::npos); writeFault=-1;
   auto native4k=fixture(6,true,4096); auto plan4k=prepareInstall(native4k,image,true); assert(!plan4k.bios);
   install(plan4k,[](const auto &,int) {}); assert(!partitionSnapshot(*plan4k.disk).empty());

   // XP converts an existing GPT fixture to MBR while keeping its data partition first.
   auto xp=fixture(7,true); xpHost=true; auto xpPlan=prepareInstall(xp,image,true);
   assert(xpPlan.bios && xpPlan.confirmation.find("Create MBR")!=std::string::npos && xpPlan.confirmation.find("data partition first")!=std::string::npos);
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
   assert(newLayout(xp).primary.size()==512 && prepareInstall(xp,image,true).confirmation.find("Create GPT")!=std::string::npos);

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
   input.activate(Next); deliver(input); assert(input.page==1 && input.iso.identity==image.identity);
   input.activate(Back); input.setImagePath(fixtureRoot+"/different.iso");
   assert(input.page==0 && input.iso.identity.empty() && input.message().empty() && input.source.empty());

   auto drive=fixture(5,true); hardware={drive}; Installer wizard; wizard.iso=image;
   wizard.activate(Next); wizard.scannedRevision=wizard.revision-1; wizard.scanReady=true; wizard.tick(); assert(wizard.page==0);
   deliver(wizard); assert(wizard.page==1 && wizard.pending==None); wizard.activate(Select,drive.id); wizard.activate(Erase);
   wizard.activate(Next); before=writeCalls; deliver(wizard); assert(wizard.page==1 && !wizard.plan && writeCalls==before);
   approve=true; wizard.activate(Next); deliver(wizard); assert(wizard.pending==Confirm);
   hardware.clear(); deliver(wizard); assert(wizard.page==1 && !wizard.error.empty() && writeCalls==before);
   hardware={drive}; deliver(wizard); wizard.activate(Select,drive.id); wizard.activate(Next); deliver(wizard); deliver(wizard);
   if (wizard.page!=2) fprintf(stderr,"Wizard page %d, pending %d: %s\n",wizard.page,int(wizard.pending),wizard.error.c_str());
   assert(wizard.page==2);
   wizard.activate(Close); assert(native==None);
   for (int i=0; i<1000 && wizard.page==2; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); wizard.tick(); }
   wizard.stop(); assert(wizard.page==3 && wizard.error.empty() && wizard.percent==100); wizard.activate(Next); assert(native==Close);
   puts("Installer: ISO, SHA-256, GPT/FAT32 writes, existing files, Windows multiboot, firmware rollback, disconnection and confirmation checks passed.");
}
