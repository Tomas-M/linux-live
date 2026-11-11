#include "disk-format.h"
#include "hash.h"
#include <codecvt>
#include <cstring>
#include <ctime>
#include <locale>
#include <map>
#include <set>

// Decode unaligned on-disk values independently of the host's byte order.
uint32_t disk32(const unsigned char *p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
uint64_t disk64(const unsigned char *p) { return uint64_t(disk32(p))|uint64_t(disk32(p+4))<<32; }
static void put16(unsigned char *p,uint16_t n) { p[0]=n; p[1]=n>>8; }
static void put32(unsigned char *p,uint32_t n) { for (int i=0; i<4; i++) p[i]=n>>(i*8); }
static void put64(unsigned char *p,uint64_t n) { for (int i=0; i<8; i++) p[i]=n>>(i*8); }

// Preserve representable CHS geometry while using saturated values beyond its legacy limit.
static void chs(unsigned char *p,uint64_t lba)
{
   uint64_t cylinder=lba/(255*63);
   if (cylinder>1023) { p[0]=0xfe; p[1]=p[2]=0xff; return; }
   p[0]=lba/63%255; p[1]=(lba%63+1)|((cylinder>>2)&0xc0); p[2]=cylinder;
}

// GPT uses the standard reflected Ethernet CRC, including both partition arrays.
uint32_t crc32(const void *value,size_t bytes)
{
   auto data=static_cast<const unsigned char *>(value); uint32_t crc=0xffffffff;
   while (bytes--) { crc^=*data++; for (int bit=0; bit<8; bit++) crc=(crc>>1)^((crc&1)?0xedb88320:0); }
   return ~crc;
}

// Snapshot only partition metadata, avoiding false changes from normal writes inside a mounted filesystem.
std::string partitionSnapshot(Disk &disk,bool strict)
{
   uint32_t sector=disk.device.sector; uint64_t sectors=disk.device.diskBytes/sector;
   if (!strict)
   {
      std::vector<unsigned char> area(65536); Hash hash;
      disk.read(0,area.data(),area.size()); hash.add(area.data(),area.size());
      disk.read(disk.device.diskBytes-area.size(),area.data(),area.size()); hash.add(area.data(),area.size());
      auto digest=hash.finish(); return std::string(reinterpret_cast<const char *>(digest.data()),digest.size());
   }
   std::vector<unsigned char> mbr(sector); disk.read(0,mbr.data(),sector);
   Hash hash; hash.add(mbr.data(),mbr.size());
   if (!disk.device.number && !disk.device.filesystem.empty() && disk.device.partitions.empty())
   {
      auto digest=hash.finish(); return std::string(reinterpret_cast<const char *>(digest.data()),digest.size());
   }
   std::vector<unsigned char> primary(sector); disk.read(sector,primary.data(),sector);
   if (!memcmp(primary.data(),"EFI PART",8))
   {
      uint32_t size=disk32(primary.data()+12),count=disk32(primary.data()+80),entrySize=disk32(primary.data()+84);
      if (size<92 || size>sector || count<1 || count>4096 || entrySize!=128) throw std::runtime_error("The disk has an invalid or unsupported GPT header.");
      uint32_t expected=disk32(primary.data()+16); put32(primary.data()+16,0);
      if (crc32(primary.data(),size)!=expected) throw std::runtime_error("The primary GPT header failed its integrity check. Repair the partition table first.");
      put32(primary.data()+16,expected);
      uint64_t backupAt=disk64(primary.data()+32),tableAt=disk64(primary.data()+72);
      size_t tableBytes=size_t(count)*128,rounded=(tableBytes+sector-1)/sector*sector;
      if (backupAt>=sectors || backupAt<2 || tableAt<2 || tableAt>sectors-rounded/sector) throw std::runtime_error("The GPT metadata lies outside this disk.");
      std::vector<unsigned char> table(rounded),backup(sector),other(rounded);
      disk.read(tableAt*sector,table.data(),rounded); disk.read(backupAt*sector,backup.data(),sector);
      uint32_t backupSize=disk32(backup.data()+12),backupCrc=disk32(backup.data()+16); put32(backup.data()+16,0);
      bool header=!memcmp(backup.data(),"EFI PART",8) && backupSize==size && crc32(backup.data(),size)==backupCrc;
      bool geometry=disk64(backup.data()+24)==backupAt && disk64(backup.data()+32)==1 && !memcmp(primary.data()+40,backup.data()+40,32);
      bool entries=disk32(backup.data()+80)==count && disk32(backup.data()+84)==128 && disk32(backup.data()+88)==disk32(primary.data()+88);
      uint64_t otherAt=disk64(backup.data()+72);
      bool tableRange=otherAt>=2 && otherAt<=sectors-rounded/sector && otherAt+rounded/sector<=backupAt;
      bool usable=disk64(primary.data()+24)==1 && disk64(primary.data()+40)>=tableAt+rounded/sector && disk64(primary.data()+48)<otherAt;
      bool ordered=disk64(primary.data()+40)<=disk64(primary.data()+48) && backupAt>tableAt+rounded/sector;
      if (!header || !geometry || !entries || !tableRange || !usable || !ordered || crc32(table.data(),tableBytes)!=disk32(primary.data()+88)) throw std::runtime_error("The GPT copies are inconsistent or damaged. Repair the partition table first.");
      disk.read(otherAt*sector,other.data(),rounded);
      if (table!=other) throw std::runtime_error("The primary and backup GPT partition arrays differ.");
      std::vector<std::pair<uint64_t,uint64_t>> ranges; std::set<std::string> identities;
      for (uint32_t i=0; i<count; i++)
      {
         auto entry=table.data()+i*128; bool used=false; for (int b=0; b<16; b++) used=used || entry[b]; if (!used) continue;
         std::string id(reinterpret_cast<const char *>(entry+16),16);
         bool empty=std::all_of(id.begin(),id.end(),[](unsigned char c) { return c==0; });
         if (empty || !identities.insert(id).second) throw std::runtime_error("The GPT has missing or duplicate partition identifiers. Repair its partition table first.");
         uint64_t first=disk64(entry+32),last=disk64(entry+40);
         if (first>last || first<disk64(primary.data()+40) || last>disk64(primary.data()+48)) throw std::runtime_error("A GPT partition is outside the usable disk area.");
         ranges.push_back({first,last});
      }
      std::sort(ranges.begin(),ranges.end());
      for (size_t i=1; i<ranges.size(); i++) if (ranges[i].first<=ranges[i-1].second) throw std::runtime_error("The disk has overlapping GPT partitions.");
      hash.add(primary.data(),primary.size()); hash.add(table.data(),table.size()); hash.add(backup.data(),backup.size());
   }
   else if (mbr[510]==0x55 && mbr[511]==0xaa)
   {
      std::set<uint64_t> visited; std::vector<std::pair<uint64_t,uint64_t>> primaryRanges;
      for (int i=0; i<4; i++)
      {
         auto entry=mbr.data()+446+i*16; unsigned type=entry[4];
         if (type==0xee) throw std::runtime_error("A protective MBR has no valid GPT header. Repair the disk first.");
         uint64_t start=disk32(entry+8),count=disk32(entry+12);
         if (!type && !count) continue;
         if (!type || !count || !start || start>=sectors || count>sectors-start) throw std::runtime_error("An MBR partition has invalid boundaries. Repair the partition table first.");
         primaryRanges.push_back({start,start+count});
         if (type!=5 && type!=15 && type!=0x85) continue;
         uint64_t base=start,next=base,limit=base+count; std::vector<std::pair<uint64_t,uint64_t>> logicalRanges;
         while (next)
         {
            if (next<base || next>=limit || visited.size()>1024 || !visited.insert(next).second) throw std::runtime_error("The disk has an invalid extended partition chain.");
            std::vector<unsigned char> ebr(sector); disk.read(next*sector,ebr.data(),sector); hash.add(ebr.data(),ebr.size());
            if (ebr[510]!=0x55 || ebr[511]!=0xaa) throw std::runtime_error("An extended partition record is damaged.");
            logicalRanges.push_back({next,next+1});
            auto data=ebr.data()+446; uint64_t relative=disk32(data+8),bytes=disk32(data+12);
            if (data[4] || bytes)
            {
               if (!data[4] || !relative || !bytes || relative>=limit-next || bytes>limit-next-relative) throw std::runtime_error("A logical partition extends outside its MBR container.");
               logicalRanges.push_back({next+relative,next+relative+bytes});
            }
            auto link=data+16; relative=disk32(link+8);
            if (relative && link[4]!=5 && link[4]!=15 && link[4]!=0x85) throw std::runtime_error("The extended partition chain has an invalid link type.");
            next=relative?base+relative:0;
         }
         std::sort(logicalRanges.begin(),logicalRanges.end());
         for (size_t n=1; n<logicalRanges.size(); n++) if (logicalRanges[n].first<logicalRanges[n-1].second) throw std::runtime_error("The disk has overlapping logical partitions or extended boot records.");
      }
      std::sort(primaryRanges.begin(),primaryRanges.end());
      for (size_t n=1; n<primaryRanges.size(); n++) if (primaryRanges[n].first<primaryRanges[n-1].second) throw std::runtime_error("The disk has overlapping MBR partitions.");
   }
   auto digest=hash.finish(); return std::string(reinterpret_cast<const char *>(digest.data()),digest.size());
}

// Parse the random hexadecimal identity directly into the GPT's byte representation.
static void identity(unsigned char *out,const std::string &text)
{
   for (int i=0; i<16; i++) out[i]=strtoul(text.substr(i*2,2).c_str(),nullptr,16);
   out[7]=(out[7]&15)|0x40; out[8]=(out[8]&63)|0x80;
}

// Use GPT normally; XP needs MBR with its accessible data partition before the ESP.
Layout newLayout(const Device &device,bool mbr)
{
   if (device.sector!=512 && device.sector!=4096) throw std::runtime_error("The partition layout requires 512-byte or 4096-byte logical sectors.");
   if (device.diskBytes<1000000000 || device.diskBytes%device.sector) throw std::runtime_error("The disk cannot hold an aligned full-capacity Slax layout of at least 1 GB.");
   Layout result; uint64_t sector=device.sector,total=device.diskBytes/sector,tableSectors=16384/sector;
   uint64_t espBytes=sector==4096?512ULL*1024*1024:256ULL*1024*1024;
   result.efi.label="SLAXEFI"; result.data.label="SLAXDATA"; result.efi.filesystem=result.data.filesystem="FAT32";
   for (int i=0; i<2; i++)
   {
      auto text=randomId().substr(24,8); for (char &c:text) if (c>='a' && c<='f') c-=32;
      (i?result.data:result.efi).uuid=text.substr(0,4)+"-"+text.substr(4);
   }
   if (mbr)
   {
      if (sector!=512 || total>uint64_t(UINT32_MAX)+1) throw std::runtime_error("Windows XP requires an MBR disk with 512-byte logical sectors and at most 2 TiB of capacity. Use Slax or newer Windows for this disk.");
      // Reserve old GPT metadata outside both the BIOS core and the new filesystems.
      result.bios.start=34*sector; result.bios.bytes=1024*1024-result.bios.start;
      result.data.start=1024*1024; result.data.bytes=((total-33)*sector-espBytes-result.data.start)/(1024*1024)*(1024*1024); result.data.number=1;
      result.efi.start=result.data.start+result.data.bytes; result.efi.bytes=(total-33)*sector-result.efi.start; result.efi.number=2;
      result.mbr.resize(sector); put32(result.mbr.data()+440,strtoul(randomId().substr(0,8).c_str(),nullptr,16)|1);
      const Partition parts[]={result.data,result.efi};
      for (int i=0; i<2; i++)
      {
         auto entry=result.mbr.data()+446+i*16; entry[0]=i?0:0x80; entry[4]=i?0xef:0x0c;
         uint64_t start=parts[i].start/sector,count=parts[i].bytes/sector;
         chs(entry+1,start); chs(entry+5,start+count-1); put32(entry+8,start); put32(entry+12,count);
      }
      result.mbr[510]=0x55; result.mbr[511]=0xaa;
      return result;
   }
   // Reserve BIOS embedding space outside the GPT's usable range, without exposing a raw partition.
   result.bios.start=(tableSectors+2)*sector; result.bios.bytes=1024*1024-result.bios.start;
   result.efi.start=1024*1024; result.efi.bytes=espBytes; result.efi.number=1;
   result.data.start=result.efi.start+espBytes; result.data.bytes=(total-tableSectors-1)*sector-result.data.start; result.data.number=2;
   if (result.data.bytes/sector>UINT32_MAX) throw std::runtime_error("The disk cannot hold the requested full-capacity GPT/FAT32 layout.");
   result.mbr.resize(sector); result.mbr[446+4]=0xee; put32(result.mbr.data()+446+8,1); put32(result.mbr.data()+446+12,std::min<uint64_t>(total-1,UINT32_MAX)); result.mbr[510]=0x55; result.mbr[511]=0xaa;
   result.mbr[447]=0; result.mbr[448]=2; result.mbr[449]=0; result.mbr[451]=result.mbr[452]=result.mbr[453]=0xff;
   result.table.resize(16384);
   const unsigned char types[2][16]={{0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b},{0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}};
   const char *names[]={"EFI System Partition","Slax data"}; Partition parts[]={result.efi,result.data};
   for (int i=0; i<2; i++)
   {
      auto entry=result.table.data()+i*128; memcpy(entry,types[i],16); identity(entry+16,randomId());
      put64(entry+32,parts[i].start/sector); put64(entry+40,(parts[i].start+parts[i].bytes)/sector-1);
      for (size_t c=0; c<strlen(names[i]); c++) put16(entry+56+c*2,names[i][c]);
   }
   result.primary.resize(sector); auto header=result.primary.data();
   memcpy(header,"EFI PART",8); put32(header+8,0x10000); put32(header+12,92); put64(header+24,1); put64(header+32,total-1);
   put64(header+40,result.efi.start/sector); put64(header+48,total-tableSectors-2); identity(header+56,randomId()); put64(header+72,2);
   put32(header+80,128); put32(header+84,128); put32(header+88,crc32(result.table.data(),result.table.size())); put32(header+16,crc32(header,92));
   result.backup=result.primary; header=result.backup.data(); put64(header+24,total-1); put64(header+32,1); put64(header+72,total-tableSectors-1); put32(header+16,0); put32(header+16,crc32(header,92));
   return result;
}

// Commit the MBR last; remove old GPT signatures when replacing GPT with XP's MBR layout.
void writeLayout(Disk &disk,const Layout &layout)
{
   uint64_t sector=disk.device.sector,total=disk.device.diskBytes;
   if (layout.primary.empty())
   {
      std::vector<unsigned char> empty(33*sector);
      disk.write(sector,empty.data(),empty.size()); disk.write(total-empty.size(),empty.data(),empty.size());
   }
   else
   {
      disk.write(total-sector-layout.table.size(),layout.table.data(),layout.table.size()); disk.write(total-sector,layout.backup.data(),sector);
      disk.write(sector*2,layout.table.data(),layout.table.size()); disk.write(sector,layout.primary.data(),sector);
   }
   disk.write(0,layout.mbr.data(),sector); disk.flush();
   partitionSnapshot(disk);
}

// Represent generated boot configuration through the same verified copy path as ISO files.
Content generated(const std::string &path,const std::string &text,bool directory)
{
   return {{path,UINT64_MAX,uint64_t(text.size()),directory},text};
}

// Map supported data filesystems to the stock GRUB reader used before external modules are available.
std::string biosFilesystem(const Partition &target)
{
   auto fs=target.filesystem;
   for (char &c:fs) if (c>='A' && c<='Z') c+=32;
   if (fs=="vfat" || fs=="fat32" || fs=="fat16") return "fat";
   if (fs=="ext3" || fs=="ext4") return "ext2";
   return fs;
}

// An occupied embedding area may belong to another loader, including its recovery/parity data.
static bool emptyArea(Disk &disk,uint64_t start,uint64_t bytes)
{
   std::vector<unsigned char> buffer(1024*1024);
   while (bytes)
   {
      size_t count=std::min<uint64_t>(buffer.size(),bytes); disk.read(start,buffer.data(),count);
      if (std::any_of(buffer.begin(),buffer.begin()+count,[](unsigned char c) { return c!=0; })) return false;
      start+=count; bytes-=count;
   }
   return true;
}

// Reclaim only a zero-filled extent for a declared new BIOS partition, outside every existing partition.
static Partition emptyBootPartition(Disk &disk,const DiskRange &gap,uint64_t bytes)
{
   uint64_t sector=disk.device.sector,limit=std::min<uint64_t>(gap.start+gap.bytes,(uint64_t(UINT32_MAX)+1)*sector);
   if (gap.start>=limit || bytes>limit-gap.start) return {};
   uint64_t scan=std::min<uint64_t>(limit-gap.start,16*1024*1024);
   std::vector<unsigned char> buffer(1024*1024);
   for (uint64_t first:{gap.start,limit-scan})
   {
      uint64_t run=0;
      for (uint64_t at=first; at<first+scan;)
      {
         size_t count=std::min<uint64_t>(buffer.size(),first+scan-at); disk.read(at,buffer.data(),count);
         for (size_t n=0; n<count; n+=sector)
         {
            bool zero=std::all_of(buffer.begin()+n,buffer.begin()+n+sector,[](unsigned char c) { return c==0; });
            run=zero?run+sector:0;
            if (run>=bytes) { Partition part; part.start=at+n+sector-bytes; part.bytes=bytes; return part; }
         }
         at+=count;
      }
   }
   return {};
}

// Construct the exact contiguous core image used for both recognition and verified BIOS writes.
static std::vector<unsigned char> biosCore(const BiosImage &image,uint64_t start)
{
   size_t sectors=(image.core.size()+511)/512;
   std::vector<unsigned char> core(sectors*512); memcpy(core.data(),image.core.data(),image.core.size());
   memset(core.data()+512-24,0,12); put64(core.data()+512-12,start/512+1); put16(core.data()+512-4,sectors-1); put16(core.data()+512-2,0x820);
   return core;
}

// Add boot partitions only in unused space, preserving every existing partition byte and identifier.
BootLayout planBootLayout(Disk &disk,const BiosImage *bios,uint64_t efiBytes,bool addEfi,bool replaceBios)
{
   partitionSnapshot(disk);
   BootLayout result; uint64_t sector=disk.device.sector,total=disk.device.diskBytes/sector;
   std::vector<unsigned char> mbr(sector),header(sector),backup,table;
   disk.read(0,mbr.data(),sector); disk.read(sector,header.data(),sector);
   result.originalMbr.assign(reinterpret_cast<const char *>(mbr.data()),mbr.size());
   if (!disk.device.number && !disk.device.filesystem.empty() && disk.device.partitions.empty()) return result;
   result.gpt=!memcmp(header.data(),"EFI PART",8);
   bool signature=mbr[510]==0x55 && mbr[511]==0xaa;
   if (!signature && !result.gpt) return result;
   uint64_t first=1,last=total,tableAt=0,backupAt=0,otherAt=0;
   std::vector<DiskRange> occupied;
   std::vector<unsigned> slots;
   uint64_t coreSize=bios?(bios->core.size()+sector-1)/sector*sector:0;
   if (result.gpt)
   {
      if (!signature) throw std::runtime_error("The GPT disk has no valid protective MBR signature. Repair its partition table first.");
      // Hybrid GPT/MBR loaders cannot safely be replaced or extended automatically.
      for (unsigned i=0; i<4; i++) if (mbr[446+i*16+4] && mbr[446+i*16+4]!=0xee) throw std::runtime_error("This disk has a hybrid GPT/MBR table. Use a standard GPT or MBR layout; its existing boot configuration will not be changed.");
      first=disk64(header.data()+40); last=disk64(header.data()+48)+1;
      tableAt=disk64(header.data()+72); backupAt=disk64(header.data()+32);
      uint32_t count=disk32(header.data()+80); table.resize((uint64_t(count)*128+sector-1)/sector*sector); backup.resize(sector);
      disk.read(tableAt*sector,table.data(),table.size()); disk.read(backupAt*sector,backup.data(),sector); otherAt=disk64(backup.data()+72);
      const unsigned char biosType[]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};
      for (unsigned i=0; i<count; i++)
      {
         auto entry=table.data()+i*128;
         if (std::all_of(entry,entry+16,[](unsigned char c) { return c==0; })) { slots.push_back(i); continue; }
         uint64_t start=disk64(entry+32)*sector,bytes=(disk64(entry+40)+1)*sector-start;
         occupied.push_back({start,bytes});
         bool bootPartition=!memcmp(entry,biosType,16);
         bool fits=coreSize && bytes>=coreSize && bytes<=16*1024*1024;
         if (!result.bios.bytes && bootPartition && fits)
         {
            bool formatted=std::any_of(disk.device.partitions.begin(),disk.device.partitions.end(),[&](const Partition &part) { return part.number==i+1 && !part.filesystem.empty(); });
            if (!formatted && (replaceBios || emptyArea(disk,start,bytes))) { result.bios.start=start; result.bios.bytes=coreSize; result.bios.number=i+1; }
            else if (!formatted)
            {
               uint64_t at=disk64(mbr.data()+0x5c);
               bool bounded=at>=start/sector && at<(start+bytes)/sector && coreSize/sector<=(start+bytes)/sector-at;
               if (bounded && result.originalMbr.compare(0,440,biosBootSector(*bios,at*sector),0,440)==0)
               {
                  auto expected=biosCore(*bios,at*sector); std::vector<unsigned char> existing(expected.size()); disk.read(at*sector,existing.data(),existing.size());
                  if (existing==expected)
                  {
                     result.bios.start=at*sector; result.bios.bytes=coreSize; result.bios.number=i+1;
                     result.biosBefore.assign(reinterpret_cast<const char *>(existing.data()),existing.size());
                  }
               }
            }
         }
      }
      // FirstUsableLBA deliberately excludes Slax's reserved BIOS space from partition allocation.
      uint64_t reserved=(tableAt+table.size()/sector)*sector,limit=first*sector;
      bool room=sector==512 && coreSize>=1024 && limit>reserved && limit-reserved<=16*1024*1024;
      if (!result.bios.bytes && room && coreSize<=limit-reserved)
      {
         if (replaceBios || emptyArea(disk,reserved,limit-reserved)) { result.bios.start=reserved; result.bios.bytes=coreSize; }
         else
         {
            uint64_t at=disk64(mbr.data()+0x5c);
            bool bounded=at>=reserved/sector && at<first && coreSize/sector<=first-at;
            if (bounded && result.originalMbr.compare(0,440,biosBootSector(*bios,at*sector),0,440)==0)
            {
               auto expected=biosCore(*bios,at*sector); std::vector<unsigned char> existing(expected.size()); disk.read(at*sector,existing.data(),existing.size());
               if (existing==expected)
               {
                  result.bios.start=at*sector; result.bios.bytes=coreSize;
                  result.biosBefore.assign(reinterpret_cast<const char *>(existing.data()),existing.size());
               }
            }
         }
      }
   }
   else
   {
      for (unsigned i=0; i<4; i++)
      {
         auto entry=mbr.data()+446+i*16; uint64_t start=disk32(entry+8),count=disk32(entry+12);
         if (!entry[4] && !count) { slots.push_back(i); continue; }
         if (!entry[4] || !count || !start || start>=total || count>total-start) throw std::runtime_error("An MBR partition has invalid boundaries. Repair the partition table first.");
         occupied.push_back({start*sector,count*sector});
      }
      // An existing Slax MBR proves ownership even when another partition cannot be mounted.
      if (bios && sector==512)
      {
         uint64_t at=disk64(mbr.data()+0x5c);
         if (at<total && result.originalMbr.compare(0,440,biosBootSector(*bios,at*sector),0,440)==0)
            reuseBios(disk,result,*bios,biosBootSector(*bios,at*sector));
      }
   }
   std::sort(occupied.begin(),occupied.end(),[](const DiskRange &a,const DiskRange &b) { return a.start<b.start; });
   std::vector<DiskRange> gaps; uint64_t at=first*sector;
   for (const auto &part:occupied)
   {
      if (part.start<at) throw std::runtime_error("The disk has overlapping partitions. Repair its partition table first.");
      if (part.start>at) gaps.push_back({at,part.start-at});
      at=part.start+part.bytes;
   }
   if (at<last*sector) gaps.push_back({at,last*sector-at});
   auto allocate=[&](uint64_t bytes,bool bios)
   {
      Partition part;
      for (auto &gap:gaps)
      {
         uint64_t start=bios?gap.start:(gap.start+1048575)/1048576*1048576;
         if (start<gap.start || start-gap.start>gap.bytes || bytes>gap.bytes-(start-gap.start)) continue;
         // MBR has no explicit BIOS partition: accept only completely empty, bounded gaps.
         bool replace=bios && !result.gpt && replaceBios && gap.start==sector && gap.bytes<=16*1024*1024;
         if (bios && !result.gpt && !replace && (gap.bytes>16*1024*1024 || !emptyArea(disk,gap.start,gap.bytes))) continue;
         if (!replace && !emptyArea(disk,start,bytes)) continue;
         part.start=start; part.bytes=bytes; gap.bytes-=start+bytes-gap.start; gap.start=start+bytes; break;
      }
      return part;
   };
   auto add=[&](Partition &part,bool bios)
   {
      if (!part.bytes || slots.empty()) return;
      unsigned slot=slots.front(); slots.erase(slots.begin()); part.number=slot+1;
      if (result.gpt)
      {
         const unsigned char biosType[]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};
         const unsigned char efiType[]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
         auto entry=table.data()+slot*128; memset(entry,0,128); memcpy(entry,bios?biosType:efiType,16); identity(entry+16,randomId());
         put64(entry+32,part.start/sector); put64(entry+40,(part.start+part.bytes)/sector-1);
         const char *name=bios?"Slax BIOS boot":"EFI System Partition";
         for (size_t c=0; c<strlen(name); c++) put16(entry+56+c*2,name[c]);
      }
      else
      {
         auto entry=mbr.data()+446+slot*16; memset(entry,0,16); entry[4]=bios?0xda:0xef; chs(entry+1,part.start/sector); chs(entry+5,(part.start+part.bytes)/sector-1);
         put32(entry+8,part.start/sector); put32(entry+12,part.bytes/sector);
      }
   };
   if (coreSize && !result.bios.bytes)
   {
      if (!result.gpt)
      {
         result.bios=allocate(coreSize,true);
         if (!result.bios.bytes && !slots.empty()) for (auto &gap:gaps)
         {
            if (gap.start==sector) continue; // Preserve an occupied traditional MBR embedding area in full.
            result.bios=emptyBootPartition(disk,gap,coreSize);
            if (result.bios.bytes)
            {
               add(result.bios,true); gap.bytes-=result.bios.start+result.bios.bytes-gap.start; gap.start=result.bios.start+result.bios.bytes; break;
            }
         }
      }
      else if (!slots.empty())
      {
         result.bios=allocate(1024*1024,true);
         if (!result.bios.bytes) result.bios=allocate(std::max<uint64_t>(65536,coreSize),true);
         add(result.bios,true);
      }
   }
   if (addEfi && !slots.empty())
   {
      uint64_t bytes=std::max<uint64_t>(sector==4096?512ULL*1024*1024:128ULL*1024*1024,(efiBytes*2+1048575)/1048576*1048576);
      result.efi=allocate(bytes,false);
      if (result.efi.bytes && (result.gpt || (result.efi.start+result.efi.bytes)/sector<=uint64_t(UINT32_MAX)+1))
      {
         add(result.efi,false); result.efi.filesystem="FAT32"; result.efi.label="SLAXEFI"; result.efi.type=result.gpt?"c12a7328-f81f-11d2-ba4b-00a0c93ec93b":"0xef";
         auto id=randomId().substr(24,8); for (char &c:id) if (c>='a' && c<='f') c-=32;
         result.efi.uuid=id.substr(0,4)+"-"+id.substr(4); fatGeometry(result.efi.bytes,sector);
         result.ranges.push_back({result.efi.start,result.efi.bytes});
      }
      else result.efi={};
   }
   auto patch=[&](uint64_t start,const std::vector<unsigned char> &after)
   {
      std::string before(after.size(),'\0'); disk.read(start,before.data(),before.size());
      std::string value(reinterpret_cast<const char *>(after.data()),after.size());
      if (before!=value) { result.patches.push_back({start,std::move(before),std::move(value)}); result.ranges.push_back({start,after.size()}); }
   };
   if (result.gpt)
   {
      put32(header.data()+88,crc32(table.data(),disk32(header.data()+80)*128)); put32(header.data()+16,0); put32(header.data()+16,crc32(header.data(),disk32(header.data()+12)));
      put32(backup.data()+88,disk32(header.data()+88)); put32(backup.data()+16,0); put32(backup.data()+16,crc32(backup.data(),disk32(backup.data()+12)));
      patch(otherAt*sector,table); patch(backupAt*sector,backup); patch(tableAt*sector,table); patch(sector,header);
   }
   patch(0,mbr);
   if (result.bios.bytes)
   {
      if (result.biosBefore.empty())
      {
         result.biosBefore.assign(coreSize,'\0');
         if (replaceBios) disk.read(result.bios.start,result.biosBefore.data(),result.biosBefore.size());
      }
      result.ranges.push_back({result.bios.start,coreSize}); result.ranges.push_back({0,sector});
   }
   return result;
}

// Reuse a native menu's sector file only when its complete core matches inside a bounded BIOS area.
bool reuseBios(Disk &disk,BootLayout &layout,const BiosImage &image,const std::string &boot)
{
   if (disk.device.sector!=512 || boot.size()!=512 || image.core.size()<1024) return false;
   uint64_t lba=disk64(reinterpret_cast<const unsigned char *>(boot.data())+0x5c);
   if (lba>=disk.device.diskBytes/512 || boot!=biosBootSector(image,lba*512)) return false;
   uint64_t start=lba*512; auto expected=biosCore(image,start);
   if (layout.bios.bytes && layout.bios.start!=start) return false;
   uint64_t first=512,last=disk.device.diskBytes; unsigned number=0;
   if (layout.gpt)
   {
      std::vector<unsigned char> header(512); disk.read(512,header.data(),header.size());
      first=(disk64(header.data()+72)+(uint64_t(disk32(header.data()+80))*128+511)/512)*512;
      last=disk64(header.data()+40)*512;
   }
   else for (unsigned i=0; i<4; i++)
   {
      auto entry=reinterpret_cast<const unsigned char *>(layout.originalMbr.data())+446+i*16;
      if (entry[4]) last=std::min(last,uint64_t(disk32(entry+8))*512);
   }
   bool bounded=last>first && last-first<=16*1024*1024 && start>=first && start<last && expected.size()<=last-start;
   for (const auto &part:disk.device.partitions)
   {
      bool bios=part.type=="21686148-6449-6e6f-744e-656564454649" || part.type=="0xda" || part.type=="218";
      if (bios && part.filesystem.empty() && part.bytes<=16*1024*1024 && start>=part.start && start-part.start<part.bytes && expected.size()<=part.bytes-(start-part.start))
      { bounded=true; number=part.number; }
   }
   if (!bounded) return false;
   std::vector<unsigned char> existing(expected.size()); disk.read(start,existing.data(),existing.size());
   if (existing!=expected) return false;
   layout.bios.start=start; layout.bios.bytes=expected.size(); layout.bios.number=number;
   layout.biosBefore.assign(reinterpret_cast<const char *>(existing.data()),existing.size());
   layout.ranges.push_back({start,expected.size()}); layout.ranges.push_back({0,512}); return true;
}

// Recheck both partition metadata and the exact embedding bytes before activating any new boot path.
void verifyBootLayout(Disk &disk,const BootLayout &layout)
{
   if (layout.efi.bytes && !emptyArea(disk,layout.efi.start,layout.efi.bytes)) throw std::runtime_error("The space approved for the new EFI partition is no longer empty. Existing partitions were kept.");
   for (const auto &patch:layout.patches)
   {
      std::string value(patch.before.size(),'\0'); disk.read(patch.start,value.data(),value.size());
      if (value!=patch.before) throw std::runtime_error("The boot partition layout changed after confirmation. Existing boot settings were kept.");
   }
   if (!layout.biosBefore.empty())
   {
      std::string before(layout.biosBefore.size(),'\0'); disk.read(layout.bios.start,before.data(),before.size());
      if (before!=layout.biosBefore) throw std::runtime_error("The approved BIOS embedding area changed. Existing boot settings were kept.");
   }
}

// Publish table updates and the new MBR last; roll back original boot metadata on an ordinary I/O failure.
void commitBootLayout(Disk &disk,const BootLayout &layout,const BiosImage *bios,bool preserveMbr)
{
   std::vector<unsigned char> mbr(layout.originalMbr.begin(),layout.originalMbr.end());
   for (const auto &patch:layout.patches) if (!patch.start) mbr.assign(patch.after.begin(),patch.after.end());
   try
   {
      if (bios) writeBios(disk,layout.bios.start,layout.biosBefore.size(),*bios,mbr);
      if (preserveMbr) std::copy(layout.originalMbr.begin(),layout.originalMbr.begin()+440,mbr.begin());
      for (const auto &patch:layout.patches) if (patch.start) disk.write(patch.start,patch.after.data(),patch.after.size());
      disk.flush();
      if (bios || std::any_of(layout.patches.begin(),layout.patches.end(),[](const DiskPatch &p) { return p.start==0; })) disk.write(0,mbr.data(),mbr.size());
      disk.flush(); partitionSnapshot(disk);
      for (const auto &patch:layout.patches)
      {
         const std::string expected=patch.start?patch.after:std::string(mbr.begin(),mbr.end());
         std::string actual(expected.size(),'\0'); disk.read(patch.start,actual.data(),actual.size());
         if (actual!=expected) throw std::runtime_error("Boot partition metadata failed read-back verification.");
      }
      if (bios)
      {
         std::vector<unsigned char> actual(mbr.size()); disk.read(0,actual.data(),actual.size());
         if (actual!=mbr) throw std::runtime_error("The BIOS boot record failed read-back verification.");
      }
   }
   catch (...)
   {
      auto failure=std::current_exception();
      try
      {
         for (auto it=layout.patches.rbegin(); it!=layout.patches.rend(); it++) disk.write(it->start,it->before.data(),it->before.size());
         if (bios) { disk.write(0,layout.originalMbr.data(),layout.originalMbr.size()); disk.write(layout.bios.start,layout.biosBefore.data(),layout.biosBefore.size()); }
         disk.flush();
      }
      catch (...) { throw std::runtime_error("Boot setup failed and the disk refused rollback. Stop using this disk and restore its saved boot metadata from slax/boot/grub/boot-backup before restarting."); }
      std::rethrow_exception(failure);
   }
}

// Encode portable Unicode long filenames as FAT's UTF-16 directory records.
static std::u16string unicode(const std::string &name)
{
   std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> converter;
   auto result=converter.from_bytes(name);
   if (result.empty() || result.size()>255) throw std::runtime_error("A filename is too long for FAT32.");
   return result;
}

// Check full-capacity FAT32 limits before the user approves any destructive operation.
FatGeometry fatGeometry(uint64_t bytes,uint32_t sector)
{
   if (sector!=512 && sector!=4096) throw std::runtime_error("FAT32 requires supported 512-byte or 4096-byte logical sectors.");
   uint64_t sectors=bytes/sector; uint32_t cluster=1;
   if (bytes>=512ULL*1024*1024) cluster=4096/sector;
   if (bytes>=8ULL*1024*1024*1024) cluster=16384/sector;
   if (bytes>=32ULL*1024*1024*1024) cluster=32768/sector;
   cluster=std::max(1u,cluster);
   if (bytes%sector || sectors>UINT32_MAX || sectors<65525) throw std::runtime_error("This partition is outside FAT32's supported capacity range.");
   uint32_t fat=uint32_t((((sectors-32)/cluster+2)*4+sector-1)/sector);
   uint32_t clusters=(sectors-32-2*fat)/cluster;
   if (clusters<65525 || clusters>0x0ffffff5) throw std::runtime_error("A full-capacity FAT32 partition cannot use compatible cluster sizes on this disk. Use a smaller disk or an existing smaller partition.");
   return {uint32_t(sectors),cluster,fat,clusters};
}

// Format and populate a new FAT32 partition without Windows' 32 GiB formatting limit.
void writeFat(Disk &disk,const Partition &partition,const std::vector<Content> &files,FILE *source,const Progress &progress)
{
   uint32_t sector=disk.device.sector; auto geometry=fatGeometry(partition.bytes,sector);
   uint32_t sectors=geometry.sectors,cluster=geometry.cluster,fat=geometry.fat,clusters=geometry.clusters,clusterBytes=cluster*sector;
   // Zero FAT dates are invalid and make Windows XP reject ordinary files and directories.
   std::time_t now=std::time(nullptr); std::tm calendar{};
#ifdef _WIN32
   auto local=std::localtime(&now); if (local) calendar=*local; // MSVCRT keeps this buffer per thread.
#else
   auto local=localtime_r(&now,&calendar);
#endif
   if (!local) throw std::runtime_error("The system clock cannot provide a valid FAT32 timestamp.");
   if (calendar.tm_year<80 || calendar.tm_year>207)
   { calendar={}; calendar.tm_year=80; calendar.tm_mday=1; }
   uint16_t date=((calendar.tm_year-80)<<9)|((calendar.tm_mon+1)<<5)|calendar.tm_mday;
   uint16_t time=(calendar.tm_hour<<11)|(calendar.tm_min<<5)|(std::min(calendar.tm_sec,59)/2);
   struct Node { const Content *file=nullptr; std::u16string name; std::string parent,shortName; uint32_t first=0,count=0; bool directory=true; };
   std::map<std::string,Node> nodes; nodes[""]=Node{};
   std::map<std::string,std::set<std::string>> names;
   uint64_t payload=0;
   for (const auto &file:files)
   {
      auto path=file.entry.path; auto slash=path.find_last_of('/'); std::string parent=slash==std::string::npos?"":path.substr(0,slash),name=path.substr(slash==std::string::npos?0:slash+1);
      if (nodes.count(path) || !nodes.count(parent) || !nodes[parent].directory) throw std::runtime_error("The payload has duplicate files or a missing parent directory.");
      Node node; node.file=&file; node.parent=parent; node.name=unicode(name); node.directory=file.entry.directory;
      names[parent].insert(filenameKey(name));
      if (!node.directory) { if (file.entry.bytes>UINT32_MAX) throw std::runtime_error("An individual file exceeds FAT32's 4 GiB limit."); node.count=(file.entry.bytes+clusterBytes-1)/clusterBytes; payload+=file.entry.bytes; }
      nodes[path]=node;
   }
   // A short alias must not shadow any sibling's long name, even one assigned later.
   unsigned long aliasNumber=1;
   for (auto &item:nodes) if (!item.first.empty())
   {
      auto &node=item.second; char alias[12];
      do { snprintf(alias,sizeof(alias),"%c%07lX   ",node.directory?'D':'F',aliasNumber++); }
      while (!names[node.parent].insert(filenameKey(std::string(alias,8))).second);
      node.shortName=alias;
   }
   for (auto &item:nodes) if (item.second.directory)
   {
      size_t entries=item.first.empty()?2:3;
      for (const auto &child:nodes) if (!child.first.empty() && child.second.parent==item.first) entries+=(child.second.name.size()+12)/13+1;
      item.second.count=(entries*32+clusterBytes-1)/clusterBytes;
   }
   uint32_t next=2;
   for (auto &item:nodes) { item.second.first=item.second.count?next:0; next+=item.second.count; }
   if (next-2>clusters) throw std::runtime_error("The FAT32 partition has insufficient space for the payload and directories.");
   std::vector<unsigned char> buffer(1024*1024);
   uint64_t metadata=(32ULL+2ULL*fat)*sector;
   for (uint64_t at=0; at<metadata; at+=buffer.size()) disk.write(partition.start+at,buffer.data(),std::min<uint64_t>(buffer.size(),metadata-at));
   std::vector<unsigned char> boot(sector),info(sector); auto p=boot.data();
   p[0]=0xeb; p[1]=0x58; p[2]=0x90; memcpy(p+3,"MSWIN4.1",8); put16(p+11,sector); p[13]=cluster; put16(p+14,32); p[16]=2; p[21]=0xf8;
   put16(p+24,63); put16(p+26,255); put32(p+28,partition.start/sector); put32(p+32,sectors); put32(p+36,fat); put32(p+44,2); put16(p+48,1); put16(p+50,6); p[64]=0x80; p[66]=0x29;
   auto serial=partition.uuid; serial.erase(std::remove(serial.begin(),serial.end(),'-'),serial.end()); put32(p+67,strtoul(serial.c_str(),nullptr,16));
   std::string label=partition.label; label.resize(11,' '); memcpy(p+71,label.data(),11); memcpy(p+82,"FAT32   ",8); p[90]=0xeb; p[91]=0xfe; p[510]=0x55; p[511]=0xaa;
   put32(info.data(),0x41615252); put32(info.data()+484,0x61417272); put32(info.data()+488,clusters-(next-2)); put32(info.data()+492,next<clusters+2?next:0xffffffff); put32(info.data()+508,0xaa550000);
   disk.write(partition.start,boot.data(),sector); disk.write(partition.start+sector,info.data(),sector); disk.write(partition.start+6*sector,boot.data(),sector); disk.write(partition.start+7*sector,info.data(),sector);
   std::vector<unsigned char> entries((uint64_t(next)*4+sector-1)/sector*sector); put32(entries.data(),0x0ffffff8); put32(entries.data()+4,0x0fffffff);
   for (const auto &item:nodes)
      for (uint32_t c=0; c<item.second.count; c++) put32(entries.data()+(item.second.first+c)*4,c+1==item.second.count?0x0fffffff:item.second.first+c+1);
   disk.write(partition.start+32*sector,entries.data(),entries.size()); disk.write(partition.start+(32ULL+fat)*sector,entries.data(),entries.size());
   // File read-back alone cannot detect a broken BPB, allocation chain or stale free cluster.
   const std::pair<uint64_t,const std::vector<unsigned char> *> records[]={
      {0,&boot},{sector,&info},{6ULL*sector,&boot},{7ULL*sector,&info},
      {32ULL*sector,&entries},{(32ULL+fat)*sector,&entries}
   };
   disk.flush(); std::vector<unsigned char> verified(buffer.size());
   for (uint64_t at=0; at<metadata; at+=buffer.size())
   {
      size_t bytes=std::min<uint64_t>(buffer.size(),metadata-at); memset(buffer.data(),0,bytes);
      for (const auto &record:records)
      {
         uint64_t first=std::max(at,record.first),last=std::min(at+bytes,record.first+record.second->size());
         if (first<last) memcpy(buffer.data()+first-at,record.second->data()+first-record.first,last-first);
      }
      disk.read(partition.start+at,verified.data(),bytes);
      if (memcmp(buffer.data(),verified.data(),bytes)) throw std::runtime_error("FAT32 metadata verification failed. The medium may be damaged.");
   }
   uint64_t dataAt=partition.start+(32ULL+2ULL*fat)*sector,completed=0;
   for (const auto &item:nodes)
   {
      const auto &node=item.second; uint64_t at=dataAt+uint64_t(node.first-2)*clusterBytes;
      if (node.directory)
      {
         std::vector<unsigned char> directory(uint64_t(node.count)*clusterBytes); size_t slot=0;
         if (item.first.empty()) { memcpy(directory.data(),label.data(),11); directory[11]=8; slot++; }
         else
         {
            for (int i=0; i<2; i++)
            {
               auto entry=directory.data()+slot++*32; memset(entry,' ',11); entry[0]='.'; if (i) entry[1]='.'; entry[11]=16;
               uint32_t first=i?(node.parent.empty()?0:nodes.at(node.parent).first):node.first; put16(entry+20,first>>16); put16(entry+26,first);
            }
         }
         for (const auto &child:nodes) if (!child.first.empty() && child.second.parent==item.first)
         {
            const auto &file=child.second; unsigned char checksum=0;
            for (unsigned char c:file.shortName) checksum=((checksum&1)?128:0)+(checksum>>1)+c;
            size_t count=(file.name.size()+12)/13;
            const unsigned positions[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
            for (size_t n=count; n; n--)
            {
               auto entry=directory.data()+slot++*32; memset(entry,0xff,32); entry[0]=n|(n==count?0x40:0); entry[11]=15; entry[12]=0; entry[13]=checksum; put16(entry+26,0);
               for (int c=0; c<13; c++) { size_t index=(n-1)*13+c; put16(entry+positions[c],index<file.name.size()?file.name[index]:(index==file.name.size()?0:0xffff)); }
            }
            auto entry=directory.data()+slot++*32; memcpy(entry,file.shortName.data(),11); entry[11]=file.directory?16:32; put16(entry+20,file.first>>16); put16(entry+26,file.first); put32(entry+28,file.directory?0:file.file->entry.bytes);
         }
         for (size_t n=0; n<slot; n++)
         {
            auto entry=directory.data()+n*32; if (entry[11]==15) continue;
            put16(entry+14,time); put16(entry+16,date); put16(entry+18,date); put16(entry+22,time); put16(entry+24,date);
         }
         disk.write(at,directory.data(),directory.size());
         std::vector<unsigned char> verify(directory.size()); disk.read(at,verify.data(),verify.size()); if (verify!=directory) throw std::runtime_error("FAT32 directory verification failed.");
         continue;
      }
      const auto &file=*node.file; Hash expected,actual; uint64_t size=file.entry.bytes;
      SourceFile input(source,file.entry);
      for (uint64_t copied=0; copied<size;)
      {
         size_t bytes=std::min<uint64_t>(buffer.size(),size-copied),rounded=(bytes+sector-1)/sector*sector;
         memset(buffer.data(),0,rounded);
         if (file.entry.offset==UINT64_MAX) memcpy(buffer.data(),file.data.data()+copied,bytes);
         else input.read(buffer.data(),bytes);
         expected.add(buffer.data(),bytes); disk.write(at+copied,buffer.data(),rounded); copied+=bytes;
         progress("Copying "+file.entry.path,payload?int((completed+copied)*75/payload):75);
      }
      disk.flush();
      for (uint64_t copied=0; copied<size;)
      {
         size_t bytes=std::min<uint64_t>(buffer.size(),size-copied),rounded=(bytes+sector-1)/sector*sector;
         disk.read(at+copied,buffer.data(),rounded); actual.add(buffer.data(),bytes); copied+=bytes;
      }
      if (actual.finish()!=expected.finish()) throw std::runtime_error("Read-back verification failed for "+file.entry.path+". The medium may be damaged.");
      completed+=size;
   }
   disk.flush();
}

// Build a standalone first-stage sector for NTLDR/BOOTMGR without replacing the disk's MBR.
std::string biosBootSector(const BiosImage &image,uint64_t start)
{
   if (image.boot.size()!=512 || start%512) throw std::runtime_error("The BIOS chainloader geometry is invalid.");
   std::string boot=image.boot; put64(reinterpret_cast<unsigned char *>(boot.data())+0x5c,start/512);
   boot[0x64]=char(0xff); boot[510]=0x55; boot[511]=char(0xaa); return boot;
}

// Embed stock GRUB's contiguous core only in the explicitly reserved BIOS area.
void writeBios(Disk &disk,uint64_t start,uint64_t capacity,const BiosImage &image,std::vector<unsigned char> &mbr)
{
   if (disk.device.sector!=512 || image.boot.size()!=512 || image.core.size()<1024) throw std::runtime_error("The BIOS loader or device sector geometry is unsupported.");
   size_t sectors=(image.core.size()+511)/512;
   if (sectors*512>capacity || sectors>65536) throw std::runtime_error("The GRUB BIOS image does not fit in the reserved embedding area.");
   auto core=biosCore(image,start);
   disk.write(start,core.data(),core.size()); std::vector<unsigned char> verified(core.size()); disk.read(start,verified.data(),verified.size());
   if (verified!=core) throw std::runtime_error("BIOS core verification failed.");
   memcpy(mbr.data(),image.boot.data(),440); put64(mbr.data()+0x5c,start/512); mbr[0x64]=0xff;
}
