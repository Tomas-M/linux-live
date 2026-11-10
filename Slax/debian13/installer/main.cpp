#include "disk-format.h"
#include <chrono>
#include <cstring>

Installer app;

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

// Reuse Partition Manager's hard-drive, USB and virtual-storage artwork.
int deviceIcon(const Device &device)
{
   if (device.virtualDisk) return 2;
   return device.usb?1:0;
}

// Keep the native target identifier first in the metadata line, outside the icon.
std::string deviceLocation(const Device &device)
{
#ifdef _WIN32
   if (device.label.size()==2 && device.label[1]==':') return device.label+"/";
   if (!device.label.empty() && device.label[0]=='#') return device.label;
#endif
   return device.path;
}

// Filter and sort the menu while retaining every sibling in discovery and erase warnings.
std::vector<Device> visibleTargets(std::vector<Device> devices)
{
   devices.erase(std::remove_if(devices.begin(),devices.end(),[](const Device &device)
   {
      bool whole=device.number==0 && device.diskBytes!=0;
      bool largePartition=std::any_of(device.partitions.begin(),device.partitions.end(),[](const Partition &part) { return part.bytes>1000000000; });
      if (whole) return largePartition;
      if (device.bytes<=1000000000) return true;
      bool formattedPartition=std::any_of(device.partitions.begin(),device.partitions.end(),[](const Partition &part) { return part.bytes>1000000000 && !part.filesystem.empty(); });
      return device.filesystem.empty() && formattedPartition;
   }),devices.end());
   std::sort(devices.begin(),devices.end(),[](const Device &left,const Device &right)
   {
#ifdef _WIN32
      bool leftLetter=left.label.size()>1 && left.label[1]==':',rightLetter=right.label.size()>1 && right.label[1]==':';
      if (leftLetter!=rightLetter) return leftLetter;
      if (left.label!=right.label) return left.label<right.label;
#endif
      return left.path<right.path;
   });
   return devices;
}

// Read-only diagnostics expose the same target menu as both native frontends.
void printDevices(const std::vector<Device> &inventory)
{
   auto devices=visibleTargets(inventory);
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
      printf(",\"eraseBlocked\":"); json(d.eraseBlocked);
      printf(",\"filesystem\":"); json(d.filesystem);
      printf(",\"uuid\":"); json(d.uuid);
      printf(",\"disk\":"); json(d.disk);
      printf(",\"partition\":%u,\"bytes\":%llu,\"diskBytes\":%llu,\"free\":",d.number,static_cast<unsigned long long>(d.bytes),static_cast<unsigned long long>(d.diskBytes));
      if (d.free==UINT64_MAX) printf("null"); else printf("%llu",static_cast<unsigned long long>(d.free));
      putchar('}');
   }
   puts("]");
}

// Revalidate devices, ISO inventory and requested installation plans outside the GUI thread.
void Installer::startScan()
{
   running=true;
   scanner=std::thread([this]
   {
      while (running)
      {
         std::string current,requested; uint64_t version; bool eraseRequested;
         std::chrono::steady_clock::time_point checkAfter;
         { std::lock_guard<std::mutex> lock(mutex); current=source; version=revision; requested=requestId; eraseRequested=requestErase; checkAfter=sourceCheckAfter; }
         // Wait briefly after typing and reread the revision before accessing a source.
         if (std::chrono::steady_clock::now()<checkAfter) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
         auto result=scanDevices(current); Image image;
         if (!current.empty()) image=inspectImage(current);
         std::shared_ptr<InstallPlan> prepared; std::string failure;
         if (!requested.empty())
         {
            try
            {
               auto found=std::find_if(result.begin(),result.end(),[&](const Device &d) { return d.id==requested; });
               if (found==result.end()) throw std::runtime_error("The selected target is no longer connected.");
               prepared=std::make_shared<InstallPlan>(prepareInstall(*found,image,eraseRequested));
            }
            catch (const std::exception &error) { failure=error.what(); }
         }
         {
            std::lock_guard<std::mutex> lock(mutex);
            scanned=std::move(result); scannedIso=std::move(image); scannedPlan=std::move(prepared); scanError=failure;
            scannedRevision=version; scanReady=true;
         }
         for (int i=0; i<20 && running; i++)
         {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(mutex); if (revision!=version) break;
         }
      }
   });
}

// Version each requested plan so a stale discovery result can never authorize writes.
void Installer::refresh(Action action,bool debounce)
{
   pending=action;
   std::lock_guard<std::mutex> lock(mutex);
   bool checkingSource=action==Next || action==CheckSource;
   source=page<=SourcePage && !checkingSource?"":iso.path; requestId=page==TargetPage && (action==Next || action==Confirm)?selected:"";
   sourceCheckAfter=debounce?std::chrono::steady_clock::now()+std::chrono::milliseconds(300):std::chrono::steady_clock::time_point{};
   requestErase=erase; revision++;
}

// Check chosen sources immediately and debounce typing while invalidating old results.
void Installer::setImagePath(const std::string &path,bool immediate)
{
   if (page>SourcePage || (pending!=None && pending!=CheckSource)) return;
   if (iso.path==path && !immediate) return;
   iso={}; iso.path=path; selected.clear(); error.clear(); plan.reset();
   refresh(path.empty()?None:CheckSource,!immediate); invalidate();
}

// Whole-disk highlighting is derived from the last clicked row, never a second selection list.
bool Installer::isSelected(const Device &device) const
{
   if (!erase || device.diskId.empty()) return device.id==selected;
   auto clicked=std::find_if(devices.begin(),devices.end(),[this](const Device &d) { return d.id==selected; });
   return clicked!=devices.end() && device.diskId==clicked->diskId;
}

// Consume worker results on the GUI thread and launch only the exact plan the user approved.
void Installer::tick()
{
   if (page==InstallationPage)
   {
      bool done;
      { std::lock_guard<std::mutex> lock(mutex); percent=writtenPercent; done=writeDone; if (done) error=writeError; }
      if (done) { if (writer.joinable()) writer.join(); page=FinishPage; plan.reset(); }
      invalidate(); return;
   }
   bool ready=false; Image checked; std::shared_ptr<InstallPlan> prepared; std::string failure;
   {
      std::lock_guard<std::mutex> lock(mutex);
      if (scanReady && scannedRevision==revision)
      { devices=visibleTargets(std::move(scanned)); checked=std::move(scannedIso); prepared=std::move(scannedPlan); failure=scanError; ready=true; }
      scanReady=false;
   }
   if (!ready) return;
   auto found=std::find_if(devices.begin(),devices.end(),[this](const Device &d) { return d.id==selected; });
   if (found==devices.end() && !selected.empty()) selected.clear();
   if (pending!=None)
   {
      Action action=pending; pending=None;
      { std::lock_guard<std::mutex> lock(mutex); requestId.clear(); }
      if (page<=SourcePage)
      {
         // Background checks update metadata without rewriting the path being edited.
         if (action==CheckSource) checked.path=iso.path;
         iso=checked; error=iso.error;
         if (page==WelcomePage || action==CheckSource) error.clear();
         if (!error.empty())
         {
            confirming=true; invalidate(); showInstallError(error+"\n\nCheck the source path or choose a source ISO image or Slax directory."); confirming=false; error.clear();
         }
         if (page==SourcePage && iso.error.empty() && action==Next) page=TargetPage;
         else refresh(None);
      }
      else if (page==TargetPage)
      {
         error=failure;
         if (error.empty() && !prepared) error="The target could not be prepared. Select the image and target again.";
         if (!error.empty())
         {
            confirming=true; invalidate(); showInstallError(error); confirming=false; error.clear(); plan.reset();
         }
         else if (action==Next)
         {
            plan=prepared; target=found->name+" ("+found->label+") · "+sizeText(erase?found->diskBytes:found->bytes); targetId=found->id;
            confirming=true; invalidate(); bool approved=confirmInstall(plan->confirmation,erase); confirming=false;
            if (approved) refresh(Confirm); else plan.reset();
         }
         else if (error.empty() && action==Confirm)
         {
            bool sameDevice=plan && prepared->device.id==plan->device.id && prepared->device.layout==plan->device.layout && prepared->snapshot==plan->snapshot && prepared->device.uuid==plan->device.uuid;
            bool newEfi=plan && plan->bootLayout && plan->bootLayout->efi.bytes;
            bool sameEfi=plan && prepared->efi.start==plan->efi.start && prepared->efi.bytes==plan->efi.bytes && (newEfi || prepared->efi.uuid==plan->efi.uuid);
            bool sameBios=plan && prepared->bios==plan->bios && prepared->previousBios==plan->previousBios;
            if (sameBios && plan->bios && !plan->erase) sameBios=prepared->bootLayout->bios.start==plan->bootLayout->bios.start && prepared->biosImage->core==plan->biosImage->core && prepared->bootLayout->biosBefore==plan->bootLayout->biosBefore;
            bool sameFirmware=plan && prepared->bootOrder==plan->bootOrder && prepared->fallbackDigest==plan->fallbackDigest && prepared->efiLoaders==plan->efiLoaders && prepared->efiFirst==plan->efiFirst;
            bool sameSummary=plan && prepared->confirmation==plan->confirmation;
            bool sameMenu=plan && prepared->bootConfig==plan->bootConfig && prepared->bootBefore==plan->bootBefore && prepared->bootPartition.uuid==plan->bootPartition.uuid && prepared->bootFile==plan->bootFile;
            bool same=sameDevice && sameEfi && sameBios && sameFirmware && sameSummary && sameMenu;
            if (!same) { confirming=true; invalidate(); showInstallError("The installation plan changed after confirmation. Review and confirm the target again."); confirming=false; plan.reset(); }
            else
            {
               running=false;
               page=InstallationPage; percent=0;
               { std::lock_guard<std::mutex> lock(mutex); writeDone=false; writeError.clear(); writtenPercent=0; status="Preparing installation…"; }
               // Use the latest parsed ISO inventory when reopening the source for copying.
               plan=prepared; auto approved=plan;
               writer=std::thread([this,approved]
               {
                  // Let the window keep animating while the discovery worker finishes.
                  if (scanner.joinable()) scanner.join();
                  std::string failure;
                  try
                  {
                     install(*approved,[this](const std::string &text,int progress)
                     { std::lock_guard<std::mutex> lock(mutex); status=text; writtenPercent=progress; });
                  }
                  catch (const std::exception &error) { failure=error.what(); }
                  std::lock_guard<std::mutex> lock(mutex); writeError=failure; writeDone=true;
               });
            }
         }
      }
   }
   invalidate();
}

// Closing and navigation cannot interrupt an active writer or alter its immutable approved plan.
void Installer::activate(Action action,const std::string &id)
{
   if (confirming || choosing || page==InstallationPage) return;
   if (action==Close) { windowAction(Close); return; }
   // Welcome navigation never waits for discovery or permits a check to skip ahead.
   if (action==Next && page==WelcomePage) { page=SourcePage; invalidate(); return; }
   if (action==Back && page==SourcePage) { page=WelcomePage; invalidate(); return; }
   if (action==Back && page==TargetPage) { plan.reset(); page=SourcePage; refresh(None); error=iso.error; }
   bool browsing=action==Browse || action==BrowseDirectory;
   if (pending!=None && !(browsing && pending==CheckSource)) { invalidate(); return; }
   if (browsing)
   {
      choosing=true; auto path=chooseImage(action==BrowseDirectory);
      if (!path.empty()) setImagePath(path,true);
      choosing=false;
   }
   if (action==Erase)
   {
      erase=!erase; error.clear(); plan.reset();
   }
   if (action==Select)
   {
      auto found=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.id==id; });
      if (found!=devices.end()) { selected=id; error.clear(); plan.reset(); }
   }
   if (action==Next)
   {
      if (page<InstallationPage)
      {
         if (page==SourcePage && iso.path.size()>1 && iso.path.front()=='"' && iso.path.back()=='"') iso.path=iso.path.substr(1,iso.path.size()-2);
         if (page==TargetPage)
         {
            auto device=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.id==selected; });
            if (device!=devices.end()) target=device->name+" ("+device->label+") · "+sizeText(erase?device->diskBytes:device->bytes);
            percent=0;
         }
         error.clear(); refresh(Next);
      }
      else if (page==FinishPage)
      {
         if (!error.empty()) { page=TargetPage; error.clear(); percent=0; startScan(); refresh(None); }
         else windowAction(Close);
      }
   }
   invalidate();
}

// Join storage workers before releasing any native application resources.
void Installer::stop()
{
   running=false;
   if (writer.joinable()) writer.join();
   if (scanner.joinable()) scanner.join();
}

// Unknown work uses a moving indicator until real installation progress is available.
bool Installer::indeterminate() const { return page==InstallationPage && percent==0; }

// Show work feedback for requested checks and writes, not routine background discovery.
bool Installer::busy() const { return (page!=WelcomePage && pending!=None) || page==InstallationPage; }

// Shared page headings keep both native frontends in the same workflow.
std::string Installer::heading() const
{
   const char *titles[]={"Welcome to Slax Installer","Choose a Slax source","Choose a target drive","Installing Slax","Installation complete"};
   if (page==FinishPage && !error.empty()) return "Installation failed";
   return titles[page];
}

// Describe actual writes and keep the result tied to verified completion.
std::string Installer::introduction() const
{
   const char *text[]={"Install Slax on a USB drive or a local disk.","Select a source ISO image or source folder containing Slax files.","Select a disk or partition that will hold Slax.","Keep the source and target connected. Do not turn off the computer.","Slax was copied and verified, and its boot files are ready. Restart and select Slax or the target disk in the boot menu."};
   if (page==FinishPage && !error.empty()) return error;
   return text[page];
}

// Progress is supplied by real completed I/O, never by an elapsed-time simulation.
std::string Installer::progressText() const
{
   if (page==SourcePage && pending!=None) return "Checking source…";
   if (page==TargetPage && pending==Next) return "Preparing installation plan…";
   if (page==TargetPage && pending==Confirm) return "Rechecking plan…";
   std::lock_guard<std::mutex> lock(mutex); return status;
}

// Only source metadata belongs in the page; checks use the footer and errors use dialogs.
std::string Installer::message() const
{
   if (page==SourcePage && pending==None && !iso.entries.empty() && iso.error.empty()) return iso.name+" · "+sizeText(iso.payload,true)+" to copy";
   return "";
}

// Derive native button availability from the latest target facts and writer lifetime.
bool Installer::canContinue() const
{
   if (page==WelcomePage) return !confirming && !choosing;
   if (pending!=None || confirming || choosing || page==InstallationPage) return false;
   if (page==SourcePage) return !iso.path.empty();
   if (page==TargetPage)
   {
      if (!iso.error.empty()) return false;
      for (const auto &device:devices) if (device.id==selected) return true;
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
   app.setImagePath(argc==2?argv[1]:sourceImage(),true);
   return runWindow();
}
#endif
