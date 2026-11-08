#include "disk-format.h"
#include <cstring>

// Encode UEFI's little-endian binary boot-option fields without packed C structures.
static void append(std::vector<unsigned char> &out,uint64_t value,unsigned bytes)
{
   for (unsigned i=0; i<bytes; i++) out.push_back(value>>(i*8));
}

// Boot file paths and this fixed description use only ASCII, represented as UTF-16 in NVRAM.
static void unicode(std::vector<unsigned char> &out,const std::string &text)
{
   for (unsigned char c:text) append(out,c,2);
   append(out,0,2);
}

// Store readable recovery metadata without touching any existing vendor's files.
static std::string hex(const std::vector<unsigned char> &bytes)
{
   const char *digits="0123456789abcdef"; std::string text;
   for (auto byte:bytes) { text+=digits[byte>>4]; text+=digits[byte&15]; }
   return text;
}

// Add one new option, then activate it only after all boot files and the recovery record are durable.
void registerFirmware(const InstallPlan &plan,const std::string &filename,const std::string &journal)
{
   if (partitionSnapshot(*plan.disk)!=plan.snapshot) throw std::runtime_error("The approved partition table changed before firmware activation. Existing boot settings were not changed.");
   char name[16]; snprintf(name,sizeof(name),"Boot%04X",plan.bootNumber);
   if (readFirmware("BootOrder")!=plan.bootOrder || !readFirmware(name).empty()) throw std::runtime_error("The firmware boot entries changed during installation. Existing boot settings were not replaced; retry after checking the firmware configuration.");
   std::string path=filename; std::replace(path.begin(),path.end(),'/','\\');
   std::vector<unsigned char> devicePath={4,1,42,0};
   uint32_t sector=plan.device.sector; std::vector<unsigned char> header(sector); plan.disk->read(sector,header.data(),header.size());
   uint32_t number=plan.efi.number; std::vector<unsigned char> signature(16); bool gpt=!memcmp(header.data(),"EFI PART",8);
   if (gpt)
   {
      uint32_t count=disk32(header.data()+80); std::vector<unsigned char> table((uint64_t(count)*128+sector-1)/sector*sector);
      plan.disk->read(disk64(header.data()+72)*sector,table.data(),table.size()); bool found=false;
      for (uint32_t i=0; i<count; i++)
      {
         auto entry=table.data()+i*128;
         if (disk64(entry+32)*sector==plan.efi.start && (disk64(entry+40)-disk64(entry+32)+1)*sector==plan.efi.bytes)
         { std::copy(entry+16,entry+32,signature.begin()); number=i+1; found=true; break; }
      }
      if (!found) throw std::runtime_error("The EFI partition is no longer present in the approved GPT.");
   }
   else
   {
      plan.disk->read(0,header.data(),header.size()); std::copy(header.begin()+440,header.begin()+444,signature.begin());
   }
   append(devicePath,number,4); append(devicePath,plan.efi.start/sector,8); append(devicePath,plan.efi.bytes/sector,8);
   devicePath.insert(devicePath.end(),signature.begin(),signature.end()); devicePath.push_back(gpt?2:1); devicePath.push_back(gpt?2:1);
   devicePath.push_back(4); devicePath.push_back(4); append(devicePath,4+(path.size()+1)*2,2); unicode(devicePath,path);
   devicePath.insert(devicePath.end(),{0x7f,0xff,4,0});
   std::vector<unsigned char> option; append(option,1,4); append(option,devicePath.size(),2); unicode(option,"Slax Linux"); option.insert(option.end(),devicePath.begin(),devicePath.end());
   std::vector<unsigned char> order; append(order,plan.bootNumber,2); order.insert(order.end(),plan.bootOrder.begin(),plan.bootOrder.end());
   {
      FileTree tree(plan.efi,true); FILE *file=tree.open(journal,true);
      std::string backup="Slax installer UEFI recovery record\nOriginal BootOrder: "+hex(plan.bootOrder)+"\nAdded variable: "+name+"\nAdded value: "+hex(option)+"\nWindows and all other original boot variables were retained.\n";
      bool written=fwrite(backup.data(),1,backup.size(),file)==backup.size();
      try { if (!written) throw std::runtime_error("Cannot save the UEFI recovery record."); flushFile(file); } catch (...) { fclose(file); throw; }
      fclose(file); tree.sync();
   }
   try
   {
      writeFirmware(name,option);
      if (readFirmware(name)!=option || readFirmware("BootOrder")!=plan.bootOrder) throw std::runtime_error("The firmware did not retain the new boot entry consistently.");
      writeFirmware("BootOrder",order);
      if (readFirmware("BootOrder")!=order) throw std::runtime_error("The firmware did not retain the new boot order.");
   }
   catch (const std::exception &error)
   {
      std::string recovery;
      try
      {
         auto current=readFirmware("BootOrder");
         if (current==order) { writeFirmware("BootOrder",plan.bootOrder); if (readFirmware("BootOrder")!=plan.bootOrder) throw std::runtime_error("The firmware did not restore the original boot order."); }
         else if (current!=plan.bootOrder) throw std::runtime_error("The firmware boot order changed independently; it was left untouched.");
         if (readFirmware(name)==option) writeFirmware(name,{});
      }
      catch (const std::exception &failure) { recovery="\nFirmware recovery: "+std::string(failure.what())+" The original boot loaders and their entries remain present. The recovery record is in "+journal+" on the EFI partition."; }
      throw std::runtime_error(std::string(error.what())+recovery);
   }
}
