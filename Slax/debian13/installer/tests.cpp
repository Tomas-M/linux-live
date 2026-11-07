#include "installer.h"
#include <cassert>
#include <cstring>
#include <chrono>

static std::vector<Device> hardware;
static Action native=None;
static bool approve=false;

// Replace only native boundaries; exercise the actual wizard and ISO reader.
std::vector<Device> scanDevices(const std::string &) { return hardware; }
FILE *openImage(const std::string &path) { return fopen(path.c_str(),"rb"); }
std::string chooseImage() { return ""; }
bool confirmInstall(const std::string &,bool) { return approve; }
void invalidate() {}
void windowAction(Action action) { native=action; }
int runWindow() { return 0; }

// Deliver a validation result through the same versioned handoff as the worker.
static void deliver(Installer &wizard)
{
   wizard.scanned=hardware; wizard.scannedIso=inspectImage(wizard.iso.path);
   wizard.scannedRevision=wizard.revision; wizard.scanReady=true; wizard.tick();
}

// Check the complete simulation, stale scans, disconnection and protection rules.
int main(int argc,char **argv)
{
   assert(argc==2);
   Installer wizard;
   wizard.iso=inspectImage(argv[1]);
   assert(wizard.iso.error.empty() && wizard.iso.bytes>0 && wizard.canContinue());
   assert(!inspectImage("/missing.iso").error.empty());
   assert(!inspectImage(argv[0]).error.empty());
   Device disk;
   disk.id="owned-usb-fixture"; disk.path="/dev/fixture"; disk.name="USB 東京 İstanbul"; disk.label="USB";
   disk.bytes=wizard.iso.bytes+1024*1024*1024ULL; disk.free=disk.bytes; disk.fat=true;
   assert(wizard.eligibility(disk).empty());
   disk.blocked="System disk"; assert(wizard.eligibility(disk)=="System disk"); disk.blocked.clear();
   disk.fat=false; assert(!wizard.eligibility(disk).empty());
   wizard.erase=true; assert(wizard.eligibility(disk).empty());
   disk.bytes=1; assert(!wizard.eligibility(disk).empty()); disk.bytes=disk.free;
   wizard.erase=false; disk.fat=true; disk.free=0; assert(!wizard.eligibility(disk).empty()); disk.free=disk.bytes;
   hardware={disk}; wizard.devices=hardware;
   wizard.activate(Next); assert(wizard.pending==Next && wizard.page==0 && !wizard.canContinue());
   wizard.scannedRevision=wizard.revision-1; wizard.scanReady=true; wizard.tick(); assert(wizard.page==0);
   deliver(wizard); assert(wizard.page==1 && wizard.pending==None && !wizard.canContinue());
   wizard.activate(Select,disk.id); assert(wizard.selected==disk.id && wizard.canContinue());
   wizard.activate(Next); deliver(wizard); assert(wizard.page==1 && wizard.pending==None);
   approve=true;
   wizard.activate(Next); deliver(wizard); assert(wizard.pending==Confirm && wizard.page==1);
   hardware.clear(); deliver(wizard); assert(wizard.page==1 && !wizard.error.empty());
   hardware={disk}; deliver(wizard); wizard.activate(Select,disk.id); wizard.activate(Next); deliver(wizard); deliver(wizard); assert(wizard.page==2);
   for (int i=0; i<100; i++) wizard.tick();
   assert(wizard.page==3 && wizard.error.empty() && wizard.percent==100);
   wizard.activate(Next); assert(native==Close);
   wizard.page=1; wizard.activate(Next); deliver(wizard); deliver(wizard); assert(wizard.page==2);
   hardware.clear(); deliver(wizard); assert(wizard.page==3 && !wizard.error.empty());
   wizard.activate(Next); assert(wizard.page==1 && wizard.error.empty());
   hardware={disk}; deliver(wizard); wizard.activate(Select,disk.id); wizard.activate(Next);
   wizard.activate(Back); deliver(wizard); assert(wizard.page==0 && wizard.pending==None);
   wizard.activate(Select,"removed-device"); assert(wizard.selected==disk.id);

   // Exercise the real worker's request, handoff and clean shutdown as well.
   Installer threaded; threaded.iso=inspectImage(argv[1]); threaded.startScan(); threaded.refresh(Next);
   for (int i=0; i<60 && threaded.page==0; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); threaded.tick(); }
   threaded.stop(); assert(threaded.page==1);
   puts("Installer checks passed.");
}
