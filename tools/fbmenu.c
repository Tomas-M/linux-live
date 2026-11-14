/*
 * Framebuffer boot splash and profile picker. No graphics libraries required.
 * Build from tools/ with musl (i686 also runs on x86_64 with IA32 emulation):
 *   musl-gcc -Os -static -no-pie -s -ffunction-sections -fdata-sections \
 *     -fno-unwind-tables -fno-asynchronous-unwind-tables \
 *     -Wl,--gc-sections,--build-id=none -o ../initramfs/static/fbmenu fbmenu.c
 * Regenerate the embedded font with: python3 fbmenu-font.py DejaVuSansMono.ttf
 * Menu files contain: value<TAB>title<TAB>detail (or a plain value per line).
 * Add --cancel to a menu request to return an empty selection when Esc is pressed.
 * Add --refresh to return refresh on F5, including when the menu is empty.
 * Add --restore to return restore:VALUE on F8 (ignored for the value new).
 * --pause/--continue bracket driver loading; overlapping pauses are counted.
 * --shutdown BRANDDIR shows the same animation from the retained initramfs.
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
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "fbmenu-font.h"

static const char *socket_path="/run/fbmenu.sock";
#define DISABLED "/run/bootsplash.disabled"
struct request { char operation, cancel, refresh, restore; char title[128]; char file[1024]; };
struct item { char *value, *title, *detail; };
/* Keep navigation keys outside the Unicode range. */
enum { KEY_UP=0x110000, KEY_DOWN, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_F5, KEY_F8 };
enum phase { BOOT, HANDOFF, SHUTDOWN };
static int tty=-1, keyboard_fd=-1, fb=-1, graphics, disabled, raw, logfd=-1, animation=-1;
static struct termios saved;
static struct fb_var_screeninfo screen;
static struct fb_fix_screeninfo layout;
static unsigned char *pixels, *logo;
static int logo_width, logo_height, selected, count, menu_client=-1;
static unsigned accent=0x80b8e0;
static struct item *items;
static struct request menu;
static struct timespec menu_time;
static volatile sig_atomic_t running=1, released, shutdown_expired;

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
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
   value<<=(4-bytes)*8;
#endif
   size_t offset=(y+screen.yoffset)*layout.line_length+(x+screen.xoffset)*bytes;
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
static void text(int x, int y, const char *label, unsigned color, unsigned background, int width)
{
   int right=width?x+width:(int)screen.xres-16;
   while (*label) {
      const struct glyph *g=glyph(character(&label));
      if (x+g->advance>right) break;
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
   const char *navigation="Up/Down: select   Enter: confirm";
   char actions[120], hint[200];
   snprintf(actions,sizeof(actions),"%s%sEsc: %s",menu.refresh?"F5: refresh   ":"",
      menu.restore?"F8: restore   ":"",menu.cancel?"cancel":"text mode");
   snprintf(hint,sizeof(hint),"%s   %s",navigation,actions);
   if (!graphics) {
      dprintf(tty,"\033[2J\033[H%s\r\n\r\n\r\n",menu.title);
      int first=selected>8?selected-8:0;
      for (int i=first;i<count && i<first+12;i++)
         dprintf(tty,"%s%s   %s\033[K\033[0m\r\n",i==selected?"\033[7m":"",items[i].title,items[i].detail);
      if (strlen(hint)>78) dprintf(tty,"\r\n%s\r\n%s",navigation,actions);
      else dprintf(tty,"\r\n%s",hint);
      return;
   }
   clear_screen();
   int footer_extra=text_width(hint)>(int)screen.xres-32?20:0;
   int height=logo?logo_height:0;
   int rows=((int)screen.yres-height-166-footer_extra)/28;
   if (rows<1) { height=0; rows=((int)screen.yres-126-footer_extra)/28; }
   if (rows<1) rows=1;
   if (rows>6) rows=6;
   if (rows>count) rows=count;
   int top=((int)screen.yres-footer_extra-(height+(height?28:0)+40+rows*28))/2;
   if (height) draw_logo(top);
   int y=top+height+(height?28:0);
   text(((int)screen.xres-text_width(menu.title))/2,y,menu.title,0xaaaaaa,0,0);
   y+=40;
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
   /* Keep rows centered, with a readable width and metadata anchored at the right. */
   int width=title_width+(detail_width?24+detail_width:0)+32;
   if (width<480) width=480;
   if (width>720) width=720;
   if (width>(int)screen.xres-64) width=(int)screen.xres-64;
   if (detail_width>width/2) detail_width=width/2;
   title_width=width-32-(detail_width?24+detail_width:0);
   int x=((int)screen.xres-width)/2;
   for (int i=first;i<first+rows;i++) {
      unsigned background=0;
      if (i==selected) {
         background=((accent&0xff0000)/3&0xff0000)|((accent&0xff00)/3&0xff00)|((accent&255)/3);
         for (int row=-4;row<20;row++) for (int column=0;column<width;column++) pixel(x+column,y+row,background);
      }
      text(x+16,y,items[i].title,0xffffff,background,title_width);
      text(x+width-16-detail_width,y,items[i].detail,i==selected?0xffffff:0x999999,background,detail_width);
      y+=28;
   }
   /* Keep every shortcut readable on small framebuffers without widening the menu. */
   if (footer_extra) {
      text(((int)screen.xres-text_width(navigation))/2,(int)screen.yres-44,navigation,0x888888,0,0);
      text(((int)screen.xres-text_width(actions))/2,(int)screen.yres-24,actions,0x888888,0,0);
   } else text(((int)screen.xres-text_width(hint))/2,(int)screen.yres-24,hint,0x888888,0,0);
}

/* Hold every dots frame for 250 ms, independently of keyboard and boot requests. */
static void draw_dots(unsigned frame, int clear)
{
   static const unsigned masks[]={1,3,7,6,4,0};
   unsigned mask=clear?0:masks[frame%(sizeof(masks)/sizeof(*masks))];
   int y=((int)screen.yres-logo_height)/2;
   for (int row=0;row<4;row++) for (int x=0;x<28;x++) {
      int dot=x/12;
      unsigned color=(dot<3 && x%12<4 && (mask&(1<<dot)))?accent:0;
      pixel(((int)screen.xres-28)/2+x,y+logo_height+25+row,color);
   }
   if (clear) return;
   struct itimerspec delay={.it_value={.tv_nsec=250000000}};
   timerfd_settime(animation,0,&delay,NULL);
}

/* Draw the full splash only when it replaces a menu or first acquires the screen. */
static void draw_splash(unsigned frame)
{
   draw_logo(((int)screen.yres-logo_height)/2);
   draw_dots(frame,0);
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
   /* Shutdown messages already live in the console buffer; preserve them on Esc. */
   const char *clear=logfd>=0?"\033[2J\033[H\033[?25h":"\033[?25h";
   write_all(tty,clear,strlen(clear));
   klogctl(8,NULL,7);
   show_log();
   if (menu_client>=0) draw_menu(); else keyboard(0);
}

/* Consume a complete UTF-8 character, including ignored Alt combinations. */
static int read_character(unsigned char value)
{
   if (value<0x80) return value;
   if (value<0xc2 || value>0xf4) return 0;
   unsigned code=value&31, remaining=1, minimum=0x80;
   if (value>=0xf0) { code=value&7; remaining=3; minimum=0x10000; }
   else if (value>=0xe0) { code=value&15; remaining=2; minimum=0x800; }
   struct pollfd pending={keyboard_fd,POLLIN,0};
   while (remaining--) {
      if (poll(&pending,1,40)<=0 || read(keyboard_fd,&value,1)!=1 || (value&0xc0)!=0x80) return 0;
      code=(code<<6)|(value&63);
   }
   if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return 0;
   return code;
}

/* Bare Esc waits 100 ms; double Esc is immediate. Ignore unsupported sequences. */
static int key(void)
{
   unsigned char value;
   if (read(keyboard_fd,&value,1)!=1) return 0;
   if (value!=27) return read_character(value);
   struct pollfd pending={keyboard_fd,POLLIN,0};
   if (poll(&pending,1,100)<=0) return 27;
   if (read(keyboard_fd,&value,1)!=1) return 0;
   if (value==27) return 27;
   if (value!='[' && value!='O') { read_character(value); return 0; }
   char sequence[16]={0};
   int length=1;
   sequence[0]=value;
   while (poll(&pending,1,100)>0 && read(keyboard_fd,&value,1)==1) {
      if (length<(int)sizeof(sequence)-1) sequence[length]=value;
      length++;
      /* Linux function keys start with ESC [[; other ANSI keys end at 0x40..0x7e. */
      if (value>=0x40 && value<=0x7e && !(length==2 && value=='[')) break;
   }
   if (!strcmp(sequence,"[A") || !strcmp(sequence,"OA")) return KEY_UP;
   if (!strcmp(sequence,"[B") || !strcmp(sequence,"OB")) return KEY_DOWN;
   if (!strcmp(sequence,"[H") || !strcmp(sequence,"OH") || !strcmp(sequence,"[1~") || !strcmp(sequence,"[7~")) return KEY_HOME;
   if (!strcmp(sequence,"[F") || !strcmp(sequence,"OF") || !strcmp(sequence,"[4~") || !strcmp(sequence,"[8~")) return KEY_END;
   if (!strcmp(sequence,"[5~")) return KEY_PAGEUP;
   if (!strcmp(sequence,"[6~")) return KEY_PAGEDOWN;
   if (!strcmp(sequence,"[15~") || !strcmp(sequence,"[[E")) return KEY_F5;
   if (!strcmp(sequence,"[19~")) return KEY_F8;
   return 0;
}

/* Exit cleanly if initramfs shutdown or an interrupted client stops the server. */
static void stop_server(int signal_number)
{
   if (signal_number==SIGUSR1) released=1;
   running=0;
}

/* Bound the shutdown renderer even if console restoration itself stalls. */
static void shutdown_timeout(int signal_number)
{
   (void)signal_number;
   if (shutdown_expired) _exit(0);
   shutdown_expired=1; running=0;
   alarm(1);
}

/* Release display memory before a driver replaces the framebuffer. */
static void pause_graphics(unsigned frame)
{
   if (pixels) { draw_dots(frame,1); munmap(pixels,layout.smem_len); }
   pixels=NULL; graphics=0;
   if (fb>=0) close(fb);
   fb=-1;
}

/* Uncoordinated loaders must replace the firmware framebuffer before we reopen it. */
static void resume_graphics(unsigned frame, int wait_for_driver)
{
   if (pixels) return;
   if (wait_for_driver) {
      FILE *file=fopen("/sys/class/graphics/fb0/name","r");
      if (!file) return;
      char name[18]={0};
      fgets(name,sizeof(name),file); fclose(file);
      name[strcspn(name,"\r\n")]=0;
      if (!name[0] || !strncmp(name,layout.id,sizeof(layout.id))) return;
   }
   if (fb>=0) close(fb);
   if (open_graphics()) return;
   ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1;
   /* fbcon skips mode setup while our VT is graphical; activate the driver's mode. */
   struct fb_var_screeninfo mode=screen;
   mode.activate=FB_ACTIVATE_NOW|FB_ACTIVATE_FORCE;
   ioctl(fb,FBIOPUT_VSCREENINFO,&mode);
   clear_screen();
   if (menu_client>=0) draw_menu(); else draw_splash(frame);
}

/* Own the tty during boot or shutdown, so Esc works during blocking jobs. */
static int serve(const char *directory, const char *log, enum phase phase, int ready_fd, unsigned frame)
{
   int resumed=phase==HANDOFF, shutting_down=phase==SHUTDOWN;
   if (shutting_down) {
      struct sigaction action={.sa_handler=shutdown_timeout};
      sigemptyset(&action.sa_mask); sigaction(SIGALRM,&action,NULL);
      alarm(10);
   }
   signal(SIGTERM,stop_server); signal(SIGINT,stop_server); signal(SIGHUP,stop_server); signal(SIGPIPE,SIG_IGN);
   signal(SIGUSR1,stop_server);
   tty=open("/dev/tty1",O_RDWR|O_NOCTTY);
   if (tty<0 || tcgetattr(tty,&saved)) return 1;
   /* Console resets can invalidate poll readiness; keyboard reads must never block IPC. */
   keyboard_fd=open("/dev/tty1",O_RDONLY|O_NOCTTY|O_NONBLOCK);
   if (keyboard_fd<0) return 1;
   /* Run from the initramfs again, taking over only after X and getty have stopped. */
   if (shutting_down) {
      unlink(DISABLED);
      if (open_graphics() || ioctl(tty,VT_ACTIVATE,1)) return 0;
      /* A refused or stalled VT switch must not hold up shutdown. */
      for (int attempt=0;attempt<50;attempt++) {
         struct vt_stat vt;
         if (ioctl(tty,VT_GETSTATE,&vt)) return 0;
         if (vt.v_active==1) break;
         if (attempt==49) return 0;
         usleep(20000);
      }
      /* Discard the old login screen; subsequent shutdown messages remain for Esc. */
      const char clear[]="\033[2J\033[H\033[?25l";
      write_all(tty,clear,sizeof(clear)-1);
   }
   animation=ready_fd>=0?6:timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
   if (animation<0) return 1;
   if (directory) branding(directory);
   if (log) logfd=open(log,O_RDONLY);
   int listener=socket(AF_UNIX,SOCK_STREAM,0), keep=0, keyboard_mode=0, taken=0;
   unsigned pauses=0;
   int coordinated=resumed && !access("/usr/libexec/fbmenu-driver",X_OK);
   ioctl(tty,KDGKBMODE,&keyboard_mode);
   struct sockaddr_un address={.sun_family=AF_UNIX};
   snprintf(address.sun_path,sizeof(address.sun_path),"%s",socket_path);
   unlink(socket_path);
   if (bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,4)) return 1;
   chmod(socket_path,0600); keyboard(1);
   if (!access(DISABLED,F_OK)) disable_splash();
   else if (resumed || shutting_down) {
      if (open_graphics()) disable_splash();
      else {
         ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1;
         if (shutting_down) clear_screen();
         if (ready_fd<0) draw_splash(frame);
      }
   }
   if ((resumed || shutting_down) && disabled) running=0;
   /* Allow X to request the VT while the splash still owns graphics mode. */
   struct vt_mode vt_mode={.mode=VT_PROCESS,.relsig=SIGUSR1,.acqsig=SIGUSR1};
   if (resumed && graphics) {
      ioctl(tty,VT_SETMODE,&vt_mode);
      /* A cooperating loader releases the framebuffer only when it needs to. */
      if (!coordinated) pause_graphics(frame);
   }
   if (ready_fd>=0) { write_all(ready_fd,"\n",1); close(ready_fd); }
   while (running) {
      if (resumed) {
         struct vt_stat vt;
         int current_mode=keyboard_mode;
         ioctl(tty,KDGKBMODE,&current_mode);
         if (current_mode!=keyboard_mode && (current_mode==K_RAW || current_mode==K_MEDIUMRAW || current_mode==K_OFF)) { taken=1; break; }
         /* Leave inactive splash pixels intact; getty restores text when X is ready. */
         if (!ioctl(tty,VT_GETSTATE,&vt) && vt.v_active!=1) { keep=!disabled; break; }
         int display_mode=KD_GRAPHICS;
         if (!ioctl(tty,KDGETMODE,&display_mode) && display_mode==KD_TEXT) break;
         if (!disabled && !pauses) resume_graphics(frame,!coordinated);
      }
      /* Init also resets termios during shutdown's exitrd handoff. */
      if (raw) {
         struct termios mode;
         if (tcgetattr(tty,&mode)) break;
         if (mode.c_lflag&ICANON) keyboard(1);
      }
      struct pollfd events[]={{listener,POLLIN,0},{keyboard_fd,raw?POLLIN:0,0},{menu_client,POLLHUP,0},{animation,POLLIN,0}};
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
            if (request.operation=='a') { pauses++; pause_graphics(frame); }
            if (request.operation=='c' && pauses) pauses--;
            if (request.operation=='m' && menu_client<0) {
               menu=request; menu_time=(struct timespec){0}; menu_client=client;
               keyboard(1);
               if (!disabled && !pauses && !open_graphics()) { ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1; }
               load_menu();
               draw_menu(); continue;
            }
            if (request.operation=='d') disable_splash();
            if (request.operation=='r') {
               /* Re-exec from /run in the real root, releasing all initrd files. */
               if (disabled) { write_all(client,"error\n",6); close(client); break; }
               unlink(socket_path);
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
            if (request.operation=='s' && !disabled && !pauses) {
               /* systemd restores KD_TEXT before handing shutdown to the initramfs. */
               int mode=KD_TEXT;
               ioctl(tty,KDGETMODE,&mode);
               if (!graphics || mode!=KD_GRAPHICS) {
                  if (open_graphics()) disable_splash();
                  else { ioctl(tty,KDSETMODE,KD_GRAPHICS); graphics=1; keyboard(1); clear_screen(); draw_splash(frame); }
               }
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
            else { disable_splash(); if (resumed || shutting_down) running=0; }
         }
         else if (menu_client>=0 && pressed==KEY_F5 && menu.refresh) finish_menu("refresh",frame);
         else if (menu_client>=0 && count) {
            int previous=selected;
            if (pressed=='j' || pressed==KEY_DOWN) selected=(selected+1)%count;
            if (pressed=='k' || pressed==KEY_UP) selected=(selected+count-1)%count;
            if (pressed=='g' || pressed==KEY_HOME) selected=0;
            if (pressed=='G' || pressed==KEY_END) selected=count-1;
            if (pressed=='u' || pressed==KEY_PAGEUP) selected=selected<5?0:selected-5;
            if (pressed=='d' || pressed==KEY_PAGEDOWN) selected=selected+5>=count?count-1:selected+5;
            int restore=pressed==KEY_F8 && menu.restore && strcmp(items[selected].value,"new");
            int confirm=pressed=='\r' || pressed=='\n';
            if (restore) write_all(menu_client,"restore:",8);
            if (confirm || restore) {
               finish_menu(items[selected].value,frame);
            } else if (selected!=previous) draw_menu();
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
            draw_dots(++frame,0);
      }
      if (disabled && menu_client<0) show_log();
   }
   if (shutdown_expired) disable_splash();
   if (released) keep=!disabled;
   if (!taken) {
      keyboard(0);
      if (!keep) { ioctl(tty,KDSETMODE,KD_TEXT); dprintf(tty,"\033[?25h"); }
      if (resumed) {
         if (released) ioctl(tty,VT_RELDISP,1);
         vt_mode.mode=VT_AUTO;
         ioctl(tty,VT_SETMODE,&vt_mode);
      }
   }
   unlink(socket_path); close(listener); close(keyboard_fd); close(tty); close(animation);
   if (shutting_down) alarm(0);
   return 0;
}

/* Connect to the initramfs owner, or start a temporary owner for a plain menu. */
static int connect_server(void)
{
   int fd=socket(AF_UNIX,SOCK_STREAM,0);
   struct sockaddr_un address={.sun_family=AF_UNIX};
   snprintf(address.sun_path,sizeof(address.sun_path),"%s",socket_path);
   if (!connect(fd,(struct sockaddr *)&address,sizeof(address))) return fd;
   close(fd); return -1;
}

/* Request an action; selected values alone go to stdout for shell substitution. */
int main(int argc, char **argv)
{
   if (argc==3 && !strcmp(argv[1],"--shutdown")) {
      /* systemd overlays /run at shutdown handoff; retain IPC in the initramfs root. */
      socket_path="/fbmenu-shutdown.sock";
      return serve(argv[2],NULL,SHUTDOWN,-1,0);
   }
   if (argc==4 && (!strcmp(argv[1],"--serve") || !strcmp(argv[1],"--resume")))
      return serve(argv[2],argv[3],!strcmp(argv[1],"--resume")?HANDOFF:BOOT,-1,0);
   if (argc==6 && !strcmp(argv[1],"--resume") && !strcmp(argv[4],"--ready"))
      return serve(argv[2],argv[3],HANDOFF,5,strtoul(argv[5],NULL,10));
   struct request request={0};
   while (argc>2 && argv[argc-1][0]=='-' && !strcmp(argv[1],"--menu")) {
      if (!strcmp(argv[argc-1],"--cancel")) request.cancel=1;
      else if (!strcmp(argv[argc-1],"--refresh")) request.refresh=1;
      else if (!strcmp(argv[argc-1],"--restore")) request.restore=1;
      else break;
      argc--;
   }
   if (argc==4 && !strcmp(argv[1],"--menu")) {
      request.operation='m';
      snprintf(request.title,sizeof(request.title),"%s",argv[2]);
      snprintf(request.file,sizeof(request.file),"%s",argv[3]);
   } else if (argc==3 && !strcmp(argv[1],"--handoff")) {
      request.operation='r'; snprintf(request.file,sizeof(request.file),"%s",argv[2]);
   } else if (argc==3 && !strcmp(argv[1],"--start")) {
      request.operation='s'; socket_path=argv[2];
   } else if (argc==2) {
      if (!strcmp(argv[1],"--start")) request.operation='s';
      if (!strcmp(argv[1],"--pause")) request.operation='a';
      if (!strcmp(argv[1],"--continue")) request.operation='c';
      if (!strcmp(argv[1],"--disable")) request.operation='d';
      if (!strcmp(argv[1],"--stop")) request.operation='q';
      if (!strcmp(argv[1],"--keep")) request.operation='h';
   }
   if (!request.operation) {
      fprintf(stderr,"Usage: fbmenu --serve BRANDDIR LOG | --shutdown BRANDDIR | --menu TITLE FILE [--cancel] [--refresh] [--restore] | --handoff ROOT | --start [SOCKET] | --pause | --continue | --disable | --stop | --keep\n");
      return 1;
   }
   int fd=connect_server(), child=0;
   if (fd<0 && (request.operation=='a' || request.operation=='c')) return 1;
   if (fd<0 && request.operation=='m') {
      child=fork();
      if (!child) _exit(serve(NULL,NULL,BOOT,-1,0));
   }
   for (int i=0;fd<0 && i<50;i++) { usleep(20000); fd=connect_server(); }
   if (fd<0 || write_all(fd,&request,sizeof(request))) return 1;
   /* Rendering must not indefinitely delay boot or shutdown if its owner stalls. */
   if (request.operation!='m') {
      struct timeval timeout={.tv_sec=2};
      setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
   }
   char buffer[1024]; ssize_t size; int failed=0, replied=0;
   while ((size=read(fd,buffer,sizeof(buffer)))>0) {
      replied=1;
      if (request.operation=='m') write_all(STDOUT_FILENO,buffer,size);
      else if (size>1) failed=1;
   }
   close(fd);
   if (child>0) {
      request.operation='q'; fd=connect_server();
      if (fd>=0) { write_all(fd,&request,sizeof(request)); close(fd); }
      waitpid(child,NULL,0);
   }
   return failed?failed:!replied;
}
