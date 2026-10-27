/* Synchronize X with the absolute pointer supplied by the vboxguest driver. */
#include <X11/Xlib.h>
#include <linux/vboxguest.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

/* Both guest and host must have absolute mouse integration enabled. */
#define VMMDEV_MOUSE_ABSOLUTE 3

/* Read the current position before X waits for its first physical mouse event. */
int main(void)
{
    int fd=open("/dev/vboxguest", O_RDWR);
    if (fd < 0) return 0;
    struct {
        struct vbg_ioctl_hdr header;
        uint32_t features;
        int32_t x, y;
    } mouse={.header={.size_in=sizeof(mouse), .version=VBG_IOCTL_HDR_VERSION,
                     .type=VMMDEVREQ_GET_MOUSE_STATUS}};
    int status=ioctl(fd, VBG_IOCTL_VMMDEV_REQUEST(sizeof(mouse)), &mouse);
    close(fd);
    if (status || mouse.header.rc < 0) return 0;
    if ((mouse.features & VMMDEV_MOUSE_ABSOLUTE) != VMMDEV_MOUSE_ABSOLUTE) return 0;
    if ((uint32_t)mouse.x > 65535 || (uint32_t)mouse.y > 65535) return 0;

    Display *display=XOpenDisplay(NULL);
    if (!display) return 1;
    int screen=DefaultScreen(display);
    int width=DisplayWidth(display, screen), height=DisplayHeight(display, screen);
    int x=(int64_t)mouse.x*width/65535, y=(int64_t)mouse.y*height/65535;
    if (x >= width) x=width-1;
    if (y >= height) y=height-1;
    XWarpPointer(display, None, RootWindow(display, screen), 0, 0, 0, 0, x, y);
    XCloseDisplay(display);
    return 0;
}
