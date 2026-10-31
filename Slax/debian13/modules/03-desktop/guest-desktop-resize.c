/* Follow optional host display preferences and restore wallpaper after resizing. */
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Apply changed host preferences, without undoing a user's manual mode selection. */
static void follow_preferred(Display *display, Window root)
{
   typedef struct { RROutput output; RRMode mode; } Preferred;
   static Preferred *previous;
   static int count;
   XRRScreenResources *resources=XRRGetScreenResourcesCurrent(display, root);
   if (!resources) return;
   Preferred *next=calloc(resources->noutput, sizeof(*next));
   for (int i=0; i<resources->noutput; i++)
   {
      XRROutputInfo *output=XRRGetOutputInfo(display, resources, resources->outputs[i]);
      if (!output) continue;
      next[i].output=resources->outputs[i];
      if (output->connection==RR_Connected && output->npreferred)
         next[i].mode=output->modes[0];
      int changed=next[i].mode!=None;
      for (int j=0; j<count; j++)
         if (previous[j].output==next[i].output && previous[j].mode==next[i].mode) changed=0;
      if (changed)
      {
         if (fork()==0)
         {
            execlp("xrandr", "xrandr", "--output", output->name, "--auto", (char *)NULL);
            _exit(127);
         }
         wait(NULL);
      }
      XRRFreeOutputInfo(output);
   }
   free(previous);
   previous=next;
   count=resources->noutput;
   XRRFreeScreenResources(resources);
}

int main(int argc, char **argv)
{
   Display *display=XOpenDisplay(NULL);
   if (!display) return 1;
   Window root=DefaultRootWindow(display);
   XWindowAttributes size;
   XGetWindowAttributes(display, root, &size);
   XSelectInput(display, root, StructureNotifyMask);
   int randr_event, randr_error;
   int follow=argc==2 && !strcmp(argv[1], "--follow-preferred");
   if (follow && XRRQueryExtension(display, &randr_event, &randr_error))
   {
      XRRSelectInput(display, root, RROutputChangeNotifyMask|RROutputPropertyNotifyMask|RRResourceChangeNotifyMask);
      follow_preferred(display, root);
   }
   else follow=0;
   char wallpaper[PATH_MAX];
   snprintf(wallpaper, sizeof(wallpaper), "%s/.fehbg", getenv("HOME"));
   while (1)
   {
      XEvent event;
      XNextEvent(display, &event);
      if (follow && event.type==randr_event+RRNotify) follow_preferred(display, root);
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
