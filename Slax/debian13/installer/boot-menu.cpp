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

// Normalize a Windows boot menu for recognition without changing its original encoding on disk.
static std::string bootText(const std::string &text)
{
   if (text.size()<2 || (text.substr(0,2)!="\xff\xfe" && text.substr(0,2)!="\xfe\xff")) return text;
   if (text.size()%2) throw std::runtime_error("The existing boot menu has invalid UTF-16 text.");
   bool little=static_cast<unsigned char>(text[0])==0xff; std::u16string wide;
   for (size_t i=2; i<text.size(); i+=2) wide+=char16_t((unsigned char)text[i+(little?0:1)]|((unsigned char)text[i+(little?1:0)]<<8));
   return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t>().to_bytes(wide);
}

// Recognize only our named native-menu paths, independent of translated BCD field labels.
static std::vector<std::string> nativeBootFiles(const std::string &menu,bool ntldr)
{
   auto text=folded(bootText(menu)); text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());
   std::vector<std::string> files;
   for (size_t at=text.find("\\slax"); at!=std::string::npos; at=text.find("\\slax",at+1))
   {
      auto end=text.find_first_of(" \t\n=\"",at+1); if (end==std::string::npos) end=text.size();
      auto file=text.substr(at+1,end-at-1);
      bool numbered=file.size()==12 && file.substr(0,4)=="slax" && file.substr(8)==".bin" && std::all_of(file.begin()+4,file.begin()+8,[](char c) { return c>='0' && c<='9'; });
      if (file!="slaxboot.bin" && !numbered) continue;
      auto first=text.rfind(ntldr?"\n":"\n\n",at),last=text.find(ntldr?"\n":"\n\n",end);
      first=first==std::string::npos?0:first+1; if (last==std::string::npos) last=text.size();
      auto block=text.substr(first,last-first);
      if (block.find(ntldr?"\"slax linux\"":"slax linux")==std::string::npos) continue;
      if (ntldr)
      {
         auto section=text.rfind('[',at),line=text.find_first_not_of(" \t",first);
         if (section==std::string::npos || text.compare(section,19,"[operating systems]") || at<2 || line!=at-2 || text.compare(at-2,2,"c:")) continue;
      }
      else
      {
         if (at && text[at-1]!=' ' && text[at-1]!='\t' && text[at-1]!='\n') continue;
         auto id=block.find('{'),manager=text.find("{9dea862c-5cdd-4e70-acc1-f32b344d4795}");
         while (manager!=std::string::npos)
         {
            auto start=text.rfind("\n\n",manager); start=start==std::string::npos?0:start+2;
            if (text.find('{',start)==manager) break;
            manager=text.find("{9dea862c-5cdd-4e70-acc1-f32b344d4795}",manager+1);
         }
         if (id==std::string::npos || block.size()-id<38 || block[id+37]!='}' || manager==std::string::npos) continue;
         auto managerEnd=text.find("\n\n",manager); if (managerEnd==std::string::npos) managerEnd=text.size();
         if (text.substr(manager,managerEnd-manager).find(block.substr(id,38))==std::string::npos) continue;
      }
      if (std::find(files.begin(),files.end(),file)==files.end()) files.push_back(file);
   }
   return files;
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
   std::string text=bootText(original);
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
            if (section=="operating systems")
            {
               entries++;
               if (name=="c:\\"+folded(file) && clean.find("\"slax linux\"",equal+1)!=std::string::npos) inserted=true;
            }
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

// An existing Slax selector must keep chainloading its original saved MBR, never the current Slax MBR.
static void retainBiosMenu(InstallPlan &plan)
{
   if (!plan.overwrite || !plan.bios || !plan.biosImage || plan.bootLayout->biosBefore.empty()) return;
   FileTree target(plan.device,false); auto text=readConfig(target,"slax/boot/grub/grub.cfg");
   // Ordinary Slax boot files are replaced; retain only our selector for a previous operating system.
   auto marker="\n   chainloader /slax/boot/grub/boot-backup/"; auto at=text.find(marker);
   if (at==std::string::npos) return;
   auto boot=biosBootSector(*plan.biosImage,plan.bootLayout->bios.start); auto layout=*plan.bootLayout;
   bool own=plan.bootLayout->originalMbr.compare(0,440,boot,0,440)==0 && reuseBios(*plan.disk,layout,*plan.biosImage,boot);
   auto search="\n   search --no-floppy --fs-uuid --set=root "+plan.device.uuid+"\n   drivemap -s (hd0) $root\n";
   bool targetMenu=text.find(search)!=std::string::npos && text.find("\n   configfile /slax/boot/grub.cfg\n")!=std::string::npos;
   if (!own || !targetMenu) throw std::runtime_error("The existing Slax BIOS selector cannot be safely recognized. Its original operating-system choices were kept; repair its boot menu before updating.");
   at+=strlen("\n   chainloader /"); auto end=text.find('\n',at);
   auto path=text.substr(at,end-at);
   bool safe=path.find("/../")==std::string::npos && path.find("/./")==std::string::npos && path.find("//")==std::string::npos;
   safe=safe && path.size()>=16 && path.substr(path.size()-16)=="original-mbr.bin";
   for (unsigned char c:path)
   {
      bool letter=(c>='a' && c<='z') || (c>='A' && c<='Z'),digit=c>='0' && c<='9';
      if (!letter && !digit && std::string("/-._").find(c)==std::string::npos) safe=false;
   }
   if (!safe) throw std::runtime_error("The existing Slax BIOS selector has an unsafe recovery path. Its boot files were kept untouched.");
   auto original=readConfig(target,path);
   bool signature=original.size()==512 && static_cast<unsigned char>(original[510])==0x55 && static_cast<unsigned char>(original[511])==0xaa;
   if (!signature || original.compare(0,440,plan.bootLayout->originalMbr,0,440)==0) throw std::runtime_error("The original BIOS loader saved by Slax is missing or invalid. Restore its recovery backup before updating this installation.");
   plan.biosMenu=text;
   if (!plan.bootWarning.empty()) plan.bootWarning+=' ';
   plan.bootWarning+="Keep the existing BIOS system-selection menu and its original operating-system choices.";
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
         if (active && plan.biosImage && !grub)
         {
            if (root.exists("ntldr") && root.exists("boot.ini")) { choice.bootConfig="boot.ini"; choice.bootBefore=readConfig(root,"boot.ini"); }
            else if (root.exists("bootmgr") && root.exists("Boot/BCD")) { choice.bootBefore=readBcd(part); if (!choice.bootBefore.empty()) choice.bootConfig="Boot/BCD"; }
            if (!choice.bootConfig.empty())
            {
               if (plan.overwrite) for (const auto &file:nativeBootFiles(choice.bootBefore,choice.bootConfig=="boot.ini"))
               {
                  auto before=readConfig(root,file);
                  auto layout=std::make_shared<BootLayout>(*plan.bootLayout);
                  if (reuseBios(*plan.disk,*layout,*plan.biosImage,before))
                  { choice.bootFile=file; choice.bootFileBefore=before; choice.bootLayout=std::move(layout); break; }
               }
               if (!plan.bios && choice.bootFileBefore.empty()) continue;
               for (unsigned i=0; choice.bootFile.empty() && i<10000; i++)
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
   if (found.size()!=1) { retainBiosMenu(plan); return; }
   plan.bootPartition=found[0].bootPartition; plan.bootConfig=found[0].bootConfig; plan.bootBefore=found[0].bootBefore; plan.bootFile=found[0].bootFile; plan.bootFileBefore=found[0].bootFileBefore;
   if (!plan.bootFileBefore.empty())
   {
      plan.bootLayout=found[0].bootLayout; plan.bios=true;
      if (plan.bootWarning.find("The BIOS loader needs ")==0) plan.bootWarning.clear();
   }
   if (grubEfiMenu(plan)) plan.registerBoot=false;
}

// Publish the last activation step only after payload, firmware files and BIOS core are durable.
void publishBootMenu(const InstallPlan &plan,const std::string &vendor)
{
   if (plan.bootConfig.empty()) return;
   FileTree root(plan.bootPartition,true),target(plan.device,true);
   std::string before=plan.bootConfig=="Boot/BCD"?readBcd(plan.bootPartition):readConfig(root,plan.bootConfig);
   if (before!=plan.bootBefore) throw std::runtime_error("The original boot menu changed after confirmation; it was kept untouched.");
   if (!target.exists(plan.recovery)) target.mkdir(plan.recovery);
   writeConfig(target,plan.recovery+"/original-menu.txt",plan.bootBefore);
   target.sync();
   if (!grubBootMenu(plan))
   {
      auto sector=biosBootSector(*plan.biosImage,plan.bootLayout->bios.start);
      if (plan.bootFileBefore.empty()) writeConfig(root,plan.bootFile,sector);
      else
      {
         if (readConfig(root,plan.bootFile)!=plan.bootFileBefore) throw std::runtime_error("The original Slax boot-sector file changed after confirmation; it was kept untouched.");
         if (sector!=plan.bootFileBefore) throw std::runtime_error("The existing Slax boot-sector file does not match the approved BIOS core.");
      }
      root.sync();
      if (plan.bootConfig=="Boot/BCD")
      {
         if (plan.bootFileBefore.empty()) try { addBcdEntry(plan); } catch (...) { root.remove(plan.bootFile,false); throw; }
         root.sync();
         return;
      }
   }
   std::string after;
   if (plan.bootConfig=="boot.ini") after=ntldrMenu(plan.bootBefore,plan.bootFile);
   else
   {
      std::string entry="\n# Slax Installer entry\nmenuentry 'Slax Linux' {\n";
      if (plan.efi.bytes)
         entry+="   if [ \"$grub_platform\" = efi ]; then\n      search --no-floppy --fs-uuid --set=root "+plan.efi.uuid+"\n      chainloader /"+vendor+"/BOOTX64.EFI\n   else\n";
      entry+="      search --no-floppy --fs-uuid --set=root "+plan.device.uuid+"\n      configfile /slax/boot/grub.cfg\n";
      if (plan.efi.bytes) entry+="   fi\n";
      entry+="}\n";
      after=plan.bootBefore; bool replaced=false;
      if (plan.overwrite) for (size_t at=after.find("\n# Slax Installer entry\n"); at!=std::string::npos; at=after.find("\n# Slax Installer entry\n",at+1))
      {
         auto end=after.find("\n}\n",at); if (end==std::string::npos) break;
         auto block=after.substr(at,end+3-at);
         if (block.find("\n      search --no-floppy --fs-uuid --set=root "+plan.device.uuid+"\n      configfile /slax/boot/grub.cfg\n")==std::string::npos) continue;
         after.replace(at,end+3-at,entry); replaced=true; break;
      }
      if (!replaced) after+=entry;
   }
   if (after==plan.bootBefore) return;
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
