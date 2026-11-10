#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Partition
{
   std::string path,label,volumeLabel,filesystem,uuid,mount,type;
   uint64_t start=0,bytes=0,free=0;
   uint32_t number=0;
   bool readonly=false;
};

struct Device:Partition
{
   std::string id,name,detail,blocked,eraseBlocked,disk,diskId,layout;
   uint64_t diskBytes=0,sequence=0;
   uint32_t sector=512;
   bool usb=false,removable=false,system=false,virtualDisk=false;
   std::vector<Partition> partitions;
};

struct ImageEntry
{
   std::string path;
   uint64_t offset=0,bytes=0;
   bool directory=false;
   uint32_t mode=0644;
   std::string source={};
};

struct Image
{
   std::string path,name,error;
   uint64_t bytes=0, payload=0;
   std::vector<ImageEntry> entries;
   std::vector<std::string> filesystems={"vfat","fat","fat16","fat32","ntfs","ext2","ext3","ext4","xfs"};
};

enum Action { None, Browse, BrowseDirectory, Back, Next, Erase, Select, CheckSource, Confirm, Close };
enum WizardPage { WelcomePage, SourcePage, TargetPage, InstallationPage, FinishPage, PageCount };
inline constexpr const char *wizardSteps[]={"Welcome","Slax source","Target drive","Installation","Finish"};
// Share the approved welcome copy between both native frontends.
inline constexpr const char *welcomeText[]={
   "Slax Installer copies Slax to your chosen drive and makes it bootable, so you can start your computer directly into Slax.",
   "You will choose a source ISO image or a folder containing Slax files, then select the drive you want to install to.",
   "You can keep your existing files or erase the entire disk. Before installation begins, you will review and confirm a plan showing what will change."
};
struct InstallPlan;

std::vector<Device> scanDevices(const std::string &source);
std::string sourceImage();
FILE *openImage(const std::string &path);
std::string chooseImage(bool directory=false);
bool confirmInstall(const std::string &target,bool erase);
void showInstallError(const std::string &message);
void invalidate();
void windowAction(Action action);
int runWindow();

Image inspectImage(const std::string &path);
std::string filenameKey(const std::string &name);
std::string sizeText(uint64_t value,bool binary=false);
void printDevices(const std::vector<Device> &devices);
std::vector<Device> visibleTargets(std::vector<Device> devices);
int deviceIcon(const Device &device);
std::string deviceLocation(const Device &device);

class Installer
{
public:
   Image iso;
   std::vector<Device> devices;
   std::string selected,target,targetId,error;
   int page=WelcomePage,percent=0;
   Action pending=None;
   bool erase=false,confirming=false,choosing=false;
   std::atomic<bool> running{true};
   mutable std::mutex mutex;
   std::vector<Device> scanned;
   Image scannedIso;
   std::string source;
   uint64_t revision=0,scannedRevision=0;
   std::chrono::steady_clock::time_point sourceCheckAfter;
   bool scanReady=false;
   std::thread scanner;
   std::thread writer;
   std::string status,writeError;
   int writtenPercent=0;
   bool writeDone=false;
   std::shared_ptr<InstallPlan> plan,scannedPlan;
   std::string requestId,scanError;
   bool requestErase=false;

   void startScan();
   void setImagePath(const std::string &path,bool immediate=false);
   void refresh(Action action,bool debounce=false);
   void tick();
   void activate(Action action,const std::string &id="");
   void stop();
   bool isSelected(const Device &device) const;
   bool indeterminate() const;
   bool busy() const;
   std::string heading() const;
   std::string introduction() const;
   std::string progressText() const;
   std::string message() const;
   bool canContinue() const;
};

extern Installer app;

// Read one original source file, retaining ISO offsets when the source is an image.
class SourceFile
{
   FILE *file=nullptr;
   bool owned=false;
public:
   SourceFile(FILE *image,const ImageEntry &entry);
   ~SourceFile();
   void read(void *data,size_t bytes);
};
