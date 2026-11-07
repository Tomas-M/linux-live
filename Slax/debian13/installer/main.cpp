#include "installer.h"
#include <chrono>
#include <cstring>

Installer app;

// Read little-endian ISO9660 fields without alignment or host-endian assumptions.
static uint32_t number(const unsigned char *p)
{
   return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}

// Find a directory entry, rejecting records outside the actual image and directory.
static bool isoEntry(FILE *file,uint64_t imageBytes,uint32_t sector,uint32_t length,const char *name,unsigned char *result)
{
   uint64_t start=uint64_t(sector)*2048;
   if (length>16*1024*1024 || start+length>imageBytes) return false;
   std::vector<unsigned char> directory(length);
#ifdef _WIN32
   if (_fseeki64(file,start,SEEK_SET)) return false;
#else
   if (fseeko(file,start,SEEK_SET)) return false;
#endif
   if (fread(directory.data(),1,length,file)!=length) return false;
   for (size_t at=0; at<length;)
   {
      const unsigned char *entry=directory.data()+at;
      if (!entry[0]) { at=(at/2048+1)*2048; continue; }
      if (entry[0]<34 || at+entry[0]>length || 33u+entry[32]>entry[0]) return false;
      std::string value(reinterpret_cast<const char *>(entry+33),entry[32]);
      auto version=value.find(';');
      if (version!=std::string::npos) value.resize(version);
      if (!value.empty() && value.back()=='.') value.pop_back();
      for (char &c:value) if (c>='a' && c<='z') c-=32;
      if (value==name)
      {
         memcpy(result,entry,entry[0]);
         return uint64_t(number(entry+2))*2048+number(entry+10)<=imageBytes;
      }
      at+=entry[0];
   }
   return false;
}

// Validate the actual Slax ISO tree using read-only file access; do not mount it.
Image inspectImage(const std::string &path)
{
   Image image;
   image.path=path;
   image.name=path.substr(path.find_last_of("/\\")+1);
   FILE *file=openImage(path);
   if (!file) { image.error="The ISO file cannot be opened. Select a readable Slax ISO image."; return image; }
#ifdef _WIN32
   _fseeki64(file,0,SEEK_END);
   auto length=_ftelli64(file);
#else
   fseeko(file,0,SEEK_END);
   auto length=ftello(file);
#endif
   image.bytes=length>0?uint64_t(length):0;
   unsigned char pvd[2048]={},slax[256]={},boot[256]={},modules[256]={},kernel[256]={},initrd[256]={},core[256]={};
   bool valid=image.bytes>=17*2048;
   if (valid)
   {
      fseek(file,16*2048,SEEK_SET);
      valid=fread(pvd,1,sizeof(pvd),file)==sizeof(pvd) && pvd[0]==1 && !memcmp(pvd+1,"CD001",5) && pvd[6]==1;
   }
   if (valid) valid=pvd[156]>=34 && pvd[128]==0 && pvd[129]==8;
   if (valid) valid=number(pvd+80)>=17 && uint64_t(number(pvd+80))*2048<=image.bytes;
   if (valid) valid=isoEntry(file,image.bytes,number(pvd+158),number(pvd+166),"SLAX",slax) && (slax[25]&2);
   if (valid) valid=isoEntry(file,image.bytes,number(slax+2),number(slax+10),"BOOT",boot) && (boot[25]&2);
   if (valid) valid=isoEntry(file,image.bytes,number(slax+2),number(slax+10),"MODULES",modules) && (modules[25]&2);
   if (valid) valid=isoEntry(file,image.bytes,number(boot+2),number(boot+10),"VMLINUZ",kernel) && !(kernel[25]&2) && number(kernel+10)>0;
   if (valid) valid=isoEntry(file,image.bytes,number(boot+2),number(boot+10),"INITRFS.IMG",initrd) && !(initrd[25]&2) && number(initrd+10)>0;
   if (valid) valid=isoEntry(file,image.bytes,number(modules+2),number(modules+10),"01_CORE.SB",core) && !(core[25]&2) && number(core+10)>0;
   fclose(file);
   if (!valid) image.error="This file is not a complete Slax ISO image. Select an ISO containing slax/boot and slax/modules.";
   image.payload=image.bytes;
   return image;
}

// Format capacities in decimal GB and image sizes in binary MiB, as in the reference.
std::string sizeText(uint64_t value,bool binary)
{
   char text[80];
   if (binary) snprintf(text,sizeof(text),"%.0f MiB",double(value)/1048576);
   else snprintf(text,sizeof(text),value>=1000000000000ULL?"%.1f TB":"%.1f GB",double(value)/(value>=1000000000000ULL?1e12:1e9));
   return text;
}

// Emit escaped UTF-8 JSON for read-only discovery and automated platform checks.
static void json(const std::string &value)
{
   putchar('"');
   for (unsigned char c:value)
   {
      if (c=='"' || c=='\\') printf("\\%c",c);
      else if (c<32) printf("\\u%04x",c);
      else putchar(c);
   }
   putchar('"');
}

// Keep a diagnostic entry point that uses exactly the GUI's disk discovery.
void printDevices(const std::vector<Device> &devices)
{
   printf("[");
   for (size_t i=0; i<devices.size(); i++)
   {
      const auto &d=devices[i];
      if (i) printf(",");
      printf("{\"id\":"); json(d.id);
      printf(",\"path\":"); json(d.path);
      printf(",\"name\":"); json(d.name);
      printf(",\"label\":"); json(d.label);
      printf(",\"blocked\":"); json(d.blocked);
      printf(",\"bytes\":%llu,\"free\":%llu,\"fat\":%s}",static_cast<unsigned long long>(d.bytes),static_cast<unsigned long long>(d.free),d.fat?"true":"false");
   }
   puts("]");
}

// Discover and revalidate outside the GUI thread, including explicit Next requests.
void Installer::startScan()
{
   scanner=std::thread([this]
   {
      while (running)
      {
         std::string current;
         uint64_t version;
         { std::lock_guard<std::mutex> lock(mutex); current=source; version=revision; }
         auto result=scanDevices(current);
         Image image;
         if (!current.empty()) image=inspectImage(current);
         {
            std::lock_guard<std::mutex> lock(mutex);
            scanned=std::move(result); scannedIso=std::move(image);
            scannedRevision=version; scanReady=true;
         }
         for (int i=0; i<20 && running; i++)
         {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(mutex);
            if (revision!=version) break;
         }
      }
   });
}

// Invalidate older scans when the selected source or requested action changes.
void Installer::refresh(Action action)
{
   pending=action;
   std::lock_guard<std::mutex> lock(mutex);
   source=iso.path;
   revision++;
}

// Derive availability from fresh device facts and the current installation option.
std::string Installer::eligibility(const Device &device) const
{
   if (!device.blocked.empty()) return device.blocked;
   if (device.bytes<iso.payload+16*1024*1024) return "This drive is too small for the selected ISO.";
   if (!erase && !device.fat) return "Select erase to prepare this drive as FAT32.";
   if (!erase && device.free<iso.payload) return "Not enough free FAT32 space. Select erase or another drive.";
   return "";
}

// Apply only current scan results and keep selection attached to physical identity.
void Installer::tick()
{
   bool ready=false;
   Image checked;
   {
      std::lock_guard<std::mutex> lock(mutex);
      if (scanReady && scannedRevision==revision) { devices=scanned; checked=scannedIso; ready=true; }
      scanReady=false;
   }
   if (ready)
   {
      auto found=std::find_if(devices.begin(),devices.end(),[this](const Device &d) { return d.id==selected; });
      if (found==devices.end() && !selected.empty())
      {
         if (page==1) error="The selected USB drive was disconnected. Select a drive to continue.";
         selected.clear();
         confirming=false;
      }
      if (pending!=None)
      {
         Action action=pending;
         pending=None;
         if (page==0)
         {
            iso=checked; error=iso.error;
            if (error.empty() && action==Next) page=1;
         }
         else if (page==1)
         {
            if (found==devices.end()) error="The selected drive is no longer available. Select a USB drive.";
            else error=eligibility(*found);
            if (error.empty() && (!checked.error.empty() || checked.bytes!=iso.bytes))
            {
               error="The ISO image is unavailable or changed. Go back and select it again.";
               iso.error=error;
            }
            if (error.empty() && action==Next)
            {
               target=found->name+" ("+found->label+") · "+sizeText(found->bytes);
               targetId=found->id;
               confirming=true;
               invalidate();
               bool approved=confirmInstall(target,erase);
               confirming=false;
               if (approved) refresh(Confirm);
            }
            if (error.empty() && action==Confirm) { page=2; percent=0; }
         }
      }
      if (page==2)
      {
         auto writing=std::find_if(devices.begin(),devices.end(),[this](const Device &d) { return d.id==targetId; });
         if (writing==devices.end() || !eligibility(*writing).empty())
            error="The USB drive stopped responding during the simulation. Reconnect it and try again. No data was written.";
         else if (!checked.error.empty() || checked.bytes!=iso.bytes)
            error="The source ISO is no longer available. Select it again and retry. No data was written.";
         if (!error.empty()) page=3;
      }
      invalidate();
   }
   if (page==2)
   {
      percent=std::min(percent+1,100);
      if (percent==100) page=3;
      invalidate();
   }
}

// Route all mouse and keyboard actions through one wizard state machine.
void Installer::activate(Action action,const std::string &id)
{
   if (confirming || choosing) return;
   if (action==Close || action==Cancel) { windowAction(Close); return; }
   if (action==Back && page==1)
   {
      refresh(None);
      page=0; error=iso.error;
   }
   if (pending!=None) { invalidate(); return; }
   if (action==Browse && !choosing)
   {
      choosing=true;
      auto path=chooseImage();
      if (!path.empty())
      {
         iso={}; iso.path=path; iso.name=path.substr(path.find_last_of("/\\")+1);
         selected.clear(); error.clear();
         refresh(Browse);
      }
      choosing=false;
   }
   if (action==Erase)
   {
      erase=!erase; error.clear();
      for (const auto &device:devices) if (device.id==selected) error=eligibility(device);
   }
   if (action==Select)
   {
      auto device=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.id==id; });
      if (device!=devices.end()) { selected=id; error=eligibility(*device); }
   }
   if (action==Next)
   {
      if (page<2) { error.clear(); refresh(Next); }
      else if (page==3)
      {
         if (!error.empty()) { page=1; error.clear(); percent=0; }
         else windowAction(Close);
      }
   }
   invalidate();
}

// Join read-only discovery before releasing native window resources.
void Installer::stop()
{
   running=false;
   if (scanner.joinable()) scanner.join();
}

// Share page headings while each platform supplies its own native controls.
std::string Installer::heading() const
{
   const char *titles[]={"Choose an ISO file","Choose a USB drive","Installing Slax","Simulation complete"};
   if (page==3 && !error.empty()) return "Installation failed";
   return titles[page];
}

// Keep the simulation explicit in the final result and all progress phases.
std::string Installer::introduction() const
{
   const char *text[]={"Select the Slax ISO image you want to install.","Select the USB drive that will hold Slax.","Keep the USB drive connected during installation.","The installation was simulated successfully. No files or boot records were written to the USB drive."};
   if (page==3 && !error.empty()) return error;
   return text[page];
}

// Describe preparation according to the selected preserve-or-erase option.
std::string Installer::progressText() const
{
   if (percent<15) return erase?"Simulating drive preparation…":"Checking available space…";
   if (percent<80) return "Simulating file copying…";
   return "Simulating boot setup…";
}

// Keep image information, checking and validation feedback in one stable message slot.
std::string Installer::message() const
{
   if (pending!=None) return "Checking image and USB drive…";
   if (!error.empty()) return error;
   if (page==0 && !iso.path.empty() && iso.error.empty()) return iso.name+" · "+sizeText(iso.bytes,true);
   return "";
}

// Native default buttons derive their enabled state from the shared current facts.
bool Installer::canContinue() const
{
   if (pending!=None || confirming || choosing || page==2) return false;
   if (page==0) return !iso.path.empty() && iso.error.empty();
   if (page==1)
   {
      if (!iso.error.empty()) return false;
      for (const auto &device:devices) if (device.id==selected) return eligibility(device).empty();
      return false;
   }
   return true;
}

#if !defined(_WIN32) && !defined(INSTALLER_TEST)
// Linux diagnostics stay usable without an X server or a graphical session.
int main(int argc,char **argv)
{
   if ((argc==2 || argc==3) && !strcmp(argv[1],"--list-devices")) { printDevices(scanDevices(argc==3?argv[2]:"")); return 0; }
   if (argc==3 && !strcmp(argv[1],"--check-iso"))
   {
      auto image=inspectImage(argv[2]);
      if (!image.error.empty()) { fprintf(stderr,"%s\n",image.error.c_str()); return 1; }
      printf("%s · %s\n",image.name.c_str(),sizeText(image.bytes,true).c_str());
      return 0;
   }
   if (argc==2) { app.iso=inspectImage(argv[1]); app.source=app.iso.path; app.error=app.iso.error; }
   return runWindow();
}
#endif
