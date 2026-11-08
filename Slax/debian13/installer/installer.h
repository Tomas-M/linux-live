#pragma once

#include <algorithm>
#include <atomic>
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
   bool usb=false,system=false;
   std::vector<Partition> partitions;
};

struct ImageEntry
{
   std::string path;
   uint64_t offset=0,bytes=0;
   bool directory=false;
};

struct Image
{
   std::string path,name,error,identity;
   uint64_t bytes=0, payload=0;
   std::vector<ImageEntry> entries;
   std::vector<std::string> filesystems={"vfat","fat","fat16","fat32","ntfs","ext2","ext3","ext4","xfs"};
};

enum Action { None, Browse, Back, Next, Cancel, Erase, Select, Confirm, Close };
struct InstallPlan;

std::vector<Device> scanDevices(const std::string &source);
std::string sourceImage();
FILE *openImage(const std::string &path);
std::string chooseImage();
bool confirmInstall(const std::string &target,bool erase);
void invalidate();
void windowAction(Action action);
int runWindow();

Image inspectImage(const std::string &path);
std::string imageIdentity(FILE *file);
std::string filenameKey(const std::string &name);
std::string sizeText(uint64_t value,bool binary=false);
void printDevices(const std::vector<Device> &devices);
std::vector<Device> visibleTargets(std::vector<Device> devices);

class Installer
{
public:
   Image iso;
   std::vector<Device> devices;
   std::string selected,target,targetId,error;
   int page=0,percent=0;
   Action pending=None;
   bool erase=false,confirming=false,choosing=false;
   std::atomic<bool> running{true};
   mutable std::mutex mutex;
   std::vector<Device> scanned;
   Image scannedIso;
   std::string source;
   uint64_t revision=0,scannedRevision=0;
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
   void setImagePath(const std::string &path,bool automatic=false);
   void refresh(Action action);
   void tick();
   void activate(Action action,const std::string &id="");
   void stop();
   std::string eligibility(const Device &device) const;
   std::string heading() const;
   std::string introduction() const;
   std::string progressText() const;
   std::string message() const;
   bool canContinue() const;
};

extern Installer app;
