#include "disk-format.h"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>

// This headless test entry point can write only a loop device backed by its explicitly owned fixture.
std::string chooseImage(bool) { return ""; }
bool confirmInstall(const std::string &,bool) { return false; }
void showInstallError(const std::string &) {}
void invalidate() {}
void windowAction(Action) {}
int runWindow() { return 1; }

// Exercise production Linux handles, locking, filesystem writes and BIOS generation on an owned loop.
int main(int argc,char **argv)
{
   try
   {
      if (argc!=4 && argc!=5) throw std::runtime_error("Usage: test-native ISO LOOP_DEVICE OWNED_BACKING_FILE [preserve]");
      bool erase=argc==4;
      if (!erase && std::string(argv[4])!="preserve") throw std::runtime_error("Unknown native test mode.");
      std::string loop=argv[2],backing=std::filesystem::canonical(argv[3]); struct stat info;
      if (loop.find("/dev/loop")!=0 || stat(loop.c_str(),&info) || !S_ISBLK(info.st_mode) || major(info.st_rdev)!=7) throw std::runtime_error("Tests accept only Linux loop block devices.");
      if (backing.find("/tmp/slax-installer-native.")!=0) throw std::runtime_error("The loop backing file is not in an owned installer test workspace.");
      std::ifstream file("/sys/class/block/"+loop.substr(5)+"/loop/backing_file"); std::string actual; std::getline(file,actual);
      if (actual.empty() || std::filesystem::canonical(actual)!=backing) throw std::runtime_error("The loop device does not belong to the expected fixture.");
      auto image=inspectImage(argv[1]); if (!image.error.empty()) throw std::runtime_error(image.error);
      auto devices=scanDevices(image.path);
      auto found=std::find_if(devices.begin(),devices.end(),[&](const Device &d) { return d.disk==loop && (erase?d.number==0:d.number>0 && d.bytes>=1000000000); });
      if (found==devices.end()) throw std::runtime_error("The owned loop device was not discovered.");
      // This owned loop models removable media; its test transport is unrelated to the work VM firmware.
      found->usb=true;
      auto plan=prepareInstall(*found,image,erase);
      // Replacing a read-only plan must not wake udev; concurrent writers must still be refused.
      for (int i=0; i<20; i++)
      {
         auto previous=std::make_unique<Disk>(*found);
         Disk refreshed(*found); previous.reset();
      }
      plan.disk->reserve();
      {
         Disk competing(*found); bool blocked=false;
         try { competing.reserve(); }
         catch (const std::exception &error) { blocked=std::string(error.what()).find("Another disk operation is already using this physical disk")==0; }
         if (!blocked) throw std::runtime_error("The physical disk lock did not reject a concurrent writer.");
      }
      install(plan,[](const std::string &text,int percent)
      {
         static int previous=-1;
         if (percent!=previous && percent%10==0) { fprintf(stderr,"%d%% %s\n",percent,text.c_str()); previous=percent; }
      });
      puts("Native Linux installation into the owned loop completed.");
      return 0;
   }
   catch (const std::exception &error) { fprintf(stderr,"%s\n",error.what()); return 1; }
}
