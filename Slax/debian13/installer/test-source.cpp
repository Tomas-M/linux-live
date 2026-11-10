#include "installer.h"
#include <chrono>
#include <stdexcept>

// Exercise source detection and wizard transitions without opening installation dialogs.
std::string chooseImage(bool) { return ""; }
bool confirmInstall(const std::string &,bool) { return false; }
void showInstallError(const std::string &) {}
void invalidate() {}
void windowAction(Action) {}
int runWindow() { return 1; }

// Run from a mounted fixture, including symlinks and bind mounts, without writing any disk.
int main(int argc,char **argv)
{
   try
   {
      if (argc!=3) throw std::runtime_error("Usage: test-source EXPECTED_SOURCE valid|invalid");
      std::string expected=argv[1]; bool valid=std::string(argv[2])=="valid";
      auto source=sourceImage();
      if (source!=expected) throw std::runtime_error("Unexpected detected source: "+source);
      auto image=inspectImage(source);
      if (image.error.empty()!=valid) throw std::runtime_error("Unexpected source validation: "+image.error);
      app.setImagePath(source,true); app.startScan();
      auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
      while (std::chrono::steady_clock::now()<deadline)
      {
         app.tick();
         if (app.pending==None) break;
         std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      app.stop();
      if (app.page!=WelcomePage || app.pending!=None || (!valid && app.iso.error.empty())) throw std::runtime_error("Unexpected automatic source validation.");
      printf("PASS: %s, %zu payload entries, wizard page %d\n",source.c_str(),image.entries.size(),app.page);
      return 0;
   }
   catch (const std::exception &error) { app.stop(); fprintf(stderr,"%s\n",error.what()); return 1; }
}
