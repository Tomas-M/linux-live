#include "disk-format.h"
#include <codecvt>
#include <locale>
#include <cstring>

// Keep configuration comparisons ASCII-only while preserving all original international text.
static std::string folded(std::string text)
{
   for (char &c:text) if (c>='A' && c<='Z') c+=32;
   return text;
}

// Boot configurations are small regular files opened through their retained filesystem root.
static std::string readConfig(FileTree &tree,const std::string &path)
{
   if (!tree.exists(path)) return "";
   FILE *file=tree.open(path,false); std::string text; char buffer[4096]; size_t bytes;
   while ((bytes=fread(buffer,1,sizeof(buffer),file)))
   {
      text.append(buffer,bytes);
      if (text.size()>1024*1024) { fclose(file); throw std::runtime_error("The existing boot configuration is unexpectedly large."); }
   }
   bool error=ferror(file); fclose(file);
   if (error) throw std::runtime_error("Cannot read the existing boot configuration.");
   return text;
}

// Verify every new boot file before it can become reachable from the original system menu.
static void writeConfig(FileTree &tree,const std::string &path,const std::string &text)
{
   FILE *file=tree.open(path,true);
   try
   {
      if (fwrite(text.data(),1,text.size(),file)!=text.size()) throw std::runtime_error("Cannot write the new boot configuration.");
      flushFile(file);
   }
   catch (...) { fclose(file); tree.remove(path,false); throw; }
   fclose(file);
   if (readConfig(tree,path)!=text) { tree.remove(path,false); throw std::runtime_error("The new boot configuration failed read-back verification."); }
}

// Add an NTLDR boot-sector file entry without rewriting existing entries or the default system.
std::string ntldrMenu(const std::string &original,const std::string &file)
{
   bool utf16=original.size()>=2 && (original.substr(0,2)=="\xff\xfe" || original.substr(0,2)=="\xfe\xff");
   bool little=utf16 && static_cast<unsigned char>(original[0])==0xff;
   std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> converter;
   std::string text=original;
   if (utf16)
   {
      if (original.size()%2) throw std::runtime_error("The existing boot.ini has invalid UTF-16 text.");
      std::u16string wide;
      for (size_t i=2; i<original.size(); i+=2) wide+=char16_t((unsigned char)original[i+(little?0:1)]|((unsigned char)original[i+(little?1:0)]<<8));
      text=converter.to_bytes(wide);
   }
   std::string newline=text.find("\r\n")!=std::string::npos?"\r\n":"\n",out,section;
   bool defaults=false,systems=false,inserted=false; unsigned entries=0;
   for (size_t start=0; start<text.size();)
   {
      size_t end=text.find('\n',start); if (end==std::string::npos) end=text.size(); else end++;
      auto line=text.substr(start,end-start),key=folded(line); start=end;
      size_t first=key.find_first_not_of(" \t\r\n"); if (first==std::string::npos) { out+=line; continue; }
      auto clean=key.substr(first); if (clean.substr(0,3)=="\xef\xbb\xbf") clean.erase(0,3);
      if (clean.empty()) { out+=line; continue; }
      if (clean.front()=='[')
      {
         if (section=="operating systems" && !inserted) { if (!out.empty() && out.back()!='\n') out+=newline; out+="C:\\"+file+"=\"Slax Linux\""+newline; inserted=true; }
         auto close=clean.find(']'); if (close==std::string::npos) throw std::runtime_error("The existing boot.ini has an invalid section.");
         section=clean.substr(1,close-1); if (section=="operating systems") { if (systems) throw std::runtime_error("The existing boot.ini has duplicate operating-system sections."); systems=true; }
      }
      else if (clean.front()!=';' && clean.front()!='#')
      {
         auto equal=clean.find('=');
         if (equal!=std::string::npos)
         {
            auto name=clean.substr(0,equal); name.erase(name.find_last_not_of(" \t")+1);
            if (section=="boot loader" && name=="default") defaults=true;
            if (section=="boot loader" && name=="timeout")
            {
               auto value=clean.substr(equal+1); size_t at=value.find_first_not_of(" \t");
               if (at!=std::string::npos && value[at]=='0' && value.find_first_not_of("0 \t\r\n",at)==std::string::npos) line=line.substr(0,line.find('=')+1)+"10"+newline;
            }
            if (section=="operating systems") entries++;
         }
      }
      out+=line;
   }
   if (!defaults || !systems || !entries) throw std::runtime_error("The existing boot.ini is not a supported NTLDR boot menu.");
   if (!inserted) { if (!out.empty() && out.back()!='\n') out+=newline; out+="C:\\"+file+"=\"Slax Linux\""+newline; }
   if (!utf16) return out;
   auto wide=converter.from_bytes(out); out=original.substr(0,2);
   for (char16_t c:wide) { out+=char(little?c&255:c>>8); out+=char(little?c>>8:c&255); }
   return out;
}

// Recognize only an existing GRUB's standard optional custom.cfg hook.
bool grubBootMenu(const InstallPlan &plan) { return plan.bootConfig=="boot/grub/custom.cfg" || plan.bootConfig=="boot/grub2/custom.cfg"; }

// A BIOS GRUB hook must not suppress an unrelated Windows UEFI boot path.
bool grubEfiMenu(const InstallPlan &plan)
{
   auto path=folded(plan.efiFirst);
   return grubBootMenu(plan) && (path.find("grub")!=std::string::npos || path.find("shim")!=std::string::npos);
}

// Prefer an unambiguous original menu; unsupported loaders stay behind the text selector.
void planBootMenu(InstallPlan &plan)
{
   if (plan.erase || !plan.device.number) return;
   const auto &mbr=plan.bootLayout->originalMbr;
   bool grub=mbr.find("GRUB")!=std::string::npos;
   bool microsoft=mbr.compare(0,6,"\x33\xc0\x8e\xd0\xbc\x00",6)==0 || mbr.compare(0,7,"\xfa\x33\xc0\x8e\xd0\xbc\x00",7)==0;
   bool activeRead=false,legacy=false;
   for (const auto &loader:plan.efiLoaders) if (folded(loader.second).find("grub")!=std::string::npos || folded(loader.second).find("shim")!=std::string::npos) grub=true;
   std::vector<InstallPlan> found;
   for (const auto &part:plan.device.partitions)
   {
      if (part.readonly || part.filesystem.empty()) continue;
      try
      {
         FileTree root(part,false); InstallPlan choice; choice.bootPartition=part;
         bool active=!plan.bootLayout->gpt && part.number>=1 && part.number<=4 && (static_cast<unsigned char>(mbr[446+(part.number-1)*16])&0x80);
         if (active)
         {
            activeRead=true;
            for (const char *file:{"ntldr","bootmgr","io.sys","ldlinux.sys","boot/grub/grub.cfg","boot/grub2/grub.cfg"}) if (root.exists(file)) legacy=true;
         }
         if (active && plan.bios && !grub)
         {
            if (root.exists("ntldr") && root.exists("boot.ini")) { choice.bootConfig="boot.ini"; choice.bootBefore=readConfig(root,"boot.ini"); }
            else if (root.exists("bootmgr") && root.exists("Boot/BCD")) { choice.bootBefore=readBcd(part); if (!choice.bootBefore.empty()) choice.bootConfig="Boot/BCD"; }
            if (!choice.bootConfig.empty())
            {
               for (unsigned i=0; i<10000; i++)
               {
                  char name[13]; if (!i) strcpy(name,"slaxboot.bin"); else snprintf(name,sizeof(name),"slax%04u.bin",i);
                  if (!root.exists(name)) { choice.bootFile=name; break; }
               }
               if (choice.bootFile.empty()) throw std::runtime_error("No free Slax boot-sector filename is available.");
               if (choice.bootConfig=="boot.ini") ntldrMenu(choice.bootBefore,choice.bootFile);
               found.push_back(std::move(choice)); continue;
            }
         }
         if (!grub || (!plan.biosImage && !plan.efi.bytes)) continue;
         for (const char *folder:{"boot/grub/","boot/grub2/"})
         {
            auto config=readConfig(root,std::string(folder)+"grub.cfg"); bool hook=false;
            for (const char *source:{"source $prefix/custom.cfg","source ${prefix}/custom.cfg","source $config_directory/custom.cfg","source ${config_directory}/custom.cfg"})
            {
               size_t at=config.find(source); if (at==std::string::npos) continue;
               size_t line=config.rfind('\n',at); line=line==std::string::npos?0:line+1;
               if (config.find_first_not_of(" \t",line)==at) hook=true;
            }
            if (!hook) continue;
            choice.bootConfig=std::string(folder)+"custom.cfg"; choice.bootBefore=readConfig(root,choice.bootConfig); found.push_back(choice);
         }
      }
      catch (const std::exception &) { continue; }
   }
   // A standard Windows partition bootstrap on an ordinary data disk is not an installed OS.
   if (microsoft && !legacy && (activeRead || plan.bootLayout->gpt)) plan.previousBios.clear();
   if (found.size()!=1) return;
   plan.bootPartition=found[0].bootPartition; plan.bootConfig=found[0].bootConfig; plan.bootBefore=found[0].bootBefore; plan.bootFile=found[0].bootFile;
   if (grubEfiMenu(plan)) plan.registerBoot=false;
}

// Publish the last activation step only after payload, firmware files and BIOS core are durable.
void publishBootMenu(const InstallPlan &plan,const std::string &vendor)
{
   if (plan.bootConfig.empty()) return;
   FileTree root(plan.bootPartition,true),target(plan.device,true);
   std::string before=plan.bootConfig=="Boot/BCD"?readBcd(plan.bootPartition):readConfig(root,plan.bootConfig);
   if (before!=plan.bootBefore) throw std::runtime_error("The original boot menu changed after confirmation; it was kept untouched.");
   if (!target.exists("slax/boot/grub/boot-backup")) target.mkdir("slax/boot/grub/boot-backup");
   writeConfig(target,"slax/boot/grub/boot-backup/original-menu.txt",plan.bootBefore);
   if (!grubBootMenu(plan))
   {
      writeConfig(root,plan.bootFile,biosBootSector(*plan.biosImage,plan.bootLayout->bios.start)); root.sync();
      if (plan.bootConfig=="Boot/BCD")
      {
         try { addBcdEntry(plan); } catch (...) { root.remove(plan.bootFile,false); throw; }
         return;
      }
   }
   std::string after;
   if (plan.bootConfig=="boot.ini") after=ntldrMenu(plan.bootBefore,plan.bootFile);
   else
   {
      after=plan.bootBefore+"\n# Slax Installer entry\nmenuentry 'Slax Linux' {\n";
      if (plan.efi.bytes)
         after+="   if [ \"$grub_platform\" = efi ]; then\n      search --no-floppy --fs-uuid --set=root "+plan.efi.uuid+"\n      chainloader /"+vendor+"/BOOTX64.EFI\n   else\n";
      after+="      search --no-floppy --fs-uuid --set=root "+plan.device.uuid+"\n      configfile /slax/boot/grub.cfg\n";
      if (plan.efi.bytes) after+="   fi\n";
      after+="}\n";
   }
   std::string temporary=plan.bootConfig+".slax-"+randomId();
   try
   {
      writeConfig(root,temporary,after);
      if (root.exists(plan.bootConfig)) root.replace(temporary,plan.bootConfig); else root.move(temporary,plan.bootConfig);
      if (readConfig(root,plan.bootConfig)!=after) throw std::runtime_error("The updated boot menu failed read-back verification.");
      root.sync();
   }
   catch (...) { root.remove(temporary,false); throw; }
}
