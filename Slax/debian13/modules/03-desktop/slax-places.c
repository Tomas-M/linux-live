/* Event-driven filesystem catalogue and validated native sidebar mount requests. */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib-unix.h>
#include <libudev.h>
#include <libmount/libmount.h>
#include <blkid/blkid.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

typedef struct
{
    char *id,*name,*group,*icon,*type,*source,*mount,*target,*reason,*parent;
    gboolean automount,readonly,needs_check;
} Place;
static GMainLoop *loop;
static guint refresh_source;
static gboolean automount_shares;
static int vmware=-1;
static char *vmware_names;
static gboolean schedule_refresh(gpointer unused);
static dev_t mount_device(struct libmnt_fs *filesystem);
static gboolean system_mount(const char *path);

/* Keep externally supplied filesystem and share names as literal UTF-8 strings. */
static char *decode_label(const char *encoded)
{
    GString *label=g_string_new(NULL);
    for (const char *p=encoded;*p;p++)
    {
        if (p[0]=='\\' && p[1]=='x' && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3]))
        {
            g_string_append_c(label,g_ascii_xdigit_value(p[2])*16+g_ascii_xdigit_value(p[3]));
            p+=3;
        }
        else g_string_append_c(label,*p);
    }
    char *valid=g_utf8_make_valid(label->str,label->len);
    g_string_free(label,TRUE);
    return valid;
}

/* Opaque identities are re-derived before privileged operations; callers supply no paths. */
static Place *place_add(GPtrArray *places,const char *kind,const char *identity,const char *name,const char *group,const char *icon,const char *type,const char *source)
{
    Place *place=g_new0(Place,1);
    char *hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,identity,-1);
    place->id=g_strconcat(kind,":",hash,NULL);
    place->name=g_strdup(name);
    place->group=g_strdup(group);
    place->icon=g_strdup(icon);
    place->type=g_strdup(type);
    place->source=g_strdup(source);
    place->reason=g_strdup("");
    g_ptr_array_add(places,place);
    g_free(hash);
    return place;
}

/* Catalogue entries own every string, including paths retained across refreshes. */
static void place_free(gpointer data)
{
    Place *place=data;
    g_free(place->id); g_free(place->name); g_free(place->group); g_free(place->icon);
    g_free(place->type); g_free(place->source); g_free(place->mount); g_free(place->target);
    g_free(place->reason); g_free(place->parent);
    g_free(place);
}

#include "slax-places-vbox.h"

/* Whole-device signatures are useful only when the kernel exposes no partition children. */
static gboolean has_partitions(const char *path)
{
    GDir *directory=g_dir_open(path,0,NULL);
    if (!directory) return FALSE;
    const char *name;
    gboolean found=FALSE;
    while ((name=g_dir_read_name(directory)))
    {
        char *partition=g_build_filename(path,name,"partition",NULL);
        found=g_file_test(partition,G_FILE_TEST_EXISTS);
        g_free(partition);
        if (found) break;
    }
    g_dir_close(directory);
    return found;
}

/* Loop images used by Slax internals stay hidden; ordinary user images remain mountable. */
static gboolean hidden_loop(struct udev_device *device,struct libmnt_table *mounts)
{
    struct udev_device *parent=udev_device_get_parent_with_subsystem_devtype(device,"block","disk");
    if (!parent) parent=device;
    char *path=g_build_filename(udev_device_get_syspath(parent),"loop/backing_file",NULL),*backing=NULL;
    gsize length=0;
    gboolean bound=g_file_get_contents(path,&backing,&length,NULL) && length>1;
    g_free(path); g_free(backing);
    if (!bound) return TRUE;
    struct libmnt_iter *iterator=mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs *filesystem;
    gboolean hidden=FALSE;
    while (mnt_table_next_fs(mounts,iterator,&filesystem)==0)
    {
        if (mount_device(filesystem)!=udev_device_get_devnum(device)) continue;
        const char *target=mnt_fs_get_target(filesystem),*type=mnt_fs_get_fstype(filesystem);
        gboolean optical=!strcmp(type,"iso9660") || !strcmp(type,"udf");
        gboolean image=optical && g_str_has_prefix(target,"/media/ISO-");
        if (system_mount(target) || image) { hidden=TRUE; break; }
    }
    mnt_free_iter(iterator);
    return hidden;
}

/* Hardware generation and the actual filesystem identify a volume independently of its label. */
static char *block_identity(struct udev_device *device,const char *type,const char *uuid)
{
    struct udev_device *parent=udev_device_get_parent_with_subsystem_devtype(device,"block","disk");
    if (!parent) parent=device;
    const char *sequence=udev_device_get_sysattr_value(parent,"diskseq");
    return g_strdup_printf("%s:%s:%s:%s",udev_device_get_syspath(device),sequence?sequence:"",uuid?uuid:"",type);
}

/* Probe after locking so stale udev data cannot authorize mounting a replacement filesystem. */
static gboolean block_matches(Place *place)
{
    blkid_probe probe=blkid_new_probe_from_filename(place->source);
    if (!probe) return FALSE;
    gboolean same=FALSE;
    const char *type=NULL,*uuid=NULL;
    struct stat status;
    if (blkid_do_safeprobe(probe)==0 && blkid_probe_lookup_value(probe,"TYPE",&type,NULL)==0 && stat(place->source,&status)==0)
    {
        blkid_probe_lookup_value(probe,"UUID",&uuid,NULL);
        struct udev *udev=udev_new();
        struct udev_device *device=udev_device_new_from_devnum(udev,'b',status.st_rdev);
        if (device)
        {
            char *identity=block_identity(device,type,uuid);
            char *hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,identity,-1);
            same=!strcmp(strchr(place->id,':')+1,hash);
            g_free(identity); g_free(hash); udev_device_unref(device);
        }
        udev_unref(udev);
    }
    blkid_free_probe(probe);
    return same;
}

/* Device discovery does not depend on fstab or the boot-time automount policy. */
static void block_list(GPtrArray *places,struct udev *udev,struct libmnt_table *mounts)
{
    struct udev_enumerate *enumerator=udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerator,"block");
    udev_enumerate_scan_devices(enumerator);
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry,udev_enumerate_get_list_entry(enumerator))
    {
        struct udev_device *device=udev_device_new_from_syspath(udev,udev_list_entry_get_name(entry));
        if (!device) continue;
        const char *node=udev_device_get_devnode(device),*name=udev_device_get_sysname(device);
        const char *usage=udev_device_get_property_value(device,"ID_FS_USAGE");
        const char *type=udev_device_get_property_value(device,"ID_FS_TYPE");
        const char *part=udev_device_get_property_value(device,"ID_PART_ENTRY_TYPE");
        gboolean internal=g_str_has_prefix(name,"zram");
        if (g_str_has_prefix(name,"loop")) internal=hidden_loop(device,mounts);
        gboolean esp=g_strcmp0(part,"c12a7328-f81f-11d2-ba4b-00a0c93ec93b")==0 || g_strcmp0(part,"0xef")==0;
        if (!node || !type || g_strcmp0(usage,"filesystem") || internal || esp || has_partitions(udev_device_get_syspath(device)))
        {
            udev_device_unref(device);
            continue;
        }
        struct udev_device *parent=udev_device_get_parent_with_subsystem_devtype(device,"block","disk");
        if (!parent) parent=device;
        const char *uuid=udev_device_get_property_value(device,"ID_FS_UUID");
        const char *label=udev_device_get_property_value(device,"ID_FS_LABEL_ENC");
        const char *icon="slax-places-drive";
        if (udev_device_get_parent_with_subsystem_devtype(device,"usb","usb_device")) icon="slax-places-usb";
        if (udev_device_get_property_value(device,"ID_CDROM")) icon="slax-places-disc";
        char *identity=block_identity(device,type,uuid);
        char *display=label?decode_label(label):g_strdup(name);
        Place *place=place_add(places,"block",identity,display,"Devices",icon,type,node);
        place->needs_check=!g_strcmp0(udev_device_get_property_value(device,"SLAX_FS_NEEDS_CHECK"),"1");
        place->parent=g_strdup(udev_device_get_devnode(parent));
        place->target=g_build_filename("/media",name,NULL);
        g_free(display);
        g_free(identity);
        udev_device_unref(device);
    }
    udev_enumerate_unref(enumerator);
}

/* Virtio exposes mountable virtiofs and 9p tags through the driver's device attributes. */
static void virtio_list(GPtrArray *places,const char *driver,const char *attribute,const char *type)
{
    char *path=g_build_filename("/sys/bus/virtio/drivers",driver,NULL);
    GDir *directory=g_dir_open(path,0,NULL);
    const char *name;
    while (directory && (name=g_dir_read_name(directory)))
    {
        char *tag_path=g_build_filename(path,name,attribute,NULL),*tag=NULL;
        if (g_file_get_contents(tag_path,&tag,NULL,NULL))
        {
            g_strchomp(tag);
            if (*tag && g_utf8_validate(tag,-1,NULL)) place_add(places,type,tag,tag,"Shared folders","folder-remote",type,tag);
        }
        g_free(tag); g_free(tag_path);
    }
    if (directory) g_dir_close(directory);
    g_free(path);
}

/* Existing VMware tools enumerate host-advertised shares without network guessing. */
static char *vmware_query(void)
{
    /* vmwgfx can load VMware modules under other hypervisors; check the actual VM type once. */
    if (vmware<0)
    {
        char *kind=NULL;
        char *argv[]={"/usr/bin/systemd-detect-virt","--vm",NULL};
        g_spawn_sync(NULL,argv,NULL,G_SPAWN_STDERR_TO_DEV_NULL,NULL,NULL,&kind,NULL,NULL,NULL);
        vmware=kind && !strcmp(kind,"vmware\n");
        g_free(kind);
    }
    if (!vmware) return NULL;
    char *output=NULL;
    int status;
    char *argv[]={"/usr/bin/vmware-hgfsclient",NULL};
    if (!g_spawn_sync(NULL,argv,NULL,G_SPAWN_STDERR_TO_DEV_NULL,NULL,NULL,&output,NULL,&status,NULL) || status) { g_free(output); return NULL; }
    return output;
}

/* The last VMware share list also lets its provider-only timer detect host changes. */
static void vmware_list(GPtrArray *places)
{
    char *output=vmware_query();
    g_free(vmware_names); vmware_names=g_strdup(output);
    if (!output) return;
    char **names=g_strsplit(output,"\n",-1);
    for (int i=0;names[i];i++)
        if (*names[i])
        {
            char *source=g_strconcat(".host:/",names[i],NULL);
            Place *place=place_add(places,"vmware",source,names[i],"Shared folders","folder-remote","fuse.vmhgfs-fuse",source);
            place->automount=TRUE;
            char *folder=g_uri_escape_string(names[i]," ",TRUE);
            place->target=g_build_filename("/mnt/hgfs",folder,NULL);
            g_free(folder);
            g_free(source);
        }
    g_strfreev(names); g_free(output);
}

/* VMware's listing tool has no notification API; poll only that provider on VMware. */
static gboolean vmware_refresh(gpointer unused)
{
    char *names=vmware_query();
    if (g_strcmp0(names,vmware_names)) schedule_refresh(NULL);
    g_free(names);
    return G_SOURCE_CONTINUE;
}

/* Resolve block sources, including Btrfs's anonymous superblock device numbers. */
static dev_t mount_device(struct libmnt_fs *filesystem)
{
    struct stat status;
    const char *source=mnt_fs_get_source(filesystem);
    if (source && stat(source,&status)==0 && S_ISBLK(status.st_mode)) return status.st_rdev;
    return mnt_fs_get_devno(filesystem);
}

/* Keep live boot/storage mounts owned by initramfs, even through their /media aliases. */
static gboolean system_mount(const char *path)
{
    return g_str_has_prefix(path,"/run/initramfs/") || g_unix_is_mount_path_system_internal(path);
}

/* Match actual mounts to their native provider entries before adding orphan mounts. */
static void attach_mounts(GPtrArray *places,struct libmnt_table *mounts)
{
    struct libmnt_iter *iterator=mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs *filesystem;
    while (mnt_table_next_fs(mounts,iterator,&filesystem)==0)
    {
        const char *path=mnt_fs_get_target(filesystem),*source=mnt_fs_get_source(filesystem),*type=mnt_fs_get_fstype(filesystem);
        Place *matched=NULL;
        for (guint i=0;i<places->len;i++)
        {
            Place *place=g_ptr_array_index(places,i);
            struct stat status;
            gboolean block=g_str_has_prefix(place->id,"block:") && stat(place->source,&status)==0 && status.st_rdev==mount_device(filesystem);
            gboolean share=g_strcmp0(place->type,type)==0 && g_strcmp0(place->source,source)==0;
            if (block || share) { matched=place; break; }
        }
        if (matched)
        {
            if (system_mount(path))
            {
                g_free(matched->reason);
                matched->reason=g_strdup("In use by Slax; cannot unmount");
            }
            if (!matched->mount || g_str_has_prefix(path,"/media/"))
            {
                g_free(matched->mount);
                char *alias=matched->target?realpath(matched->target,NULL):NULL;
                matched->mount=g_strdup(alias && !strcmp(alias,path)?matched->target:path);
                free(alias);
            }
            continue;
        }
        gboolean shared=g_str_has_prefix(type,"fuse.") || mnt_fs_is_netfs(filesystem) || !strcmp(type,"vboxsf") || !strcmp(type,"virtiofs") || !strcmp(type,"9p");
        gboolean hidden=system_mount(path) || !strcmp(type,"squashfs");
        if (hidden || (!shared && mnt_fs_is_pseudofs(filesystem))) continue;
        if (g_str_has_prefix(type,"fuse.gvfs")) continue;
        char *name=g_path_get_basename(path);
        char *identity=g_strdup_printf("%d:%s:%s",mnt_fs_get_id(filesystem),source,path);
        const char *group="Devices",*icon="slax-places-drive";
        if (shared) { group="Shared folders"; icon="folder-remote"; }
        if (g_str_has_prefix(name,"ISO-") && (!strcmp(type,"iso9660") || !strcmp(type,"udf")))
        {
            group="Disk images";
            icon="slax-places-disc";
            char *backing=g_strdup_printf("/sys/dev/block/%u:%u/loop/backing_file",major(mnt_fs_get_devno(filesystem)),minor(mnt_fs_get_devno(filesystem)));
            char *filename=NULL;
            if (g_file_get_contents(backing,&filename,NULL,NULL))
            {
                g_strchomp(filename);
                g_free(name); name=g_path_get_basename(filename);
            }
            g_free(filename); g_free(backing);
        }
        Place *place=place_add(places,"mount",identity,name,group,icon,type,source);
        place->mount=g_strdup(path);
        g_free(identity); g_free(name);
    }
    mnt_free_iter(iterator);
}

/* Root-owned fstab entries supplement native devices with configured remote locations. */
static void fstab_list(GPtrArray *places)
{
    struct libmnt_table *table=mnt_new_table_from_file("/etc/fstab");
    if (!table) return;
    struct libmnt_iter *iterator=mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs *filesystem;
    while (mnt_table_next_fs(table,iterator,&filesystem)==0)
    {
        const char *target=mnt_fs_get_target(filesystem),*source=mnt_fs_get_source(filesystem),*type=mnt_fs_get_fstype(filesystem);
        if (!target || !source || !type || system_mount(target) || mnt_fs_is_pseudofs(filesystem) || !strcmp(type,"swap")) continue;
        gboolean found=FALSE;
        for (guint i=0;i<places->len;i++)
        {
            Place *place=g_ptr_array_index(places,i);
            if (!g_strcmp0(target,place->target) || !g_strcmp0(target,place->mount) || !g_strcmp0(source,place->source)) { found=TRUE; break; }
        }
        if (found) continue;
        /* Unavailable block devices are not filesystem locations merely because fstab remembers them. */
        if (g_str_has_prefix(source,"/dev/") || g_str_has_prefix(source,"UUID=") || g_str_has_prefix(source,"LABEL=")) continue;
        char *name=g_path_get_basename(target);
        Place *place=place_add(places,"fstab",target,name,"Shared folders","folder-remote",type,source);
        place->target=g_strdup(target);
        g_free(name);
    }
    mnt_free_iter(iterator); mnt_free_table(table);
}

/* Stable ordering moves only entries affected by a device or mount event. */
static int compare_places(gconstpointer first,gconstpointer second)
{
    const Place *a=*(Place*const*)first,*b=*(Place*const*)second;
    const char *groups[]={"Devices","Shared folders","Disk images"};
    int first_group=0,second_group=0;
    for (int i=0;i<3;i++) { if (!strcmp(a->group,groups[i])) first_group=i; if (!strcmp(b->group,groups[i])) second_group=i; }
    if (first_group!=second_group) return first_group-second_group;
    if (!strcmp(a->group,"Devices")) return strcmp(a->source,b->source);
    return g_utf8_collate(a->name,b->name);
}

/* The same fresh catalogue authorizes actions and supplies the unprivileged sidebar. */
static GPtrArray *list_places(void)
{
    GPtrArray *places=g_ptr_array_new_with_free_func(place_free);
    struct libmnt_table *mounts=mnt_new_table_from_file("/proc/self/mountinfo");
    struct udev *udev=udev_new();
    block_list(places,udev,mounts);
    udev_unref(udev);
    vbox_list(places);
    virtio_list(places,"virtio_fs","tag","virtiofs");
    virtio_list(places,"9pnet_virtio","mount_tag","9p");
    vmware_list(places);
    fstab_list(places);
    attach_mounts(places,mounts);
    mnt_free_table(mounts);
    for (guint i=0;i<places->len;i++)
    {
        Place *place=g_ptr_array_index(places,i);
        if (!place->mount && place->needs_check)
        {
            g_free(place->reason); place->reason=g_strdup("Needs filesystem check");
        }
        if (!place->target && !place->mount)
        {
            char *name=g_uri_escape_string(place->source," ",TRUE);
            char *folder=g_strconcat("sf_",name,NULL);
            place->target=g_build_filename("/media",folder,NULL);
            g_free(name); g_free(folder);
        }
    }
    g_ptr_array_sort(places,compare_places);
    return places;
}

/* GVariant's single-line literal format preserves whitespace, Unicode and control characters. */
static void print_places(GPtrArray *places)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder,G_VARIANT_TYPE("a(sssssssss)"));
    for (guint i=0;i<places->len;i++)
    {
        Place *place=g_ptr_array_index(places,i);
        g_variant_builder_add(&builder,"(sssssssss)",place->id,place->name,place->group,place->icon,place->type,place->source,place->mount?place->mount:"",place->target?place->target:"",place->reason);
    }
    GVariant *array=g_variant_ref_sink(g_variant_builder_end(&builder));
    char *line=g_variant_print(array,TRUE);
    puts(line); fflush(stdout);
    g_free(line); g_variant_unref(array);
}

/* Run fixed native programs without interpreting device names or share paths as shell text. */
static gboolean run_command(char **argv,GError **error)
{
    int status;
    char *message=NULL;
    gboolean started=g_spawn_sync(NULL,argv,NULL,G_SPAWN_STDOUT_TO_DEV_NULL,NULL,NULL,NULL,&message,&status,error);
    if (started && !g_spawn_check_wait_status(status,error))
    {
        g_clear_error(error);
        g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"%s",message && *message?message:"The filesystem operation failed.");
        started=FALSE;
    }
    g_free(message);
    return started;
}

/* Managed mount paths must not traverse links or directories writable by another account. */
static gboolean prepare_target(const char *target,GError **error)
{
    char *canonical=g_canonicalize_filename(target,NULL);
    gboolean valid=g_path_is_absolute(target) && !strcmp(canonical,target);
    g_free(canonical);
    if (!valid) { g_set_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"The configured mount location contains an invalid path."); return FALSE; }
    char *path=g_strdup(target);
    for (char *p=path+1;;p++)
        if (*p=='/' || !*p)
        {
            char separator=*p;
            *p=0;
            struct stat status;
            if (mkdir(path,0755)<0 && errno!=EEXIST) break;
            if (lstat(path,&status)<0 || !S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode&0022)) break;
            if (!separator) { g_free(path); return TRUE; }
            *p=separator;
        }
    g_set_error(error,G_IO_ERROR,G_IO_ERROR_PERMISSION_DENIED,"The mount location must be a system-owned directory without symbolic links.");
    g_free(path);
    return FALSE;
}

/* An explicit unmount is remembered only for this boot so host automount cannot undo it. */
static char *suppression_path(const char *id)
{
    return g_build_filename("/run/slax-places/unmounted",id,NULL);
}

/* Revalidate opaque identity after the same locks used by Slax's udev automounter. */
static gboolean change_mount(const char *id,gboolean unmount,char **result,GError **error)
{
    GPtrArray *places=list_places();
    Place *place=NULL;
    for (guint i=0;i<places->len;i++)
        if (!strcmp(((Place*)g_ptr_array_index(places,i))->id,id)) { place=g_ptr_array_index(places,i); break; }
    if (!place) { g_set_error(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"This filesystem is no longer available."); g_ptr_array_unref(places); return FALSE; }
    int device_fd=-1;
    gboolean block=g_str_has_prefix(id,"block:");
    char *basename=block?g_path_get_basename(place->source):g_strdup(strchr(id,':')+1);
    char *lock_path=g_strdup_printf("/run/lock/slax-automount-%s.lock",basename);
    if (!strcmp(place->group,"Disk images")) { g_free(lock_path); lock_path=g_strdup("/run/lock/slax-mount-iso.lock"); }
    int lock_fd=open(lock_path,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    g_free(lock_path); g_free(basename);
    if (lock_fd<0 || flock(lock_fd,LOCK_EX)<0) goto lock_error;
    if (block)
    {
        device_fd=open(place->parent,O_RDONLY|O_CLOEXEC);
        if (device_fd<0 || flock(device_fd,LOCK_SH)<0) goto lock_error;
    }
    g_ptr_array_unref(places); places=list_places(); place=NULL;
    for (guint i=0;i<places->len;i++)
        if (!strcmp(((Place*)g_ptr_array_index(places,i))->id,id)) { place=g_ptr_array_index(places,i); break; }
    gboolean success=FALSE;
    gboolean new_mount=place && !unmount && !place->mount;
    char *suppressed=suppression_path(id);
    if (!place) g_set_error(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"This filesystem changed or was disconnected. Try again.");
    else if (*place->reason) g_set_error(error,G_IO_ERROR,G_IO_ERROR_BUSY,"%s. This filesystem cannot be changed here.",place->reason);
    else if (block && new_mount && !block_matches(place)) g_set_error(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"The filesystem changed. Wait for its current entry and try again.");
    else if (automount_shares && g_file_test(suppressed,G_FILE_TEST_EXISTS)) success=TRUE;
    else if (unmount && !place->mount) success=TRUE;
    else if (!unmount && place->mount) { *result=g_strdup(place->mount); success=TRUE; }
    else if (unmount)
    {
        /* Publish manual intent before mount notifications can wake the shared-folder service. */
        gboolean remembered=g_file_test(suppressed,G_FILE_TEST_EXISTS);
        if (!block && (g_mkdir_with_parents("/run/slax-places/unmounted",0755)<0 || !g_file_set_contents(suppressed,"",0,error)))
        {
            if (!*error) g_set_error(error,G_IO_ERROR,g_io_error_from_errno(errno),"Cannot remember the manual unmount: %s",g_strerror(errno));
            goto finished;
        }
        char *argv[]={"/bin/umount","--",place->mount,NULL};
        success=run_command(argv,error);
        if (!success && !remembered) unlink(suppressed);
        if (success && g_str_has_prefix(place->mount,"/media/")) rmdir(place->mount);
        if (success && place->target && g_str_has_prefix(place->target,"/mnt/hgfs/")) rmdir(place->target);
    }
    else if (prepare_target(place->target,error))
    {
        char *options=place->readonly?"ro,nodev,nosuid":"nodev,nosuid";
        char *argv[]={"/bin/mount","-t",place->type,"-o",options,"--",place->source,place->target,NULL};
        if (block)
        {
            char *mount_argv[]={"/usr/libexec/slax-places-mount",place->source,place->target,"manual",NULL};
            success=run_command(mount_argv,error);
        }
        else if (g_str_has_prefix(id,"fstab:"))
        {
            char *mount_argv[]={"/bin/mount","--",place->target,NULL};
            success=run_command(mount_argv,error);
        }
        else if (!strcmp(place->type,"fuse.vmhgfs-fuse"))
        {
            char *share_options=g_strdup_printf("%s,allow_other,default_permissions,gid=%u,umask=002",options,getgrnam("users")->gr_gid);
            char *mount_argv[]={"/usr/bin/vmhgfs-fuse",place->source,place->target,"-o",share_options,NULL};
            success=run_command(mount_argv,error);
            g_free(share_options);
        }
        else
        {
            char *share_options=NULL;
            if (!strcmp(place->type,"vboxsf")) { share_options=g_strdup_printf("%s,gid=%u,dmode=0775,fmode=0664",options,getgrnam("vboxsf")->gr_gid); argv[4]=share_options; }
            if (!strcmp(place->type,"9p")) argv[4]="trans=virtio,version=9p2000.L,nodev,nosuid";
            success=run_command(argv,error);
            g_free(share_options);
        }
        if (success) *result=g_strdup(place->target);
        else rmdir(place->target);
    }
    if (success && !unmount && !automount_shares) unlink(suppressed);
finished:
    g_free(suppressed);
    g_ptr_array_unref(places);
    if (device_fd>=0) close(device_fd);
    close(lock_fd);
    return success;
lock_error:
    g_set_error(error,G_IO_ERROR,g_io_error_from_errno(errno),"Cannot lock this filesystem: %s",g_strerror(errno));
    if (device_fd>=0) close(device_fd);
    if (lock_fd>=0) close(lock_fd);
    g_ptr_array_unref(places);
    return FALSE;
}

/* Reconcile asynchronously outside GTK and preserve the existing automount boot policy. */
static gboolean refresh_places(gpointer unused)
{
    refresh_source=0;
    GPtrArray *places=list_places();
    if (!automount_shares) print_places(places);
    else
        for (guint i=0;i<places->len;i++)
        {
            Place *place=g_ptr_array_index(places,i);
            if (!place->automount || place->mount || *place->reason) continue;
            char *suppressed=suppression_path(place->id);
            gboolean skip=g_file_test(suppressed,G_FILE_TEST_EXISTS);
            g_free(suppressed);
            if (skip) continue;
            char *path=NULL;
            GError *error=NULL;
            if (!change_mount(place->id,FALSE,&path,&error)) { g_warning("Cannot mount shared folder: %s",error->message); g_clear_error(&error); }
            g_free(path);
        }
    g_ptr_array_unref(places);
    return G_SOURCE_REMOVE;
}

/* Coalesce a burst of udev, mount and provider notifications into one fresh snapshot. */
static gboolean schedule_refresh(gpointer unused)
{
    if (!refresh_source) refresh_source=g_timeout_add(100,refresh_places,NULL);
    return G_SOURCE_REMOVE;
}

/* The udev database already contains the new filesystem metadata when its event arrives. */
static gboolean on_device(gint fd,GIOCondition condition,gpointer data)
{
    struct udev_device *device;
    while ((device=udev_monitor_receive_device(data))) udev_device_unref(device);
    schedule_refresh(NULL);
    return G_SOURCE_CONTINUE;
}

/* File monitors cover fstab edits and VMware's shared-folder directory. */
static void on_file(GFileMonitor *monitor,GFile *file,GFile *other,GFileMonitorEvent event,gpointer unused)
{
    schedule_refresh(NULL);
}

/* Provider and kernel events, rather than a rescan timer, drive the running catalogue. */
static int watch_places(void)
{
    struct udev *udev=udev_new();
    struct udev_monitor *devices=udev_monitor_new_from_netlink(udev,"udev");
    udev_monitor_enable_receiving(devices);
    g_unix_fd_add(udev_monitor_get_fd(devices),G_IO_IN,on_device,devices);
    GUnixMountMonitor *mounts=g_unix_mount_monitor_get();
    g_signal_connect_swapped(mounts,"mounts-changed",G_CALLBACK(schedule_refresh),NULL);
    g_signal_connect_swapped(mounts,"mountpoints-changed",G_CALLBACK(schedule_refresh),NULL);
    const char *paths[]={"/etc/fstab","/mnt/hgfs"};
    for (guint i=0;i<G_N_ELEMENTS(paths);i++)
    {
        GFile *file=g_file_new_for_path(paths[i]);
        gboolean directory=!strcmp(paths[i],"/mnt/hgfs");
        GFileMonitor *monitor=directory?g_file_monitor_directory(file,G_FILE_MONITOR_NONE,NULL,NULL):g_file_monitor_file(file,G_FILE_MONITOR_NONE,NULL,NULL);
        if (monitor) g_signal_connect(monitor,"changed",G_CALLBACK(on_file),NULL);
        g_object_unref(file);
    }
    if (g_file_test("/dev/vboxuser",G_FILE_TEST_EXISTS)) g_thread_unref(g_thread_new("shared-folders",vbox_watch,NULL));
    loop=g_main_loop_new(NULL,FALSE);
    refresh_places(NULL);
    if (vmware) g_timeout_add_seconds(5,vmware_refresh,NULL);
    g_main_loop_run(loop);
    return 0;
}

/* Only the fixed mount/unmount commands require polkit; discovery stays unprivileged. */
int main(int argc,char **argv)
{
    /* Discovery uses stdout as a protocol stream, including when GLib debugging is enabled. */
    g_log_writer_default_set_use_stderr(TRUE);
    if (argc==2 && !strcmp(argv[1],"--watch"))
    {
        prctl(PR_SET_PDEATHSIG,SIGTERM);
        if (getppid()==1) return 0;
        return watch_places();
    }
    if (argc==2 && !strcmp(argv[1],"--list")) { GPtrArray *places=list_places(); print_places(places); g_ptr_array_unref(places); return 0; }
    if (argc==2 && !strcmp(argv[1],"--automount-shares") && geteuid()==0)
    {
        char *command=NULL;
        g_file_get_contents("/proc/cmdline",&command,NULL,NULL);
        char **parameters=g_strsplit_set(command," \t\r\n",-1);
        gboolean enabled=g_strv_contains((const char*const*)parameters,"automount") && !g_strv_contains((const char*const*)parameters,"noautomount");
        g_strfreev(parameters); g_free(command);
        if (!enabled) return 0;
        automount_shares=TRUE;
        return watch_places();
    }
    if (argc!=3 || (strcmp(argv[1],"mount") && strcmp(argv[1],"unmount")) || geteuid()!=0) { fputs("Invalid filesystem request.\n",stderr); return 1; }
    char *path=NULL;
    GError *error=NULL;
    if (!change_mount(argv[2],!strcmp(argv[1],"unmount"),&path,&error))
    {
        fprintf(stderr,"%s\n",error->message);
        g_error_free(error);
        return 1;
    }
    GVariant *result=g_variant_ref_sink(g_variant_new_string(path?path:""));
    char *text=g_variant_print(result,TRUE);
    puts(text);
    g_variant_unref(result); g_free(text); g_free(path);
    return 0;
}
