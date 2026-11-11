#include "disk-format.h"
#include <cstring>
#include <codecvt>
#include <locale>
#include <filesystem>

// Read only regular, rooted configuration files when recognizing an earlier Slax installation.
static std::string efiConfig(FileTree &tree,const std::string &path)
{
   if (!tree.exists(path)) return "";
   std::unique_ptr<FILE,int (*)(FILE *)> file(tree.open(path,false),fclose);
   std::string text(1024*1024,'\0'); size_t bytes=fread(text.data(),1,text.size(),file.get());
   if (ferror(file.get()) || bytes==text.size()) throw std::runtime_error("An existing EFI configuration cannot be read safely.");
   text.resize(bytes); return text;
}

// A leftover Slax configuration alone must never identify another vendor's fallback loader as ours.
static bool sameEfiFile(FileTree &tree,const std::string &left,const std::string &right)
{
   if (!tree.exists(left) || !tree.exists(right)) return false;
   std::unique_ptr<FILE,int (*)(FILE *)> first(tree.open(left,false),fclose),second(tree.open(right,false),fclose);
   unsigned char a[65536],b[65536]; size_t total=0;
   while (true)
   {
      size_t bytes=fread(a,1,sizeof(a),first.get()),other=fread(b,1,sizeof(b),second.get());
      if (ferror(first.get()) || ferror(second.get())) throw std::runtime_error("An existing EFI loader cannot be read safely.");
      total+=bytes;
      if (bytes!=other || memcmp(a,b,bytes) || total>32*1024*1024) return false;
      if (bytes<sizeof(a)) return total!=0;
   }
}

// Refresh only Slax vendor directories whose copied configuration explicitly targets this filesystem.
void readSlaxEfi(InstallPlan &plan)
{
   if (!plan.overwrite || !plan.efi.bytes || (plan.bootLayout && plan.bootLayout->efi.bytes)) return;
   FileTree tree(plan.efi,false);
   if (!tree.exists("EFI/Slax") || !tree.isDirectory("EFI") || !tree.isDirectory("EFI/Slax")) return;
   auto root=std::filesystem::u8path(tree.rootPath());
   for (const auto &entry:std::filesystem::directory_iterator(root/"EFI"/"Slax"))
   {
      auto name=entry.path().filename().u8string();
      if (name.size()!=32) continue;
      if (!std::all_of(name.begin(),name.end(),[](unsigned char c) { return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'); })) continue;
      auto vendor="EFI/Slax/"+name;
      if (!tree.isDirectory(vendor)) continue;
      bool complete=true;
      for (const char *file:{"BOOTX64.EFI","grubx64.efi","grub.cfg","boot/grub.cfg"})
         if (!tree.exists(vendor+"/"+file) || tree.isDirectory(vendor+"/"+file)) complete=false;
      if (!complete) continue;
      auto config="\n"+efiConfig(tree,vendor+"/boot/grub.cfg");
      config.erase(std::remove(config.begin(),config.end(),'\r'),config.end());
      if (config.find("\nset bootuuid="+plan.device.uuid+"\n")==std::string::npos) continue;
      std::string bootstrap="set slax_path=$cmdpath\nif [ -n \"$fw_path\" ]; then set slax_path=$fw_path; fi\nif regexp --set=1:slax_efi '^\\(([^)]+)\\)' \"$slax_path\"; then set root=$slax_efi; fi\nset prefix=($root)/"+vendor+"/boot/grub\nconfigfile /"+vendor+"/boot/grub/menu.cfg\n";
      if (efiConfig(tree,vendor+"/grub.cfg")!=bootstrap) continue;
      plan.efiVendors.push_back(vendor);
      bool fallback=efiConfig(tree,"EFI/BOOT/grub.cfg")==bootstrap;
      if (fallback && sameEfiFile(tree,"EFI/BOOT/BOOTX64.EFI",vendor+"/BOOTX64.EFI") && sameEfiFile(tree,"EFI/BOOT/grubx64.efi",vendor+"/grubx64.efi")) plan.efiFallback=vendor;
   }
   std::sort(plan.efiVendors.begin(),plan.efiVendors.end());
}

// Retain ordinary OS loaders on this ESP as menu choices, matching UEFI's partition identity and path.
void readEfiLoaders(InstallPlan &plan)
{
   if (plan.bootOrder.empty() || (plan.bootLayout && plan.bootLayout->efi.bytes)) return;
   FileTree esp(plan.efi,false); uint32_t sector=plan.device.sector;
   std::vector<unsigned char> header(sector),signature(16); plan.disk->read(sector,header.data(),sector);
   bool gpt=!memcmp(header.data(),"EFI PART",8);
   if (gpt)
   {
      uint64_t at=disk64(header.data()+72)*sector+uint64_t(plan.efi.number-1)*128;
      std::vector<unsigned char> entries(sector); plan.disk->read(at/sector*sector,entries.data(),entries.size());
      auto entry=entries.data()+at%sector; std::copy(entry+16,entry+32,signature.begin());
   }
   else { plan.disk->read(0,header.data(),sector); std::copy(header.begin()+440,header.begin()+444,signature.begin()); }
   bool slaxDefault=false;
   for (size_t n=0; n<plan.bootOrder.size(); n+=2)
   {
      char name[16]; snprintf(name,sizeof(name),"Boot%04X",plan.bootOrder[n]|plan.bootOrder[n+1]<<8);
      auto value=readFirmware(name);
      if (value.size()<8 || !(disk32(value.data())&1) || (disk32(value.data())&0x1f08)) continue;
      std::u16string title,path; size_t at=6;
      while (at+2<=value.size() && (value[at] || value[at+1])) { title+=char16_t(value[at]|value[at+1]<<8); at+=2; }
      at+=2; size_t bytes=value[4]|value[5]<<8;
      if (at>value.size() || bytes>value.size()-at) continue;
      bool options=at+bytes!=value.size();
      size_t end=at+bytes; bool matches=false,complete=false;
      while (at+4<=end)
      {
         auto node=value.data()+at; size_t length=node[2]|node[3]<<8;
         if (length<4 || length>end-at) break;
         if (node[0]==4 && node[1]==1 && length==42)
         {
            bool geometry=disk32(node+4)==plan.efi.number && disk64(node+8)*sector==plan.efi.start && disk64(node+16)*sector==plan.efi.bytes;
            matches=geometry && node[41]==(gpt?2:1) && !memcmp(node+24,signature.data(),gpt?16:4);
         }
         if (node[0]==4 && node[1]==4 && !(length%2))
            for (size_t i=4; i+2<=length && (node[i] || node[i+1]); i+=2) path+=char16_t(node[i]|node[i+1]<<8);
         if (node[0]==0x7f) { complete=node[1]==0xff && length==4 && at+4==end; break; }
         at+=length;
      }
      if (!matches || !complete || path.empty()) continue;
      std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> utf8;
      std::string file,label;
      try { file=utf8.to_bytes(path); label=utf8.to_bytes(title); } catch (...) { continue; }
      std::replace(file.begin(),file.end(),'\\','/'); auto key=filenameKey(file);
      bool slax=std::any_of(plan.efiVendors.begin(),plan.efiVendors.end(),[&](const std::string &vendor) { return key==filenameKey("/"+vendor+"/BOOTX64.EFI"); });
      if (slax && !options) { if (plan.efiBootEntry.empty()) plan.efiBootEntry=name; if (!n) slaxDefault=true; continue; }
      bool vendor=key.find(filenameKey("/EFI/"))==0 && key.find(filenameKey("/EFI/Slax/"))!=0 && key.find("/../")==std::string::npos;
      bool replacedFallback=plan.fallback && key.find(filenameKey("/EFI/BOOT/"))==0;
      bool controls=std::any_of(file.begin(),file.end(),[](unsigned char c) { return c<32; });
      if (!vendor || replacedFallback || controls || !esp.exists(file.substr(1))) continue;
      bool windows=key==filenameKey(plan.windowsLoader);
      if (options && !windows) continue; // Other binary load options still need their original firmware entry.
      if (!n || (slaxDefault && plan.efiFirst.empty())) plan.efiFirst=file;
      if (windows) continue;
      bool duplicate=std::any_of(plan.efiLoaders.begin(),plan.efiLoaders.end(),[&](const auto &entry) { return filenameKey(entry.second)==key; });
      if (!duplicate) plan.efiLoaders.push_back({label.empty()?"Previous operating system":label,file});
   }
}

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
   std::vector<unsigned char> order;
   if (!plan.efiFirst.empty()) append(order,plan.bootNumber,2);
   order.insert(order.end(),plan.bootOrder.begin(),plan.bootOrder.end());
   if (plan.efiFirst.empty()) append(order,plan.bootNumber,2);
   {
      FileTree tree(plan.device,true); FILE *file=tree.open(journal,true);
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
