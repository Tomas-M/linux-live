#include "storage.h"
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
         { std::lock_guard<std::mutex> lock(mutex); current=source; version=revision; requested=requestId; eraseRequested=requestErase; }
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
void Installer::refresh(Action action)
{
   pending=action;
   std::lock_guard<std::mutex> lock(mutex);
   source=page==0 && action!=Next?"":iso.path; requestId=page==1 && (action==Next || action==Confirm)?selected:"";
   requestErase=erase; revision++;
}

// Validate automatic sources immediately; edited paths wait for Next.
void Installer::setImagePath(const std::string &path,bool automatic)
{
   if (page!=0 || pending!=None || iso.path==path) return;
   iso={}; iso.path=path; selected.clear(); error.clear(); plan.reset();
   refresh(automatic?Next:None); invalidate();
}

// Keep the immediate explanation consistent with the deeper native preflight.
std::string Installer::eligibility(const Device &device) const { return basicEligibility(device,iso,erase); }

// Consume worker results on the GUI thread and launch only the exact plan the user approved.
void Installer::tick()
{
   if (page==2)
   {
      bool done;
      { std::lock_guard<std::mutex> lock(mutex); percent=writtenPercent; done=writeDone; if (done) error=writeError; }
      if (done) { if (writer.joinable()) writer.join(); page=3; plan.reset(); }
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
   if (found==devices.end() && !selected.empty()) { selected.clear(); if (page==1) error="The selected target is no longer available. Select a listed disk or partition."; }
   if (pending!=None)
   {
      Action action=pending; pending=None;
      { std::lock_guard<std::mutex> lock(mutex); requestId.clear(); }
      if (page==0)
      {
         iso=checked; error=iso.error;
         if (error.empty() && action==Next) page=1;
         else refresh(None);
      }
      else if (page==1)
      {
         error=failure;
         if (error.empty() && !prepared) error="The target could not be prepared. Select the image and target again.";
         if (error.empty() && checked.identity!=iso.identity) error="The ISO file identity changed. Copy the image to a stable local drive and select it again.";
         if (error.empty() && action==Next)
         {
            plan=prepared; target=found->name+" ("+found->label+") · "+sizeText(erase?found->diskBytes:found->bytes); targetId=found->id;
            confirming=true; invalidate(); bool approved=confirmInstall(plan->confirmation,erase); confirming=false;
            if (approved) refresh(Confirm); else plan.reset();
         }
         else if (error.empty() && action==Confirm)
         {
            bool sameDevice=plan && prepared->device.id==plan->device.id && prepared->device.layout==plan->device.layout && prepared->snapshot==plan->snapshot;
            bool sameEfi=plan && prepared->efi.start==plan->efi.start && prepared->efi.uuid==plan->efi.uuid;
            bool sameFirmware=plan && prepared->bootOrder==plan->bootOrder && prepared->fallbackDigest==plan->fallbackDigest;
            bool sameImage=plan && prepared->image.identity==plan->image.identity && prepared->confirmation==plan->confirmation;
            bool same=sameDevice && sameEfi && sameFirmware && sameImage;
            if (!same) { error="The installation plan changed after confirmation. Review and confirm the target again."; plan.reset(); }
            else
            {
               running=false; if (scanner.joinable()) scanner.join();
               page=2; percent=0;
               { std::lock_guard<std::mutex> lock(mutex); writeDone=false; writeError.clear(); writtenPercent=0; status="Preparing installation…"; }
               auto approved=plan;
               writer=std::thread([this,approved]
               {
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
   if (confirming || choosing || page==2) return;
   if (action==Close || action==Cancel) { windowAction(Close); return; }
   if (action==Back && page==1) { plan.reset(); page=0; refresh(None); error=iso.error; }
   if (pending!=None) { invalidate(); return; }
   if (action==Browse)
   {
      choosing=true; auto path=chooseImage();
      if (!path.empty()) setImagePath(path);
      choosing=false;
   }
   if (action==Erase)
   {
      erase=!erase; error.clear(); plan.reset();
      for (const auto &device:devices) if (device.id==selected) error=eligibility(device);
   }
   if (action==Select)
   {
      auto found=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.id==id; });
      if (found!=devices.end()) { selected=id; error=eligibility(*found); plan.reset(); }
   }
   if (action==Next)
   {
      if (page<2)
      {
         if (page==0 && iso.path.size()>1 && iso.path.front()=='"' && iso.path.back()=='"') iso.path=iso.path.substr(1,iso.path.size()-2);
         error.clear(); refresh(Next);
      }
      else if (page==3)
      {
         if (!error.empty()) { page=1; error.clear(); percent=0; startScan(); refresh(None); }
         else windowAction(Close);
      }
   }
   invalidate();
}

// Join storage workers before releasing any native application resources.
void Installer::stop()
{
   running=false;
   if (scanner.joinable()) scanner.join();
   if (writer.joinable()) writer.join();
}

// Shared page headings keep both native frontends in the same workflow.
std::string Installer::heading() const
{
   const char *titles[]={"Choose an ISO file","Choose a target drive","Installing Slax","Installation complete"};
   if (page==3 && !error.empty()) return "Installation failed";
   return titles[page];
}

// Describe actual writes and keep the result tied to verified completion.
std::string Installer::introduction() const
{
   const char *text[]={"Select the Slax ISO image you want to install.","Select a disk or partition that will hold Slax.","Keep the source and target connected. Do not turn off the computer.","Slax was copied and verified, and its boot files are ready. Restart and select Slax or the target disk in the boot menu."};
   if (page==3 && !error.empty()) return error;
   return text[page];
}

// Progress is supplied by real completed I/O, never by an elapsed-time simulation.
std::string Installer::progressText() const
{
   std::lock_guard<std::mutex> lock(mutex); return status;
}

// Keep metadata, checks and validation feedback in the same consistently spaced slot.
std::string Installer::message() const
{
   if (pending!=None) return "Checking the ISO and target configuration…";
   if (!error.empty())
   {
      auto reason=error;
      while (!reason.empty() && (reason.back()=='\r' || reason.back()=='\n')) reason.pop_back();
      return std::string(page==0?"The selected ISO image cannot be used.":"The selected target cannot be used.")+"\nReason: "+reason+"\n"+
         (page==0?"Check the path or choose a current Slax ISO image.":"Review the target or choose another disk or partition.");
   }
   if (page==0 && !iso.identity.empty() && iso.error.empty()) return iso.name+" · "+sizeText(iso.payload,true)+" to copy";
   return "";
}

// Derive native button availability from the latest target facts and writer lifetime.
bool Installer::canContinue() const
{
   if (pending!=None || confirming || choosing || page==2) return false;
   if (page==0) return !iso.path.empty();
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
   app.setImagePath(argc==2?argv[1]:sourceImage(),argc!=2);
   return runWindow();
}
#endif
