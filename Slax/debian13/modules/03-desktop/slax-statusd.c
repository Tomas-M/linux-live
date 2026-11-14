/* Privileged storage operations for Slax Status. Clients send one JSON line per connection. */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define RUNTIME "/run/slax-status"
#define BOOT "/run/initramfs/memory/profile"
#define DYN "/run/initramfs/bin/@mount.dynfilefs"
#define LIMIT (8*1024*1024)

typedef struct { char *id,*device,*path,*label,*fstype,*uuid; dev_t number,filesystem; gboolean owned,writable; guint64 free,total; } Drive;
typedef struct { char *id,*number,*path,*name,*kind,*store,*inner; Drive *drive; ino_t inode; gboolean active,owned; guint64 reusable,size; gint64 used,stats_at; } Profile;
typedef struct { char *id,*profile,*point,*path,*browse,*image; uid_t uid; gboolean overlay; } View;
static GPtrArray *drives,*profiles,*views;
static JsonArray *block_devices;
static JsonObject *selection;
static GMutex operation_lock;
static GMainLoop *main_loop;
static uid_t guest_uid=(uid_t)-1;
static gint64 scanned;
static char *active_path,*run_kind,*run_start,*run_error;
static gboolean recovered;
static gboolean pin(Profile *profile, const char *id, GError **error);
static void recover_mounts(void);
static gboolean clear_next(GError **error);
static gboolean restore_selection(gboolean release, GError **error);
static gboolean initialize_storage(int directory, Drive *drive, GError **error);

/* Use structured errors throughout IPC; command stderr remains ordinary text. */
static gboolean fail(GError **error, int code, const char *message)
{
   g_set_error_literal(error,G_IO_ERROR,g_io_error_from_errno(code),message);
   return FALSE;
}

/* Run only caller-independent executables with explicit argv entries. */
static char *command_at(const char *const *argv, int directory, GError **error)
{
   GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE);
   if (directory>=0) g_subprocess_launcher_take_fd(launcher,dup(directory),198);
   GSubprocess *process=g_subprocess_launcher_spawnv(launcher,argv,error); g_object_unref(launcher);
   if (!process) return NULL;
   char *out=NULL,*err=NULL;
   gboolean ok=g_subprocess_communicate_utf8(process,NULL,NULL,&out,&err,error);
   if (ok && !g_subprocess_get_successful(process))
      ok=fail(error,EIO,err && *err ? g_strstrip(err) : "The storage operation failed.");
   g_free(err); g_object_unref(process);
   if (!ok) { g_free(out); return NULL; }
   return out;
}

/* Commands without user-controlled backing paths need no inherited directory handle. */
static char *command(const char *const *argv, GError **error)
{
   return command_at(argv,-1,error);
}

/* Parse bounded JSON without assuming member types supplied by clients or disks. */
static JsonObject *parse(const char *text, GError **error)
{
   JsonParser *parser=json_parser_new();
   JsonObject *object=NULL;
   if (text && strlen(text)<=LIMIT && json_parser_load_from_data(parser,text,-1,error))
   {
      JsonNode *root=json_parser_get_root(parser);
      if (JSON_NODE_HOLDS_OBJECT(root)) object=json_object_ref(json_node_get_object(root));
      else fail(error,EINVAL,"Expected a JSON object.");
   }
   else if (!text || strlen(text)>LIMIT) fail(error,EFBIG,"The storage report is too large.");
   g_object_unref(parser);
   return object;
}

/* Missing and explicitly null optional strings have the same meaning. */
static const char *string(JsonObject *object, const char *name)
{
   JsonNode *node=object ? json_object_get_member(object,name) : NULL;
   return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node)==G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

/* Booleans never coerce client strings or numeric values. */
static gboolean boolean(JsonObject *object, const char *name)
{
   JsonNode *node=object ? json_object_get_member(object,name) : NULL;
   return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node)==G_TYPE_BOOLEAN && json_node_get_boolean(node);
}

/* Optional identities are represented by JSON null, not empty fabricated IDs. */
static void nullable(JsonObject *object, const char *name, const char *value)
{
   if (value && *value) json_object_set_string_member(object,name,value);
   else json_object_set_null_member(object,name);
}

/* Read metadata without following symlinks or accepting special files. */
static char *read_fd(int fd, GError **error)
{
   if (fd<0) { fail(error,errno,g_strerror(errno)); return NULL; }
   struct stat st;
   if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>LIMIT)
   { close(fd); fail(error,EINVAL,"Invalid storage metadata."); return NULL; }
   GString *text=g_string_new(NULL);
   char buffer[8192]; ssize_t count;
   while ((count=read(fd,buffer,sizeof(buffer)))>0)
   {
      if (text->len+count>LIMIT) break;
      g_string_append_len(text,buffer,count);
   }
   close(fd);
   if (count || memchr(text->str,0,text->len) || !g_utf8_validate(text->str,text->len,NULL))
   { g_string_free(text,TRUE); fail(error,EIO,"Could not read storage metadata."); return NULL; }
   return g_string_free(text,FALSE);
}

/* Ordinary runtime files also reject symlinks and special nodes. */
static char *read_file(const char *path, GError **error)
{
   return read_fd(open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),error);
}

/* Metadata replacement is durable and cannot follow a substituted destination symlink. */
static gboolean atomic_at(int parent, const char *name, const char *text, GError **error)
{
   if (parent<0) return fail(error,errno,g_strerror(errno));
   char temporary[80]; g_snprintf(temporary,sizeof(temporary),".status-%08x-%08x",g_random_int(),g_random_int());
   int fd=openat(parent,temporary,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0644);
   gboolean ok=fd>=0;
   size_t length=strlen(text),offset=0;
   while (ok && offset<length)
   {
      ssize_t count=write(fd,text+offset,length-offset);
      if (count>0) offset+=count; else if (count<0 && errno==EINTR) continue; else ok=FALSE;
   }
   if (ok) ok=!fsync(fd);
   if (fd>=0) close(fd);
   if (ok) ok=!renameat(parent,temporary,parent,name) && !fsync(parent);
   if (!ok) { int saved=errno; unlinkat(parent,temporary,0); fail(error,saved,g_strerror(saved)); }
   return ok;
}

/* Runtime directories are owned by this daemon and never supplied by clients. */
static gboolean atomic_file(const char *directory, const char *name, const char *text, GError **error)
{
   int fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
   gboolean ok=atomic_at(fd,name,text,error); if (fd>=0) close(fd); return ok;
}

/* The runtime journal survives a daemon restart but is naturally discarded on reboot. */
static gboolean write_json(const char *directory, const char *name, JsonObject *object, GError **error)
{
   JsonNode *node=json_node_new(JSON_NODE_OBJECT); json_node_set_object(node,object);
   char *text=json_to_string(node,FALSE); gboolean ok=atomic_file(directory,name,text,error);
   g_free(text); json_node_free(node); return ok;
}

/* Only fixed metadata basenames are removed through this helper. */
static gboolean remove_at(int fd, const char *name, GError **error)
{
   if (fd<0) return fail(error,errno,g_strerror(errno));
   gboolean ok=!unlinkat(fd,name,0) || errno==ENOENT;
   if (ok) ok=!fsync(fd);
   if (!ok) fail(error,errno,g_strerror(errno));
   return ok;
}

/* Fixed daemon-owned runtime records can be removed by their fixed pathname. */
static gboolean remove_file(const char *directory, const char *name, GError **error)
{
   int fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
   gboolean ok=remove_at(fd,name,error); if (fd>=0) close(fd); return ok;
}

/* Profile numbers and snapshot identities are distinct, strictly bounded namespaces. */
static gboolean digits(const char *value)
{
   if (!value || !*value || strlen(value)>18) return FALSE;
   for (const char *p=value;*p;p++) if (*p<'0' || *p>'9') return FALSE;
   return TRUE;
}

/* Snapshot IDs stay exact across JSON, filenames and FUSE commands. */
static gboolean identity(const char *value)
{
   if (!value || strlen(value)!=16) return FALSE;
   for (const char *p=value;*p;p++) if (!g_ascii_isdigit(*p) && (*p<'a' || *p>'f')) return FALSE;
   return TRUE;
}

/* UTF-8 labels remain international; controls and line delimiters cannot enter metadata. */
static gboolean valid_name(const char *name, gboolean snapshot, GError **error)
{
   if (!name || strlen(name)>250 || !g_utf8_validate(name,-1,NULL)) return fail(error,EINVAL,"Use a name of at most 250 UTF-8 bytes.");
   for (const char *p=name;*p;p=g_utf8_next_char(p))
      if (g_unichar_iscntrl(g_utf8_get_char(p))) return fail(error,EINVAL,"Names cannot contain control characters.");
   if (snapshot && (!*name || strpbrk(name,"<>:\"/\\|?*") || g_str_has_prefix(name,"auto-") || g_str_has_prefix(name,"live-start-") || name[strlen(name)-1]=='.' || name[strlen(name)-1]==' '))
      return fail(error,EINVAL,"Choose a restore point name without reserved prefixes, path punctuation or a final dot or space.");
   return TRUE;
}

/* Device, inode and filesystem identity prevent stale handles from targeting replacement media. */
static char *handle(const char *device, const char *uuid, const char *number, ino_t inode)
{
   char *key=g_strdup_printf("%s\n%s\n%s\n%"G_GUINT64_FORMAT,device,uuid ? uuid : "",number ? number : "",(guint64)inode);
   char *sum=g_compute_checksum_for_string(G_CHECKSUM_SHA256,key,-1); g_free(key);
   sum[24]=0; return sum;
}

/* Prefer the filesystem root over bind-mounted profile subdirectories of the same disk. */
static char *mounted(dev_t device)
{
   char *text=NULL,*result=NULL;
   if (!g_file_get_contents("/proc/self/mountinfo",&text,NULL,NULL)) return NULL;
   char **lines=g_strsplit(text,"\n",-1); size_t best=G_MAXSIZE;
   for (char **line=lines;*line;line++)
   {
      char *separator=strstr(*line," - "); if (!separator) continue;
      char **fields=g_strsplit(*line," ",-1),**tail=g_strsplit(separator+3," ",4); struct stat node;
      unsigned major_number=0,minor_number=0;
      sscanf(fields[2],"%u:%u",&major_number,&minor_number);
      gboolean match=makedev(major_number,minor_number)==device;
      if (tail[1] && !stat(tail[1],&node) && S_ISBLK(node.st_mode) && node.st_rdev==device) match=TRUE;
      if (match)
      {
         char *root=g_strcompress(fields[3]),*path=g_strcompress(fields[4]);
         if (strlen(root)<best || (strlen(root)==best && (!result || strlen(path)<strlen(result))))
         { best=strlen(root); g_free(result); result=g_strdup(path); }
         g_free(root); g_free(path);
      }
      g_strfreev(fields); g_strfreev(tail);
   }
   g_strfreev(lines); g_free(text); return result;
}

/* Compare actual mount-table entries, not a directory's existence or contents. */
static gboolean mountpoint(const char *path)
{
   FILE *table=setmntent("/proc/self/mounts","r"); struct mntent *entry; gboolean found=FALSE;
   while (table && (entry=getmntent(table))) if (!strcmp(entry->mnt_dir,path)) { found=TRUE; break; }
   if (table) endmntent(table);
   return found;
}

/* Track physical capacity separately from allocator space reusable only by a particular profile. */
static void capacity(Drive *drive)
{
   struct statvfs st;
   if (statvfs(drive->path,&st)) { drive->writable=FALSE; drive->free=drive->total=0; return; }
   drive->free=(guint64)st.f_bavail*st.f_frsize;
   drive->total=(guint64)st.f_blocks*st.f_frsize;
   drive->writable=!(st.f_flag&ST_RDONLY);
}

/* NTFS writes follow the same hibernation rule as Live Kit's early mounting code. */
static gboolean ntfs_safe(const char *path)
{
   GDir *dir=g_dir_open(path,0,NULL); const char *entry; gboolean safe=TRUE,found=FALSE;
   if (!dir) return FALSE;
   while ((entry=g_dir_read_name(dir))) if (!g_ascii_strcasecmp(entry,"hiberfil.sys"))
   {
      if (found) { safe=FALSE; break; }
      found=TRUE;
      char *file=g_build_filename(path,entry,NULL);
      int fd=open(file,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW); struct stat st; unsigned char bytes[4096];
      safe=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && read(fd,bytes,sizeof(bytes))==(ssize_t)sizeof(bytes);
      if (safe) for (guint i=0;i<sizeof(bytes);i++) if (bytes[i]) { safe=FALSE; break; }
      if (fd>=0) close(fd);
      g_free(file);
   }
   g_dir_close(dir); return safe;
}

/* Read the initramfs-owned active-profile facts after pivot_root. */
static void boot_state(void)
{
   g_free(active_path); g_free(run_kind); g_free(run_start);
   active_path=read_file(BOOT "/profile",NULL); if (active_path) g_strchomp(active_path);
   run_kind=read_file(BOOT "/kind",NULL); if (run_kind) g_strchomp(run_kind); else run_kind=g_strdup("ram");
   run_start=read_file(BOOT "/start",NULL); if (run_start) g_strchomp(run_start);
}

/* Retain live store objects across refreshes, including their mounts and protected descriptors. */
static Profile *find_profile(const char *id)
{
   if (!id) return NULL;
   for (guint i=0;i<profiles->len;i++) { Profile *p=g_ptr_array_index(profiles,i); if (!strcmp(p->id,id)) return p; }
   return NULL;
}

/* Drives are addressed by daemon-issued handles rather than client filesystem paths. */
static Drive *find_drive(const char *id)
{
   if (!id) return NULL;
   for (guint i=0;i<drives->len;i++) { Drive *d=g_ptr_array_index(drives,i); if (!strcmp(d->id,id)) return d; }
   return NULL;
}

/* Walk every path component below the mounted drive without following user-controlled symlinks. */
static int profile_fd(Profile *profile)
{
   size_t prefix=strlen(profile->drive->path);
   if (strncmp(profile->path,profile->drive->path,prefix) || (prefix>1 && profile->path[prefix]!='/')) { errno=ESTALE; return -1; }
   int fd=open(profile->drive->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
   char **parts=g_strsplit(profile->path+prefix,"/",-1);
   for (char **part=parts;fd>=0 && *part;part++)
   {
      if (!**part) continue;
      int child=openat(fd,*part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); close(fd); fd=child;
   }
   g_strfreev(parts); return fd;
}

/* Native branch metadata is itself opened without following a replaced boot directory. */
static int metadata_fd(Profile *profile)
{
   int fd=profile_fd(profile);
   if (fd>=0 && !strcmp(profile->kind,"native")) { int boot=openat(fd,"boot",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); close(fd); fd=boot; }
   return fd;
}

/* The profile's numeric directory and its sibling last-used marker share this verified parent. */
static int changes_fd(Profile *profile)
{
   int folder=profile_fd(profile);
   if (folder<0) return -1;
   int parent=openat(folder,"..",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); close(folder); return parent;
}

/* All profile metadata updates remain relative to the validated directory descriptor. */
static gboolean write_profile(Profile *profile, const char *name, const char *text, GError **error)
{
   int fd=metadata_fd(profile); gboolean ok=text ? atomic_at(fd,name,text,error) : remove_at(fd,name,error);
   if (fd>=0) close(fd);
   return ok;
}

/* Rollback does not allocate metadata again when an atomic replacement never changed it. */
static gboolean restore_profile_file(Profile *profile, const char *name, const char *text, GError **error)
{
   int fd=metadata_fd(profile); char *current=fd>=0 ? read_fd(openat(fd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   if (fd>=0) close(fd);
   gboolean same=!g_strcmp0(current,text); g_free(current);
   return same || write_profile(profile,name,text,error);
}

/* Revalidate every mutation against the registered directory instead of trusting a stale GUI row. */
static gboolean validate(Profile *profile, gboolean write, GError **error)
{
   if (!profile) return fail(error,ENOENT,"This profile is no longer available. Refresh the list.");
   struct stat st,device;
   int fd=profile_fd(profile); gboolean valid=fd>=0 && !fstat(fd,&st) && st.st_ino==profile->inode;
   if (fd>=0) close(fd);
   if (!valid || stat(profile->drive->device,&device) || !S_ISBLK(device.st_mode) || device.st_rdev!=profile->drive->number || st.st_dev!=profile->drive->filesystem)
      return fail(error,ESTALE,"The profile drive changed or was disconnected.");
   if (write && profile->drive->uuid && *profile->drive->uuid)
   {
      const char *argv[]={"/sbin/blkid","-p","-s","UUID","-o","value",profile->drive->device,NULL};
      char *uuid=command(argv,error);
      if (!uuid) return FALSE;
      gboolean same=!strcmp(g_strchomp(uuid),profile->drive->uuid); g_free(uuid);
      if (!same) return fail(error,ESTALE,"The profile drive was replaced.");
   }
   capacity(profile->drive);
   if (write && !profile->drive->writable) return fail(error,EROFS,"The profile drive is read-only.");
   return TRUE;
}

/* Discover only known numeric profile directories; symlinks never become managed profiles. */
static void scan_profiles_at(Drive *drive, const char *changes)
{
   Profile parent={.drive=drive,.path=(char *)changes};
   int folder=profile_fd(&parent);
   if (folder<0) return;
   close(folder);
   GDir *dir=g_dir_open(changes,0,NULL); const char *entry;
   if (!dir) return;
   while ((entry=g_dir_read_name(dir)))
   {
      if (!digits(entry)) continue;
      char *path=g_build_filename(changes,entry,NULL); struct stat st;
      if (lstat(path,&st) || !S_ISDIR(st.st_mode)) { g_free(path); continue; }
      char *id=handle(drive->device,drive->uuid,entry,st.st_ino);
      Profile *profile=find_profile(id);
      if (!profile)
      {
         profile=g_new0(Profile,1); profile->id=id; profile->number=g_strdup(entry); profile->path=path; profile->drive=drive; profile->inode=st.st_ino;
         char *file=g_build_filename(path,"session.txt",NULL),*container=g_build_filename(path,"changes.dat",NULL);
         profile->kind=g_strdup(g_file_test(file,G_FILE_TEST_EXISTS)||g_file_test(container,G_FILE_TEST_EXISTS) ? "container" : "native");
         g_free(file); g_free(container); g_ptr_array_add(profiles,profile);
      }
      else
      {
         struct stat old;
         if (lstat(profile->path,&old) || old.st_dev!=st.st_dev || old.st_ino!=st.st_ino) { g_free(profile->path); profile->path=path; }
         else g_free(path);
         g_free(id);
      }
      profile->active=active_path && !strcmp(profile->path,active_path);
      if (active_path)
      {
         struct stat active;
         if (!stat(active_path,&active)) profile->active=active.st_dev==st.st_dev && active.st_ino==st.st_ino;
      }
      if (profile->active && !strcmp(profile->kind,"container") && !profile->store)
      {
         profile->store=read_file(BOOT "/dynfilefs",NULL); if (profile->store) g_strchomp(profile->store);
         profile->inner=read_file(BOOT "/changes",NULL); if (profile->inner) g_strchomp(profile->inner);
      }
      int fd=metadata_fd(profile);
      g_free(profile->name); profile->name=fd>=0 ? read_fd(openat(fd,"session.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
      if (fd>=0) close(fd);
      if (profile->name) { char *newline=strpbrk(profile->name,"\r\n"); if (newline) *newline=0; }
      if (!profile->name || !*profile->name) { g_free(profile->name); profile->name=g_strdup_printf("Profile #%s",profile->number); }
   }
   g_dir_close(dir);
}

/* Translate the boot path through mount roots, including aliases and perchdir bind mounts. */
static char *active_profile_path(Drive *drive)
{
   struct stat active;
   if (!active_path || stat(active_path,&active) || active.st_dev!=drive->filesystem) return NULL;
   char *text=NULL,*logical=NULL,*drive_root=NULL; size_t best=0;
   if (!g_file_get_contents("/proc/self/mountinfo",&text,NULL,NULL)) return NULL;
   char **lines=g_strsplit(text,"\n",-1);
   for (char **line=lines;*line;line++)
   {
      char **fields=g_strsplit(*line," ",-1);
      if (g_strv_length(fields)<6) { g_strfreev(fields); continue; }
      char *root=g_strcompress(fields[3]),*path=g_strcompress(fields[4]); size_t length=strlen(path);
      if (!strcmp(path,drive->path)) { g_free(drive_root); drive_root=g_strdup(root); }
      gboolean contains=g_str_has_prefix(active_path,path) && (length==1 || !active_path[length] || active_path[length]=='/');
      if (contains && length>best)
      { const char *relative=active_path+length; while (*relative=='/') relative++; g_free(logical); logical=g_build_filename(root,relative,NULL); best=length; }
      g_free(root); g_free(path); g_strfreev(fields);
   }
   g_strfreev(lines); g_free(text); char *result=NULL;
   if (logical && drive_root)
   {
      size_t length=strlen(drive_root);
      gboolean contains=g_str_has_prefix(logical,drive_root) && (length==1 || !logical[length] || logical[length]=='/');
      if (contains)
      {
         const char *relative=logical+length; while (*relative=='/') relative++;
         result=g_build_filename(drive->path,relative,NULL); struct stat mapped;
         if (stat(result,&mapped) || mapped.st_dev!=active.st_dev || mapped.st_ino!=active.st_ino) g_clear_pointer(&result,g_free);
      }
   }
   g_free(logical); g_free(drive_root); return result;
}

/* Include an explicitly selected custom profile directory, as supported by perchdir. */
static void scan_profiles(Drive *drive)
{
   char *changes=g_build_filename(drive->path,"slax","changes",NULL);
   scan_profiles_at(drive,changes);
   char *active=active_profile_path(drive);
   if (active)
   {
      char *custom=g_path_get_dirname(active);
      if (strcmp(changes,custom)) scan_profiles_at(drive,custom);
      g_free(custom);
   }
   g_free(active); g_free(changes);
}

/* lsblk discovery includes removable media without mounting pseudo-disks or duplicate whole disks. */
static void scan_devices(JsonArray *array, gboolean mount_missing)
{
   for (guint i=0;i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i);
      JsonArray *children=json_object_has_member(item,"children") ? json_object_get_array_member(item,"children") : NULL;
      if (children) { scan_devices(children,mount_missing); continue; }
      const char *device=string(item,"name"),*type=string(item,"type"),*fs=string(item,"fstype");
      if (!device || !type || !fs || (strcmp(type,"part") && strcmp(type,"disk")) || !strcmp(fs,"swap") || !strcmp(fs,"iso9660") || !strcmp(fs,"squashfs")) continue;
      struct stat node;
      if (stat(device,&node) || !S_ISBLK(node.st_mode)) continue;
      Drive *drive=NULL;
      for (guint j=0;j<drives->len;j++) { Drive *d=g_ptr_array_index(drives,j); if (d->number==node.st_rdev) { drive=d; break; } }
      if (!drive)
      {
         drive=g_new0(Drive,1); drive->device=g_strdup(device); drive->number=node.st_rdev;
         drive->uuid=g_strdup(string(item,"uuid")); drive->fstype=g_strdup(fs); drive->id=handle(device,drive->uuid,NULL,0);
         const char *label=string(item,"label"); drive->label=g_strdup(label ? label : "");
         drive->path=mounted(node.st_rdev);
         if (drive->path && g_str_has_prefix(drive->path,RUNTIME "/disks/")) drive->owned=TRUE;
         if (!drive->path && !mount_missing)
         { g_free(drive->device); g_free(drive->uuid); g_free(drive->fstype); g_free(drive->id); g_free(drive->label); g_free(drive); continue; }
         if (!drive->path)
         {
            drive->path=g_strdup_printf(RUNTIME "/disks/%u-%u",major(node.st_rdev),minor(node.st_rdev));
            g_mkdir_with_parents(drive->path,0700);
            gboolean ntfs=!strcmp(fs,"ntfs") || !strcmp(fs,"ntfs3");
            const char *argv[]={"/bin/mount","-o",ntfs ? "ro,nosuid,nodev" : "nosuid,nodev",device,drive->path,NULL};
            char *output=command(argv,NULL);
            if (output)
            {
               g_free(output); drive->owned=TRUE;
               if (ntfs && ntfs_safe(drive->path))
               { const char *rw[]={"/bin/mount","-o","remount,rw",drive->path,NULL}; g_free(command(rw,NULL)); }
            }
         }
         g_ptr_array_add(drives,drive);
      }
      struct stat mount;
      if (!mountpoint(drive->path) || stat(drive->path,&mount)) { drive->writable=FALSE; continue; }
      drive->filesystem=mount.st_dev; capacity(drive); scan_profiles(drive);
   }
}

/* Refresh discovery on demand; ordinary tray polling only reads inexpensive live counters. */
static gboolean discover(gboolean force, GError **error)
{
   boot_state();
   if (!force && scanned && g_get_monotonic_time()-scanned<15*G_USEC_PER_SEC) return TRUE;
   const char *argv[]={"/bin/lsblk","--json","--bytes","--paths","--output","NAME,TYPE,FSTYPE,LABEL,UUID",NULL};
   char *output=command(argv,error);
   if (!output) return FALSE;
   JsonObject *data=parse(output,error); g_free(output);
   if (!data) return FALSE;
   if (json_object_has_member(data,"blockdevices"))
   {
      g_clear_pointer(&block_devices,json_array_unref); block_devices=json_array_ref(json_object_get_array_member(data,"blockdevices"));
      scan_devices(block_devices,force);
   }
   json_object_unref(data); scanned=g_get_monotonic_time();
   if (!recovered) { recover_mounts(); recovered=TRUE; }
   return TRUE;
}

/* Store reports use the live FUSE interface whenever that store is already mounted. */
static JsonObject *report(Profile *profile, gboolean history, GError **error)
{
   char *output=NULL;
   if (profile->store && *profile->store)
   {
      char *path=g_build_filename(profile->store,history ? "snapshots.json" : "stats",NULL);
      output=read_file(path,error); g_free(path);
   }
   else
   {
      char *path=g_build_filename(profile->path,"changes.dat",NULL);
      if (!g_file_test(path,G_FILE_TEST_EXISTS))
      {
         g_free(path); JsonObject *empty=json_object_new(); json_object_set_null_member(empty,"currentParent");
         if (history) json_object_set_array_member(empty,"snapshots",json_array_new());
         return empty;
      }
      int fd=profile_fd(profile); struct stat file;
      if (fd<0 || fstatat(fd,"changes.dat",&file,AT_SYMLINK_NOFOLLOW) || !S_ISREG(file.st_mode))
      { if (fd>=0) close(fd); g_free(path); fail(error,EINVAL,"The profile backing metadata is not a regular file."); return NULL; }
      const char *argv[]={DYN,"-f","/proc/self/fd/198/changes.dat",history ? "--snapshots" : "--info","--json",NULL};
      output=command_at(argv,fd,error); close(fd); g_free(path);
   }
   if (!output) return NULL;
   JsonObject *object=parse(output,error); g_free(output); return object;
}

/* Runtime metadata identifies guarded/queued points without trusting their display names. */
static char *profile_file(Profile *profile, const char *name)
{
   int fd=metadata_fd(profile);
   char *text=fd>=0 ? read_fd(openat(fd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   if (fd>=0) close(fd);
   if (text) g_strchomp(text);
   return text;
}

/* Resolve stable IDs to live records immediately before issuing a name-based DynFileFS command. */
static JsonObject *point(JsonObject *catalog, const char *id)
{
   if (!identity(id) || !catalog || !json_object_has_member(catalog,"snapshots")) return NULL;
   JsonArray *array=json_object_get_array_member(catalog,"snapshots");
   for (guint i=0;i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i);
      if (!g_strcmp0(string(item,"id"),id) && !boolean(item,"deleted")) return item;
   }
   return NULL;
}

/* A single write is required by DynFileFS's snapshot control protocol. */
static gboolean snapshot_command(Profile *profile, const char *operation, const char *name, GError **error)
{
   char *path=g_build_filename(profile->store,"snapshot",NULL),*text=g_strdup_printf("%s %s\n",operation,name);
   int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW); ssize_t length=strlen(text);
   gboolean ok=fd>=0 && write(fd,text,length)==length;
   if (!ok) fail(error,errno,g_strerror(errno));
   if (fd>=0) close(fd);
   g_free(path); g_free(text); return ok;
}

/* Mount an inactive store only for an explicit operation, leaving inspection non-mutating. */
static gboolean mount_store(Profile *profile, gboolean inner, const char *protect, GError **error)
{
   if (strcmp(profile->kind,"container")) return fail(error,ENOTSUP,"Native directory profiles do not have restore points.");
   if (!validate(profile,inner || !profile->store,error)) return FALSE;
   if (!profile->store)
   {
      char *backing=g_build_filename(profile->path,"changes.dat",NULL);
      if (!g_file_test(backing,G_FILE_TEST_EXISTS) && inner)
      {
         int folder=profile_fd(profile); gboolean initialized=folder>=0 && initialize_storage(folder,profile->drive,error);
         if (folder>=0) close(folder);
         if (!initialized) { g_free(backing); return FALSE; }
      }
      if (!g_file_test(backing,G_FILE_TEST_IS_REGULAR)) { g_free(backing); return fail(error,ENOENT,"This profile has no saved restore points."); }
      profile->store=g_build_filename(RUNTIME,"stores",profile->id,NULL);
      g_mkdir_with_parents(profile->store,0700);
      char *queued=profile_file(profile,"restore.txt");
      if (queued && !identity(queued)) { g_free(queued); g_free(backing); g_clear_pointer(&profile->store,g_free); return fail(error,EINVAL,"The queued restore identity is invalid."); }
      int fd=profile_fd(profile); struct stat file;
      if (fd<0 || fstatat(fd,"changes.dat",&file,AT_SYMLINK_NOFOLLOW) || !S_ISREG(file.st_mode))
      { if (fd>=0) close(fd); g_free(backing); g_free(queued); g_clear_pointer(&profile->store,g_free); return fail(error,EINVAL,"The profile backing metadata is not a regular file."); }
      const char *argv[10]={DYN,"-f","/proc/self/fd/198/changes.dat","-m",profile->store}; int argc=5;
      if (queued) { argv[argc++]="--protect"; argv[argc++]=queued; }
      if (protect && g_strcmp0(queued,protect)) { argv[argc++]="--protect"; argv[argc++]=protect; }
      argv[argc]=NULL;
      char *out=command_at(argv,fd,error); close(fd); g_free(backing);
      g_free(queued);
      if (!out) { g_clear_pointer(&profile->store,g_free); return FALSE; }
      g_free(out); profile->owned=TRUE;
   }
   if (inner && !profile->inner)
   {
      profile->inner=g_strdup_printf("%s.fs",profile->store); g_mkdir_with_parents(profile->inner,0700);
      char *image=g_build_filename(profile->store,"virtual.dat",NULL);
      const char *argv[]={"/bin/mount","-t","xfs","-o","loop,nouuid,nosuid,nodev",image,profile->inner,NULL};
      char *out=command(argv,error); g_free(image);
      if (!out) { g_clear_pointer(&profile->inner,g_free); return FALSE; }
      g_free(out);
   }
   return TRUE;
}

/* Report protected identities and read-only views without inventing DynFileFS catalog fields. */
static JsonObject *history(Profile *profile, GError **error)
{
   if (!validate(profile,FALSE,error)) return NULL;
   if (strcmp(profile->kind,"container"))
   { JsonObject *empty=json_object_new(); json_object_set_null_member(empty,"currentParent"); json_object_set_array_member(empty,"snapshots",json_array_new()); return empty; }
   JsonObject *catalog=report(profile,TRUE,error);
   if (!catalog) return NULL;
   char *run=profile_file(profile,"run.txt"),*queued=profile_file(profile,"restore.txt");
   if (run) { char *line=strchr(run,'\n'); if (line) *line=0; }
   JsonArray *array=json_object_get_array_member(catalog,"snapshots");
   for (guint i=0;i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i); const char *id=string(item,"id"),*name=string(item,"name"); gboolean open=FALSE;
      for (guint j=0;j<views->len;j++) { View *view=g_ptr_array_index(views,j); if (!strcmp(view->profile,profile->id) && !g_strcmp0(view->point,id)) open=TRUE; }
      char *marker=identity(id) ? g_strconcat("guard.",id,NULL) : NULL,*owner=marker ? profile_file(profile,marker) : NULL;
      gboolean guard=owner && !g_strcmp0(owner,name); g_free(owner); g_free(marker);
      json_object_set_boolean_member(item,"filesOpen",open);
      json_object_set_boolean_member(item,"hidden",guard);
      gboolean queued_point=!g_strcmp0(queued,id);
      gboolean other_pin=boolean(item,"protected") && !queued_point;
      if (queued_point && selection && !g_strcmp0(string(selection,"profileId"),profile->id) && !g_strcmp0(string(selection,"pointId"),id)) other_pin=boolean(selection,"wasProtected");
      json_object_set_boolean_member(item,"protected",other_pin || guard || open || !g_strcmp0(run,id));
   }
   g_free(run); g_free(queued); return catalog;
}

/* Keep the device recognizable even when its optional filesystem label is long or duplicated. */
static char *device_text(const char *device, const char *label)
{
   return label && *label ? g_strdup_printf("%s (%s)",device,label) : g_strdup(device);
}

/* The discovery report also contains optical labels, without probing devices on each tray poll. */
static const char *device_label(JsonArray *items, const char *device)
{
   for (guint i=0;items && i<json_array_get_length(items);i++)
   {
      JsonObject *item=json_array_get_object_element(items,i);
      if (!g_strcmp0(string(item,"name"),device)) return string(item,"label");
      JsonArray *children=json_object_has_member(item,"children") ? json_object_get_array_member(item,"children") : NULL;
      const char *label=device_label(children,device); if (label) return label;
   }
   return NULL;
}

/* Determine current source from the mounted module backing, never from the original boot device. */
static char *source(void)
{
   char *path=read_file(BOOT "/source",NULL);
   if (!path) return g_strdup("Live system");
   g_strchomp(path);
   if (strstr(path,"/memory/toram")) { g_free(path); return g_strdup("RAM"); }
   FILE *table=setmntent("/proc/self/mounts","r"); struct mntent *entry; char *device=NULL; size_t best=0;
   while (table && (entry=getmntent(table)))
   {
      size_t length=strlen(entry->mnt_dir);
      if (length>best && g_str_has_prefix(path,entry->mnt_dir) && (!path[length] || path[length]=='/'))
      { g_free(device); device=g_strdup(entry->mnt_fsname); best=length; }
   }
   if (table) endmntent(table);
   if (device && g_str_has_prefix(device,"/dev/loop"))
   {
      char *name=g_path_get_basename(device),*sys=g_build_filename("/sys/class/block",name,"loop/backing_file",NULL),*backing=NULL;
      if (g_file_get_contents(sys,&backing,NULL,NULL)) { g_strchomp(backing); g_free(path); path=g_strdup_printf("ISO · %s",backing); }
      g_free(backing); g_free(sys); g_free(name);
   }
   else if (device) { g_free(path); path=device_text(device,device_label(block_devices,device)); }
   g_free(device); return path;
}

/* Compose a cheap status model. Offline allocator reports are cached until an explicit refresh. */
static JsonObject *state(GError **error)
{
   if (!discover(FALSE,error)) return NULL;
   JsonObject *result=json_object_new(),*run=json_object_new(); JsonArray *ds=json_array_new(),*ps=json_array_new(),*vs=json_array_new();
   Profile *active=NULL;
   for (guint i=0;i<drives->len;i++)
   {
      Drive *drive=g_ptr_array_index(drives,i); struct stat st;
      if (stat(drive->path,&st) || st.st_dev!=drive->filesystem) continue;
      capacity(drive); JsonObject *object=json_object_new();
      json_object_set_string_member(object,"id",drive->id); json_object_set_string_member(object,"label",drive->label);
      json_object_set_string_member(object,"device",drive->device); json_object_set_string_member(object,"path",drive->path);
      json_object_set_int_member(object,"freeBytes",drive->free); json_object_set_int_member(object,"totalBytes",drive->total); json_object_set_boolean_member(object,"writable",drive->writable);
      json_array_add_object_element(ds,object);
   }
   for (guint i=0;i<profiles->len;i++)
   {
      Profile *profile=g_ptr_array_index(profiles,i);
      if (!validate(profile,FALSE,NULL)) continue;
      if (profile->active) active=profile;
      if (!strcmp(profile->kind,"container") && (profile->store || !profile->stats_at || g_get_monotonic_time()-profile->stats_at>30*G_USEC_PER_SEC))
      {
         GError *local=NULL; JsonObject *stats=report(profile,FALSE,&local);
         if (stats)
         { if (json_object_has_member(stats,"reusableBytes")) profile->reusable=json_object_get_int_member(stats,"reusableBytes"); json_object_unref(stats); if (profile->active) g_clear_pointer(&run_error,g_free); }
         if (local && profile->active) { g_free(run_error); run_error=g_strdup(local->message); }
         g_clear_error(&local); profile->stats_at=g_get_monotonic_time();
      }
      JsonObject *object=json_object_new();
      json_object_set_string_member(object,"id",profile->id); json_object_set_string_member(object,"numericId",profile->number); json_object_set_string_member(object,"name",profile->name);
      json_object_set_string_member(object,"device",profile->drive->device); json_object_set_string_member(object,"driveId",profile->drive->id); json_object_set_string_member(object,"kind",profile->kind);
      json_object_set_boolean_member(object,"active",profile->active); json_object_set_boolean_member(object,"writable",profile->drive->writable);
      json_object_set_int_member(object,"freeBytes",profile->drive->free); json_object_set_int_member(object,"totalBytes",profile->drive->total); json_object_set_int_member(object,"reusableBytes",profile->reusable);
      json_object_set_int_member(object,"sizeBytes",profile->size); json_object_set_int_member(object,"lastUsed",profile->used);
      json_array_add_object_element(ps,object);
   }
   char *where=source(); json_object_set_string_member(run,"source",where); g_free(where);
   nullable(run,"profileId",active ? active->id : NULL); json_object_set_string_member(run,"kind",active ? active->kind : "ram");
   char *storage=active ? device_text(active->drive->device,active->drive->label) : g_strdup("RAM");
   json_object_set_string_member(run,"storage",storage); g_free(storage); nullable(run,"startId",run_start);
   char *policy=active ? profile_file(active,"run.txt") : NULL;
   gboolean save=active && (!policy || !g_str_has_suffix(policy,"\ndiscard"));
   json_object_set_boolean_member(run,"save",save);
   json_object_set_boolean_member(run,"canSetSave",active && !strcmp(active->kind,"container") && active->drive->writable && identity(run_start));
   json_object_set_string_member(run,"error",run_error ? run_error : "");
   guint64 free_bytes=0,total_bytes=0;
   if (active) { free_bytes=active->drive->free; total_bytes=active->drive->total; }
   else
   {
      char *memory=NULL; if (g_file_get_contents("/proc/meminfo",&memory,NULL,NULL))
      { char **lines=g_strsplit(memory,"\n",-1); for (char **line=lines;*line;line++) { if (g_str_has_prefix(*line,"MemAvailable:")) free_bytes=g_ascii_strtoull(*line+13,NULL,10)*1024; if (g_str_has_prefix(*line,"MemTotal:")) total_bytes=g_ascii_strtoull(*line+9,NULL,10)*1024; } g_strfreev(lines); g_free(memory); }
   }
   json_object_set_int_member(run,"freeBytes",free_bytes); json_object_set_int_member(run,"totalBytes",total_bytes); json_object_set_int_member(run,"reusableBytes",active ? active->reusable : 0); g_free(policy);
   Profile *next=selection ? find_profile(string(selection,"profileId")) : NULL;
   if (next)
   {
      JsonObject *queued=json_object_new(); json_object_set_string_member(queued,"profileId",next->id); nullable(queued,"pointId",string(selection,"pointId")); json_object_set_string_member(queued,"label",next->name);
      nullable(queued,"pointName",string(selection,"pointName"));
      json_object_set_int_member(queued,"created",json_object_has_member(selection,"created") ? json_object_get_int_member(selection,"created") : 0);
      json_object_set_object_member(result,"next",queued);
   }
   else json_object_set_null_member(result,"next");
   for (guint i=0;i<views->len;i++)
   {
      View *view=g_ptr_array_index(views,i); JsonObject *object=json_object_new();
      json_object_set_string_member(object,"id",view->id); json_object_set_string_member(object,"profileId",view->profile); json_object_set_string_member(object,"pointId",view->point); json_object_set_string_member(object,"path",view->browse); json_array_add_object_element(vs,object);
   }
   json_object_set_array_member(result,"drives",ds); json_object_set_array_member(result,"profiles",ps); json_object_set_array_member(result,"views",vs); json_object_set_object_member(result,"run",run);
   return result;
}

/* Remove only a verified profile tree, without traversing symlinks or crossing filesystem boundaries. */
static gboolean remove_tree(int fd, dev_t device, GError **error)
{
   DIR *dir=fdopendir(dup(fd));
   if (!dir) return fail(error,errno,g_strerror(errno));
   struct dirent *entry; gboolean ok=TRUE;
   while (ok && (entry=readdir(dir)))
   {
      if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
      struct stat st;
      if (fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)) { ok=fail(error,errno,g_strerror(errno)); break; }
      if (S_ISDIR(st.st_mode))
      {
         if (st.st_dev!=device) { ok=fail(error,EBUSY,"A filesystem is still mounted inside this profile."); break; }
         int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
         if (child<0) { ok=fail(error,errno,g_strerror(errno)); break; }
         struct statx parent_mount,child_mount;
         if (statx(fd,"",AT_EMPTY_PATH,STATX_MNT_ID,&parent_mount) || statx(child,"",AT_EMPTY_PATH,STATX_MNT_ID,&child_mount) || parent_mount.stx_mnt_id!=child_mount.stx_mnt_id)
         { close(child); ok=fail(error,EBUSY,"A filesystem is still mounted inside this profile."); break; }
         ok=remove_tree(child,device,error); close(child);
         if (ok && unlinkat(fd,entry->d_name,AT_REMOVEDIR)) ok=fail(error,errno,g_strerror(errno));
      }
      else if (unlinkat(fd,entry->d_name,0)) ok=fail(error,errno,g_strerror(errno));
   }
   closedir(dir); return ok;
}

/* A metadata label change never renames the physical profile directory. */
static gboolean rename_profile(Profile *profile, const char *name, GError **error)
{
   if (!validate(profile,TRUE,error) || !valid_name(name,FALSE,error)) return FALSE;
   char *line=g_strconcat(name,"\n",NULL);
   gboolean ok=write_profile(profile,"session.txt",line,error);
   if (ok) { g_free(profile->name); profile->name=*name ? g_strdup(name) : g_strdup_printf("Profile #%s",profile->number); }
   g_free(line); return ok;
}

/* Format a new sparse store before publishing its profile; no current profile contents are copied. */
static gboolean initialize_storage(int directory, Drive *drive, GError **error)
{
   char *uuid=g_uuid_string_random(),*mount=g_build_filename(RUNTIME,"initialize",uuid,NULL); g_free(uuid);
   if (g_mkdir_with_parents(mount,0700)) { g_free(mount); return fail(error,errno,g_strerror(errno)); }
   guint64 mib=MAX((drive->total+1073741823)/1073741824*1024,16000);
   char *size=g_strdup_printf("%"G_GUINT64_FORMAT,mib);
   const char *create[]={DYN,"-f","/proc/self/fd/198/changes.dat","-s",size,"-p","4000","-m",mount,NULL};
   char *out=command_at(create,directory,error); gboolean ok=out!=NULL,mounted_store=ok; g_free(out); g_free(size);
   char *image=g_build_filename(mount,"virtual.dat",NULL);
   if (ok)
   {
      const char *format[]={"/run/initramfs/bin/mkfs.xfs.custom",image,NULL}; out=command(format,error); ok=out!=NULL; g_free(out);
   }
   if (ok)
   {
      char *path=g_build_filename(mount,"snapshot",NULL),*blank=read_file(path,error); g_free(path);
      if (blank)
      { g_strchomp(blank); Profile temporary={.store=mount}; ok=snapshot_command(&temporary,"delete",blank,error); g_free(blank); }
      else ok=FALSE;
   }
   if (mounted_store)
   {
      const char *unmount[]={"/bin/umount",mount,NULL}; out=command(unmount,ok ? error : NULL);
      if (!out) ok=FALSE;
      g_free(out);
   }
   rmdir(mount); g_free(image); g_free(mount); return ok;
}

/* New profile creation is an atomic empty-directory publication, never a current-state clone. */
static JsonObject *create_profile(Drive *drive, const char *name, GError **error)
{
   if (!drive) { fail(error,ENOENT,"The selected drive is unavailable."); return NULL; }
   if (!valid_name(name,FALSE,error)) return NULL;
   struct stat st;
   if (stat(drive->path,&st) || st.st_dev!=drive->filesystem) { fail(error,ESTALE,"The selected drive was disconnected."); return NULL; }
   capacity(drive);
   if (!drive->writable) { fail(error,EROFS,"The selected drive is read-only."); return NULL; }
   char *changes=g_build_filename(drive->path,"slax","changes",NULL);
   int disk=open(drive->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
   if (disk>=0) mkdirat(disk,"slax",0755);
   int slax=disk>=0 ? openat(disk,"slax",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW) : -1;
   if (disk>=0) close(disk);
   if (slax>=0) mkdirat(slax,"changes",0755);
   int parent=slax>=0 ? openat(slax,"changes",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW) : -1;
   if (slax>=0) close(slax);
   if (parent<0) { g_free(changes); fail(error,errno,g_strerror(errno)); return NULL; }
   guint64 largest=0; GDir *dir=g_dir_open(changes,0,NULL); const char *entry;
   if (dir) { while ((entry=g_dir_read_name(dir))) if (digits(entry)) largest=MAX(largest,g_ascii_strtoull(entry,NULL,10)); g_dir_close(dir); }
   char *number=g_strdup_printf("%"G_GUINT64_FORMAT,largest+1),*temporary=g_strdup_printf(".new.%08x-%08x",g_random_int(),g_random_int());
   gboolean ok=!mkdirat(parent,temporary,0755);
   int folder=ok ? openat(parent,temporary,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW) : -1;
   gboolean native=FALSE;
   if (folder>=0)
   {
      int probe=openat(folder,".posix",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
      if (probe>=0)
      {
         native=strncmp(drive->fstype,"ntfs",4) && !symlinkat(".posix",folder,".posix.link") && !fchmod(probe,0700) && !fstat(probe,&st) && (st.st_mode&S_IXUSR);
         close(probe); unlinkat(folder,".posix",0); unlinkat(folder,".posix.link",0);
      }
      if (native) ok=!mkdirat(folder,"boot",0755);
      int meta=native ? openat(folder,"boot",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW) : dup(folder);
      char *label=g_strconcat(name,"\n",NULL);
      if (ok) ok=atomic_at(meta,"session.txt",label,error);
      if (meta>=0) close(meta);
      g_free(label);
      if (ok && !native) ok=initialize_storage(folder,drive,error);
      if (ok)
      {
         /* renameat2 avoids replacing a directory made concurrently by another boot manager. */
         if (renameat2(parent,temporary,parent,number,RENAME_NOREPLACE) || fsync(parent)) ok=FALSE;
      }
      if (!ok) { remove_tree(folder,drive->filesystem,NULL); unlinkat(parent,temporary,AT_REMOVEDIR); }
      close(folder);
   }
   else ok=FALSE;
   if (!ok && (!error || !*error)) fail(error,errno,g_strerror(errno));
   close(parent); g_free(temporary); g_free(changes);
   JsonObject *result=NULL;
   if (ok)
   {
      scan_profiles(drive);
      for (guint i=0;i<profiles->len;i++)
      { Profile *profile=g_ptr_array_index(profiles,i); if (profile->drive==drive && !strcmp(profile->number,number)) { result=json_object_new(); json_object_set_string_member(result,"profileId",profile->id); break; } }
   }
   g_free(number); return result;
}

/* Engine protection survives the broker and remains active through clean DynFileFS unmount. */
static gboolean pin(Profile *profile, const char *id, GError **error)
{
   return identity(id) ? snapshot_command(profile,"protect",id,error) : fail(error,EINVAL,"Invalid restore point identity.");
}

/* Detach only loops belonging to our already-unmounted image before closing its FUSE provider. */
static gboolean detach_image(const char *image, GError **error)
{
   const char *query[]={"/sbin/losetup","--noheadings","--output","NAME","--associated",image,NULL};
   char *output=command(query,error); if (!output) return FALSE;
   char **lines=g_strsplit(output,"\n",-1); gboolean ok=TRUE;
   for (char **line=lines;ok && *line;line++)
   {
      char *name=g_strstrip(*line); if (!*name) continue;
      if (!g_str_has_prefix(name,"/dev/loop") || !digits(name+9)) { ok=fail(error,EINVAL,"Unexpected loop device identity."); break; }
      const char *detach[]={"/sbin/losetup","--detach",name,NULL}; char *result=command(detach,error); ok=result!=NULL; g_free(result);
   }
   g_strfreev(lines); g_free(output); return ok;
}

/* Shutdown uses ordinary unmounting; pending history remains protected by DynFileFS itself. */
static gboolean close_store(Profile *profile, GError **error)
{
   if (!profile->owned || !profile->store) return TRUE;
   for (guint i=0;i<views->len;i++) if (!strcmp(((View *)g_ptr_array_index(views,i))->profile,profile->id)) return fail(error,EBUSY,"Close this profile's file views first.");
   if (profile->inner)
   {
      const char *argv[]={"/bin/umount",profile->inner,NULL}; char *out=command(argv,error);
      if (!out) return FALSE;
      g_free(out); rmdir(profile->inner); g_clear_pointer(&profile->inner,g_free);
   }
   char *image=g_build_filename(profile->store,"virtual.dat",NULL); gboolean detached=detach_image(image,error); g_free(image);
   if (!detached) return FALSE;
   const char *argv[]={"/bin/umount",profile->store,NULL}; char *out=command(argv,error);
   if (!out) return FALSE;
   g_free(out); rmdir(profile->store); g_clear_pointer(&profile->store,g_free); profile->owned=FALSE;
   return TRUE;
}

/* Refuse to delete active, mounted, pinned or borrowed stores; acquire DynFileFS's actual lock. */
static gboolean delete_profile(Profile *profile, GError **error)
{
   if (!validate(profile,TRUE,error)) return FALSE;
   if (profile->active) return fail(error,EBUSY,"The running profile cannot be deleted.");
   if (selection && !g_strcmp0(string(selection,"profileId"),profile->id) && !clear_next(error)) return FALSE;
   gboolean was_owned=profile->owned;
   if (!close_store(profile,error)) return FALSE;
   FILE *table=setmntent("/proc/self/mounts","r"); struct mntent *entry; gboolean busy=FALSE;
   char *prefix=g_strconcat(profile->path,"/",NULL);
   while (table && (entry=getmntent(table))) if (!strcmp(entry->mnt_dir,profile->path) || g_str_has_prefix(entry->mnt_dir,prefix)) { busy=TRUE; break; }
   if (table) endmntent(table);
   g_free(prefix);
   if (busy) return fail(error,EBUSY,"A filesystem is still mounted inside this profile.");
   int fd=profile_fd(profile),parent=changes_fd(profile),lock=-1;
   if (fd<0) return fail(error,errno,g_strerror(errno));
   if (parent<0) { close(fd); return fail(error,errno,g_strerror(errno)); }
   struct stat opened;
   if (fstat(fd,&opened) || opened.st_ino!=profile->inode || opened.st_dev!=profile->drive->filesystem)
   { close(fd); close(parent); return fail(error,ESTALE,"The profile directory changed before deletion."); }
   if (!strcmp(profile->kind,"container"))
   {
      gint64 deadline=g_get_monotonic_time()+(was_owned ? 3*G_USEC_PER_SEC : 0);
      while (TRUE)
      {
         lock=openat(fd,"changes.dat.lock",O_CREAT|O_RDWR|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW,0600);
         struct stat held,named;
         if (lock>=0 && !fstat(lock,&held) && S_ISREG(held.st_mode) && !flock(lock,LOCK_EX|LOCK_NB) && !fstatat(fd,"changes.dat.lock",&named,AT_SYMLINK_NOFOLLOW) && held.st_ino==named.st_ino) break;
         if (lock>=0) close(lock);
         if (g_get_monotonic_time()>=deadline) { close(fd); close(parent); return fail(error,EBUSY,"This profile is used by another process."); }
         /* FUSE unmount returns just before its daemon finishes publishing and releases the lock. */
         g_usleep(20000);
      }
   }
   char *temporary=g_strdup_printf(".remove.%08x-%08x",g_random_int(),g_random_int()); struct stat named;
   gboolean ok=!renameat2(parent,profile->number,parent,temporary,RENAME_NOREPLACE);
   if (ok && (fstatat(parent,temporary,&named,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(named.st_mode) || named.st_ino!=profile->inode)) ok=fail(error,ESTALE,"The profile directory changed before deletion.");
   if (ok) ok=remove_tree(fd,profile->drive->filesystem,error);
   close(fd);
   if (ok && unlinkat(parent,temporary,AT_REMOVEDIR)) ok=fail(error,errno,g_strerror(errno));
   if (!ok) { renameat2(parent,temporary,parent,profile->number,RENAME_NOREPLACE); if (!error || !*error) fail(error,errno,g_strerror(errno)); }
   g_free(temporary);
   if (lock>=0) close(lock);
   if (ok)
   {
      char *selected=read_fd(openat(parent,"last_session.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL);
      if (selected) g_strchomp(selected);
      if (!g_strcmp0(selected,profile->number)) remove_at(parent,"last_session.txt",NULL);
      g_free(selected); scanned=0;
   }
   close(parent);
   return ok;
}

/* Preserve the original file permissions; the file manager opens the requesting user's own home. */
static char *browse_path(const char *root, uid_t uid)
{
   struct passwd *user=getpwuid(uid);
   char *path=user && user->pw_dir[0]=='/' ? g_build_filename(root,user->pw_dir+1,NULL) : g_strdup(root);
   if (!g_file_test(path,G_FILE_TEST_IS_DIR)) { g_free(path); path=g_strdup(root); }
   return path;
}

/* Publish a view intent before mounting so even a crash between mounts remains recoverable. */
static gboolean save_view(View *view, GError **error)
{
   JsonObject *record=json_object_new(); json_object_set_string_member(record,"profileId",view->profile); json_object_set_string_member(record,"pointId",view->point); json_object_set_int_member(record,"uid",view->uid);
   char *base=g_path_get_dirname(view->path); gboolean ok=write_json(base,"view.json",record,error);
   g_free(base); json_object_unref(record); return ok;
}

/* Mount immutable snapshots without replay, then overlay their upper layer over the current base modules. */
static JsonObject *open_files(Profile *profile, const char *id, uid_t uid, GError **error)
{
   if (!identity(id)) { fail(error,EINVAL,"Invalid restore point identity."); return NULL; }
   for (guint i=0;i<views->len;i++)
   {
      View *view=g_ptr_array_index(views,i);
      if (view->uid==uid && !strcmp(view->profile,profile->id) && !strcmp(view->point,id))
      { JsonObject *result=json_object_new(); json_object_set_string_member(result,"viewId",view->id); json_object_set_string_member(result,"path",view->browse); return result; }
   }
   gboolean new_store=!profile->store;
   if (!mount_store(profile,FALSE,id,error)) return NULL;
   JsonObject *catalog=report(profile,TRUE,error),*saved=point(catalog,id);
   if (!saved) { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The restore point is no longer available."); return NULL; }
   View *view=g_new0(View,1); view->id=g_uuid_string_random(); view->uid=uid; view->profile=g_strdup(profile->id); view->point=g_strdup(id);
   char *base=g_build_filename(RUNTIME,"views",view->id,NULL); view->image=g_build_filename(base,"image",NULL); view->path=g_build_filename(base,"files",NULL);
   g_mkdir_with_parents(view->image,0755); g_mkdir_with_parents(view->path,0755);
   gboolean mounted_image=FALSE;
   if (!save_view(view,error)) { json_object_unref(catalog); goto failed; }
   char *filename=g_strconcat(string(saved,"name"),".dat",NULL),*image=g_build_filename(profile->store,"snapshots",filename,NULL);
   const char *mount[]={"/bin/mount","-t","xfs","-o","loop,ro,nouuid,norecovery,nosuid,nodev,noexec",image,view->image,NULL};
   char *out=command(mount,error); mounted_image=out!=NULL; g_free(out); g_free(image); g_free(filename); json_object_unref(catalog);
   if (new_store)
   {
      char *queued=profile_file(profile,"restore.txt");
      if (g_strcmp0(queued,id)) snapshot_command(profile,"unprotect",id,NULL);
      g_free(queued);
   }
   if (!mounted_image) goto failed;
   char *upper=g_build_filename(view->image,"changes",NULL); GString *lower=g_string_new("lowerdir=");
   g_string_append(lower,g_file_test(upper,G_FILE_TEST_IS_DIR) ? upper : view->image); g_free(upper);
   GPtrArray *modules=g_ptr_array_new_with_free_func(g_free); GDir *dir=g_dir_open("/run/initramfs/memory/bundles",0,error); const char *entry;
   if (!dir) { g_string_free(lower,TRUE); g_ptr_array_unref(modules); goto failed; }
   while ((entry=g_dir_read_name(dir))) g_ptr_array_add(modules,g_strdup(entry));
   g_dir_close(dir);
   /* Module names are ordinal-prefixed; compare strings through their pointer-array slots. */
   for (guint i=0;i<modules->len;i++) for (guint j=i+1;j<modules->len;j++)
      if (strverscmp(g_ptr_array_index(modules,i),g_ptr_array_index(modules,j))<0)
      { gpointer swap=modules->pdata[i]; modules->pdata[i]=modules->pdata[j]; modules->pdata[j]=swap; }
   for (guint i=0;i<modules->len;i++) g_string_append_printf(lower,":/run/initramfs/memory/bundles/%s",(char *)g_ptr_array_index(modules,i));
   g_ptr_array_unref(modules); g_string_append(lower,",ro,nosuid,nodev,noexec");
   const char *overlay[]={"/bin/mount","-t","overlay","-o",lower->str,"overlay",view->path,NULL}; out=command(overlay,error); g_string_free(lower,TRUE);
   if (!out) goto failed;
   g_free(out); view->overlay=TRUE;
   view->browse=browse_path(view->path,uid);
   g_ptr_array_add(views,view); g_free(base);
   JsonObject *result=json_object_new(); json_object_set_string_member(result,"viewId",view->id); json_object_set_string_member(result,"path",view->browse); return result;
failed:
   if (new_store)
   {
      char *queued=profile_file(profile,"restore.txt");
      if (g_strcmp0(queued,id)) snapshot_command(profile,"unprotect",id,NULL);
      g_free(queued);
   }
   if (mounted_image) { const char *unmount[]={"/bin/umount",view->image,NULL}; g_free(command(unmount,NULL)); }
   if (mountpoint(view->image))
   { view->browse=g_strdup(view->path); g_ptr_array_add(views,view); g_free(base); return NULL; }
   remove_file(base,"view.json",NULL); rmdir(view->path); rmdir(view->image); rmdir(base); g_free(base);
   g_free(view->id); g_free(view->profile); g_free(view->point); g_free(view->path); g_free(view->image); g_free(view);
   return NULL;
}

/* Keep a busy view recorded so the user can close the file-manager window and try again. */
static gboolean close_view(View *view, GError **error)
{
   char *out;
   if (view->overlay)
   {
      const char *root[]={"/bin/umount",view->path,NULL}; out=command(root,error);
      if (!out) return FALSE;
      g_free(out); view->overlay=FALSE;
   }
   const char *image[]={"/bin/umount",view->image,NULL}; out=command(image,error);
   if (!out) return FALSE;
   g_free(out); rmdir(view->path); rmdir(view->image); char *base=g_path_get_dirname(view->path); remove_file(base,"view.json",NULL); rmdir(base); g_free(base);
   g_ptr_array_remove(views,view);
   g_free(view->id); g_free(view->profile); g_free(view->point); g_free(view->path); g_free(view->browse); g_free(view->image); g_free(view); return TRUE;
}

/* Reattach only daemon-owned mount locations and root-owned view records after an unexpected restart. */
static void recover_mounts(void)
{
   char *text=read_file(RUNTIME "/selection.json",NULL);
   if (text) { selection=parse(text,NULL); g_free(text); }
   for (guint i=0;i<profiles->len;i++)
   {
      Profile *profile=g_ptr_array_index(profiles,i);
      if (!profile->active && !profile->store)
      {
         char *path=g_build_filename(RUNTIME,"stores",profile->id,NULL);
         if (mountpoint(path))
         {
            profile->store=path; profile->owned=TRUE;
            char *inner=g_strdup_printf("%s.fs",path);
            if (mountpoint(inner)) profile->inner=inner; else g_free(inner);
         }
         else g_free(path);
      }
      char *queued=profile_file(profile,"restore.txt");
      if (queued && identity(queued) && profile->store) pin(profile,queued,NULL);
      g_free(queued);
   }
   GDir *dir=g_dir_open(RUNTIME "/views",0,NULL); const char *name;
   while (dir && (name=g_dir_read_name(dir)))
   {
      if (!g_uuid_string_is_valid(name)) continue;
      char *base=g_build_filename(RUNTIME,"views",name,NULL),*record=g_build_filename(base,"view.json",NULL);
      text=read_file(record,NULL); JsonObject *data=text ? parse(text,NULL) : NULL; g_free(text); g_free(record);
      const char *profile_id=string(data,"profileId"),*point_id=string(data,"pointId");
      if (!find_profile(profile_id) || !identity(point_id) || !json_object_has_member(data,"uid")) { if (data) json_object_unref(data); g_free(base); continue; }
      View *view=g_new0(View,1); view->id=g_strdup(name); view->profile=g_strdup(profile_id); view->point=g_strdup(point_id); view->uid=json_object_get_int_member(data,"uid");
      view->path=g_build_filename(base,"files",NULL); view->image=g_build_filename(base,"image",NULL); view->overlay=mountpoint(view->path); view->browse=browse_path(view->path,view->uid);
      if (mountpoint(view->image)) g_ptr_array_add(views,view);
      else
      {
         remove_file(base,"view.json",NULL); rmdir(view->path); rmdir(view->image); rmdir(base);
         g_free(view->id); g_free(view->profile); g_free(view->point); g_free(view->path); g_free(view->browse); g_free(view->image); g_free(view);
      }
      json_object_unref(data); g_free(base);
   }
   if (dir) g_dir_close(dir);
   while (selection && json_object_has_member(selection,"previous"))
   {
      JsonObject *previous=json_object_ref(json_object_get_object_member(selection,"previous"));
      if (!restore_selection(TRUE,NULL)) { json_object_unref(previous); break; }
      selection=previous; write_json(RUNTIME,"selection.json",selection,NULL);
   }
}

/* Run policy is independent of the explicit next-start selection. */
static gboolean save_run(Profile *profile, gboolean save, GError **error)
{
   if (!validate(profile,TRUE,error)) return FALSE;
   if (!profile->active || strcmp(profile->kind,"container") || !identity(run_start)) return fail(error,ENOTSUP,"This run cannot change its save setting.");
   char *text=g_strdup_printf("%s\n%s\n",run_start,save ? "save" : "discard");
   gboolean ok=write_profile(profile,"run.txt",text,error); g_free(text); return ok;
}

/* Cancel only this daemon's pending selection, restoring the previous per-drive boot marker. */
static gboolean restore_selection(gboolean release, GError **error)
{
   if (!selection) return TRUE;
   Profile *profile=find_profile(string(selection,"profileId"));
   if (!validate(profile,TRUE,error)) return FALSE;
   const char *previous=string(selection,"restore"),*selected=string(selection,"pointId");
   char *previous_id=previous ? g_strdup(previous) : NULL; if (previous_id) g_strchomp(previous_id);
   if (profile->store && previous_id && !pin(profile,previous_id,error)) { g_free(previous_id); return FALSE; }
   if (!restore_profile_file(profile,"restore.txt",string(selection,"restore"),error)) { g_free(previous_id); return FALSE; }
   const char *intent=string(selection,"intentName");
   if (intent && !restore_profile_file(profile,intent,string(selection,"intent"),error)) { g_free(previous_id); return FALSE; }
   int parent=changes_fd(profile); JsonObject *last=json_object_get_object_member(selection,"last");
   const char *text=string(last,"text");
   char *current=parent>=0 ? read_fd(openat(parent,"last_session.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   gboolean ok=!g_strcmp0(current,text) || (text ? atomic_at(parent,"last_session.txt",text,error) : remove_at(parent,"last_session.txt",error)); g_free(current);
   if (ok && text)
   {
      struct timespec times[2]={
         {.tv_sec=json_object_get_int_member(last,"atime"),.tv_nsec=json_object_get_int_member(last,"atimeNs")},
         {.tv_sec=json_object_get_int_member(last,"mtime"),.tv_nsec=json_object_get_int_member(last,"mtimeNs")}
      };
      if (utimensat(parent,"last_session.txt",times,AT_SYMLINK_NOFOLLOW) || fsync(parent)) ok=fail(error,errno,g_strerror(errno));
   }
   if (parent>=0) close(parent);
   if (!ok) { g_free(previous_id); return FALSE; }
   if (release && selected && profile->store && g_strcmp0(selected,previous_id) && !boolean(selection,"wasProtected")) snapshot_command(profile,"unprotect",selected,NULL);
   g_free(previous_id);
   if (!remove_file(RUNTIME,"selection.json",error)) return FALSE;
   json_object_unref(selection); selection=NULL; return TRUE;
}

/* Explicit cancel releases the superseded engine pin only after restoring durable policy. */
static gboolean clear_next(GError **error)
{
   return restore_selection(TRUE,error);
}

/* Deleting a queued point cancels only that exact request, never another profile's next start. */
static gboolean cancel_point(Profile *profile, const char *id, gboolean *cancelled, GError **error)
{
   char *queued=profile_file(profile,"restore.txt"); gboolean match=!g_strcmp0(queued,id); g_free(queued);
   if (!match) return TRUE;
   gboolean own=selection && !g_strcmp0(string(selection,"profileId"),profile->id) && !g_strcmp0(string(selection,"pointId"),id);
   gboolean ok=own ? clear_next(error) : write_profile(profile,"restore.txt",NULL,error);
   queued=profile_file(profile,"restore.txt"); gboolean remains=!g_strcmp0(queued,id); g_free(queued);
   if (ok && remains) ok=write_profile(profile,"restore.txt",NULL,error);
   queued=profile_file(profile,"restore.txt"); *cancelled=g_strcmp0(queued,id)!=0; g_free(queued);
   if (ok && (!own || remains))
   {
      char *intent=g_strconcat("keep-current.",id,NULL); ok=write_profile(profile,intent,NULL,error); g_free(intent);
      if (ok && profile->store) ok=snapshot_command(profile,"unprotect",id,error);
   }
   return ok;
}

/* Journal the previous marker before changing anything, making cancel survive UI and daemon restarts. */
static gboolean select_next(Profile *profile, const char *id, gboolean keep, GError **error)
{
   if (!validate(profile,TRUE,error)) return FALSE;
   JsonObject *catalog=NULL,*saved=NULL;
   if (id)
   {
      if (strcmp(profile->kind,"container") || !identity(id)) return fail(error,EINVAL,"This profile cannot use that restore point.");
      catalog=report(profile,TRUE,error); saved=point(catalog,id);
      if (!saved)
      { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The restore point is no longer available."); return FALSE; }
   }
   Profile *previous_profile=selection ? find_profile(string(selection,"profileId")) : NULL;
   char *previous_id=selection ? g_strdup(string(selection,"pointId")) : NULL;
   if (id && profile->store && !pin(profile,id,error)) { if (catalog) json_object_unref(catalog); g_free(previous_id); return FALSE; }
   JsonObject *previous_selection=selection ? json_object_ref(selection) : NULL;
   JsonObject *pending=json_object_new(),*last=json_object_new();
   if (previous_selection) json_object_set_object_member(pending,"previous",json_object_ref(previous_selection));
   json_object_set_string_member(pending,"profileId",profile->id); nullable(pending,"pointId",id);
   if (saved)
   {
      json_object_set_string_member(pending,"pointName",boolean(saved,"automatic") ? "Automatic restore point" : string(saved,"name"));
      json_object_set_int_member(pending,"created",json_object_get_int_member(saved,"created"));
      gboolean protected=boolean(saved,"protected");
      char *queued=profile_file(profile,"restore.txt"); if (!g_strcmp0(queued,id)) protected=FALSE; g_free(queued);
      if (previous_selection && previous_profile==profile && !g_strcmp0(previous_id,id)) protected=boolean(previous_selection,"wasProtected");
      json_object_set_boolean_member(pending,"wasProtected",protected);
   }
   int parent=changes_fd(profile),meta=metadata_fd(profile); struct stat st={0};
   char *previous=parent>=0 ? read_fd(openat(parent,"last_session.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   if (parent>=0) fstatat(parent,"last_session.txt",&st,AT_SYMLINK_NOFOLLOW);
   nullable(last,"text",previous); g_free(previous);
   json_object_set_int_member(last,"atime",st.st_atim.tv_sec); json_object_set_int_member(last,"atimeNs",st.st_atim.tv_nsec);
   json_object_set_int_member(last,"mtime",st.st_mtim.tv_sec); json_object_set_int_member(last,"mtimeNs",st.st_mtim.tv_nsec);
   json_object_set_object_member(pending,"last",last);
   previous=meta>=0 ? read_fd(openat(meta,"restore.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   nullable(pending,"restore",previous); g_free(previous);
   char *intent=id ? g_strconcat("keep-current.",id,NULL) : NULL; nullable(pending,"intentName",intent);
   previous=intent && meta>=0 ? read_fd(openat(meta,intent,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   nullable(pending,"intent",previous); g_free(previous);
   if (meta>=0) close(meta);
   gboolean ok=parent>=0 && write_json(RUNTIME,"selection.json",pending,error);
   gboolean started=ok;
   if (ok) { if (selection) json_object_unref(selection); selection=pending; pending=NULL; }
   if (ok && id)
   {
      if (keep)
      {
         char *existing=profile_file(profile,intent);
         if (!existing)
         {
            GDateTime *now=g_date_time_new_now_utc(); char *date=g_date_time_format(now,"%Y-%m-%d %H-%M-%S");
            char *label=g_strdup_printf("Before restore %s %08x\n",date,g_random_int());
            ok=write_profile(profile,intent,label,error); g_free(label); g_free(date); g_date_time_unref(now);
         }
         g_free(existing);
      }
      else ok=write_profile(profile,intent,NULL,error);
      char *line=g_strconcat(id,"\n",NULL);
      if (ok) ok=write_profile(profile,"restore.txt",line,error);
      g_free(line);
   }
   else if (ok)
   {
      ok=write_profile(profile,"restore.txt",NULL,error);
      if (ok && !strcmp(profile->kind,"container"))
      {
         char *policy=profile_file(profile,"run.txt");
         if (policy)
         {
            char *end=strchr(policy,'\n'); if (end) *end=0;
            if (!identity(policy)) ok=fail(error,EINVAL,"The profile run policy is invalid.");
            else { char *text=g_strdup_printf("%s\nsave\n",policy); ok=write_profile(profile,"run.txt",text,error); g_free(text); }
         }
         g_free(policy);
      }
   }
   if (ok)
   {
      char *line=g_strconcat(profile->number,"\n",NULL); ok=atomic_at(parent,"last_session.txt",line,error); g_free(line);
      /* Boot compares whole seconds and FAT timestamps round down to two-second units. */
      time_t latest=time(NULL);
      for (guint i=0;i<profiles->len;i++)
      {
         Profile *other=g_ptr_array_index(profiles,i); int fd=changes_fd(other); struct stat marker;
         if (fd>=0 && !fstatat(fd,"last_session.txt",&marker,AT_SYMLINK_NOFOLLOW)) latest=MAX(latest,marker.st_mtime);
         if (fd>=0) close(fd);
      }
      struct timespec times[2]={{.tv_nsec=UTIME_OMIT},{.tv_sec=latest+2}};
      if (ok && (utimensat(parent,"last_session.txt",times,AT_SYMLINK_NOFOLLOW) || fsync(parent))) ok=fail(error,errno,g_strerror(errno));
   }
   if (parent>=0) close(parent);
   if (ok && previous_selection)
   {
      const char *old_intent=string(previous_selection,"intentName");
      if (previous_profile!=profile) ok=restore_profile_file(previous_profile,"restore.txt",string(previous_selection,"restore"),error);
      if (ok && old_intent && (previous_profile!=profile || g_strcmp0(old_intent,intent))) ok=restore_profile_file(previous_profile,old_intent,string(previous_selection,"intent"),error);
      if (ok && previous_profile->drive!=profile->drive)
      {
         JsonObject *old_last=json_object_get_object_member(previous_selection,"last"); int old_parent=changes_fd(previous_profile); const char *old_text=string(old_last,"text");
         ok=old_text ? atomic_at(old_parent,"last_session.txt",old_text,error) : remove_at(old_parent,"last_session.txt",error);
         if (old_parent>=0) close(old_parent);
      }
      /* Cancel of B returns to the original baseline, while the provisional journal retained A for rollback. */
      if (ok && previous_profile->drive==profile->drive) json_object_set_member(selection,"last",json_node_copy(json_object_get_member(previous_selection,"last")));
      if (ok && previous_profile==profile) json_object_set_member(selection,"restore",json_node_copy(json_object_get_member(previous_selection,"restore")));
      if (ok && previous_profile==profile && !g_strcmp0(old_intent,intent)) json_object_set_member(selection,"intent",json_node_copy(json_object_get_member(previous_selection,"intent")));
   }
   if (ok) { json_object_remove_member(selection,"previous"); ok=write_json(RUNTIME,"selection.json",selection,error); }
   if (catalog) json_object_unref(catalog);
   if (pending) json_object_unref(pending);
   g_free(intent);
   if (!ok && started && restore_selection(TRUE,NULL) && previous_selection)
   {
      selection=json_object_ref(previous_selection); write_json(RUNTIME,"selection.json",selection,NULL);
      if (previous_profile->store && previous_id) pin(previous_profile,previous_id,NULL);
   }
   if (ok && previous_profile && previous_profile->store && previous_id && !boolean(previous_selection,"wasProtected") && (previous_profile!=profile || g_strcmp0(previous_id,id))) snapshot_command(previous_profile,"unprotect",previous_id,NULL);
   g_free(previous_id);
   if (previous_selection) json_object_unref(previous_selection);
   return ok;
}

/* Mutations share one worker lock; requests never provide privileged filesystem paths. */
static JsonObject *dispatch(JsonObject *request, uid_t uid, GError **error)
{
   const char *op=string(request,"op");
   if (!op) { fail(error,EINVAL,"Missing operation."); return NULL; }
   if (!discover(boolean(request,"refresh"),error)) return NULL;
   if (!strcmp(op,"state")) return state(error);
   Profile *profile=find_profile(string(request,"profileId"));
   if (!strcmp(op,"history")) return history(profile,error);
   if (!strcmp(op,"profile.create")) return create_profile(find_drive(string(request,"driveId")),string(request,"name"),error);
   if (!strcmp(op,"next.clear")) return clear_next(error) ? json_object_new() : NULL;
   if (!strcmp(op,"run.save"))
   {
      for (guint i=0;i<profiles->len;i++) { Profile *p=g_ptr_array_index(profiles,i); if (p->active) { profile=p; break; } }
      JsonNode *save=json_object_get_member(request,"save");
      if (!save || json_node_get_value_type(save)!=G_TYPE_BOOLEAN) { fail(error,EINVAL,"Missing save setting."); return NULL; }
      return save_run(profile,boolean(request,"save"),error) ? json_object_new() : NULL;
   }
   if (!strcmp(op,"files.close"))
   {
      const char *id=string(request,"viewId");
      for (guint i=0;id && i<views->len;i++)
      { View *view=g_ptr_array_index(views,i); if (!strcmp(view->id,id) && (!uid || uid==view->uid)) return close_view(view,error) ? json_object_new() : NULL; }
      fail(error,ENOENT,"This file view is no longer open."); return NULL;
   }
   if (!validate(profile,FALSE,error)) return NULL;
   if (!strcmp(op,"profile.rename")) return rename_profile(profile,string(request,"name"),error) ? json_object_new() : NULL;
   if (!strcmp(op,"profile.delete")) return delete_profile(profile,error) ? json_object_new() : NULL;
   if (!strcmp(op,"files.open")) return open_files(profile,string(request,"pointId"),uid,error);
   if (!strcmp(op,"next.set")) return select_next(profile,string(request,"pointId"),boolean(request,"keepCurrent"),error) ? json_object_new() : NULL;
   if (!strcmp(op,"point.create") || !strcmp(op,"point.delete"))
   {
      gboolean create=!strcmp(op,"point.create");
      const char *name=string(request,"name"),*id=string(request,"pointId");
      if (create && !valid_name(name,TRUE,error)) return NULL;
      if (!validate(profile,TRUE,error) || !mount_store(profile,create,NULL,error)) return NULL;
      char *queued=profile_file(profile,"restore.txt");
      if (queued && !pin(profile,queued,error)) { g_free(queued); return NULL; }
      g_free(queued);
      JsonObject *catalog=history(profile,error);
      if (!catalog) return NULL;
      gboolean cancelled=FALSE;
      if (!create)
      {
         JsonObject *saved=point(catalog,id);
         if (!saved || boolean(saved,"protected")) { json_object_unref(catalog); fail(error,EBUSY,"This restore point is unavailable, protected or still in use."); return NULL; }
         if (!cancel_point(profile,id,&cancelled,error))
         { json_object_unref(catalog); if (cancelled) g_prefix_error(error,"The next-start selection was cancelled, but deletion could not continue: "); return NULL; }
         if (cancelled)
         {
            json_object_unref(catalog); catalog=history(profile,error); saved=point(catalog,id);
            if (!saved) { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The next-start selection was cancelled; the restore point is no longer available."); return NULL; }
         }
         name=string(saved,"name");
      }
      gboolean ok=snapshot_command(profile,create ? "create" : "delete",name,error); json_object_unref(catalog);
      if (!ok) { if (cancelled) g_prefix_error(error,"The next-start selection was cancelled, but the restore point could not be deleted: "); return NULL; }
      JsonObject *result=json_object_new();
      if (create)
      {
         catalog=report(profile,TRUE,error);
         if (!catalog) { json_object_unref(result); return NULL; }
         nullable(result,"pointId",string(catalog,"currentParent")); json_object_unref(catalog);
      }
      return result;
   }
   fail(error,EINVAL,"Unknown operation."); return NULL;
}

/* Error codes are stable UI categories; the message includes the concrete recovery context. */
static const char *error_code(GError *error)
{
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_BUSY)) return "busy";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_READ_ONLY)) return "readonly";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_PERMISSION_DENIED)) return "denied";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_NO_SPACE)) return "no_space";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND)) return "not_found";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT)) return "invalid";
   if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED)) return "unsupported";
   return "io";
}

/* Each connection has bounded framing and kernel-authenticated caller identity. */
static gboolean request_connection(GThreadedSocketService *service, GSocketConnection *connection, GObject *source_object, gpointer data)
{
   (void)service; (void)source_object; (void)data;
   GSocket *socket=g_socket_connection_get_socket(connection); struct ucred peer; socklen_t length=sizeof(peer);
   if (getsockopt(g_socket_get_fd(socket),SOL_SOCKET,SO_PEERCRED,&peer,&length) || (peer.uid && peer.uid!=guest_uid)) return TRUE;
   g_socket_set_timeout(socket,15);
   GInputStream *input=g_io_stream_get_input_stream(G_IO_STREAM(connection)); GString *line=g_string_new(NULL); GError *error=NULL;
   char byte; gssize count;
   while (line->len<65536 && (count=g_input_stream_read(input,&byte,1,NULL,&error))==1 && byte!='\n') g_string_append_c(line,byte);
   JsonObject *request=NULL,*result=NULL,*response=json_object_new();
   if (!error && line->len<65536) request=parse(line->str,&error);
   else if (!error) fail(&error,EFBIG,"The request is too large.");
   g_string_free(line,TRUE);
   if (request)
   {
      g_mutex_lock(&operation_lock); result=dispatch(request,peer.uid,&error); g_mutex_unlock(&operation_lock); json_object_unref(request);
   }
   json_object_set_boolean_member(response,"ok",result!=NULL);
   if (result) json_object_set_object_member(response,"result",result);
   else
   {
      JsonObject *failure=json_object_new(); json_object_set_string_member(failure,"code",error_code(error)); json_object_set_string_member(failure,"message",error ? error->message : "The storage operation failed."); json_object_set_object_member(response,"error",failure);
   }
   JsonNode *root=json_node_new(JSON_NODE_OBJECT); json_node_take_object(root,response); char *serialized=json_to_string(root,FALSE),*reply=g_strconcat(serialized,"\n",NULL);
   g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),reply,strlen(reply),NULL,NULL,NULL);
   g_free(reply); g_free(serialized); json_node_free(root); g_clear_error(&error); return TRUE;
}

/* Stop accepting commands before the ordinary mount cleanup. */
static gboolean stop(gpointer data)
{
   (void)data; g_main_loop_quit(main_loop); return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
   if (argc!=1 || geteuid()) { fprintf(stderr,"Usage: slax-statusd (run as root)\n"); return 1; }
   (void)argv;
   struct passwd *guest=getpwnam("guest"); if (guest) guest_uid=guest->pw_uid;
   umask(022); g_mkdir_with_parents(RUNTIME,0755); g_mkdir_with_parents(RUNTIME "/stores",0700); g_mkdir_with_parents(RUNTIME "/disks",0700); g_mkdir_with_parents(RUNTIME "/views",0755);
   int lock=open(RUNTIME "/lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
   if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { fprintf(stderr,"Slax Status is already running.\n"); return 1; }
   drives=g_ptr_array_new(); profiles=g_ptr_array_new(); views=g_ptr_array_new();
   GSocketService *service=g_threaded_socket_service_new(4); GSocketAddress *address=g_unix_socket_address_new(RUNTIME "/socket"); GError *error=NULL;
   unlink(RUNTIME "/socket");
   if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),address,G_SOCKET_TYPE_STREAM,G_SOCKET_PROTOCOL_DEFAULT,NULL,NULL,&error))
   { fprintf(stderr,"%s\n",error->message); return 1; }
   chmod(RUNTIME "/socket",0666); g_object_unref(address);
   g_signal_connect(service,"run",G_CALLBACK(request_connection),NULL); g_socket_service_start(service);
   main_loop=g_main_loop_new(NULL,FALSE); g_unix_signal_add(SIGTERM,stop,NULL); g_unix_signal_add(SIGINT,stop,NULL); g_main_loop_run(main_loop);
   g_socket_service_stop(service); g_mutex_lock(&operation_lock);
   for (guint i=views->len;i>0;i--) close_view(g_ptr_array_index(views,i-1),NULL);
   for (guint i=0;i<profiles->len;i++) close_store(g_ptr_array_index(profiles,i),NULL);
   for (guint i=0;i<drives->len;i++)
   { Drive *drive=g_ptr_array_index(drives,i); if (drive->owned) { const char *argv[]={"/bin/umount",drive->path,NULL}; g_free(command(argv,NULL)); } }
   unlink(RUNTIME "/socket"); close(lock); return 0;
}
