/* VirtualBox shared-folder discovery through Linux's unprivileged guest ABI. */
#include <linux/vboxguest.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* The SHFL service's cancellation result is outside Linux's reduced VBox error header. */
#define SHFL_WAIT_CANCELLED (-70)

typedef struct { int fd; uint32_t client; } VBoxFolders;
typedef struct vmmdev_hgcm_function_parameter VBoxParameter;
typedef struct
{
    struct vbg_ioctl_hgcm_call header;
    VBoxParameter parameters[5];
} VBoxCall;

/* Use the kernel's correctly packed pointer parameter for either native word size. */
static VBoxParameter vbox_buffer(void *address,unsigned size)
{
    VBoxParameter parameter={.type=VMMDEV_HGCM_PARM_TYPE_LINADDR};
    parameter.u.pointer.size=size;
    parameter.u.pointer.u.linear_addr=(uintptr_t)address;
    return parameter;
}

/* Pass the documented SHFL service parameters without assuming host configuration. */
static int vbox_call(VBoxFolders *folders, unsigned function, VBoxParameter *parameters, unsigned count, unsigned timeout)
{
    VBoxCall call={.header={.hdr={.version=VBG_IOCTL_HDR_VERSION},.client_id=folders->client,.function=function,.timeout_ms=timeout,.parm_count=count}};
    call.header.hdr.size_in=sizeof(call.header)+count*sizeof(VBoxParameter);
    if (count) memcpy(call.parameters,parameters,count*sizeof(VBoxParameter));
    int result;
    do result=ioctl(folders->fd,VBG_IOCTL_HGCM_CALL(call.header.hdr.size_in),&call); while (result<0 && errno==EINTR);
    if (result<0) return -1;
    if (count) memcpy(parameters,call.parameters,count*sizeof(VBoxParameter));
    return call.header.hdr.rc;
}

/* A separate connection lets the blocking mapping notification leave discovery free. */
static VBoxFolders vbox_open(void)
{
    VBoxFolders folders={.fd=open("/dev/vboxuser",O_RDWR|O_CLOEXEC)};
    if (folders.fd<0) return folders;
    struct vbg_ioctl_hgcm_connect connect={.hdr={.size_in=sizeof(connect),.size_out=sizeof(struct vbg_ioctl_hdr)+sizeof(uint32_t),.version=VBG_IOCTL_HDR_VERSION}};
    connect.u.in.loc.type=VMMDEV_HGCM_LOC_LOCALHOST_EXISTING;
    strcpy(connect.u.in.loc.u.localhost.service_name,"VBoxSharedFolders");
    if (ioctl(folders.fd,VBG_IOCTL_HGCM_CONNECT,&connect)<0 || connect.hdr.rc<0)
    {
        g_debug("Cannot connect VirtualBox folder interface: %d (%s)",connect.hdr.rc,g_strerror(errno));
        close(folders.fd);
        folders.fd=-1;
        return folders;
    }
    folders.client=connect.u.out.client_id;
    if (vbox_call(&folders,16,NULL,0,10000)<0)
    {
        g_debug("Cannot select VirtualBox UTF-8 names (%s)",g_strerror(errno));
        close(folders.fd);
        folders.fd=-1;
    }
    return folders;
}

/* The host supplies exact UTF-8 names and mapping versions, including unmounted shares. */
static void vbox_list(GPtrArray *places)
{
    VBoxFolders folders=vbox_open();
    if (folders.fd<0) return;
    unsigned capacity=16;
    struct { uint32_t status,root; } *mappings=NULL;
    VBoxParameter query[3];
    int result;
    do
    {
        mappings=g_realloc_n(mappings,capacity,sizeof(*mappings));
        query[0]=(VBoxParameter){.type=1,.u.value32=1};
        query[1]=(VBoxParameter){.type=1,.u.value32=capacity};
        query[2]=vbox_buffer(mappings,capacity*sizeof(*mappings));
        result=vbox_call(&folders,1,query,3,10000);
        if (result<0 || query[1].u.value32<=capacity) break;
        if (query[1].u.value32>65536) { result=-1; break; }
        capacity=query[1].u.value32;
    } while (capacity<=65536);
    if (result>=0 && query[1].u.value32<=capacity)
        for (unsigned i=0;i<query[1].u.value32;i++)
        {
            struct __attribute__((packed)) { uint16_t size,length; char text[65532]; } name={.size=65532},target={.size=65532};
            VBoxParameter info[5]={
                {.type=1,.u.value32=mappings[i].root},
                vbox_buffer(&name,sizeof(name)),
                vbox_buffer(&target,sizeof(target)),
                {.type=2,.u.value64=2},
                {.type=1}
            };
            if (vbox_call(&folders,21,info,5,10000)<0) continue;
            if (name.length>=sizeof(name.text) || !g_utf8_validate(name.text,name.length,NULL)) continue;
            name.text[name.length]=0;
            char *identity=g_strdup_printf("%u:%u:%s",mappings[i].root,info[4].u.value32,name.text);
            Place *place=place_add(places,"vbox",identity,name.text,"Shared folders","folder-remote","vboxsf",name.text);
            place->automount=(info[3].u.value64&2)!=0;
            place->readonly=(info[3].u.value64&1)==0;
            g_free(identity);
        }
    g_free(mappings);
    close(folders.fd);
}

/* Mapping-change waits are native notifications, including additions made after login. */
static gpointer vbox_watch(gpointer unused)
{
    VBoxFolders folders=vbox_open();
    if (folders.fd<0) return NULL;
    VBoxParameter version={.type=1,.u.value32=UINT32_MAX};
    int result;
    g_debug("Observing VirtualBox mapping changes");
    while (TRUE)
    {
        result=vbox_call(&folders,22,&version,1,UINT32_MAX);
        if (result==VERR_INTERRUPTED) continue;
        /* The host also completes mapping-change notifications with VERR_CANCELLED. */
        if (result<0 && result!=SHFL_WAIT_CANCELLED) break;
        g_main_context_invoke(NULL,schedule_refresh,NULL);
    }
    g_debug("VirtualBox mapping notifications ended: %d",result);
    close(folders.fd);
    return NULL;
}
