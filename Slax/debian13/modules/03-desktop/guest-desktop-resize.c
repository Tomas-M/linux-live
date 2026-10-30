/* Restore the wallpaper after the guest agent changes the X11 screen size. */
#include <X11/Xlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
   Display *display=XOpenDisplay(NULL);
   if (!display) return 1;
   Window root=DefaultRootWindow(display);
   XWindowAttributes size;
   XGetWindowAttributes(display, root, &size);
   XSelectInput(display, root, StructureNotifyMask);
   char wallpaper[PATH_MAX];
   snprintf(wallpaper, sizeof(wallpaper), "%s/.fehbg", getenv("HOME"));
   while (1)
   {
      XEvent event;
      XNextEvent(display, &event);
      if (event.type!=ConfigureNotify) continue;
      if (event.xconfigure.width==size.width && event.xconfigure.height==size.height) continue;
      size.width=event.xconfigure.width;
      size.height=event.xconfigure.height;
      if (access(wallpaper, R_OK)!=0) continue;
      if (fork()==0)
      {
         execl("/bin/sh", "sh", wallpaper, (char *)NULL);
         _exit(127);
      }
      wait(NULL);
   }
}
