#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <setupapi.h>
#include <winioctl.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <set>
#include <memory>
#include <cmath>
#include <cstring>
#include "installer.h"
#include "images.h"

static HWND window;
static float scale=1;

// Convert Windows Unicode paths at the platform boundary, preserving all characters.
static std::wstring wide(const std::string &value)
{
   int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),value.size(),nullptr,0);
   std::wstring result(length,L'\0');
   if (length) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),value.size(),result.data(),length);
   return result;
}

// Expose native names to the shared GUI as UTF-8.
static std::string utf8(const std::wstring &value)
{
   int length=WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),nullptr,0,nullptr,nullptr);
   std::string result(length,'\0');
   if (length) WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),result.data(),length,nullptr,nullptr);
   return result;
}

// Resolve all physical disks behind a volume, including multi-extent volumes.
static std::set<DWORD> volumeDisks(const std::wstring &name)
{
   std::set<DWORD> result;
   std::wstring path=name;
   if (!path.empty() && path.back()==L'\\') path.pop_back();
   HANDLE volume=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
   if (volume==INVALID_HANDLE_VALUE) return result;
   std::vector<unsigned char> buffer(65536);
   DWORD returned=0;
   if (DeviceIoControl(volume,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,buffer.data(),buffer.size(),&returned,nullptr))
   {
      auto extents=reinterpret_cast<VOLUME_DISK_EXTENTS *>(buffer.data());
      size_t offset=offsetof(VOLUME_DISK_EXTENTS,Extents);
      if (returned>=offset && extents->NumberOfDiskExtents<=(returned-offset)/sizeof(DISK_EXTENT))
         for (DWORD i=0; i<extents->NumberOfDiskExtents; i++) result.insert(extents->Extents[i].DiskNumber);
   }
   CloseHandle(volume);
   return result;
}

// Map a file to physical storage without depending on a drive letter or disk number.
static std::set<DWORD> fileDisks(const std::wstring &filename)
{
   wchar_t root[32768],volume[64];
   if (!GetVolumePathNameW(filename.c_str(),root,32768)) return {};
   if (!GetVolumeNameForVolumeMountPointW(root,volume,64)) return {};
   return volumeDisks(volume);
}

// Use native SetupAPI and storage IOCTLs; never invoke PowerShell, WMI or disk writers.
std::vector<Device> scanDevices(const std::string &source)
{
   static const GUID diskClass={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
   std::vector<Device> result;
   wchar_t windows[32768];
   GetWindowsDirectoryW(windows,32768);
   auto system=fileDisks(windows),origin=source.empty()?std::set<DWORD>{}:fileDisks(wide(source));
   HDEVINFO set=SetupDiGetClassDevsW(&diskClass,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
   if (set==INVALID_HANDLE_VALUE) return result;
   struct Volume { std::set<DWORD> disks; std::string label; uint64_t free; bool fat; };
   std::vector<Volume> volumes;
   wchar_t volumeName[64];
   HANDLE search=FindFirstVolumeW(volumeName,64);
   if (search!=INVALID_HANDLE_VALUE)
   {
      do
      {
         Volume volume;
         volume.disks=volumeDisks(volumeName);
         wchar_t filesystem[64],paths[32768];
         DWORD needed=0;
         ULARGE_INTEGER free={},total={},all={};
         volume.fat=GetVolumeInformationW(volumeName,nullptr,0,nullptr,nullptr,nullptr,filesystem,64) && !_wcsicmp(filesystem,L"FAT32");
         volume.free=GetDiskFreeSpaceExW(volumeName,&free,&total,&all)?free.QuadPart:0;
         if (GetVolumePathNamesForVolumeNameW(volumeName,paths,32768,&needed))
         {
            for (wchar_t *path=paths; *path; path+=wcslen(path)+1)
            {
               std::wstring name=path;
               if (name.size()==3 && name[1]==L':')
               {
                  if (!volume.label.empty()) volume.label+=", ";
                  volume.label+=utf8(name.substr(0,2));
               }
            }
         }
         volumes.push_back(volume);
      } while (FindNextVolumeW(search,volumeName,64));
      FindVolumeClose(search);
   }
   SP_DEVICE_INTERFACE_DATA diskInterface={}; diskInterface.cbSize=sizeof(diskInterface);
   for (DWORD index=0; SetupDiEnumDeviceInterfaces(set,nullptr,&diskClass,index,&diskInterface); index++)
   {
      DWORD needed=0;
      SetupDiGetDeviceInterfaceDetailW(set,&diskInterface,nullptr,0,&needed,nullptr);
      if (needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<unsigned char> detailBuffer(needed);
      auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailBuffer.data());
      detail->cbSize=sizeof(*detail);
      SP_DEVINFO_DATA info={}; info.cbSize=sizeof(info);
      if (!SetupDiGetDeviceInterfaceDetailW(set,&diskInterface,detail,needed,nullptr,&info)) continue;
      HANDLE disk=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
      if (disk==INVALID_HANDLE_VALUE) continue;
      STORAGE_PROPERTY_QUERY query={}; query.PropertyId=StorageDeviceProperty; query.QueryType=PropertyStandardQuery;
      unsigned char descriptor[8192]={};
      DWORD returned=0;
      bool queried=DeviceIoControl(disk,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),descriptor,sizeof(descriptor),&returned,nullptr);
      auto storage=reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(descriptor);
      if (!queried || returned<sizeof(STORAGE_DEVICE_DESCRIPTOR) || storage->BusType!=BusTypeUsb || storage->DeviceType!=0) { CloseHandle(disk); continue; }
      STORAGE_DEVICE_NUMBER number={};
      unsigned char geometry[1024]={};
      bool numbered=DeviceIoControl(disk,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&returned,nullptr);
      bool sized=DeviceIoControl(disk,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,nullptr,0,geometry,sizeof(geometry),&returned,nullptr);
      if (numbered && sized && returned>=offsetof(DISK_GEOMETRY_EX,Data))
      {
         Device device;
         auto size=reinterpret_cast<DISK_GEOMETRY_EX *>(geometry);
         device.bytes=size->DiskSize.QuadPart;
         device.path="\\\\.\\PhysicalDrive"+std::to_string(number.DeviceNumber);
         wchar_t instance[4096]={},name[1024]={};
         SetupDiGetDeviceInstanceIdW(set,&info,instance,4096,nullptr);
         SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_FRIENDLYNAME,nullptr,reinterpret_cast<BYTE *>(name),sizeof(name),nullptr);
         device.name=name[0]?utf8(name):"USB drive";
         device.id=utf8(instance[0]?instance:detail->DevicePath)+":"+std::to_string(device.bytes);
         device.detail=storage->RemovableMedia?"Removable USB drive":"USB drive";
         bool writable=DeviceIoControl(disk,IOCTL_DISK_IS_WRITABLE,nullptr,0,nullptr,0,&returned,nullptr);
         if (!writable && GetLastError()==ERROR_WRITE_PROTECT) device.blocked="This drive is read-only.";
         if (system.empty()) device.blocked="The running system disk could not be identified.";
         if (system.count(number.DeviceNumber)) device.blocked="This drive contains the running Windows system.";
         if (origin.count(number.DeviceNumber)) device.blocked="This drive contains the selected ISO image.";
         for (const auto &volume:volumes)
         {
            if (!volume.disks.count(number.DeviceNumber)) continue;
            if (!volume.label.empty()) { if (!device.label.empty()) device.label+=", "; device.label+=volume.label; }
            if (volume.fat && volume.free>=device.free)
            {
               device.fat=true; device.free=volume.free;
               device.detail=sizeText(device.free)+" free · FAT32";
            }
         }
         if (device.label.empty()) device.label="USB";
         if (device.bytes) result.push_back(device);
      }
      CloseHandle(disk);
   }
   SetupDiDestroyDeviceInfoList(set);
   std::sort(result.begin(),result.end(),[](const Device &a,const Device &b) { return a.id<b.id; });
   return result;
}

// Open only ordinary image files in read mode, with native Unicode path handling.
FILE *openImage(const std::string &path)
{
   auto filename=wide(path);
   DWORD attributes=GetFileAttributesW(filename.c_str());
   if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY) || path.find("\\\\.")==0) return nullptr;
   return _wfopen(filename.c_str(),L"rb");
}

enum { Rail=100,StepNumber=110,StepName=120,Brand=130,Wordmark,Step=140,Heading,Intro,PathLabel,Path,BrowseButton,Message,Drives,Empty,EraseButton,Target,Status,Percent,Progress,Result,Footer,BackButton,NextButton,CancelButton,Separator };
static const DWORD windowStyle=WS_OVERLAPPEDWINDOW;

struct WinCard
{
   Device device;
   HWND window=nullptr,badge=nullptr,name=nullptr,detail=nullptr,radio=nullptr;
   bool pressed=false;
};

static ULONG_PTR graphicsToken;

struct WinUi
{
   HWND panel=nullptr,controls[180]={};
   HFONT body=nullptr,small=nullptr,title=nullptr,intro=nullptr,bold=nullptr,badge=nullptr;
   HBRUSH rail=nullptr,green=nullptr,selected=nullptr;
   HBITMAP brand[2]={};
   std::vector<std::unique_ptr<WinCard>> cards;
   int scroll=0,deviceScroll=0,page=-1;
   bool updating=false,layingout=false,layingoutDevices=false;
} ui;

// Set native text only when changed, preserving edit selection and accessibility focus.
static void text(HWND control,const std::string &value)
{
   std::wstring current(GetWindowTextLengthW(control)+1,L'\0');
   GetWindowTextW(control,current.data(),current.size()); current.resize(wcslen(current.c_str()));
   auto next=wide(value);
   if (current!=next) SetWindowTextW(control,next.c_str());
}

// Address the fixed native form fields through their control IDs.
static void text(int id,const std::string &value) { text(ui.controls[id],value); }

// Create ordinary themed Windows controls with the system's message-font family.
static HWND control(int id,const wchar_t *type,DWORD style,HWND parent,DWORD extended=0)
{
   HWND item=CreateWindowExW(extended,type,L"",WS_CHILD|WS_VISIBLE|style,0,0,1,1,parent,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
   ui.controls[id]=item;
   SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(ui.body),TRUE);
   return item;
}

// Keep physical sizing consistent with normal per-monitor Windows DPI behavior.
static RECT windowBounds(int width,int height)
{
   RECT bounds={0,0,LONG(width*scale),LONG(height*scale)};
   using Adjust=BOOL (WINAPI *)(LPRECT,DWORD,BOOL,DWORD,UINT);
   auto adjust=reinterpret_cast<Adjust>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
   if (adjust) adjust(&bounds,windowStyle,FALSE,0,UINT(96*scale));
   else AdjustWindowRectEx(&bounds,windowStyle,FALSE,0);
   return bounds;
}

// Use native GDI fonts; no downloaded font or redistributable runtime is required.
static void fonts()
{
   LOGFONTW systemFont={};
   SystemParametersInfoW(SPI_GETICONTITLELOGFONT,sizeof(systemFont),&systemFont,0);
   HFONT old[]={ui.body,ui.small,ui.title,ui.intro,ui.bold,ui.badge};
   HFONT *slots[]={&ui.body,&ui.small,&ui.title,&ui.intro,&ui.bold,&ui.badge};
   int sizes[]={14,13,28,16,14,20};
   for (int i=0; i<6; i++)
   {
      LOGFONTW font=systemFont; font.lfHeight=-LONG(sizes[i]*scale); font.lfWeight=i==2 || i==4 || i==5?FW_SEMIBOLD:FW_NORMAL;
      *slots[i]=CreateFontIndirectW(&font);
   }
   for (auto item:ui.controls) if (item) SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(ui.body),TRUE);
   if (ui.controls[Heading]) SendMessageW(ui.controls[Heading],WM_SETFONT,reinterpret_cast<WPARAM>(ui.title),TRUE);
   if (ui.controls[Intro]) SendMessageW(ui.controls[Intro],WM_SETFONT,reinterpret_cast<WPARAM>(ui.intro),TRUE);
   for (int id:{Step,Footer}) if (ui.controls[id]) SendMessageW(ui.controls[id],WM_SETFONT,reinterpret_cast<WPARAM>(ui.small),TRUE);
   for (int i=0; i<4; i++) if (ui.controls[StepName+i]) SendMessageW(ui.controls[StepName+i],WM_SETFONT,reinterpret_cast<WPARAM>(i==app.page?ui.bold:ui.body),TRUE);
   for (const auto &card:ui.cards)
   {
      SendMessageW(card->badge,WM_SETFONT,reinterpret_cast<WPARAM>(card->device.label.size()>3?ui.bold:ui.badge),TRUE);
      SendMessageW(card->name,WM_SETFONT,reinterpret_cast<WPARAM>(ui.bold),TRUE);
      SendMessageW(card->detail,WM_SETFONT,reinterpret_cast<WPARAM>(ui.small),TRUE);
   }
   for (auto font:old) if (font) DeleteObject(font);
}

// Convert the two original PNGs once into ordinary STATIC bitmap resources.
static void branding()
{
   for (int i=0; i<2; i++)
   {
      size_t length=i?sizeof(wordmark_png):sizeof(clover_png);
      HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,length);
      memcpy(GlobalLock(data),i?wordmark_png:clover_png,length); GlobalUnlock(data);
      IStream *stream; CreateStreamOnHGlobal(data,TRUE,&stream);
      HBITMAP bitmap;
      {
         Gdiplus::Bitmap source(stream);
         Gdiplus::Bitmap resized(int((i?83:31)*scale),int((i?30:33)*scale),PixelFormat32bppPARGB);
         {
            Gdiplus::Graphics graphics(&resized);
            graphics.Clear(Gdiplus::Color(255,236,243,231));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(&source,0,0,int(resized.GetWidth()),int(resized.GetHeight()));
         }
         resized.GetHBITMAP(Gdiplus::Color(255,236,243,231),&bitmap);
      }
      stream->Release();
      SendMessageW(ui.controls[Brand+i],STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bitmap));
      if (ui.brand[i]) DeleteObject(ui.brand[i]);
      ui.brand[i]=bitmap;
   }
}

// Position native child windows in logical pixels, applying content scrolling once.
static void place(int id,float x,float y,float width,float height,bool content=true)
{
   MoveWindow(ui.controls[id],int(x*scale),int(y*scale)-(content?ui.scroll:0),std::max(1,int(width*scale)),std::max(1,int(height*scale)),TRUE);
}

// Ask Windows to measure wrapped text with the exact font used by its STATIC control.
static float textHeight(int id,float width)
{
   HWND item=ui.controls[id];
   std::wstring value(GetWindowTextLengthW(item)+1,L'\0'); GetWindowTextW(item,value.data(),value.size());
   HDC dc=GetDC(item); auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(item,WM_GETFONT,0,0)));
   RECT rect={0,0,LONG(width*scale),0}; DrawTextW(dc,value.c_str(),-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
   SelectObject(dc,previous); ReleaseDC(item,dc);
   return std::max(1.0f,rect.bottom/scale);
}

// Draw only the card's decoration; its text and radio button remain standard controls.
static void rounded(Gdiplus::Graphics &graphics,Gdiplus::RectF rect,float radius,COLORREF fill,COLORREF border=0)
{
   float diameter=radius*2;
   Gdiplus::GraphicsPath path;
   path.AddArc(rect.X,rect.Y,diameter,diameter,180,90);
   path.AddArc(rect.GetRight()-diameter,rect.Y,diameter,diameter,270,90);
   path.AddArc(rect.GetRight()-diameter,rect.GetBottom()-diameter,diameter,diameter,0,90);
   path.AddArc(rect.X,rect.GetBottom()-diameter,diameter,diameter,90,90); path.CloseFigure();
   Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(fill),GetGValue(fill),GetBValue(fill))); graphics.FillPath(&brush,&path);
   if (border)
   {
      Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(border),GetGValue(border),GetBValue(border)),std::max(1.0f,std::round(scale)));
      graphics.DrawPath(&pen,&path);
   }
}

// Paint the reference's rounded outline and drive badge behind native child controls.
static void paintCard(WinCard &card,HDC dc)
{
   RECT bounds; GetClientRect(card.window,&bounds); FillRect(dc,&bounds,GetSysColorBrush(COLOR_WINDOW));
   Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
   bool selected=card.device.id==app.selected;
   float border=std::max(1.0f,std::round(scale));
   rounded(graphics,{border/2,border/2,bounds.right-border,bounds.bottom-border},7*scale,selected?RGB(238,247,231):GetSysColor(COLOR_WINDOW),selected?RGB(57,119,33):RGB(217,224,216));
   rounded(graphics,{12*scale,10*scale,42*scale,42*scale},6*scale,RGB(236,243,231));
}

// Keep native cards inside a normal vertically scrollable container, with no horizontal bar.
static void layoutCards()
{
   if (ui.layingoutDevices) return;
   ui.layingoutDevices=true;
   HWND panel=ui.controls[Drives]; RECT bounds; GetClientRect(panel,&bounds);
   SCROLLINFO info={}; info.cbSize=sizeof(info); info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
   info.nMin=0; info.nMax=std::max(0,int((int(ui.cards.size())*70-8)*scale)-1); info.nPage=bounds.bottom; info.nPos=ui.deviceScroll;
   SetScrollInfo(panel,SB_VERT,&info,TRUE); ui.deviceScroll=GetScrollPos(panel,SB_VERT); GetClientRect(panel,&bounds);
   for (size_t i=0; i<ui.cards.size(); i++) MoveWindow(ui.cards[i]->window,0,int(i*70*scale)-ui.deviceScroll,bounds.right,int(62*scale),TRUE);
   ui.layingoutDevices=false;
}

// Preserve ordinary radio keyboard behavior while cycling physical device identities.
static LRESULT CALLBACK radioKeys(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
{
   if (message==WM_KEYDOWN && (wp==VK_UP || wp==VK_DOWN))
   {
      auto current=reinterpret_cast<WinCard *>(data);
      auto item=std::find_if(ui.cards.begin(),ui.cards.end(),[&](const auto &card) { return card.get()==current; });
      int index=std::max(0,std::min(int(ui.cards.size())-1,int(item-ui.cards.begin())+(wp==VK_UP?-1:1)));
      app.activate(Select,ui.cards[index]->device.id); SetFocus(ui.cards[index]->radio); return 0;
   }
   return DefSubclassProc(hwnd,message,wp,lp);
}

// A card is a decorated container around standard STATIC and BUTTON controls.
static LRESULT CALLBACK cardEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   auto card=reinterpret_cast<WinCard *>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
   if (message==WM_NCCREATE)
   {
      card=static_cast<WinCard *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams); card->window=hwnd;
      SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(card));
   }
   if (!card) return DefWindowProcW(hwnd,message,wp,lp);
   if (message==WM_SIZE && card->radio)
   {
      RECT bounds; GetClientRect(hwnd,&bounds); int width=bounds.right;
      MoveWindow(card->badge,int(12*scale),int(18*scale),int(42*scale),int(26*scale),TRUE);
      MoveWindow(card->name,int(66*scale),int(9*scale),std::max(1,width-int(108*scale)),int(22*scale),TRUE);
      MoveWindow(card->detail,int(66*scale),int(31*scale),std::max(1,width-int(108*scale)),int(21*scale),TRUE);
      MoveWindow(card->radio,width-int(29*scale),int(22*scale),int(18*scale),int(18*scale),TRUE); return 0;
   }
   if (message==WM_PAINT)
   {
      PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint); paintCard(*card,dc); EndPaint(hwnd,&paint); return 0;
   }
   if (message==WM_ERASEBKGND || message==WM_PRINTCLIENT) { paintCard(*card,reinterpret_cast<HDC>(wp)); return 1; }
   if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN)
   {
      HDC dc=reinterpret_cast<HDC>(wp); bool selected=card->device.id==app.selected;
      SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
      if (reinterpret_cast<HWND>(lp)==card->badge) { SetTextColor(dc,RGB(57,119,33)); return reinterpret_cast<LRESULT>(ui.rail); }
      if (reinterpret_cast<HWND>(lp)==card->detail) SetTextColor(dc,RGB(84,96,86));
      return reinterpret_cast<LRESULT>(selected?ui.selected:GetSysColorBrush(COLOR_WINDOW));
   }
   if (message==WM_COMMAND && !ui.updating) { app.activate(Select,card->device.id); return 0; }
   if (message==WM_LBUTTONDOWN) { card->pressed=true; SetCapture(hwnd); return 0; }
   if (message==WM_LBUTTONUP)
   {
      bool pressed=card->pressed; card->pressed=false; ReleaseCapture();
      RECT bounds; GetClientRect(hwnd,&bounds); POINT point={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
      if (pressed && PtInRect(&bounds,point)) { app.activate(Select,card->device.id); SetFocus(card->radio); }
      return 0;
   }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Add one native radio row with its exact device identity attached to its lifetime.
static void createCard(const Device &device)
{
   auto card=std::make_unique<WinCard>(); card->device=device;
   HINSTANCE instance=GetModuleHandleW(nullptr);
   card->window=CreateWindowExW(WS_EX_CONTROLPARENT,L"SlaxInstallerCard",wide(device.name).c_str(),WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,1,int(62*scale),ui.controls[Drives],nullptr,instance,card.get());
   card->badge=CreateWindowExW(0,L"STATIC",wide(device.label).c_str(),WS_CHILD|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->name=CreateWindowExW(0,L"STATIC",wide(device.name).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->detail=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->radio=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,0,0,1,1,card->window,nullptr,instance,nullptr);
   SendMessageW(card->badge,WM_SETFONT,reinterpret_cast<WPARAM>(device.label.size()>3?ui.bold:ui.badge),TRUE);
   SendMessageW(card->name,WM_SETFONT,reinterpret_cast<WPARAM>(ui.bold),TRUE); SendMessageW(card->detail,WM_SETFONT,reinterpret_cast<WPARAM>(ui.small),TRUE);
   SetWindowSubclass(card->radio,radioKeys,1,reinterpret_cast<DWORD_PTR>(card.get()));
   ui.cards.push_back(std::move(card));
}

// Delegate scrolling to a native scrollbar while the cards retain their own controls.
static LRESULT CALLBACK deviceEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_GETDLGCODE) return DLGC_WANTARROWS;
   if (message==WM_KEYDOWN && (wp==VK_UP || wp==VK_DOWN) && !ui.cards.empty())
   {
      app.activate(Select,ui.cards.front()->device.id); SetFocus(ui.cards.front()->radio); return 0;
   }
   if (message==WM_SIZE || message==WM_APP) { if (ui.controls[Drives]) layoutCards(); return 0; }
   if (message==WM_VSCROLL || message==WM_MOUSEWHEEL)
   {
      SCROLLINFO info={}; info.cbSize=sizeof(info); info.fMask=SIF_ALL; GetScrollInfo(hwnd,SB_VERT,&info); int next=info.nPos;
      if (message==WM_MOUSEWHEEL) next-=GET_WHEEL_DELTA_WPARAM(wp)*int(35*scale)/WHEEL_DELTA;
      else
      {
         int code=LOWORD(wp);
         if (code==SB_LINEUP) next-=int(35*scale);
         if (code==SB_LINEDOWN) next+=int(35*scale);
         if (code==SB_PAGEUP) next-=info.nPage;
         if (code==SB_PAGEDOWN) next+=info.nPage;
         if (code==SB_TOP) next=0;
         if (code==SB_BOTTOM) next=info.nMax;
         if (code==SB_THUMBTRACK || code==SB_THUMBPOSITION) next=info.nTrackPos;
      }
      SetScrollPos(hwnd,SB_VERT,next,TRUE); ui.deviceScroll=GetScrollPos(hwnd,SB_VERT); layoutCards(); return 0;
   }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Lay out native fields in a standard scrollable child window, keeping the footer fixed.
static void layoutContent()
{
   if (ui.layingout || !ui.panel) return;
   ui.layingout=true;
   RECT rect; GetClientRect(ui.panel,&rect);
   float width=rect.right/scale,x=30,w=width-60,y=29;
      place(Step,x,y,w,20); y+=26;
   float h=textHeight(Heading,w); place(Heading,x,y,w,h); y+=h+9;
   h=textHeight(Intro,w); place(Intro,x,y,w,h); y+=h+22;
   for (int id:{PathLabel,Path,BrowseButton,Drives,Empty,EraseButton,Target,Status,Percent,Progress,Result})
   {
      bool isoField=app.page==0 && (id==PathLabel || id==Path || id==BrowseButton);
      bool usbField=app.page==1 && (id==Drives || id==Empty || id==EraseButton);
      bool progressField=app.page==2 && (id==Status || id==Percent || id==Progress);
      bool show=isoField || usbField || progressField;
      if (id==Target) show=app.page>=2;
      if (id==Result) show=app.page==3;
      if (id==Drives) show=app.page==1 && !app.devices.empty();
      if (id==Empty) show=app.page==1 && app.devices.empty();
      if (bool(GetWindowLongW(ui.controls[id],GWL_STYLE)&WS_VISIBLE)!=show) ShowWindow(ui.controls[id],show?SW_SHOW:SW_HIDE);
   }
   if (app.page==0)
   {
      for (int id:{PathLabel,Path,BrowseButton}) ShowWindow(ui.controls[id],SW_SHOW);
      place(PathLabel,x,y,w,21); y+=27;
      place(Path,x,y,w-94,35); place(BrowseButton,x+w-86,y,86,35); y+=48;
   }
   if (app.page==1)
   {
      if (app.devices.empty()) { ShowWindow(ui.controls[Empty],SW_SHOW); h=textHeight(Empty,w); place(Empty,x,y,w,h); y+=h; }
      else
      {
         h=std::min(210,std::max(62,int(app.devices.size())*70-8)); ShowWindow(ui.controls[Drives],SW_SHOW); place(Drives,x,y,w,h); y+=h;
      }
      y+=10; ShowWindow(ui.controls[EraseButton],SW_SHOW); place(EraseButton,x,y,w,34); y+=34;
   }
   if (app.page>=2)
   {
      if (app.page==3) { ShowWindow(ui.controls[Result],SW_SHOW); place(Result,x,y,42,42); y+=55; }
      ShowWindow(ui.controls[Target],SW_SHOW); h=textHeight(Target,w-24); place(Target,x+12,y+10,w-24,h); y+=h+20;
      if (app.page==2)
      {
         y+=21;
         for (int id:{Status,Percent,Progress}) ShowWindow(ui.controls[id],SW_SHOW);
         place(Status,x,y,w-50,21); place(Percent,x+w-45,y,45,21); y+=27; place(Progress,x,y,w,16); y+=16;
      }
   }
   bool messageVisible=app.page==0 || (app.page==1 && !app.message().empty());
   ShowWindow(ui.controls[Message],messageVisible?SW_SHOW:SW_HIDE);
   if (messageVisible)
   {
      if (app.page==1) y+=13;
      h=textHeight(Message,w);
      if (app.page==0) h=std::max(42.0f,h);
      place(Message,x,y,w,h); y+=h;
   }
   SCROLLINFO scroll={}; scroll.cbSize=sizeof(scroll); scroll.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
   scroll.nMin=0; scroll.nMax=int((y+(app.page==1?12:29))*scale)-1; scroll.nPage=rect.bottom; scroll.nPos=ui.scroll;
   SetScrollInfo(ui.panel,SB_VERT,&scroll,TRUE);
   int current=GetScrollPos(ui.panel,SB_VERT);
   if (current!=ui.scroll) { ScrollWindowEx(ui.panel,0,ui.scroll-current,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE); ui.scroll=current; }
   RECT after; GetClientRect(ui.panel,&after);
   ui.layingout=false;
   if (after.right!=rect.right) PostMessageW(ui.panel,WM_APP,0,0);
}

// Let Windows provide its titlebar, shadow, resize borders and work-area maximization.
static void layoutWindow()
{
   if (!ui.panel) return;
   RECT rect; GetClientRect(window,&rect);
   float width=rect.right/scale,height=rect.bottom/scale,footer=height-58;
   float rail=std::max(175.0f,width*0.3f);
   for (int id:{Rail,Brand,Wordmark}) ShowWindow(ui.controls[id],SW_SHOW);
   place(Rail,0,0,rail,footer,false);
   place(Brand,25,28,31,33,false); place(Wordmark,65,29,83,30,false);
   for (int i=0; i<4; i++)
   {
      ShowWindow(ui.controls[StepNumber+i],SW_SHOW); ShowWindow(ui.controls[StepName+i],SW_SHOW);
      place(StepNumber+i,22,86+i*40,24,24,false); place(StepName+i,56,83+i*40,rail-66,31,false);
   }
   MoveWindow(ui.panel,int(rail*scale),0,int((width-rail)*scale),std::max(1,int(footer*scale)),TRUE);
   place(Separator,0,footer,width,1,false); place(Footer,15,footer+11,width-275,35,false); ShowWindow(ui.controls[Footer],SW_SHOW);
   place(CancelButton,width-253,footer+11,70,35,false); place(BackButton,width-175,footer+11,59,35,false); place(NextButton,width-108,footer+11,92,35,false);
   layoutContent();
}

// Refresh native controls without rebuilding an unchanged device list or losing selection.
static void updateWindow()
{
   if (ui.updating || !ui.panel) return;
   ui.updating=true;
   text(Step,"Step "+std::to_string(app.page+1)+" of 4"); text(Heading,app.heading()); text(Intro,app.introduction());
   text(Path,app.iso.path); text(Message,app.message());
   text(Target,app.target); text(Status,app.progressText()); text(Percent,std::to_string(app.percent)+"%");
   text(NextButton,app.page==3?(app.error.empty()?"Finish":"Try again"):"Next");
   SendMessageW(ui.controls[Progress],PBM_SETPOS,app.percent,0);
   SendMessageW(ui.controls[EraseButton],BM_SETCHECK,app.erase?BST_CHECKED:BST_UNCHECKED,0);
   SendMessageW(ui.controls[Result],STM_SETICON,reinterpret_cast<WPARAM>(LoadIconW(nullptr,app.error.empty()?IDI_INFORMATION:IDI_ERROR)),0);
   bool changed=ui.cards.size()!=app.devices.size();
   for (size_t i=0; !changed && i<ui.cards.size(); i++) changed=ui.cards[i]->device.id!=app.devices[i].id;
   // Defer native row rebuilding until the user's pointer release completes.
   if (changed && !(GetAsyncKeyState(VK_LBUTTON)&0x8000))
   {
      for (auto &card:ui.cards) DestroyWindow(card->window);
      ui.cards.clear(); ui.deviceScroll=0;
      for (const auto &device:app.devices) createCard(device);
   }
   for (auto &card:ui.cards)
   {
      auto device=std::find_if(app.devices.begin(),app.devices.end(),[&](const Device &d) { return d.id==card->device.id; });
      if (device==app.devices.end()) continue;
      card->device=*device;
      text(card->name,device->name); text(card->badge,device->label);
      SendMessageW(card->badge,WM_SETFONT,reinterpret_cast<WPARAM>(device->label.size()>3?ui.bold:ui.badge),TRUE);
      std::string reason=app.eligibility(*device);
      text(card->detail,sizeText(device->bytes)+" · "+(reason.empty()?device->detail:reason));
      SendMessageW(card->radio,BM_SETCHECK,device->id==app.selected?BST_CHECKED:BST_UNCHECKED,0);
      RedrawWindow(card->window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
   }
   EnableWindow(ui.controls[Drives],app.pending==None); EnableWindow(ui.controls[BrowseButton],app.pending==None);
   EnableWindow(ui.controls[EraseButton],!app.devices.empty() && app.pending==None);
   EnableWindow(ui.controls[NextButton],app.canContinue()); EnableWindow(ui.controls[BackButton],app.page==1); EnableWindow(ui.controls[CancelButton],app.page!=2);
   ShowWindow(ui.controls[BackButton],app.page<3?SW_SHOW:SW_HIDE); ShowWindow(ui.controls[CancelButton],app.page<3?SW_SHOW:SW_HIDE);
   if (ui.page!=app.page)
   {
      ui.page=app.page; ui.scroll=0;
      SetFocus(app.page==1?ui.controls[Drives]:ui.controls[NextButton]);
   }
   for (int i=0; i<4; i++)
   {
      SendMessageW(ui.controls[StepName+i],WM_SETFONT,reinterpret_cast<WPARAM>(i==app.page?ui.bold:ui.body),TRUE);
      InvalidateRect(ui.controls[StepNumber+i],nullptr,TRUE); InvalidateRect(ui.controls[StepName+i],nullptr,TRUE);
   }
   layoutWindow();
   layoutCards();
   ui.updating=false;
}

// Update on the GUI thread using ordinary control messages.
void invalidate()
{
   if (window) updateWindow();
}

// Use Windows' normal close and destruction path.
void windowAction(Action action)
{
   if (action==Close) PostMessageW(window,WM_CLOSE,0,0);
}

// Keep Browse native, modal, keyboard accessible and independent of shell quoting.
std::string chooseImage()
{
   std::vector<wchar_t> filename(32768); auto previous=wide(app.iso.path); wcsncpy(filename.data(),previous.c_str(),filename.size()-1);
   OPENFILENAMEW dialog={}; dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=window;
   dialog.lpstrFilter=L"ISO images (*.iso)\0*.iso\0All files\0*.*\0"; dialog.lpstrFile=filename.data(); dialog.nMaxFile=filename.size();
   dialog.lpstrTitle=L"Choose a Slax ISO image"; dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
   return GetOpenFileNameW(&dialog)?utf8(filename.data()):"";
}

// Confirm with a standard Windows dialog; No is the keyboard default.
bool confirmInstall(const std::string &target,bool erase)
{
   std::string text="Simulate installation?\n\n"+target+"\n\n"+(erase?"Drive preparation, file copying and boot setup will only be simulated. No data will be erased.":"File copying and boot setup will only be simulated. No data will be changed.");
   return MessageBoxW(window,wide(text).c_str(),L"Slax USB Installer",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES;
}

// Native scrolling moves child controls while Windows renders the scrollbar itself.
static LRESULT CALLBACK contentEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_COMMAND || message==WM_NOTIFY || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT) return SendMessageW(window,message,wp,lp);
   if (message==WM_SIZE || message==WM_APP) { layoutContent(); return 0; }
   if (message==WM_VSCROLL || message==WM_MOUSEWHEEL)
   {
      SCROLLINFO info={}; info.cbSize=sizeof(info); info.fMask=SIF_ALL; GetScrollInfo(hwnd,SB_VERT,&info);
      int next=info.nPos;
      if (message==WM_MOUSEWHEEL) next-=GET_WHEEL_DELTA_WPARAM(wp)*int(30*scale)/WHEEL_DELTA;
      else
      {
         int code=LOWORD(wp);
         if (code==SB_LINEUP) next-=int(20*scale);
         if (code==SB_LINEDOWN) next+=int(20*scale);
         if (code==SB_PAGEUP) next-=info.nPage;
         if (code==SB_PAGEDOWN) next+=info.nPage;
         if (code==SB_TOP) next=info.nMin;
         if (code==SB_BOTTOM) next=info.nMax;
         if (code==SB_THUMBTRACK || code==SB_THUMBPOSITION) next=info.nTrackPos;
      }
      SetScrollPos(hwnd,SB_VERT,next,TRUE); ui.scroll=GetScrollPos(hwnd,SB_VERT); layoutContent(); return 0;
   }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Bring keyboard-focused native fields into view after ordinary Tab navigation.
static void revealFocus()
{
   HWND focused=GetFocus();
   if (!IsChild(ui.panel,focused)) return;
   if (IsChild(ui.controls[Drives],focused))
   {
      RECT row,client; GetWindowRect(GetParent(focused),&row); MapWindowPoints(nullptr,ui.controls[Drives],reinterpret_cast<POINT *>(&row),2); GetClientRect(ui.controls[Drives],&client);
      int offset=0;
      if (row.top<0) offset=row.top;
      else if (row.bottom>client.bottom) offset=row.bottom-client.bottom;
      if (offset) { SetScrollPos(ui.controls[Drives],SB_VERT,ui.deviceScroll+offset,TRUE); ui.deviceScroll=GetScrollPos(ui.controls[Drives],SB_VERT); layoutCards(); }
   }
   RECT field,client; GetWindowRect(focused,&field); MapWindowPoints(nullptr,ui.panel,reinterpret_cast<POINT *>(&field),2); GetClientRect(ui.panel,&client);
   int offset=0;
   if (field.top<0) offset=field.top-int(6*scale);
   else if (field.bottom>client.bottom) offset=field.bottom-client.bottom+int(6*scale);
   if (offset) { SetScrollPos(ui.panel,SB_VERT,ui.scroll+offset,TRUE); ui.scroll=GetScrollPos(ui.panel,SB_VERT); layoutContent(); }
}

// Handle native control notifications; leave all nonclient behavior to Windows.
static LRESULT CALLBACK events(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_COMMAND && !ui.updating)
   {
      int id=LOWORD(wp);
      if (id==BrowseButton) app.activate(Browse);
      if (id==BackButton) app.activate(Back);
      if (id==NextButton) app.activate(Next);
      if (id==CancelButton) app.activate(Cancel);
      if (id==EraseButton) app.activate(Erase);
      return 0;
   }
   if (message==WM_CTLCOLORSTATIC)
   {
      auto item=reinterpret_cast<HWND>(lp); auto dc=reinterpret_cast<HDC>(wp); int id=GetDlgCtrlID(item);
      SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
      if (id==Message) SetTextColor(dc,!app.error.empty() && app.pending==None?RGB(162,41,41):RGB(84,96,86));
      if (id==Step || (id>=StepName && id<StepName+4)) SetTextColor(dc,RGB(57,119,33));
      if (id>=StepNumber && id<StepNumber+4 && id-StepNumber==app.page) { SetTextColor(dc,RGB(255,255,255)); return reinterpret_cast<LRESULT>(ui.green); }
      bool sidebar=id==Rail || (id>=StepNumber && id<StepNumber+4) || (id>=StepName && id<StepName+4);
      return reinterpret_cast<LRESULT>(sidebar?ui.rail:GetSysColorBrush(COLOR_WINDOW));
   }
   if (message==WM_SIZE) { layoutWindow(); return 0; }
   if (message==WM_GETMINMAXINFO)
   {
      auto info=reinterpret_cast<MINMAXINFO *>(lp); auto bounds=windowBounds(720,400);
      info->ptMinTrackSize={bounds.right-bounds.left,bounds.bottom-bounds.top}; return 0;
   }
   if (message==0x02e0)
   {
      scale=HIWORD(wp)/96.0f; fonts(); branding(); auto rect=reinterpret_cast<RECT *>(lp);
      SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
   }
   if (message==WM_TIMER) { app.tick(); return 0; }
   if (message==WM_CLOSE) { if (!app.confirming && !app.choosing) DestroyWindow(hwnd); return 0; }
   if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Start an ordinary Windows application with standard controls and no private titlebar.
int runWindow()
{
   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
   using DpiContext=BOOL (WINAPI *)(HANDLE);
   auto dpi=reinterpret_cast<DpiContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));
   if (dpi) dpi(reinterpret_cast<HANDLE>(-4));
   HDC screen=GetDC(nullptr); scale=GetDeviceCaps(screen,LOGPIXELSX)/96.0f; ReleaseDC(nullptr,screen);
   INITCOMMONCONTROLSEX common={sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
   Gdiplus::GdiplusStartupInput graphicsStartup; Gdiplus::GdiplusStartup(&graphicsToken,&graphicsStartup,nullptr);
   HINSTANCE instance=GetModuleHandleW(nullptr);
   WNDCLASSW cls={}; cls.lpfnWndProc=events; cls.hInstance=instance; cls.lpszClassName=L"SlaxUsbInstaller";
   cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1)); cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&cls);
   cls.lpfnWndProc=contentEvents; cls.lpszClassName=L"SlaxInstallerContent"; RegisterClassW(&cls);
   cls.lpfnWndProc=deviceEvents; cls.lpszClassName=L"SlaxInstallerDevices"; RegisterClassW(&cls);
   cls.lpfnWndProc=cardEvents; cls.lpszClassName=L"SlaxInstallerCard"; RegisterClassW(&cls);
   auto bounds=windowBounds(832,446); int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
   RECT workarea; SystemParametersInfoW(SPI_GETWORKAREA,0,&workarea,0);
   width=std::min(width,int(workarea.right-workarea.left-16)); height=std::min(height,int(workarea.bottom-workarea.top-16));
   window=CreateWindowExW(WS_EX_CONTROLPARENT,L"SlaxUsbInstaller",L"Slax USB Installer",windowStyle,workarea.left+(workarea.right-workarea.left-width)/2,workarea.top+(workarea.bottom-workarea.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
   if (!window) return 1;
   ui.rail=CreateSolidBrush(RGB(236,243,231)); ui.green=CreateSolidBrush(RGB(57,119,33)); ui.selected=CreateSolidBrush(RGB(238,247,231)); fonts();
   control(Rail,L"STATIC",SS_LEFT,window); control(Brand,L"STATIC",SS_BITMAP,window); control(Wordmark,L"STATIC",SS_BITMAP,window);
   const char *steps[]={"ISO image","USB drive","Installation","Finish"};
   for (int i=0; i<4; i++)
   {
      control(StepNumber+i,L"STATIC",SS_CENTER|SS_CENTERIMAGE,window); text(StepNumber+i,std::to_string(i+1));
      control(StepName+i,L"STATIC",SS_LEFT|SS_CENTERIMAGE,window); text(StepName+i,steps[i]);
   }
   ui.panel=CreateWindowExW(WS_EX_CONTROLPARENT,L"SlaxInstallerContent",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,0,0,1,1,window,nullptr,instance,nullptr);
   for (int id:{Step,Heading,Intro,PathLabel,Message,Empty,Target,Status,Percent})
   {
      bool single=id==Step || id==PathLabel || id==Status || id==Percent;
      control(id,L"STATIC",SS_LEFT|SS_NOPREFIX|(single?SS_CENTERIMAGE:SS_EDITCONTROL),ui.panel);
   }
   control(Path,L"EDIT",ES_READONLY|ES_AUTOHSCROLL|WS_TABSTOP,ui.panel,WS_EX_CLIENTEDGE); control(BrowseButton,L"BUTTON",BS_PUSHBUTTON|WS_TABSTOP,ui.panel);
   control(Drives,L"SlaxInstallerDevices",WS_VSCROLL|WS_CLIPCHILDREN|WS_TABSTOP,ui.panel,WS_EX_CONTROLPARENT);
   control(EraseButton,L"BUTTON",BS_AUTOCHECKBOX|WS_TABSTOP,ui.panel); control(Progress,PROGRESS_CLASSW,0,ui.panel); SendMessageW(ui.controls[Progress],PBM_SETRANGE32,0,100);
   control(Result,L"STATIC",SS_ICON,ui.panel); control(Separator,L"STATIC",SS_ETCHEDHORZ,window); control(Footer,L"STATIC",SS_LEFT|SS_NOPREFIX|SS_CENTERIMAGE,window);
   control(CancelButton,L"BUTTON",BS_PUSHBUTTON|WS_TABSTOP,window); control(BackButton,L"BUTTON",BS_PUSHBUTTON|WS_TABSTOP,window); control(NextButton,L"BUTTON",BS_DEFPUSHBUTTON|WS_TABSTOP,window);
   text(PathLabel,"ISO file path"); text(BrowseButton,"Browse…"); text(Empty,"No USB drives detected. Connect a USB drive to continue.");
   text(EraseButton,"Erase all data on the selected device"); text(Footer,"Slax USB Installer · Simulation"); text(BackButton,"Back"); text(CancelButton,"Cancel");
   fonts(); branding(); updateWindow(); SetTimer(window,1,100,nullptr); ShowWindow(window,SW_SHOW); UpdateWindow(window); app.startScan();
   MSG message;
   while (GetMessageW(&message,nullptr,0,0)>0)
   {
      if (message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE && app.page!=2) { app.activate(Close); continue; }
      if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
      if (message.message==WM_KEYDOWN && (message.wParam==VK_TAB || message.wParam==VK_UP || message.wParam==VK_DOWN)) revealFocus();
   }
   app.stop(); window=nullptr;
   for (auto font:{ui.body,ui.small,ui.title,ui.intro,ui.bold,ui.badge}) DeleteObject(font);
   for (auto image:ui.brand) DeleteObject(image);
   ui.cards.clear(); DeleteObject(ui.rail); DeleteObject(ui.green); DeleteObject(ui.selected);
   Gdiplus::GdiplusShutdown(graphicsToken);
   return 0;
}

// Native command-line checks share discovery and ISO validation with the GUI.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int)
{
   int count; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
   if ((count==2 || count==3) && !wcscmp(args[1],L"--list-devices")) { printDevices(scanDevices(count==3?utf8(args[2]):"")); LocalFree(args); return 0; }
   if (count==3 && !wcscmp(args[1],L"--check-iso"))
   {
      auto image=inspectImage(utf8(args[2])); LocalFree(args);
      if (!image.error.empty()) { fprintf(stderr,"%s\n",image.error.c_str()); return 1; }
      printf("%s · %s\n",image.name.c_str(),sizeText(image.bytes,true).c_str()); return 0;
   }
   if (count==2) { app.iso=inspectImage(utf8(args[1])); app.source=app.iso.path; app.error=app.iso.error; }
   LocalFree(args);
   return runWindow();
}

