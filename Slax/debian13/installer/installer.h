#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Device
{
   std::string id, path, name, label, detail, blocked;
   uint64_t bytes=0, free=0;
   bool fat=false;
};

struct Image
{
   std::string path, name, error;
   uint64_t bytes=0, payload=0;
};

enum Action { None, Browse, Back, Next, Cancel, Erase, Select, Confirm, Close };

std::vector<Device> scanDevices(const std::string &source);
FILE *openImage(const std::string &path);
std::string chooseImage();
bool confirmInstall(const std::string &target,bool erase);
void invalidate();
void windowAction(Action action);
int runWindow();

Image inspectImage(const std::string &path);
std::string sizeText(uint64_t value,bool binary=false);
void printDevices(const std::vector<Device> &devices);

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
   std::mutex mutex;
   std::vector<Device> scanned;
   Image scannedIso;
   std::string source;
   uint64_t revision=0,scannedRevision=0;
   bool scanReady=false;
   std::thread scanner;

   void startScan();
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
