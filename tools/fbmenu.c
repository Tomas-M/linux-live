/*
 * Framebuffer boot splash and session picker. No graphics libraries required.
 * Build from tools/ with musl (i686 also runs on x86_64 with IA32 emulation):
 *   musl-gcc -Os -static -no-pie -s -ffunction-sections -fdata-sections \
 *     -fno-unwind-tables -fno-asynchronous-unwind-tables \
 *     -Wl,--gc-sections,--build-id=none -o ../initramfs/static/fbmenu fbmenu.c
 * Regenerate the embedded font with: python3 fbmenu-font.py DejaVuSans.ttf
 * Menu files contain: value<TAB>title<TAB>detail (or a plain value per line).
 * Add --cancel to a menu request to return an empty selection when Esc is pressed.
 * One process owns both rendering and keyboard input; clients use a local socket.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "fbmenu-font.h"

#define SOCKET "/run/fbmenu.sock"
#define DISABLED "/run/bootsplash.disabled"
struct request { char operation, cancel; char title[128]; char file[1024]; };
struct item { char *value, *title, *detail; };
static int tty=-1, fb=-1, graphics, disabled, raw, logfd=-1, animation=-1;
static struct termios saved;
static struct fb_var_screeninfo screen;
static struct fb_fix_screeninfo layout;
static unsigned char *pixels, *logo;
static int logo_width, logo_height, selected, count, menu_client=-1;
static unsigned accent=0x80b8e0;
static struct item *items;
static struct request menu;
static struct timespec menu_time;
static volatile sig_atomic_t running=1;

/* Finish a message even when a signal interrupts a local socket or tty write. */
static int write_all(int fd, const void *data, size_t size)
{
   const char *p=data;
   while (size) {
      ssize_t n=write(fd,p,size);
      if (n<0 && errno==EINTR) continue;
      if (n<=0) return -1;
      p+=n; size-=n;
   }
   return 0;
}

/* Switch keyboard ownership without changing the firmware's framebuffer mode. */
static void keyboard(int enable)
{
   struct termios mode=saved;
   if (enable) cfmakeraw(&mode);
   mode.c_oflag=saved.c_oflag;
   tcsetattr(tty,TCSANOW,&mode);
   raw=enable;
}

/* Pack RGB channels using the framebuffer's actual color offsets. */
static void pixel(int x, int y, unsigned color)
{
   if (x<0 || y<0 || x>=(int)screen.xres || y>=(int)screen.yres) return;
   uint32_t value=(((color>>16)&255)>>(8-screen.red.length))<<screen.red.offset;
   value|=(((color>>8)&255)>>(8-screen.green.length))<<screen.green.offset;
   value|=((color&255)>>(8-screen.blue.length))<<screen.blue.offset;
   unsigned bytes=screen.bits_per_pixel/8;
   size_t offset=(y+screen.yoffset)*layout.line_length+(x+screen.xoffset)*bytes;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
   value<<=(4-bytes)*8;
#endif
   if (offset+bytes<=layout.smem_len) memcpy(pixels+offset,&value,bytes);
}

/* Decode UTF-8; unavailable font glyphs use a visible replacement character. */
static unsigned character(const char **text)
{
   const unsigned char *p=(const unsigned char *)*text;
   unsigned value=*p++, remaining=0;
   if (value>=0xf0) { value&=7; remaining=3; }
   else if (value>=0xe0) { value&=15; remaining=2; }
   else if (value>=0xc0) { value&=31; remaining=1; }
   while (remaining-- && (*p&0xc0)==0x80) value=(value<<6)|(*p++&63);
   *text=(const char *)p;
   return value;
}

/* Locate a glyph in the sorted, compact font subset. */
static const struct glyph *glyph(unsigned code)
{
   int lo=0, hi=sizeof(font_glyphs)/sizeof(*font_glyphs)-1;
   while (lo<=hi) {
      int mid=(lo+hi)/2;
      if (font_glyphs[mid].code==code) return &font_glyphs[mid];
      if (font_glyphs[mid].code<code) lo=mid+1; else hi=mid-1;
   }
   return &font_glyphs['?'-32];
}

/* Measure the same advances used by the rasterizer. */
static int text_width(const char *text)
{
   int width=0;
   while (*text) width+=glyph(character(&text))->advance;
   return width;
}

/* Render antialiased glyphs from nibble RLE, clipping long labels to the screen. */
static void text(int x, int y, const char *label, unsigned color, unsigned background)
{
   while (*label) {
      const struct glyph *g=glyph(character(&label));
      const unsigned char *p=font_pixels+g->offset;
      unsigned pos=0;
      while (pos<g->width*g->height) {
         unsigned run=(*p>>4)+1, alpha=*p++&15;
         unsigned shade=0;
         for (unsigned shift=0;shift<24;shift+=8)
            shade|=((((color>>shift)&255)*alpha+((background>>shift)&255)*(15-alpha))/15)<<shift;
         while (run-- && pos<g->width*g->height) {
            if (alpha) pixel(x+g->left+pos%g->width,y+g->top+pos/g->width,shade);
            pos++;
         }
      }
      x+=g->advance;
      if (x>=(int)screen.xres-16) break;
   }
}

/* Clear only visible scanlines, leaving virtual framebuffer storage alone. */
static void clear_screen(void)
{
   for (unsigned y=0;y<screen.yres;y++)
      memset(pixels+(y+screen.yoffset)*layout.line_length+screen.xoffset*(screen.bits_per_pixel/8),0,screen.xres*(screen.bits_per_pixel/8));
}

/* Keep the existing logo unscaled in every supported packed RGB framebuffer. */
static void draw_logo(int y)
{
   if (!logo) return;
   for (int row=0;row<logo_height;row++) for (int x=0;x<logo_width;x++) {
      const unsigned char *p=logo+(row*logo_width+x)*4;
      pixel(((int)screen.xres-logo_width)/2+x,y+row,(p[2]<<16)|(p[1]<<8)|p[0]);
   }
}

/* Map fbdev without requesting any resolution change. */
static int open_graphics(void)
{
   if (pixels) return 0;
   fb=open("/dev/fb0",O_RDWR);
   if (fb<0) return -1;
   if (ioctl(fb,FBIOGET_VSCREENINFO,&screen) || ioctl(fb,FBIOGET_FSCREENINFO,&layout)) return -1;
   if (layout.type!=FB_TYPE_PACKED_PIXELS || layout.visual!=FB_VISUAL_TRUECOLOR ||
       screen.bits_per_pixel<16 || screen.bits_per_pixel>32 || screen.bits_per_pixel%8) return -1;
   if (!screen.red.length || screen.red.length>8 || !screen.green.length || screen.green.length>8 || !screen.blue.length || screen.blue.length>8) return -1;
   if ((screen.yoffset+screen.yres)*layout.line_length>layout.smem_len || (screen.xoffset+screen.xres)*(screen.bits_per_pixel/8)>layout.line_length) return -1;
   pixels=mmap(NULL,layout.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fb,0);
   if (pixels==MAP_FAILED) { pixels=NULL; return -1; }
   return 0;
}

/* Load optional branding; the menu remains usable without a distro logo. */
static void branding(const char *directory)
{
   char path[1200], line[128];
   snprintf(path,sizeof(path),"%s/config",directory);
   FILE *file=fopen(path,"r");
   if (!file) return;
   while (fgets(line,sizeof(line),file)) {
      if (!strncmp(line,"LOGO_WIDTH=",11)) logo_width=strtol(line+11,NULL,10);
      if (!strncmp(line,"LOGO_HEIGHT=",12)) logo_height=strtol(line+12,NULL,10);
      if (!strncmp(line,"MENU_COLOR=",11)) accent=strtoul(line+11,NULL,16);
   }
   fclose(file);
   if (logo_width<=0 || logo_height<=0 || logo_width>4096 || logo_height>4096) return;
   snprintf(path,sizeof(path),"%s/logo.32",directory);
   file=fopen(path,"rb");
   if (!file) return;
   size_t size=(size_t)logo_width*logo_height*4;
   logo=malloc(size);
   if (!logo || fread(logo,1,size,file)!=size) { free(logo); logo=NULL; }
   fclose(file);
}

/* Reload a changing device list while preserving selection by its value. */
static int load_menu(void)
{
   struct stat status;
   if (stat(menu.file,&status)) return 0;
   if (status.st_mtim.tv_sec==menu_time.tv_sec && status.st_mtim.tv_nsec==menu_time.tv_nsec) return 0;
   FILE *file=fopen(menu.file,"r");
   if (!file) return 0;
   char *previous=count?strdup(items[selected].value):NULL;
   for (int i=0;i<count;i++) free(items[i].value);
   free(items); items=NULL; count=0; selected=0;
   char *line=NULL; size_t size=0;
   while (getline(&line,&size,file)>0) {
      line[strcspn(line,"\r\n")]=0;
      if (!*line) continue;
      struct item *next=realloc(items,(count+1)*sizeof(*items));
      if (!next) break;
      items=next;
      struct item *item=&items[count++];
      item->value=strdup(line);
      item->title=strchr(item->value,'\t');
      if (item->title) *item->title++=0; else item->title=item->value;
      item->detail=strchr(item->title,'\t');
      if (item->detail) *item->detail++=0; else item->detail="";
      if (previous && !strcmp(previous,item->value)) selected=count-1;
   }
   free(previous); free(line); fclose(file);
   menu_time=status.st_mtim;
   return 1;
}

/* Center logo and menu together; scroll the list within the available height. */
static void draw_menu(void)
{
   const char *hint=menu.cancel?"Up/Down: select     Enter: confirm     Esc: cancel":"Up/Down: select     Enter: confirm     Esc: text mode";
   if (!graphics) {
      dprintf(tty,"\033[2J\033[H%s\r\n\r\n",menu.title);
      int first=selected>8?selected-8:0;
      for (int i=first;i<count && i<first+12;i++)
         dprintf(tty,"%s%s   %s\033[K\033[0m\r\n",i==selected?"\033[7m":"",items[i].title,items[i].detail);
      dprintf(tty,"\r\n%s",hint);
      return;
   }
   clear_screen();
   int height=logo?logo_height:0;
   int rows=((int)screen.yres-height-150)/40;
   if (rows<1) { height=0; rows=((int)screen.yres-110)/40; }
   if (rows<1) rows=1;
   if (rows>6) rows=6;
   if (rows>count) rows=count;
   int top=((int)screen.yres-(height+(height?32:0)+32+rows*40))/2;
   if (height) draw_logo(top);
   int y=top+height+(height?32:0);
   text(((int)screen.xres-text_width(menu.title))/2,y,menu.title,0xaaaaaa,0);
   y+=36;
   int first=selected-rows/2;
   if (first>count-rows) first=count-rows;
   if (first<0) first=0;
   int title_width=0, detail_width=0;
   for (int i=0;i<count;i++) {
      int width=text_width(items[i].title);
      if (width>title_width) title_width=width;
      width=text_width(items[i].detail);
      if (width>detail_width) detail_width=width;
   }
   int width=title_width+(detail_width?36+detail_width:0)+40;
   if (width>(int)screen.xres-32) width=(int)screen.xres-32;
   int x=((int)screen.xres-width)/2;
   for (int i=first;i<first+rows;i++) {
      unsigned background=0;
      if (i==selected) {
         background=((accent&0xff0000)/3&0xff0000)|((accent&0xff00)/3&0xff00)|((accent&255)/3);
         for (int row=-7;row<29;row++) for (int column=0;column<width;column++) pixel(x+column,y+row,background);
      }
      text(x+20,y,items[i].title,0xffffff,background);
      text(x+20+title_width+36,y,items[i].detail,i==selected?0xffffff:0x999999,background);
      y+=40;
   }
   text(((int)screen.xres-text_width(hint))/2,(int)screen.yres-32,hint,0x888888,0);
}

/* Hold every dots frame for 250 ms, independently of keyboard and boot requests. */
static void draw_dots(unsigned frame)
{
   static const unsigned masks[]={1,3,7,6,4,0};
   unsigned mask=masks[frame%(sizeof(masks)/sizeof(*masks))];
   int y=((int)screen.yres-logo_height)/2;
   for (int row=0;row<4;row++) for (int x=0;x<28;x++) {
      int dot=x/12;
      unsigned color=(dot<3 && x%12<4 && (mask&(1<<dot)))?accent:0;
      pixel(((int)screen.xres-28)/2+x,y+logo_height+25+row,color);
   }
   struct itimerspec delay={.it_value={.tv_nsec=250000000}};
   timerfd_settime(animation,0,&delay,NULL);
}

/* Draw the full splash only when it replaces a menu or first acquires the screen. */
static void draw_splash(unsigned frame)
{
   draw_logo(((int)screen.yres-logo_height)/2);
   draw_dots(frame);
}

/* Show captured output immediately, then follow subsequent boot log writes. */
static void show_log(void)
{
   char buffer[4096]; ssize_t size;
   if (logfd<0) return;
   while ((size=read(logfd,buffer,sizeof(buffer)))>0) write_all(tty,buffer,size);
}

/* Return a selection or an empty cancellation result, preserving the splash. */
static void finish_menu(const char *value, unsigned frame)
{
   write_all(menu_client,value,strlen(value));
   write_all(menu_client,"\n",1); close(menu_client); menu_client=-1;
   if (graphics) { clear_screen(); draw_splash(frame); }
   else { keyboard(0); dprintf(tty,"\033[2J\033[H"); if (logfd>=0) lseek(logfd,0,SEEK_SET); show_log(); }
}

/* Leave graphics for the rest of this boot and show captured output. */
static void disable_splash(void)
{
   if (disabled) return;
   disabled=1; graphics=0;
   close(open(DISABLED,O_WRONLY|O_CREAT,0600));
   ioctl(tty,KDSETMODE,KD_TEXT);
   const char clear[]="\033[2J\033[H\033[?25h";
   write_all(tty,clear,sizeof(clear)-1);
   int printk=open("/proc/sys/kernel/printk",O_WRONLY);
   if (printk>=0) { write_all(printk,"7\n",2); close(printk); }
   show_log();
   if (menu_client>=0) draw_menu(); else keyboard(0);
}

/* Distinguish a bare Esc from terminal arrow-key escape sequences. */
static int key(void)
{
   unsigned char value;
   if (read(tty,&value,1)!=1) return 0;
   if (value!=27) return value;
   struct pollfd pending={tty,POLLIN,0};
   if (poll(&pending,1,40)<=0 || read(tty,&value,1)!=1) return 27;
   if (value!='[' && value!='O') return 27;
   if (poll(&pending,1,40)<=0 || read(tty,&value,1)!=1) return 0;
   if (value>='1' && value<='6') {
      unsigned char suffix;
      if (poll(&pending,1,40)>0) read(tty,&suffix,1);
   }
   switch (value) {
      case 'A': return 'k'; case 'B': return 'j';
      case 'H': case '1': return 'g'; case 'F': case '4': return 'G';
      case '5': return 'u'; case '6': return 'd';
   }
   return 0;
}

/* Exit cleanly if initramfs shutdown or an interrupted client stops the server. */
static void stop_server(int signal_number)
{
   (void)signal_number; running=0;
}

/* Own the tty for the whole initramfs phase, so Esc works during blocking jobs. */
static int serve(const char *directory, const char *log, int resumed, int ready_fd, unsigned frame)
{
   signal(SIGTERM,stop_server); signal(SIGINT,stop_server); signal(SIGHUP,stop_server); signal(SIGPIPE,SIG_IGN);
   tty=open("/dev/tty1",O_RDWR|O_NOCTTY);
   if (tty<0 || tcgetattr(tty,&saved)) return 1;
   animation=ready_fd>=0?6:timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
   if (animation<0) return 1;
   if (directory) branding(directory);
   if (log) logfd=open(log,O_RDONLY);
   int listener=socket(AF_UNIX,SOCK_STREAM,0), keep=0, keyboard_mode=0, taken=0;
   ioctl(tty,KDGKBMODE,&keyboard_mode);
   struct sockaddr_un address={.sun_family=AF_UNIX,.sun_path=SOCKET};
   unlink(SOCKET);
   if (bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,4)) return 1;
   chmod(SOCKET,0600); keyboard(1);
   if (!access(DISABLED,F_OK)) disable_splash();
   else if (resumed) {
      if (open_graphics()) disable_splash();
      else {
         ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1;
         if (ready_fd<0) draw_splash(frame);
      }
   }
   if (resumed && disabled) running=0;
   if (ready_fd>=0) { write_all(ready_fd,"\n",1); close(ready_fd); }
   while (running) {
      if (resumed) {
         struct vt_stat vt;
         struct termios mode;
         int current_mode=keyboard_mode;
         ioctl(tty,KDGKBMODE,&current_mode);
         if (current_mode!=keyboard_mode && (current_mode==K_RAW || current_mode==K_MEDIUMRAW || current_mode==K_OFF)) { taken=1; break; }
         if (!ioctl(tty,VT_GETSTATE,&vt) && vt.v_active!=1) break;
         if (tcgetattr(tty,&mode) || (raw && (mode.c_lflag&ICANON))) break;
      }
      struct pollfd events[]={{listener,POLLIN,0},{tty,raw?POLLIN:0,0},{menu_client,POLLHUP,0},{animation,POLLIN,0}};
      int ready=poll(events,4,250);
      if (ready<0 && errno!=EINTR) break;
      if (events[0].revents&POLLIN) {
         int client=accept(listener,NULL,NULL);
         struct request request;
         size_t received=0;
         while (received<sizeof(request)) {
            ssize_t n=read(client,(char *)&request+received,sizeof(request)-received);
            if (n<=0) break;
            received+=n;
         }
         if (received==sizeof(request)) {
            if (request.operation=='m' && menu_client<0) {
               menu=request; menu_time=(struct timespec){0}; menu_client=client;
               keyboard(1);
               if (!disabled && !open_graphics()) { ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1; }
               load_menu(); draw_menu(); continue;
            }
            if (request.operation=='d') disable_splash();
            if (request.operation=='r') {
               /* Re-exec from /run in the real root, releasing all initrd files. */
               if (disabled) { write_all(client,"error\n",6); close(client); break; }
               unlink(SOCKET);
               if (chroot(request.file) || chdir("/")) {
                  write_all(client,"error\n",6); close(client); break;
               }
               keyboard(0);
               /* The new owner acknowledges only after it has acquired the tty. */
               int null=open("/dev/null",O_RDWR);
               for (int i=0;i<3;i++) dup2(null,i);
               /* Carry the live timer and dots phase through exec without resetting either. */
               int ticking=fcntl(animation,F_DUPFD,6);
               dup2(client,5); dup2(ticking,6);
               for (int i=3;i<256;i++) if (i!=5 && i!=6) close(i);
               char progress[16];
               snprintf(progress,sizeof(progress),"%u",frame);
               execl("/run/fbmenu","fbmenu","--resume","/run/bootsplash","/var/log/boot.log","--ready",progress,(char *)NULL);
               int console=open("/dev/tty1",O_RDWR);
               ioctl(console,KDSETMODE,KD_TEXT);
               _exit(1);
            }
            if (request.operation=='s' && !disabled && !graphics) {
               if (open_graphics()) disable_splash();
               else { ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1; keyboard(1); clear_screen(); draw_splash(frame); }
            }
            if (request.operation=='q' || request.operation=='h') { keep=request.operation=='h' && graphics; running=0; }
            write_all(client,"\n",1);
         }
         close(client);
      }
      if (events[1].revents&POLLIN) {
         int pressed=key();
         if (pressed==27) {
            if (menu_client>=0 && menu.cancel) finish_menu("",frame);
            else { disable_splash(); if (resumed) running=0; }
         }
         else if (menu_client>=0 && count) {
            if (pressed=='j') selected=(selected+1)%count;
            if (pressed=='k') selected=(selected+count-1)%count;
            if (pressed=='g') selected=0;
            if (pressed=='G') selected=count-1;
            if (pressed=='u') selected=selected<5?0:selected-5;
            if (pressed=='d') selected=selected+5>=count?count-1:selected+5;
            if (pressed=='\r' || pressed=='\n') {
               finish_menu(items[selected].value,frame);
            } else draw_menu();
         }
      }
      if (events[1].revents&(POLLHUP|POLLERR)) break;
      if (events[2].revents&POLLHUP) {
         close(menu_client); menu_client=-1;
         if (graphics) { clear_screen(); draw_splash(frame); } else keyboard(0);
      }
      if (menu_client>=0) { if (load_menu()) draw_menu(); }
      if (events[3].revents&POLLIN) {
         uint64_t expired;
         if (read(animation,&expired,sizeof(expired))==sizeof(expired) && graphics && menu_client<0)
            draw_dots(++frame);
      }
      if (disabled && menu_client<0) show_log();
   }
   if (!taken) {
      keyboard(0);
      if (!keep) { ioctl(tty,KDSETMODE,KD_TEXT); dprintf(tty,"\033[?25h"); }
   }
   unlink(SOCKET); close(listener); close(tty); close(animation);
   return 0;
}

/* Connect to the initramfs owner, or start a temporary owner for a plain menu. */
static int connect_server(void)
{
   int fd=socket(AF_UNIX,SOCK_STREAM,0);
   struct sockaddr_un address={.sun_family=AF_UNIX,.sun_path=SOCKET};
   if (!connect(fd,(struct sockaddr *)&address,sizeof(address))) return fd;
   close(fd); return -1;
}

/* Request an action; selected values alone go to stdout for shell substitution. */
int main(int argc, char **argv)
{
   if (argc==4 && (!strcmp(argv[1],"--serve") || !strcmp(argv[1],"--resume")))
      return serve(argv[2],argv[3],!strcmp(argv[1],"--resume"),-1,0);
   if (argc==6 && !strcmp(argv[1],"--resume") && !strcmp(argv[4],"--ready"))
      return serve(argv[2],argv[3],1,5,strtoul(argv[5],NULL,10));
   int cancel=argc==5 && !strcmp(argv[4],"--cancel");
   if (cancel) argc--;
   struct request request={0};
   if (argc==4 && !strcmp(argv[1],"--menu")) {
      request.operation='m';
      request.cancel=cancel;
      snprintf(request.title,sizeof(request.title),"%s",argv[2]);
      snprintf(request.file,sizeof(request.file),"%s",argv[3]);
   } else if (argc==3 && !strcmp(argv[1],"--handoff")) {
      request.operation='r'; snprintf(request.file,sizeof(request.file),"%s",argv[2]);
   } else if (argc==2) {
      if (!strcmp(argv[1],"--start")) request.operation='s';
      if (!strcmp(argv[1],"--disable")) request.operation='d';
      if (!strcmp(argv[1],"--stop")) request.operation='q';
      if (!strcmp(argv[1],"--keep")) request.operation='h';
   }
   if (!request.operation) {
      fprintf(stderr,"Usage: fbmenu --serve BRANDDIR LOG | --menu TITLE FILE [--cancel] | --handoff ROOT | --start | --disable | --stop | --keep\n");
      return 1;
   }
   int fd=connect_server(), child=0;
   if (fd<0 && request.operation=='m') {
      child=fork();
      if (!child) _exit(serve(NULL,NULL,0,-1,0));
   }
   for (int i=0;fd<0 && i<50;i++) { usleep(20000); fd=connect_server(); }
   if (fd<0 || write_all(fd,&request,sizeof(request))) return 1;
   char buffer[1024]; ssize_t size; int failed=0, replied=0;
   while ((size=read(fd,buffer,sizeof(buffer)))>0) {
      replied=1;
      if (request.operation!='m' && size>1) failed=1;
      if (request.operation=='m') write_all(STDOUT_FILENO,buffer,size);
   }
   close(fd);
   if (child>0) {
      request.operation='q'; fd=connect_server();
      if (fd>=0) { write_all(fd,&request,sizeof(request)); close(fd); }
      waitpid(child,NULL,0);
   }
   return failed || !replied;
}
