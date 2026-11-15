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
#define BOOT "/run/initramfs/memory/storage"
#define DYN "/run/initramfs/bin/@mount.dynfilefs"
#define LIMIT (8*1024*1024)

typedef struct { char *id,*device,*path,*label,*fstype,*uuid; dev_t number,filesystem; gboolean writable; guint64 free,total; } Drive;
typedef struct { char *id,*path,*mount,*inner; Drive *drive; ino_t inode; gboolean active; guint64 reusable; gint64 stats_at; } Store;
typedef struct { char *id,*store,*point,*path,*image; uid_t uid; pid_t pid; guint64 started; gboolean overlay; } View;
static GPtrArray *drives,*stores,*views;
static JsonArray *block_devices;
static JsonObject *selection;
static GMutex operation_lock;
static GMainLoop *main_loop;
static uid_t guest_uid=(uid_t)-1;
static gint64 scanned;
static char *active_path,*run_start,*run_error;
static gboolean recovered;
static gboolean pin(Store *store, const char *id, GError **error);
static void recover_mounts(void);
static gboolean clear_next(GError **error);
static gboolean restore_selection(gboolean release, GError **error);
static gboolean select_next(Store *store, const char *id, GError **error);
static gboolean save_run(Store *store, gboolean save, GError **error);

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

/* Snapshot IDs stay exact across JSON, filenames and FUSE commands. */
static gboolean identity(const char *value)
{
   if (!value || strlen(value)!=16) return FALSE;
   for (const char *p=value;*p;p++) if (!g_ascii_isdigit(*p) && (*p<'a' || *p>'f')) return FALSE;
   return TRUE;
}

/* UTF-8 labels remain international; controls and line delimiters cannot enter metadata. */
static gboolean valid_name(const char *name, GError **error)
{
   if (!name || strlen(name)>250 || !g_utf8_validate(name,-1,NULL)) return fail(error,EINVAL,"Use a name of at most 250 UTF-8 bytes.");
   for (const char *p=name;*p;p=g_utf8_next_char(p))
      if (g_unichar_iscntrl(g_utf8_get_char(p))) return fail(error,EINVAL,"Names cannot contain control characters.");
   gboolean reserved=g_str_has_prefix(name,"auto-") || g_str_has_prefix(name,"live-start-");
   if (!*name || reserved || strpbrk(name,"<>:\"/\\|?*") || name[strlen(name)-1]=='.' || name[strlen(name)-1]==' ')
      return fail(error,EINVAL,"Choose a restore point name without reserved prefixes, path punctuation or a final dot or space.");
   return TRUE;
}

/* Device, inode and filesystem identity prevent stale handles from targeting replacement media. */
static char *handle(const char *device, const char *uuid, const char *path, ino_t inode)
{
   char *key=g_strdup_printf("%s\n%s\n%s\n%"G_GUINT64_FORMAT,device,uuid ? uuid : "",path ? path : "",(guint64)inode);
   char *sum=g_compute_checksum_for_string(G_CHECKSUM_SHA256,key,-1); g_free(key);
   sum[24]=0; return sum;
}

/* Prefer the filesystem root over bind-mounted store subdirectories of the same disk. */
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

/* Track physical capacity separately from allocator space reusable only by a particular store. */
static void capacity(Drive *drive)
{
   struct statvfs st;
   if (statvfs(drive->path,&st)) { drive->writable=FALSE; drive->free=drive->total=0; return; }
   drive->free=(guint64)st.f_bavail*st.f_frsize;
   drive->total=(guint64)st.f_blocks*st.f_frsize;
   drive->writable=!(st.f_flag&ST_RDONLY);
}

/* Read the initramfs-owned active-store facts after pivot_root. */
static void boot_state(void)
{
   g_free(active_path); g_free(run_start);
   active_path=read_file(BOOT "/storage",NULL); if (active_path) g_strchomp(active_path);
   run_start=read_file(BOOT "/start",NULL); if (run_start) g_strchomp(run_start);
}

/* Retain live store objects across refreshes, including their mounts and protected descriptors. */
static Store *find_store(const char *id)
{
   if (!id) return NULL;
   for (guint i=0;i<stores->len;i++) { Store *p=g_ptr_array_index(stores,i); if (!strcmp(p->id,id)) return p; }
   return NULL;
}

/* Walk every path component below the mounted drive without following user-controlled symlinks. */
static int store_fd(Store *store)
{
   size_t prefix=strlen(store->drive->path);
   if (strncmp(store->path,store->drive->path,prefix) || (prefix>1 && store->path[prefix]!='/')) { errno=ESTALE; return -1; }
   int fd=open(store->drive->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
   char **parts=g_strsplit(store->path+prefix,"/",-1);
   for (char **part=parts;fd>=0 && *part;part++)
   {
      if (!**part) continue;
      int child=openat(fd,*part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); close(fd); fd=child;
   }
   g_strfreev(parts); return fd;
}

/* All store metadata updates remain relative to the validated directory descriptor. */
static gboolean write_store(Store *store, const char *name, const char *text, GError **error)
{
   int fd=store_fd(store); gboolean ok=text ? atomic_at(fd,name,text,error) : remove_at(fd,name,error);
   if (fd>=0) close(fd);
   return ok;
}

/* Rollback does not allocate metadata again when an atomic replacement never changed it. */
static gboolean restore_store_file(Store *store, const char *name, const char *text, GError **error)
{
   int fd=store_fd(store); char *current=fd>=0 ? read_fd(openat(fd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   if (fd>=0) close(fd);
   gboolean same=!g_strcmp0(current,text); g_free(current);
   return same || write_store(store,name,text,error);
}

/* Revalidate every mutation against the registered directory instead of trusting a stale GUI row. */
static gboolean validate(Store *store, gboolean write, GError **error)
{
   if (!store || !store->active) return fail(error,ENOENT,"The running changes storage is unavailable.");
   struct stat st,device,backing;
   int fd=store_fd(store); gboolean valid=fd>=0 && !fstat(fd,&st) && st.st_ino==store->inode;
   if (valid) valid=!fstatat(fd,"changes.dat",&backing,AT_SYMLINK_NOFOLLOW) && S_ISREG(backing.st_mode);
   if (fd>=0) close(fd);
   if (!valid || stat(store->drive->device,&device) || !S_ISBLK(device.st_mode) || device.st_rdev!=store->drive->number || st.st_dev!=store->drive->filesystem)
      return fail(error,ESTALE,"The store drive changed or was disconnected.");
   if (write && store->drive->uuid && *store->drive->uuid)
   {
      const char *argv[]={"/sbin/blkid","-p","-s","UUID","-o","value",store->drive->device,NULL};
      char *uuid=command(argv,error);
      if (!uuid) return FALSE;
      gboolean same=!strcmp(g_strchomp(uuid),store->drive->uuid); g_free(uuid);
      if (!same) return fail(error,ESTALE,"The store drive was replaced.");
   }
   capacity(store->drive);
   if (write && !store->drive->writable) return fail(error,EROFS,"The store drive is read-only.");
   return TRUE;
}

/* Only the direct changes.dat store is managed; legacy subdirectories remain untouched. */
static void scan_stores_at(Drive *drive, const char *changes)
{
   Store parent={.drive=drive,.path=(char *)changes};
   int folder=store_fd(&parent); struct stat st,file;
   if (folder<0) return;
   gboolean valid=!fstat(folder,&st) && !fstatat(folder,"changes.dat",&file,AT_SYMLINK_NOFOLLOW) && S_ISREG(file.st_mode);
   close(folder); if (!valid) return;
   char *id=handle(drive->device,drive->uuid,changes+strlen(drive->path),st.st_ino);
   Store *store=find_store(id);
   if (!store)
   {
      store=g_new0(Store,1); store->id=id; store->path=g_strdup(changes); store->drive=drive; store->inode=st.st_ino;
      g_ptr_array_add(stores,store);
   }
   else g_free(id);
   struct stat active;
   store->active=active_path && !stat(active_path,&active) && active.st_dev==st.st_dev && active.st_ino==st.st_ino;
   if (store->active && !store->mount)
   {
      store->mount=read_file(BOOT "/dynfilefs",NULL); if (store->mount) g_strchomp(store->mount);
      store->inner=read_file(BOOT "/changes",NULL); if (store->inner) g_strchomp(store->inner);
   }
}

/* Translate the boot path through mount roots, including aliases and perchdir bind mounts. */
static char *active_store_path(Drive *drive)
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

/* Locate only the already-mounted backing disk of this run; never mount discovery targets. */
static void scan_devices(JsonArray *array)
{
   struct stat active;
   if (!active_path || stat(active_path,&active)) return;
   for (guint i=0;i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i);
      JsonArray *children=json_object_has_member(item,"children") ? json_object_get_array_member(item,"children") : NULL;
      if (children) scan_devices(children);
      const char *device=string(item,"name"),*fs=string(item,"fstype");
      if (!device || !fs || !*fs) continue;
      const char *partition=string(item,"parttype");
      if (partition && (!g_ascii_strcasecmp(partition,"c12a7328-f81f-11d2-ba4b-00a0c93ec93b") || !g_ascii_strcasecmp(partition,"0xef"))) continue;
      struct stat node,root;
      if (stat(device,&node) || !S_ISBLK(node.st_mode)) continue;
      char *path=mounted(node.st_rdev);
      gboolean current=path && !stat(path,&root) && root.st_dev==active.st_dev;
      if (!current) { g_free(path); continue; }
      Drive *drive=NULL;
      for (guint j=0;j<drives->len;j++) { Drive *d=g_ptr_array_index(drives,j); if (d->number==node.st_rdev) { drive=d; break; } }
      if (!drive)
      {
         drive=g_new0(Drive,1); drive->device=g_strdup(device); drive->number=node.st_rdev;
         drive->uuid=g_strdup(string(item,"uuid")); drive->fstype=g_strdup(fs); drive->id=handle(device,drive->uuid,NULL,0);
         const char *label=string(item,"label"); drive->label=g_strdup(label ? label : "");
         drive->path=path; path=NULL; g_ptr_array_add(drives,drive);
      }
      g_free(path); drive->filesystem=active.st_dev; capacity(drive);
      char *changes=active_store_path(drive);
      if (changes) { scan_stores_at(drive,changes); g_free(changes); }
   }
}

/* Refresh discovery on demand; ordinary tray polling only reads inexpensive live counters. */
static gboolean discover(gboolean force, GError **error)
{
   boot_state();
   if (!force && scanned && g_get_monotonic_time()-scanned<15*G_USEC_PER_SEC) return TRUE;
   const char *argv[]={"/bin/lsblk","--json","--bytes","--paths","--output","NAME,TYPE,FSTYPE,LABEL,UUID,PARTTYPE",NULL};
   char *output=command(argv,error);
   if (!output) return FALSE;
   JsonObject *data=parse(output,error); g_free(output);
   if (!data) return FALSE;
   if (json_object_has_member(data,"blockdevices"))
   {
      g_clear_pointer(&block_devices,json_array_unref); block_devices=json_array_ref(json_object_get_array_member(data,"blockdevices"));
      for (guint i=0;i<stores->len;i++) ((Store *)g_ptr_array_index(stores,i))->active=FALSE;
      scan_devices(block_devices);
   }
   json_object_unref(data); scanned=g_get_monotonic_time();
   if (!recovered) { recover_mounts(); recovered=TRUE; }
   return TRUE;
}

/* Store reports use the live FUSE interface whenever that store is already mounted. */
static JsonObject *report(Store *store, gboolean history, GError **error)
{
   char *output=NULL;
   if (store->mount && *store->mount)
   {
      char *path=g_build_filename(store->mount,history ? "snapshots.json" : "stats",NULL);
      output=read_file(path,error); g_free(path);
   }
   else
   {
      int fd=store_fd(store); struct stat file;
      if (fd<0 || fstatat(fd,"changes.dat",&file,AT_SYMLINK_NOFOLLOW) || !S_ISREG(file.st_mode))
      { if (fd>=0) close(fd); fail(error,EINVAL,"The storage metadata is not a regular file."); return NULL; }
      const char *argv[]={DYN,"-f","/proc/self/fd/198/changes.dat",history ? "--snapshots" : "--info","--json",NULL};
      output=command_at(argv,fd,error); close(fd);
   }
   if (!output) return NULL;
   JsonObject *object=parse(output,error); g_free(output); return object;
}

/* Runtime metadata identifies guarded/queued points without trusting their display names. */
static char *store_file(Store *store, const char *name)
{
   int fd=store_fd(store);
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
static gboolean snapshot_command(Store *store, const char *operation, const char *name, GError **error)
{
   char *path=g_build_filename(store->mount,"snapshot",NULL),*text=g_strdup_printf("%s %s\n",operation,name);
   int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW); ssize_t length=strlen(text);
   gboolean ok=fd>=0 && write(fd,text,length)==length;
   if (!ok) fail(error,errno,g_strerror(errno));
   if (fd>=0) close(fd);
   g_free(path); g_free(text); return ok;
}

/* The active container and its inner filesystem are owned and mounted by early boot. */
static gboolean active_mount(Store *store, gboolean write, GError **error)
{
   if (!validate(store,write,error)) return FALSE;
   if (!store->mount || !store->inner || !mountpoint(store->mount) || !mountpoint(store->inner))
      return fail(error,ENOENT,"The running changes filesystem is no longer mounted.");
   return TRUE;
}

/* Report protected identities and read-only views without inventing DynFileFS catalog fields. */
static JsonObject *history(Store *store, GError **error)
{
   if (!validate(store,FALSE,error)) return NULL;
   JsonObject *catalog=report(store,TRUE,error);
   if (!catalog) return NULL;
   char *run=store_file(store,"run.txt"),*queued=store_file(store,"restore.txt");
   json_object_set_boolean_member(catalog,"hasStarted",run!=NULL);
   if (run) { char *line=strchr(run,'\n'); if (line) *line=0; }
   JsonArray *array=json_object_get_array_member(catalog,"snapshots"); const char *unformatted=NULL;
   for (guint i=0;i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i); const char *id=string(item,"id"),*name=string(item,"name"); gboolean open=FALSE;
      for (guint j=0;j<views->len;j++) { View *view=g_ptr_array_index(views,j); if (!strcmp(view->store,store->id) && !g_strcmp0(view->point,id)) open=TRUE; }
      char *marker=identity(id) ? g_strconcat("guard.",id,NULL) : NULL,*owner=marker ? store_file(store,marker) : NULL;
      gboolean guard=owner && !g_strcmp0(owner,name); g_free(owner); g_free(marker);
      if (guard && !string(item,"parent")) unformatted=id;
      json_object_set_boolean_member(item,"filesOpen",open);
      json_object_set_boolean_member(item,"hidden",guard);
      gboolean queued_point=!g_strcmp0(queued,id);
      gboolean other_pin=boolean(item,"protected") && !queued_point;
      if (queued_point && selection && !g_strcmp0(string(selection,"storeId"),store->id) && !g_strcmp0(string(selection,"pointId"),id)) other_pin=boolean(selection,"wasProtected");
      json_object_set_boolean_member(item,"protected",other_pin || guard || open || !g_strcmp0(run,id));
   }
   /* The initial point directly follows our recorded unformatted root, not an arbitrary name. */
   for (guint i=0;unformatted && i<json_array_get_length(array);i++)
   {
      JsonObject *item=json_array_get_object_element(array,i);
      gboolean initial=!g_strcmp0(string(item,"parent"),unformatted) && !g_strcmp0(string(item,"name"),"Initial state");
      if (initial) { json_object_set_boolean_member(item,"initial",TRUE); json_object_set_boolean_member(item,"protected",TRUE); }
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
   Store *active=NULL; gboolean save=FALSE,automatic=FALSE;
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
   for (guint i=0;i<stores->len;i++)
   {
      Store *store=g_ptr_array_index(stores,i);
      if (!validate(store,FALSE,NULL)) continue;
      GError *local=NULL;
      if (store->active)
      {
         active=store;
         char *policy=store_file(store,"run.txt"),*keep=g_strdup_printf("%s\nsave",run_start ? run_start : ""),*discard=g_strdup_printf("%s\ndiscard",run_start ? run_start : "");
         save=!g_strcmp0(policy,keep); gboolean valid=save || !g_strcmp0(policy,discard);
         g_free(policy); g_free(keep); g_free(discard);
         if (!valid) fail(&local,EIO,"The current run save policy could not be read.");
      }
      if (store->mount || !store->stats_at || g_get_monotonic_time()-store->stats_at>30*G_USEC_PER_SEC)
      {
         JsonObject *stats=local ? NULL : report(store,FALSE,&local);
         if (stats)
         {
            if (json_object_has_member(stats,"reusableBytes")) store->reusable=json_object_get_int_member(stats,"reusableBytes");
            if (store->active)
            {
               JsonNode *mode=json_object_get_member(stats,"automaticSnapshots");
               gboolean supported=mode && JSON_NODE_HOLDS_VALUE(mode) && json_node_get_value_type(mode)==G_TYPE_BOOLEAN;
               if (!supported) fail(&local,ENOTSUP,"The changes storage cannot control automatic restore points.");
               else
               {
                  automatic=json_node_get_boolean(mode);
                  /* A restarted broker reconciles the engine with the durable policy before reporting it. */
                  if (automatic!=save && save_run(store,save,&local)) automatic=save;
               }
            }
            json_object_unref(stats); if (store->active && !local) g_clear_pointer(&run_error,g_free);
         }
         if (local && store->active) { g_free(run_error); run_error=g_strdup(local->message); }
         g_clear_error(&local); store->stats_at=g_get_monotonic_time();
      }
      JsonObject *object=json_object_new();
      json_object_set_string_member(object,"id",store->id); json_object_set_string_member(object,"label",store->drive->label);
      json_object_set_string_member(object,"device",store->drive->device); json_object_set_string_member(object,"driveId",store->drive->id);
      json_object_set_boolean_member(object,"active",store->active); json_object_set_boolean_member(object,"writable",store->drive->writable);
      json_object_set_int_member(object,"freeBytes",store->drive->free); json_object_set_int_member(object,"totalBytes",store->drive->total); json_object_set_int_member(object,"reusableBytes",store->reusable);
      json_array_add_object_element(ps,object);
   }
   char *where=source(); json_object_set_string_member(run,"source",where); g_free(where);
   nullable(run,"storeId",active ? active->id : NULL); json_object_set_string_member(run,"kind",active ? "container" : "ram");
   char *storage=active ? device_text(active->drive->device,active->drive->label) : g_strdup("RAM");
   json_object_set_string_member(run,"storage",storage); g_free(storage); nullable(run,"startId",run_start);
   json_object_set_boolean_member(run,"save",save);
   gboolean writable=active && active->drive->writable && identity(run_start);
   json_object_set_boolean_member(run,"canSetSave",writable && !run_error);
   json_object_set_string_member(run,"error",run_error ? run_error : "");
   guint64 free_bytes=0,total_bytes=0;
   if (active) { free_bytes=active->drive->free; total_bytes=active->drive->total; }
   else
   {
      char *memory=NULL; if (g_file_get_contents("/proc/meminfo",&memory,NULL,NULL))
      { char **lines=g_strsplit(memory,"\n",-1); for (char **line=lines;*line;line++) { if (g_str_has_prefix(*line,"MemAvailable:")) free_bytes=g_ascii_strtoull(*line+13,NULL,10)*1024; if (g_str_has_prefix(*line,"MemTotal:")) total_bytes=g_ascii_strtoull(*line+9,NULL,10)*1024; } g_strfreev(lines); g_free(memory); }
   }
   json_object_set_int_member(run,"freeBytes",free_bytes); json_object_set_int_member(run,"totalBytes",total_bytes); json_object_set_int_member(run,"reusableBytes",active ? active->reusable : 0);
   Store *next=selection ? find_store(string(selection,"storeId")) : NULL;
   if (next)
   {
      JsonObject *queued=json_object_new(); json_object_set_string_member(queued,"storeId",next->id); nullable(queued,"pointId",string(selection,"pointId")); char *label=device_text(next->drive->device,next->drive->label); json_object_set_string_member(queued,"label",label); g_free(label);
      json_object_set_string_member(queued,"device",next->drive->device);
      /* Resolve names by stable identity so a queued restore follows later renames. */
      JsonObject *catalog=report(next,TRUE,NULL),*saved=point(catalog,string(selection,"pointId"));
      const char *name=NULL;
      if (saved) name=boolean(saved,"automatic") ? "Automatic restore point" : string(saved,"name");
      nullable(queued,"pointName",name);
      if (catalog) json_object_unref(catalog);
      json_object_set_int_member(queued,"created",json_object_has_member(selection,"created") ? json_object_get_int_member(selection,"created") : 0);
      json_object_set_object_member(result,"next",queued);
   }
   else json_object_set_null_member(result,"next");
   for (guint i=0;i<views->len;i++)
   {
      View *view=g_ptr_array_index(views,i); JsonObject *object=json_object_new();
      json_object_set_string_member(object,"id",view->id); json_object_set_string_member(object,"storeId",view->store); json_object_set_string_member(object,"pointId",view->point); json_object_set_string_member(object,"path",view->path); json_array_add_object_element(vs,object);
   }
   json_object_set_array_member(result,"drives",ds); json_object_set_array_member(result,"stores",ps); json_object_set_array_member(result,"views",vs); json_object_set_object_member(result,"run",run);
   return result;
}

/* Engine protection survives the broker and remains active through clean DynFileFS unmount. */
static gboolean pin(Store *store, const char *id, GError **error)
{
   return identity(id) ? snapshot_command(store,"protect",id,error) : fail(error,EINVAL,"Invalid restore point identity.");
}

/* Process start time survives exec and distinguishes a browser from a reused PID. */
static guint64 process_start(pid_t pid)
{
   if (pid<=0) return 0;
   char *path=g_strdup_printf("/proc/%d/stat",pid),*text=NULL; g_file_get_contents(path,&text,NULL,NULL); g_free(path);
   if (!text) return 0;
   char *end=strrchr(text,')'); char **fields=end ? g_strsplit(end+2," ",21) : NULL;
   guint64 started=0;
   if (fields && g_strv_length(fields)>19 && fields[0][0]!='Z' && fields[0][0]!='X') started=g_ascii_strtoull(fields[19],NULL,10);
   g_strfreev(fields); g_free(text); return started;
}

/* Publish the authenticated browser lifetime before mounting, including crash recovery. */
static gboolean save_view(View *view, GError **error)
{
   JsonObject *record=json_object_new(); json_object_set_string_member(record,"storeId",view->store); json_object_set_string_member(record,"pointId",view->point); json_object_set_int_member(record,"uid",view->uid);
   json_object_set_int_member(record,"pid",view->pid); json_object_set_int_member(record,"started",view->started);
   char *base=g_path_get_dirname(view->path); gboolean ok=write_json(base,"view.json",record,error);
   g_free(base); json_object_unref(record); return ok;
}

/* Mount immutable snapshots without replay, then overlay their upper layer over the current base modules. */
static JsonObject *open_files(Store *store, const char *id, uid_t uid, pid_t pid, GError **error)
{
   if (!identity(id)) { fail(error,EINVAL,"Invalid restore point identity."); return NULL; }
   guint64 started=process_start(pid);
   if (!started) { fail(error,ESRCH,"The file browser is no longer running."); return NULL; }
   JsonObject *catalog=history(store,error),*saved=point(catalog,id);
   if (!saved) { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The restore point is no longer available."); return NULL; }
   if (!active_mount(store,FALSE,error)) { json_object_unref(catalog); return NULL; }
   View *view=g_new0(View,1); view->id=g_uuid_string_random(); view->uid=uid; view->pid=pid; view->started=started; view->store=g_strdup(store->id); view->point=g_strdup(id);
   char *base=g_build_filename(RUNTIME,"views",view->id,NULL); view->image=g_build_filename(base,"image",NULL); view->path=g_build_filename(base,"files",NULL);
   g_mkdir_with_parents(view->image,0755); g_mkdir_with_parents(view->path,0755);
   gboolean mounted_image=FALSE;
   if (!save_view(view,error)) { json_object_unref(catalog); goto failed; }
   char *filename=g_strconcat(id,".dat",NULL),*image=g_build_filename(store->mount,"snapshots",filename,NULL);
   const char *mount[]={"/bin/mount","-t","xfs","-o","loop,ro,nouuid,norecovery,nosuid,nodev,noexec",image,view->image,NULL};
   char *out=command(mount,error); mounted_image=out!=NULL; g_free(out); g_free(image); g_free(filename); json_object_unref(catalog);

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
   g_ptr_array_add(views,view); g_free(base);
   JsonObject *result=json_object_new(); json_object_set_string_member(result,"viewId",view->id); json_object_set_string_member(result,"path",view->path); return result;
failed:

   if (mounted_image) { const char *unmount[]={"/bin/umount",view->image,NULL}; g_free(command(unmount,NULL)); }
   if (mountpoint(view->image))
   { g_ptr_array_add(views,view); g_free(base); return NULL; }
   remove_file(base,"view.json",NULL); rmdir(view->path); rmdir(view->image); rmdir(base); g_free(base);
   g_free(view->id); g_free(view->store); g_free(view->point); g_free(view->path); g_free(view->image); g_free(view);
   return NULL;
}

/* Ordinary unmounts preserve busy views until their remaining users release them. */
static gboolean close_view(View *view, GError **error)
{
   char *out;
   if (view->overlay && mountpoint(view->path))
   {
      const char *root[]={"/bin/umount",view->path,NULL}; out=command(root,error);
      if (!out) return FALSE;
      g_free(out);
   }
   view->overlay=FALSE;
   if (mountpoint(view->image))
   {
      const char *image[]={"/bin/umount",view->image,NULL}; out=command(image,error);
      if (!out) return FALSE;
      g_free(out);
   }
   rmdir(view->path); rmdir(view->image); char *base=g_path_get_dirname(view->path); remove_file(base,"view.json",NULL); rmdir(base); g_free(base);
   g_ptr_array_remove(views,view);
   g_free(view->id); g_free(view->store); g_free(view->point); g_free(view->path); g_free(view->image); g_free(view); return TRUE;
}

/* Retry busy mounts automatically; neither GUI lifetime nor PID reuse can close a live browser. */
static gboolean expire_views(gpointer data)
{
   (void)data;
   if (!g_mutex_trylock(&operation_lock)) return G_SOURCE_CONTINUE;
   for (guint i=views->len;i>0;i--)
   {
      View *view=g_ptr_array_index(views,i-1);
      if (process_start(view->pid)!=view->started) close_view(view,NULL);
   }
   g_mutex_unlock(&operation_lock); return G_SOURCE_CONTINUE;
}

/* Reattach only daemon-owned mount locations and root-owned view records after an unexpected restart. */
static void recover_mounts(void)
{
   char *text=read_file(RUNTIME "/selection.json",NULL);
   if (text) { selection=parse(text,NULL); g_free(text); }
   GDir *dir=g_dir_open(RUNTIME "/views",0,NULL); const char *name;
   while (dir && (name=g_dir_read_name(dir)))
   {
      if (!g_uuid_string_is_valid(name)) continue;
      char *base=g_build_filename(RUNTIME,"views",name,NULL),*record=g_build_filename(base,"view.json",NULL);
      text=read_file(record,NULL); JsonObject *data=text ? parse(text,NULL) : NULL; g_free(text); g_free(record);
      const char *store_id=string(data,"storeId"),*point_id=string(data,"pointId");
      gboolean owner=data && json_object_has_member(data,"uid");
      if (owner) owner=json_object_has_member(data,"pid") && json_object_has_member(data,"started");
      if (owner) owner=json_object_get_int_member(data,"pid")>0 && json_object_get_int_member(data,"started")>0;
      if (!find_store(store_id) || !identity(point_id) || !owner) { if (data) json_object_unref(data); g_free(base); continue; }
      View *view=g_new0(View,1); view->id=g_strdup(name); view->store=g_strdup(store_id); view->point=g_strdup(point_id); view->uid=json_object_get_int_member(data,"uid");
      view->pid=json_object_get_int_member(data,"pid"); view->started=json_object_get_int_member(data,"started");
      view->path=g_build_filename(base,"files",NULL); view->image=g_build_filename(base,"image",NULL); view->overlay=mountpoint(view->path);
      if (mountpoint(view->image)) g_ptr_array_add(views,view);
      else
      {
         remove_file(base,"view.json",NULL); rmdir(view->path); rmdir(view->image); rmdir(base);
         g_free(view->id); g_free(view->store); g_free(view->point); g_free(view->path); g_free(view->image); g_free(view);
      }
      json_object_unref(data); g_free(base);
   }
   if (dir) g_dir_close(dir);
   while (selection && json_object_has_member(selection,"previous"))
   {
      JsonNode *node=json_object_get_member(selection,"previous");
      JsonObject *previous=JSON_NODE_HOLDS_OBJECT(node) ? json_object_ref(json_node_get_object(node)) : NULL;
      if (!restore_selection(TRUE,NULL)) { if (previous) json_object_unref(previous); break; }
      if (previous) { selection=previous; write_json(RUNTIME,"selection.json",selection,NULL); }
   }
}

/* Disable engine snapshots before discard; enable them only after durable save policy. */
static gboolean save_run(Store *store, gboolean save, GError **error)
{
   if (!active_mount(store,TRUE,error)) return FALSE;
   if (!store->active || !identity(run_start)) return fail(error,ENOTSUP,"This run cannot change its save setting.");
   store->stats_at=0;
   if (!save && !snapshot_command(store,"autosnapshot","off",error)) return FALSE;
   char *text=g_strdup_printf("%s\n%s\n",run_start,save ? "save" : "discard");
   gboolean ok=write_store(store,"run.txt",text,error); g_free(text);
   if (ok && save) ok=snapshot_command(store,"autosnapshot","on",error);
   /* A failed fsync may follow a successful rename: never guess that enabling is safe. */
   if (!ok) snapshot_command(store,"autosnapshot","off",NULL);
   return ok;
}

/* Cancel only this daemon's pending restore, preserving the independent run save policy. */
static gboolean restore_selection(gboolean release, GError **error)
{
   if (!selection) return TRUE;
   Store *store=find_store(string(selection,"storeId"));
   if (!validate(store,TRUE,error)) return FALSE;
   const char *previous=string(selection,"restore"),*selected=string(selection,"pointId");
   char *previous_id=previous ? g_strdup(previous) : NULL; if (previous_id) g_strchomp(previous_id);
   if (store->mount && previous_id && !pin(store,previous_id,error)) { g_free(previous_id); return FALSE; }
   if (!restore_store_file(store,"restore.txt",string(selection,"restore"),error)) { g_free(previous_id); return FALSE; }
   if (release && selected && store->mount && g_strcmp0(selected,previous_id) && !boolean(selection,"wasProtected")) snapshot_command(store,"unprotect",selected,NULL);
   g_free(previous_id);
   if (!remove_file(RUNTIME,"selection.json",error)) return FALSE;
   json_object_unref(selection); selection=NULL; return TRUE;
}

/* Explicit cancel releases the superseded engine pin only after restoring durable policy. */
static gboolean clear_next(GError **error)
{
   return restore_selection(TRUE,error);
}

/* Deleting a queued point cancels only that exact request, never another store's next start. */
static gboolean cancel_point(Store *store, const char *id, gboolean *cancelled, GError **error)
{
   char *queued=store_file(store,"restore.txt"); gboolean match=!g_strcmp0(queued,id); g_free(queued);
   if (!match) return TRUE;
   gboolean own=selection && !g_strcmp0(string(selection,"storeId"),store->id) && !g_strcmp0(string(selection,"pointId"),id);
   gboolean ok=own ? clear_next(error) : write_store(store,"restore.txt",NULL,error);
   queued=store_file(store,"restore.txt"); gboolean remains=!g_strcmp0(queued,id); g_free(queued);
   if (ok && remains) ok=write_store(store,"restore.txt",NULL,error);
   queued=store_file(store,"restore.txt"); *cancelled=g_strcmp0(queued,id)!=0; g_free(queued);
   if (ok && (!own || remains))
   {
      if (ok && store->mount) ok=snapshot_command(store,"unprotect",id,error);
   }
   return ok;
}

/* Journal replacement of the active store's restore request so Cancel survives daemon restarts. */
static gboolean select_next(Store *store, const char *id, GError **error)
{
   if (!validate(store,TRUE,error)) return FALSE;
   JsonObject *catalog=NULL,*saved=NULL;
   if (id)
   {
      if (!identity(id)) return fail(error,EINVAL,"Invalid restore point identity.");
      catalog=history(store,error); saved=point(catalog,id);
      if (!saved)
      { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The restore point is no longer available."); return FALSE; }
   }
   if (selection && g_strcmp0(string(selection,"storeId"),store->id))
   { if (catalog) json_object_unref(catalog); return fail(error,ESTALE,"The pending restore belongs to different storage."); }
   JsonObject *previous=selection ? json_object_ref(selection) : NULL;
   char *previous_id=previous ? g_strdup(string(previous,"pointId")) : NULL;
   gboolean was_protected=boolean(saved,"protected");
   if (previous && !g_strcmp0(previous_id,id)) was_protected=boolean(previous,"wasProtected");
   if (id && !pin(store,id,error)) { if (catalog) json_object_unref(catalog); if (previous) json_object_unref(previous); g_free(previous_id); return FALSE; }
   JsonObject *pending=json_object_new();
   if (previous) json_object_set_object_member(pending,"previous",json_object_ref(previous));
   else json_object_set_null_member(pending,"previous");
   json_object_set_string_member(pending,"storeId",store->id); nullable(pending,"pointId",id);
   json_object_set_boolean_member(pending,"wasProtected",was_protected);
   if (saved)
   {
      json_object_set_int_member(pending,"created",json_object_get_int_member(saved,"created"));
   }
   int fd=store_fd(store);
   char *old=fd>=0 ? read_fd(openat(fd,"restore.txt",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW),NULL) : NULL;
   if (fd>=0) close(fd);
   nullable(pending,"restore",old); g_free(old);
   gboolean ok=write_json(RUNTIME,"selection.json",pending,error),started=ok;
   if (ok) { if (selection) json_object_unref(selection); selection=pending; pending=NULL; }
   /* Selecting latest clears only an explicit restore; discard still returns to this run's start. */
   char *line=id ? g_strconcat(id,"\n",NULL) : NULL;
   if (ok) ok=write_store(store,"restore.txt",line,error);
   g_free(line);
   if (ok)
   {
      /* Keep the provisional rollback record intact until the completed journal is durable. */
      JsonObject *complete=json_object_new(); GList *members=json_object_get_members(selection);
      for (GList *member=members;member;member=member->next) json_object_set_member(complete,member->data,json_node_copy(json_object_get_member(selection,member->data)));
      g_list_free(members);
      if (previous) json_object_set_member(complete,"restore",json_node_copy(json_object_get_member(previous,"restore")));
      json_object_remove_member(complete,"previous"); ok=write_json(RUNTIME,"selection.json",complete,error);
      if (ok) { json_object_unref(selection); selection=complete; } else json_object_unref(complete);
   }
   if (catalog) json_object_unref(catalog);
   if (pending) json_object_unref(pending);
   if (!ok && started && restore_selection(TRUE,NULL) && previous)
   { selection=json_object_ref(previous); write_json(RUNTIME,"selection.json",selection,NULL); if (previous_id) pin(store,previous_id,NULL); }
   if (!ok && !started && id && !was_protected) snapshot_command(store,"unprotect",id,NULL);
   if (ok && previous_id && g_strcmp0(previous_id,id) && !boolean(previous,"wasProtected")) snapshot_command(store,"unprotect",previous_id,NULL);
   g_free(previous_id); if (previous) json_object_unref(previous); return ok;
}

/* Mutations share one worker lock; requests never provide privileged filesystem paths. */
static JsonObject *dispatch(JsonObject *request, uid_t uid, pid_t pid, GError **error)
{
   const char *op=string(request,"op");
   if (!op) { fail(error,EINVAL,"Missing operation."); return NULL; }
   if (!discover(boolean(request,"refresh"),error)) return NULL;
   if (!strcmp(op,"state")) return state(error);
   Store *store=find_store(string(request,"storeId"));
   if (!strcmp(op,"history")) return history(store,error);
   if (!strcmp(op,"next.clear")) return clear_next(error) ? json_object_new() : NULL;
   if (!strcmp(op,"run.save"))
   {
      for (guint i=0;i<stores->len;i++) { Store *p=g_ptr_array_index(stores,i); if (p->active) { store=p; break; } }
      JsonNode *save=json_object_get_member(request,"save");
      if (!save || json_node_get_value_type(save)!=G_TYPE_BOOLEAN) { fail(error,EINVAL,"Missing save setting."); return NULL; }
      return save_run(store,boolean(request,"save"),error) ? json_object_new() : NULL;
   }
   if (!validate(store,FALSE,error)) return NULL;
   if (!strcmp(op,"files.open")) return open_files(store,string(request,"pointId"),uid,pid,error);
   if (!strcmp(op,"next.set")) return select_next(store,string(request,"pointId"),error) ? json_object_new() : NULL;
   if (!strcmp(op,"point.rename"))
   {
      const char *name=string(request,"name"),*id=string(request,"pointId");
      if (!valid_name(name,error) || !active_mount(store,TRUE,error)) return NULL;
      JsonObject *catalog=history(store,error);
      if (!catalog) return NULL;
      JsonObject *saved=point(catalog,id);
      gboolean editable=saved && !boolean(saved,"initial") && !boolean(saved,"hidden");
      if (!editable) { json_object_unref(catalog); fail(error,EPERM,"This restore point cannot be renamed."); return NULL; }
      char *argument=g_strdup_printf("%s %s",id,name);
      gboolean ok=snapshot_command(store,"rename",argument,error); g_free(argument); json_object_unref(catalog);
      return ok ? json_object_new() : NULL;
   }
   if (!strcmp(op,"point.create") || !strcmp(op,"point.delete"))
   {
      gboolean create=!strcmp(op,"point.create");
      const char *name=string(request,"name"),*id=string(request,"pointId");
      if (create && !valid_name(name,error)) return NULL;
      if (!active_mount(store,TRUE,error)) return NULL;
      char *queued=store_file(store,"restore.txt");
      if (queued && !pin(store,queued,error)) { g_free(queued); return NULL; }
      g_free(queued);
      JsonObject *catalog=history(store,error);
      if (!catalog) return NULL;
      gboolean cancelled=FALSE;
      if (!create)
      {
         JsonObject *saved=point(catalog,id);
         gboolean unavailable=!saved || boolean(saved,"protected") || boolean(saved,"filesOpen");
         if (unavailable) { json_object_unref(catalog); fail(error,EBUSY,"This restore point is unavailable, protected or still in use."); return NULL; }
         if (!cancel_point(store,id,&cancelled,error))
         { json_object_unref(catalog); if (cancelled) g_prefix_error(error,"The next-start selection was cancelled, but deletion could not continue: "); return NULL; }
         if (cancelled)
         {
            json_object_unref(catalog); catalog=history(store,error); saved=point(catalog,id);
            if (!saved) { if (catalog) json_object_unref(catalog); if (!error || !*error) fail(error,ENOENT,"The next-start selection was cancelled; the restore point is no longer available."); return NULL; }
         }
         name=string(saved,"name");
      }
      gboolean ok=snapshot_command(store,create ? "create" : "delete",name,error); json_object_unref(catalog);
      if (!ok) { if (cancelled) g_prefix_error(error,"The next-start selection was cancelled, but the restore point could not be deleted: "); return NULL; }
      JsonObject *result=json_object_new();
      if (create)
      {
         catalog=report(store,TRUE,error);
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
      g_mutex_lock(&operation_lock); result=dispatch(request,peer.uid,peer.pid,&error); g_mutex_unlock(&operation_lock); json_object_unref(request);
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
   umask(022); g_mkdir_with_parents(RUNTIME,0755); g_mkdir_with_parents(RUNTIME "/views",0755);
   int lock=open(RUNTIME "/lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
   if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { fprintf(stderr,"Slax Status is already running.\n"); return 1; }
   drives=g_ptr_array_new(); stores=g_ptr_array_new(); views=g_ptr_array_new();
   GSocketService *service=g_threaded_socket_service_new(4); GSocketAddress *address=g_unix_socket_address_new(RUNTIME "/socket"); GError *error=NULL;
   unlink(RUNTIME "/socket");
   if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),address,G_SOCKET_TYPE_STREAM,G_SOCKET_PROTOCOL_DEFAULT,NULL,NULL,&error))
   { fprintf(stderr,"%s\n",error->message); return 1; }
   chmod(RUNTIME "/socket",0666); g_object_unref(address);
   g_signal_connect(service,"run",G_CALLBACK(request_connection),NULL); g_socket_service_start(service);
   main_loop=g_main_loop_new(NULL,FALSE); g_timeout_add_seconds(1,expire_views,NULL);
   g_unix_signal_add(SIGTERM,stop,NULL); g_unix_signal_add(SIGINT,stop,NULL); g_main_loop_run(main_loop);
   g_socket_service_stop(service); g_mutex_lock(&operation_lock);
   for (guint i=views->len;i>0;i--)
   {
      View *view=g_ptr_array_index(views,i-1);
      if (process_start(view->pid)!=view->started) close_view(view,NULL);
   }
   unlink(RUNTIME "/socket"); close(lock); return 0;
}
