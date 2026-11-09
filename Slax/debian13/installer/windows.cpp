#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <setupapi.h>
#include <winioctl.h>
#include <shellapi.h>
// Expose optional Vista COM interfaces while retaining XP as the executable's API target.
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000
#include <shlobj.h>
#include <shobjidl.h>
#pragma pop_macro("NTDDI_VERSION")
#include <objidl.h>
#include <gdiplus.h>
#include <set>
#include <memory>
#include <cmath>
#include <cstring>
#include "installer.h"
#include "images.h"
#include "windows-native.h"

static HWND window;
static float scale=1;

enum { Rail=100,StepNumber=110,StepName=120,Brand=130,Wordmark,Step=140,Heading,Intro,PathLabel,Path,BrowseButton,Message,Drives,Empty,EraseButton,Target,Status,Percent,Progress,Result,Footer,BackButton,NextButton,Separator };
static const DWORD windowStyle=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;

struct WinCard
{
   Device device;
   HWND window=nullptr,badge=nullptr,name=nullptr,location=nullptr,detail=nullptr,radio=nullptr;
   bool pressed=false;
};

static ULONG_PTR graphicsToken;

struct WinUi
{
   HWND panel=nullptr,controls[180]={};
   HFONT body=nullptr,small=nullptr,title=nullptr,intro=nullptr,bold=nullptr,smallBold=nullptr;
   HBRUSH rail=nullptr,green=nullptr,selected=nullptr;
   HBITMAP brand[7]={};
   HICON clover=nullptr;
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
   HWND item=CreateWindowExW(extended,type,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,0,0,1,1,parent,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
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
   HFONT old[]={ui.body,ui.small,ui.title,ui.intro,ui.bold,ui.smallBold};
   HFONT *slots[]={&ui.body,&ui.small,&ui.title,&ui.intro,&ui.bold,&ui.smallBold};
   int sizes[]={14,13,28,16,14,13};
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
      SendMessageW(card->location,WM_SETFONT,reinterpret_cast<WPARAM>(ui.smallBold),TRUE);
      SendMessageW(card->name,WM_SETFONT,reinterpret_cast<WPARAM>(ui.bold),TRUE);
      SendMessageW(card->detail,WM_SETFONT,reinterpret_cast<WPARAM>(ui.small),TRUE);
   }
   for (auto font:old) if (font) DeleteObject(font);
}

// Cache branding and Partition Manager icons as native bitmaps at the current DPI.
static void branding()
{
   auto icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,int(32*scale),int(32*scale),0));
   SendMessageW(ui.controls[Brand],STM_SETICON,reinterpret_cast<WPARAM>(icon),0);
   if (ui.clover) DestroyIcon(ui.clover);
   ui.clover=icon;
   const int widths[]={32,83,24,24,24,42,42},heights[]={32,30,24,24,24,42,42};
   for (int i=1; i<7; i++)
   {
      size_t length=installer_images[i].size;
      HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,length);
      memcpy(GlobalLock(data),installer_images[i].data,length); GlobalUnlock(data);
      IStream *stream; CreateStreamOnHGlobal(data,TRUE,&stream);
      HBITMAP bitmap;
      {
         Gdiplus::Bitmap source(stream);
         Gdiplus::Bitmap resized(int(widths[i]*scale),int(heights[i]*scale),PixelFormat32bppPARGB);
         {
            Gdiplus::Graphics graphics(&resized);
            COLORREF background=i>=5?GetSysColor(COLOR_WINDOW):RGB(236,243,231);
            graphics.Clear(Gdiplus::Color(255,GetRValue(background),GetGValue(background),GetBValue(background)));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            int inset=i>=5?int(5*scale):0;
            graphics.DrawImage(&source,inset,inset,int(resized.GetWidth())-2*inset,int(resized.GetHeight())-2*inset);
         }
         resized.GetHBITMAP(Gdiplus::Color(255,236,243,231),&bitmap);
      }
      stream->Release();
      if (i<2) SendMessageW(ui.controls[Brand+i],STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bitmap));
      else if (i<5) for (auto &card:ui.cards) { if (deviceIcon(card->device)+2==i) SendMessageW(card->badge,STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bitmap)); }
      else if (i==(app.error.empty()?5:6)) SendMessageW(ui.controls[Result],STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bitmap));
      if (ui.brand[i]) DeleteObject(ui.brand[i]);
      ui.brand[i]=bitmap;
   }
}

static bool position(HWND item,int x,int y,int width,int height);

// Measure the bold native path so the regular metadata starts immediately after it.
static void layoutCard(WinCard &card)
{
   RECT bounds; GetClientRect(card.window,&bounds); int width=bounds.right;
   auto location=wide(deviceLocation(card.device));
   HDC dc=GetDC(card.window); auto old=SelectObject(dc,ui.smallBold); SIZE size;
   GetTextExtentPoint32W(dc,location.c_str(),location.size(),&size); SelectObject(dc,old); ReleaseDC(card.window,dc);
   int available=std::max(1,width-int(108*scale)),prefix=std::min(int(size.cx),int(available*0.7));
   position(card.badge,int(21*scale),int(19*scale),int(24*scale),int(24*scale));
   position(card.name,int(66*scale),int(9*scale),available,int(22*scale));
   position(card.location,int(66*scale),int(31*scale),prefix,int(21*scale));
   position(card.detail,int(66*scale)+prefix,int(31*scale),available-prefix,int(21*scale));
   position(card.radio,width-int(29*scale),int(22*scale),int(18*scale),int(18*scale));
}

// Queue changed child geometry without copying old text pixels or repainting midway through layout.
static bool position(HWND item,int x,int y,int width,int height)
{
   width=std::max(1,width); height=std::max(1,height);
   RECT rect; GetWindowRect(item,&rect); MapWindowPoints(nullptr,GetParent(item),reinterpret_cast<POINT *>(&rect),2);
   if (rect.left==x && rect.top==y && rect.right-rect.left==width && rect.bottom-rect.top==height) return false;
   SetWindowPos(item,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
   return true;
}

// Position native child windows in logical pixels, applying content scrolling once.
static void place(int id,float x,float y,float width,float height,bool content=true)
{
   position(ui.controls[id],int(x*scale),int(y*scale)-(content?ui.scroll:0),int(width*scale),int(height*scale));
}

// Center the editable line from its actual GDI metrics after resize, font and theme changes.
static LRESULT CALLBACK pathEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)
{
   LRESULT result=DefSubclassProc(hwnd,message,wp,lp);
   // The visually single-line path must pass Tab and Enter to native dialog navigation.
   if (message==WM_GETDLGCODE) return result&~(DLGC_WANTTAB|DLGC_WANTALLKEYS);
   if (message==WM_SIZE || message==WM_SETFONT || message==WM_THEMECHANGED)
   {
      RECT rect; GetClientRect(hwnd,&rect);
      HDC dc=GetDC(hwnd); auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0)));
      TEXTMETRICW metrics; GetTextMetricsW(dc,&metrics); SelectObject(dc,old); ReleaseDC(hwnd,dc);
      int inset=std::max(2,int(std::round(3*scale)));
      rect.left+=inset; rect.right=std::max(rect.left+1,rect.right-inset);
      rect.top=std::max(0,int((rect.bottom-metrics.tmHeight)/2)); rect.bottom=rect.top+metrics.tmHeight;
      SendMessageW(hwnd,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&rect));
      InvalidateRect(hwnd,nullptr,TRUE);
   }
   return result;
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
   bool selected=app.isSelected(card.device);
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
   bool moved=false;
   for (size_t i=0; i<ui.cards.size(); i++) moved=position(ui.cards[i]->window,0,int(i*70*scale)-ui.deviceScroll,bounds.right,int(62*scale)) || moved;
   if (moved) RedrawWindow(panel,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
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
      layoutCard(*card); return 0;
   }
   if (message==WM_PAINT)
   {
      PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint); paintCard(*card,dc); EndPaint(hwnd,&paint); return 0;
   }
   if (message==WM_ERASEBKGND || message==WM_PRINTCLIENT) { paintCard(*card,reinterpret_cast<HDC>(wp)); return 1; }
   if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN)
   {
      HDC dc=reinterpret_cast<HDC>(wp); bool selected=app.isSelected(card->device);
      COLORREF background=selected?RGB(238,247,231):GetSysColor(COLOR_WINDOW);
      SetBkMode(dc,OPAQUE); SetBkColor(dc,background); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
      if (reinterpret_cast<HWND>(lp)==card->badge) { SetBkColor(dc,RGB(236,243,231)); SetTextColor(dc,RGB(57,119,33)); return reinterpret_cast<LRESULT>(ui.rail); }
      if (reinterpret_cast<HWND>(lp)==card->detail || reinterpret_cast<HWND>(lp)==card->location) SetTextColor(dc,RGB(84,96,86));
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
   card->badge=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_BITMAP|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->name=CreateWindowExW(0,L"STATIC",wide(device.name).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->detail=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->location=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,0,0,1,1,card->window,nullptr,instance,nullptr);
   card->radio=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,0,0,1,1,card->window,nullptr,instance,nullptr);
   SendMessageW(card->badge,STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(ui.brand[deviceIcon(device)+2]));
   SendMessageW(card->location,WM_SETFONT,reinterpret_cast<WPARAM>(ui.smallBold),TRUE);
   SendMessageW(card->name,WM_SETFONT,reinterpret_cast<WPARAM>(ui.bold),TRUE); SendMessageW(card->detail,WM_SETFONT,reinterpret_cast<WPARAM>(ui.small),TRUE);
   SetWindowSubclass(card->radio,radioKeys,1,reinterpret_cast<DWORD_PTR>(card.get()));
   ui.cards.push_back(std::move(card));
}

// Add a font-independent dropdown indicator while preserving the native button's frame and states.
static LRESULT CALLBACK browseEvents(HWND button,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)
{
   auto result=DefSubclassProc(button,message,wp,lp);
   if (message==WM_PAINT)
   {
      RECT rect; GetClientRect(button,&rect); auto dc=GetDC(button);
      int x=rect.right-int(13*scale),y=rect.bottom/2;
      POINT arrow[]={{x-int(4*scale),y-int(2*scale)},{x+int(4*scale),y-int(2*scale)},{x,y+int(3*scale)}};
      auto brush=SelectObject(dc,GetSysColorBrush(IsWindowEnabled(button)?COLOR_BTNTEXT:COLOR_GRAYTEXT));
      auto pen=SelectObject(dc,GetStockObject(NULL_PEN)); Polygon(dc,arrow,3);
      SelectObject(dc,pen); SelectObject(dc,brush); ReleaseDC(button,dc);
   }
   return result;
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
      next=std::max(info.nMin,std::min(next,info.nMax-std::max(0,int(info.nPage)-1)));
      if (next==info.nPos) return 0;
      SetScrollPos(hwnd,SB_VERT,next,TRUE); ui.deviceScroll=GetScrollPos(hwnd,SB_VERT); layoutCards(); return 0;
   }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Measure before moving controls so scrollbar changes cannot leave stale geometry or copied pixels.
static void layoutContent()
{
   if (ui.layingout || !ui.panel) return;
   ui.layingout=true;
   int page=app.page;
   RECT outer,viewport; GetWindowRect(ui.panel,&outer); GetClientRect(ui.panel,&viewport);
   struct Field { int id; float x,y,width,height; };
   std::vector<Field> fields;
   bool messageVisible=page==0 || (page==1 && !app.message().empty());
   for (int id:{PathLabel,Path,BrowseButton,Drives,Empty,EraseButton,Target,Status,Percent,Progress,Result,Message})
   {
      bool isoField=page==0 && (id==PathLabel || id==Path || id==BrowseButton);
      bool usbField=page==1 && (id==Drives || id==Empty || id==EraseButton);
      bool progressField=page==2 && (id==Status || id==Percent || id==Progress);
      bool show=isoField || usbField || progressField;
      if (id==Target) show=page>=2;
      if (id==Result) show=page==3;
      if (id==Percent) show=progressField && !app.indeterminate();
      if (id==Drives) show=page==1 && !app.devices.empty();
      if (id==Empty) show=page==1 && app.devices.empty();
      if (id==EraseButton) show=page==1 && !app.devices.empty();
      if (id==Message) show=messageVisible;
      if (bool(GetWindowLongW(ui.controls[id],GWL_STYLE)&WS_VISIBLE)!=show) ShowWindow(ui.controls[id],show?SW_SHOW:SW_HIDE);
   }
   // Always measure the full width first; a needed scrollbar can only increase wrapped height.
   int pixels=outer.right-outer.left;
   for (int pass=0; pass<2; pass++)
   {
      fields.clear();
      auto add=[&](int id,float x,float y,float width,float height) { fields.push_back({id,x,y,width,height}); };
      float x=30,w=pixels/scale-60,y=29,h;
      add(Step,x,y,w,20); y+=26;
      h=textHeight(Heading,w); add(Heading,x,y,w,h); y+=h+9;
      h=textHeight(Intro,w); add(Intro,x,y,w,h); y+=h+22;
      float messageWidth=w;
      float messageHeight=messageVisible?textHeight(Message,messageWidth):0;
      if (page==0)
      {
         add(PathLabel,x,y,w,21); y+=27;
         add(Path,x,y,w-108,35); add(BrowseButton,x+w-100,y,100,35); y+=48;
         messageHeight=std::max(42.0f,messageHeight);
      }
      if (page==1)
      {
         if (app.devices.empty()) { h=textHeight(Empty,w); add(Empty,x,y,w,h); }
         else
         {
            float reserve=10+std::max(34.0f,textHeight(EraseButton,w-20))+12+(messageVisible?messageHeight+13:0);
            h=std::max(62.0f,viewport.bottom/scale-y-reserve);
            add(Drives,x,y,w,h);
            y+=h+10; h=std::max(34.0f,textHeight(EraseButton,w-20)); add(EraseButton,x,y,w,h);
         }
         y+=h;
      }
      if (page>=2)
      {
         if (page==3) { add(Result,x,y,42,42); y+=55; }
         h=textHeight(Target,w-24); add(Target,x+12,y+10,w-24,h); y+=h+20;
         if (page==2)
         {
            float statusWidth=w-(app.indeterminate()?0:58);
            y+=21; h=std::max(21.0f,textHeight(Status,statusWidth));
            add(Status,x,y,statusWidth,h); add(Percent,x+w-45,y,45,21); y+=h+6; add(Progress,x,y,w,16); y+=16;
         }
      }
      if (messageVisible)
      {
         if (page==1) y+=13;
         add(Message,x,y,messageWidth,messageHeight); y+=messageHeight;
      }
      SCROLLINFO scroll={}; scroll.cbSize=sizeof(scroll); scroll.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
      scroll.nMin=0; scroll.nMax=int((y+(page==1?12:29))*scale)-1; scroll.nPage=viewport.bottom; scroll.nPos=ui.scroll;
      SetScrollInfo(ui.panel,SB_VERT,&scroll,TRUE); GetClientRect(ui.panel,&viewport);
      if (viewport.right==pixels) break;
      pixels=viewport.right;
   }
   ui.scroll=GetScrollPos(ui.panel,SB_VERT);
   for (const auto &field:fields) place(field.id,field.x,field.y,field.width,field.height);
   ui.layingout=false;
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
   place(Brand,25,28,32,32,false); place(Wordmark,66,29,83,30,false);
   for (int i=0; i<4; i++)
   {
      ShowWindow(ui.controls[StepNumber+i],SW_SHOW); ShowWindow(ui.controls[StepName+i],SW_SHOW);
      place(StepNumber+i,22,86+i*40,24,24,false); place(StepName+i,56,83+i*40,rail-66,31,false);
   }
   position(ui.panel,int(rail*scale),0,int((width-rail)*scale),int(footer*scale));
   place(Separator,0,footer,width,1,false); place(Footer,15,footer+11,width-197,35,false); ShowWindow(ui.controls[Footer],SW_SHOW);
   place(BackButton,width-175,footer+11,59,35,false); place(NextButton,width-108,footer+11,92,35,false);
   layoutContent();
}

// Refresh native controls without rebuilding an unchanged device list or losing selection.
static void updateWindow()
{
   if (ui.updating || !ui.panel) return;
   ui.updating=true;
   int page=app.page;
   text(Step,"Step "+std::to_string(page+1)+" of 4"); text(Heading,app.heading()); text(Intro,app.introduction());
   text(Path,app.iso.path); text(Message,app.message());
   text(Target,app.target); text(Status,app.progressText()); text(Percent,std::to_string(app.percent)+"%");
   text(NextButton,app.page==3?(app.error.empty()?"Finish":"Try again"):"Next");
   LONG progressStyle=GetWindowLongW(ui.controls[Progress],GWL_STYLE); bool pulse=app.indeterminate();
   if (pulse!=bool(progressStyle&PBS_MARQUEE))
   {
      if (!pulse) SendMessageW(ui.controls[Progress],PBM_SETMARQUEE,FALSE,0);
      SetWindowLongW(ui.controls[Progress],GWL_STYLE,pulse?progressStyle|PBS_MARQUEE:progressStyle&~PBS_MARQUEE);
      if (pulse) SendMessageW(ui.controls[Progress],PBM_SETMARQUEE,TRUE,40);
   }
   if (!pulse) SendMessageW(ui.controls[Progress],PBM_SETPOS,app.percent,0);
   SendMessageW(ui.controls[EraseButton],BM_SETCHECK,app.erase?BST_CHECKED:BST_UNCHECKED,0);
   auto result=ui.brand[app.error.empty()?5:6];
   if (reinterpret_cast<HBITMAP>(SendMessageW(ui.controls[Result],STM_GETIMAGE,IMAGE_BITMAP,0))!=result) SendMessageW(ui.controls[Result],STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(result));
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
      text(card->name,device->name); text(card->location,deviceLocation(*device));
      auto bitmap=ui.brand[deviceIcon(*device)+2];
      if (reinterpret_cast<HBITMAP>(SendMessageW(card->badge,STM_GETIMAGE,IMAGE_BITMAP,0))!=bitmap) SendMessageW(card->badge,STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bitmap));
      text(card->detail," · "+sizeText(device->bytes)+" · "+device->detail);
      layoutCard(*card);
      WPARAM checked=app.isSelected(*device)?BST_CHECKED:BST_UNCHECKED;
      if (WPARAM(SendMessageW(card->radio,BM_GETCHECK,0,0))!=checked)
      {
         SendMessageW(card->radio,BM_SETCHECK,checked,0);
         RedrawWindow(card->window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
      }
   }
   EnableWindow(ui.controls[Drives],app.pending==None); EnableWindow(ui.controls[BrowseButton],app.pending==None); EnableWindow(ui.controls[Path],app.pending==None);
   EnableWindow(ui.controls[EraseButton],!app.devices.empty() && app.pending==None);
   EnableWindow(ui.controls[NextButton],app.canContinue()); EnableWindow(ui.controls[BackButton],page==1);
   ShowWindow(ui.controls[BackButton],page<3?SW_SHOW:SW_HIDE);
   EnableMenuItem(GetSystemMenu(window,FALSE),SC_CLOSE,MF_BYCOMMAND|(app.page==2?MF_GRAYED:MF_ENABLED));
   if (ui.page!=page)
   {
      ui.page=page; ui.scroll=0;
      HWND focus=ui.controls[NextButton];
      if (page==1) focus=ui.controls[Drives];
      if (page==2) focus=window;
      SetFocus(focus);
   }
   for (int i=0; i<4; i++)
   {
      SendMessageW(ui.controls[StepName+i],WM_SETFONT,reinterpret_cast<WPARAM>(i==page?ui.bold:ui.body),TRUE);
      InvalidateRect(ui.controls[StepNumber+i],nullptr,TRUE); InvalidateRect(ui.controls[StepName+i],nullptr,TRUE);
   }
   layoutWindow();
   layoutCards();
   ui.updating=false;
   // Refresh a stationary pointer too, keeping native frame and child-control cursors.
   POINT point;
   if (GetCursorPos(&point))
   {
      HWND hovered=WindowFromPoint(point);
      if (hovered==window || IsChild(window,hovered))
      {
         auto hit=SendMessageW(hovered,WM_NCHITTEST,0,MAKELPARAM(point.x,point.y));
         SendMessageW(hovered,WM_SETCURSOR,reinterpret_cast<WPARAM>(hovered),MAKELPARAM(hit,WM_MOUSEMOVE));
      }
   }
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
std::string chooseImage(bool directory)
{
   if (directory)
   {
      if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return "";
      std::string path; IFileOpenDialog *picker=nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker))))
      {
         DWORD options=0; picker->GetOptions(&options); picker->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
         picker->SetTitle(L"Choose a Slax source directory");
         if (SUCCEEDED(picker->Show(window)))
         {
            IShellItem *item=nullptr;
            if (SUCCEEDED(picker->GetResult(&item)))
            {
               wchar_t *name=nullptr;
               if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))) { path=utf8(name); CoTaskMemFree(name); }
               item->Release();
            }
         }
         picker->Release();
      }
      else
      {
         // XP has no Common Item Dialog; use its native folder picker without a new DLL dependency.
         BROWSEINFOW info={}; info.hwndOwner=window; info.lpszTitle=L"Choose a Slax source directory";
         info.ulFlags=BIF_RETURNONLYFSDIRS|BIF_USENEWUI|BIF_NONEWFOLDERBUTTON;
         auto item=SHBrowseForFolderW(&info);
         if (item) { wchar_t name[MAX_PATH]; if (SHGetPathFromIDListW(item,name)) path=utf8(name); CoTaskMemFree(item); }
      }
      CoUninitialize(); return path;
   }
   std::vector<wchar_t> filename(32768); auto previous=wide(app.iso.path); wcsncpy(filename.data(),previous.c_str(),filename.size()-1);
   OPENFILENAMEW dialog={}; dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=window;
   dialog.lpstrFilter=L"ISO images (*.iso)\0*.iso\0All files\0*.*\0"; dialog.lpstrFile=filename.data(); dialog.nMaxFile=filename.size();
   dialog.lpstrTitle=L"Choose a source Slax ISO image"; dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
   return GetOpenFileNameW(&dialog)?utf8(filename.data()):"";
}

// Lay out a standard resizable review dialog with selectable, scrollable native text.
static INT_PTR CALLBACK confirmEvents(HWND dialog,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_INITDIALOG)
   {
      auto text=reinterpret_cast<const std::wstring *>(lp);
      HWND body=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",text->c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,1,1,dialog,reinterpret_cast<HMENU>(100),GetModuleHandleW(nullptr),nullptr);
      HWND yes=CreateWindowExW(0,L"BUTTON",L"Install",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,1,1,dialog,reinterpret_cast<HMENU>(IDYES),GetModuleHandleW(nullptr),nullptr);
      HWND no=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,0,0,1,1,dialog,reinterpret_cast<HMENU>(IDNO),GetModuleHandleW(nullptr),nullptr);
      for (auto item:{body,yes,no}) SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(ui.body),TRUE);
      RECT work,parent; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0); GetWindowRect(window,&parent);
      int width=std::min(int(650*scale),int(work.right-work.left-32)),height=std::min(int(430*scale),int(work.bottom-work.top-48));
      int x=std::max(int(work.left),std::min(int(work.right-width),int((parent.left+parent.right-width)/2)));
      int y=std::max(int(work.top),std::min(int(work.bottom-height),int((parent.top+parent.bottom-height)/2)));
      SetWindowPos(dialog,nullptr,x,y,width,height,SWP_NOZORDER);
      SendMessageW(dialog,DM_SETDEFID,IDNO,0); SetFocus(no); return FALSE;
   }
   if (message==WM_SIZE)
   {
      RECT client; GetClientRect(dialog,&client); int margin=int(14*scale),button=int(86*scale),height=int(35*scale),gap=int(8*scale);
      position(GetDlgItem(dialog,100),margin,margin,client.right-margin*2,client.bottom-margin*3-height);
      position(GetDlgItem(dialog,IDYES),client.right-margin-button*2-gap,client.bottom-margin-height,button,height);
      position(GetDlgItem(dialog,IDNO),client.right-margin-button,client.bottom-margin-height,button,height);
      SCROLLINFO scroll={}; scroll.cbSize=sizeof(scroll); scroll.fMask=SIF_RANGE|SIF_PAGE;
      GetScrollInfo(GetDlgItem(dialog,100),SB_VERT,&scroll); ShowScrollBar(GetDlgItem(dialog,100),SB_VERT,scroll.nPage && scroll.nMax>=int(scroll.nPage));
      RedrawWindow(dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
      return TRUE;
   }
   if (message==WM_COMMAND && (LOWORD(wp)==IDYES || LOWORD(wp)==IDNO || LOWORD(wp)==IDCANCEL)) { EndDialog(dialog,LOWORD(wp)==IDYES?IDYES:IDNO); return TRUE; }
   if (message==WM_CLOSE) { EndDialog(dialog,IDNO); return TRUE; }
   if (message==WM_GETMINMAXINFO) { auto info=reinterpret_cast<MINMAXINFO *>(lp); info->ptMinTrackSize={LONG(420*scale),LONG(260*scale)}; return TRUE; }
   return FALSE;
}

// Keep every affected partition reviewable and default to Cancel before any storage write.
bool confirmInstall(const std::string &target,bool erase)
{
   DLGTEMPLATE layout={}; layout.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_CLIPCHILDREN|DS_MODALFRAME; layout.cx=320; layout.cy=220;
   std::wstring text;
   for (auto c:wide(target)) { if (c==L'\n') text+=L'\r'; text+=c; }
   std::wstring title=erase?L"Erase disk and install Slax?":L"Install Slax?";
   std::vector<unsigned char> resource(sizeof(layout)+4+(title.size()+1)*sizeof(wchar_t));
   memcpy(resource.data(),&layout,sizeof(layout)); memcpy(resource.data()+sizeof(layout)+4,title.c_str(),(title.size()+1)*sizeof(wchar_t));
   return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE *>(resource.data()),window,confirmEvents,reinterpret_cast<LPARAM>(&text))==IDYES;
}

// Report source and target failures using the ordinary native dialog palette.
void showInstallError(const std::string &message)
{
   auto text=wide(message); MSGBOXPARAMSW dialog={}; dialog.cbSize=sizeof(dialog);
   dialog.hwndOwner=window; dialog.hInstance=GetModuleHandleW(nullptr); dialog.lpszText=text.c_str();
   dialog.lpszCaption=app.page==0?L"Cannot use this Slax source":L"Cannot install Slax"; dialog.dwStyle=MB_OK|MB_USERICON; dialog.lpszIcon=MAKEINTRESOURCEW(2);
   MessageBoxIndirectW(&dialog);
}

// Native scrolling moves child controls while Windows renders the scrollbar itself.
static LRESULT CALLBACK contentEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_COMMAND || message==WM_NOTIFY || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT) return SendMessageW(window,message,wp,lp);
   if (message==WM_SIZE || message==WM_APP) { layoutContent(); RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); return 0; }
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
      next=std::max(info.nMin,std::min(next,info.nMax-std::max(0,int(info.nPage)-1)));
      if (next==info.nPos) return 0;
      SetScrollPos(hwnd,SB_VERT,next,TRUE); ui.scroll=GetScrollPos(hwnd,SB_VERT); layoutContent(); RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); return 0;
   }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Give the sidebar its own clipped background instead of overlapping sibling STATIC windows.
static LRESULT CALLBACK railEvents(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
   if (message==WM_CTLCOLORSTATIC) return SendMessageW(window,message,wp,lp);
   if (message==WM_ERASEBKGND) { RECT rect; GetClientRect(hwnd,&rect); FillRect(reinterpret_cast<HDC>(wp),&rect,ui.rail); return 1; }
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
   if (message==WM_SETCURSOR && LOWORD(lp)==HTCLIENT && app.busy())
   {
      SetCursor(LoadCursorW(nullptr,IDC_APPSTARTING)); return TRUE;
   }
   if (message==WM_COMMAND && !ui.updating)
   {
      int id=LOWORD(wp);
      if (id==Path && HIWORD(wp)==EN_CHANGE)
      {
         std::wstring path(GetWindowTextLengthW(ui.controls[Path])+1,L'\0'); GetWindowTextW(ui.controls[Path],path.data(),path.size());
         app.setImagePath(utf8(path.c_str()));
      }
      if (id==BrowseButton)
      {
         auto menu=CreatePopupMenu();
         AppendMenuW(menu,MF_STRING,Browse,L"Source ISO image…"); AppendMenuW(menu,MF_STRING,BrowseDirectory,L"Source Slax directory…");
         RECT rect; GetWindowRect(ui.controls[BrowseButton],&rect);
         auto action=TrackPopupMenu(menu,TPM_RIGHTALIGN|TPM_TOPALIGN|TPM_RETURNCMD|TPM_NONOTIFY,rect.right,rect.bottom,0,window,nullptr);
         DestroyMenu(menu); if (action) app.activate(static_cast<Action>(action));
      }
      if (id==BackButton) app.activate(Back);
      if (id==NextButton) app.activate(Next);
      if (id==EraseButton) app.activate(Erase);
      return 0;
   }
   if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT)
   {
      auto item=reinterpret_cast<HWND>(lp); auto dc=reinterpret_cast<HDC>(wp); int id=GetDlgCtrlID(item);
      SetBkMode(dc,OPAQUE); SetBkColor(dc,GetSysColor(COLOR_WINDOW)); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
      if (id==Message) SetTextColor(dc,RGB(84,96,86));
      if (id==Step || (id>=StepName && id<StepName+4)) SetTextColor(dc,RGB(57,119,33));
      if (id>=StepNumber && id<StepNumber+4 && id-StepNumber==app.page) { SetBkColor(dc,RGB(57,119,33)); SetTextColor(dc,RGB(255,255,255)); return reinterpret_cast<LRESULT>(ui.green); }
      bool stepControl=(id>=StepNumber && id<StepNumber+4) || (id>=StepName && id<StepName+4);
      bool branding=id==Brand || id==Wordmark;
      bool sidebar=id==Rail || branding || stepControl;
      if (sidebar) SetBkColor(dc,RGB(236,243,231));
      return reinterpret_cast<LRESULT>(sidebar?ui.rail:GetSysColorBrush(COLOR_WINDOW));
   }
   if (message==WM_SIZE) { if (wp!=SIZE_MINIMIZED) { layoutWindow(); RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); } return 0; }
   if (message==WM_GETMINMAXINFO)
   {
      auto info=reinterpret_cast<MINMAXINFO *>(lp); auto bounds=windowBounds(720,400);
      info->ptMinTrackSize={bounds.right-bounds.left,bounds.bottom-bounds.top}; return 0;
   }
   if (message==0x02e0)
   {
      float next=HIWORD(wp)/96.0f; ui.scroll=int(ui.scroll*next/scale); ui.deviceScroll=int(ui.deviceScroll*next/scale);
      scale=next; fonts(); branding(); auto rect=reinterpret_cast<RECT *>(lp);
      SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
   }
   if (message==WM_TIMER) { app.tick(); return 0; }
   if (message==WM_CLOSE) { if (!app.confirming && !app.choosing && app.page!=2) DestroyWindow(hwnd); return 0; }
   if (message==WM_QUERYENDSESSION && app.page==2) return FALSE;
   if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
   return DefWindowProcW(hwnd,message,wp,lp);
}

// Start an ordinary Windows application with standard controls and no private titlebar.
int runWindow()
{
   using DpiContext=BOOL (WINAPI *)(HANDLE);
   auto dpi=reinterpret_cast<DpiContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));
   if (dpi) dpi(reinterpret_cast<HANDLE>(-4));
   HDC screen=GetDC(nullptr); scale=GetDeviceCaps(screen,LOGPIXELSX)/96.0f; ReleaseDC(nullptr,screen);
   INITCOMMONCONTROLSEX common={sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
   Gdiplus::GdiplusStartupInput graphicsStartup; Gdiplus::GdiplusStartup(&graphicsToken,&graphicsStartup,nullptr);
   HINSTANCE instance=GetModuleHandleW(nullptr);
   WNDCLASSW cls={}; cls.style=CS_HREDRAW|CS_VREDRAW; cls.lpfnWndProc=events; cls.hInstance=instance; cls.lpszClassName=L"SlaxUsbInstaller";
   cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1)); cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&cls);
   cls.lpfnWndProc=contentEvents; cls.lpszClassName=L"SlaxInstallerContent"; RegisterClassW(&cls);
   cls.lpfnWndProc=deviceEvents; cls.lpszClassName=L"SlaxInstallerDevices"; RegisterClassW(&cls);
   cls.lpfnWndProc=cardEvents; cls.lpszClassName=L"SlaxInstallerCard"; RegisterClassW(&cls);
   cls.lpfnWndProc=railEvents; cls.lpszClassName=L"SlaxInstallerRail"; RegisterClassW(&cls);
   auto bounds=windowBounds(832,540); int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
   RECT workarea; SystemParametersInfoW(SPI_GETWORKAREA,0,&workarea,0);
   width=std::min(width,int(workarea.right-workarea.left-16)); height=std::min(height,int(workarea.bottom-workarea.top-16));
   window=CreateWindowExW(WS_EX_CONTROLPARENT,L"SlaxUsbInstaller",L"Slax Installer",windowStyle,workarea.left+(workarea.right-workarea.left-width)/2,workarea.top+(workarea.bottom-workarea.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
   if (!window) return 1;
   ui.rail=CreateSolidBrush(RGB(236,243,231)); ui.green=CreateSolidBrush(RGB(57,119,33)); ui.selected=CreateSolidBrush(RGB(238,247,231)); fonts();
   control(Rail,L"SlaxInstallerRail",WS_CLIPCHILDREN,window); control(Brand,L"STATIC",SS_ICON|SS_REALSIZEIMAGE,ui.controls[Rail]); control(Wordmark,L"STATIC",SS_BITMAP,ui.controls[Rail]);
   const char *steps[]={"Slax source","Target drive","Installation","Finish"};
   for (int i=0; i<4; i++)
   {
      control(StepNumber+i,L"STATIC",SS_CENTER|SS_CENTERIMAGE,ui.controls[Rail]); text(StepNumber+i,std::to_string(i+1));
      control(StepName+i,L"STATIC",SS_LEFT|SS_CENTERIMAGE,ui.controls[Rail]); text(StepName+i,steps[i]);
   }
   ui.panel=CreateWindowExW(WS_EX_CONTROLPARENT,L"SlaxInstallerContent",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,0,0,1,1,window,nullptr,instance,nullptr);
   for (int id:{Step,Heading,Intro,PathLabel,Message,Empty,Target,Status,Percent})
   {
      bool single=id==Step || id==PathLabel || id==Percent;
      control(id,L"STATIC",SS_LEFT|SS_NOPREFIX|(single?SS_CENTERIMAGE:SS_EDITCONTROL),ui.panel);
   }
   control(Path,L"EDIT",ES_MULTILINE|ES_AUTOHSCROLL|WS_TABSTOP,ui.panel,WS_EX_CLIENTEDGE); SetWindowSubclass(ui.controls[Path],pathEvents,1,0);
   control(BrowseButton,L"BUTTON",BS_PUSHBUTTON|WS_TABSTOP,ui.panel);
   SetWindowSubclass(ui.controls[BrowseButton],browseEvents,1,0);
   control(Drives,L"SlaxInstallerDevices",WS_VSCROLL|WS_CLIPCHILDREN|WS_TABSTOP,ui.panel,WS_EX_CONTROLPARENT);
   control(EraseButton,L"BUTTON",BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,ui.panel); control(Progress,PROGRESS_CLASSW,0,ui.panel); SendMessageW(ui.controls[Progress],PBM_SETRANGE32,0,100);
   control(Result,L"STATIC",SS_BITMAP,ui.panel); control(Separator,L"STATIC",SS_ETCHEDHORZ,window); control(Footer,L"STATIC",SS_LEFT|SS_NOPREFIX|SS_CENTERIMAGE,window);
   control(BackButton,L"BUTTON",BS_PUSHBUTTON|WS_TABSTOP,window); control(NextButton,L"BUTTON",BS_DEFPUSHBUTTON|WS_TABSTOP,window);
   text(PathLabel,"Source ISO image or Slax directory"); text(BrowseButton,"Browse…"); text(Empty,"No suitable disks found. Connect a disk to continue.");
   text(EraseButton,"Erase entire selected disk (including all other partitions on it)"); text(Footer,"Slax Installer"); text(BackButton,"Back");
   fonts(); branding(); updateWindow(); SetTimer(window,1,100,nullptr); ShowWindow(window,SW_SHOW); UpdateWindow(window); app.startScan();
   MSG message;
   while (GetMessageW(&message,nullptr,0,0)>0)
   {
      if (message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE && app.page!=2) { app.activate(Close); continue; }
      if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
      if (message.message==WM_KEYDOWN && (message.wParam==VK_TAB || message.wParam==VK_UP || message.wParam==VK_DOWN)) revealFocus();
   }
   app.stop(); window=nullptr;
   for (auto font:{ui.body,ui.small,ui.title,ui.intro,ui.bold,ui.smallBold}) DeleteObject(font);
   for (auto image:ui.brand) DeleteObject(image);
   DestroyIcon(ui.clover);
   ui.cards.clear(); DeleteObject(ui.rail); DeleteObject(ui.green); DeleteObject(ui.selected);
   Gdiplus::GdiplusShutdown(graphicsToken);
   return 0;
}

// Native command-line checks share discovery and ISO validation with the GUI.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int)
{
   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
   int count; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
   if ((count==2 || count==3) && !wcscmp(args[1],L"--list-devices")) { printDevices(scanDevices(count==3?utf8(args[2]):"")); LocalFree(args); return 0; }
   if (count==3 && !wcscmp(args[1],L"--check-iso"))
   {
      auto image=inspectImage(utf8(args[2])); LocalFree(args);
      if (!image.error.empty()) { fprintf(stderr,"%s\n",image.error.c_str()); return 1; }
      printf("%s · %s\n",image.name.c_str(),sizeText(image.bytes,true).c_str()); return 0;
   }
   app.setImagePath(count==2?utf8(args[1]):sourceImage(),count!=2);
   LocalFree(args);
   return runWindow();
}

