/* Offline checks share one implementation for boot recovery and file-browser requests. */
#define UNCLEAN BOOT "/unclean"

/* Do not mistake no-op fsck wrappers or partial NTFS fixes for a completed check. */
static const char *repair_tool(const char *filesystem)
{
   if (!g_strcmp0(filesystem,"vfat")) return "/sbin/fsck.fat";
   if (!g_strcmp0(filesystem,"exfat")) return "/sbin/fsck.exfat";
   if (!g_strcmp0(filesystem,"ext2") || !g_strcmp0(filesystem,"ext3") || !g_strcmp0(filesystem,"ext4")) return "/sbin/e2fsck";
   if (!g_strcmp0(filesystem,"xfs")) return "/sbin/xfs_repair";
   if (!g_strcmp0(filesystem,"btrfs")) return "/usr/bin/btrfs";
   return NULL;
}

/* Keep repair availability and Windows guidance identical for both entry points. */
static const char *repair_error(const char *filesystem, const char *uuid)
{
   if (!uuid || !*uuid) return "Slax could not identify the filesystem safely. Check the drive from another system.";
   if (!g_strcmp0(filesystem,"ntfs") || !g_strcmp0(filesystem,"ntfs3")) return "Connect this drive to Windows and use Windows drive error checking or chkdsk before mounting it.";
   if (!repair_tool(filesystem)) return "Automatic repair is not available for this filesystem.";
   return "";
}

/* The boot flag remains authoritative across broker and tray restarts. */
static JsonObject *repair_state(void)
{
   char *device=read_file(UNCLEAN "/device",NULL);
   if (!device) return NULL;
   JsonObject *result=json_object_new(); g_strchomp(device);
   char *filesystem=read_file(UNCLEAN "/filesystem",NULL); if (filesystem) g_strchomp(filesystem);
   char *uuid=read_file(UNCLEAN "/uuid",NULL);
   const char *reason=repair_error(filesystem,uuid ? g_strchomp(uuid) : NULL);
   json_object_set_string_member(result,"device",device);
   json_object_set_string_member(result,"filesystem",filesystem ? filesystem : "");
   char *issue=read_file(UNCLEAN "/issue",NULL);
   json_object_set_string_member(result,"issue",issue ? g_strchomp(issue) : "interrupted"); g_free(issue);
   const char *label=device_value(block_devices,device,"label");
   json_object_set_string_member(result,"label",label ? label : "");
   json_object_set_string_member(result,"error",reason); json_object_set_boolean_member(result,"canRepair",!*reason);
   g_free(uuid); g_free(filesystem); g_free(device); return result;
}

/* Resolve only current blocked catalogue identities; clients cannot supply privileged paths. */
static JsonObject *repair_place(const char *id, GError **error)
{
   const char *argv[]={"/usr/libexec/slax-places","--list",NULL};
   char *output=command(argv,error);
   if (!output) return NULL;
   GVariant *places=g_variant_parse(G_VARIANT_TYPE("a(sssssssss)"),output,NULL,NULL,error),*place;
   g_free(output);
   if (!places) return NULL;
   GVariantIter iter;g_variant_iter_init(&iter,places);
   JsonObject *result=NULL;
   while ((place=g_variant_iter_next_value(&iter)))
   {
      const char *current,*device,*filesystem,*mount,*reason;
      g_variant_get_child(place,0,"&s",&current);g_variant_get_child(place,4,"&s",&filesystem);
      g_variant_get_child(place,5,"&s",&device);g_variant_get_child(place,6,"&s",&mount);g_variant_get_child(place,8,"&s",&reason);
      gboolean match=g_str_has_prefix(current,"block:") && !g_strcmp0(current,id);
      if (match && !*mount && !strcmp(reason,"Needs filesystem check"))
      {
         result=repair_state();
         if (result && g_strcmp0(string(result,"device"),device)) g_clear_pointer(&result,json_object_unref);
         if (!result)
         {
            result=json_object_new();
            const char *uuid=device_value(block_devices,device,"uuid");
            const char *detail=repair_error(filesystem,uuid);
            json_object_set_string_member(result,"device",device);json_object_set_string_member(result,"filesystem",filesystem);
            json_object_set_string_member(result,"uuid",uuid ? uuid : "");
            json_object_set_string_member(result,"error",detail);json_object_set_boolean_member(result,"canRepair",!*detail);
         }
         json_object_set_string_member(result,"placeId",id);
      }
      g_variant_unref(place);
      if (match) break;
   }
   g_variant_unref(places);
   if (!result) fail(error,ESTALE,"This filesystem changed, is mounted or no longer needs checking. Select its current entry and try again.");
   return result;
}

/* Keep diagnostics in RAM and use the checker's numeric status, never translated output. */
static int repair_check(const char *tool, const char *option, int device, int log, GError **error)
{
   gboolean btrfs=!strcmp(tool,"/usr/bin/btrfs");
   const char *argv[9]={"/usr/bin/stdbuf","-o0","-e0",tool,option,"/proc/self/fd/198",NULL};
   if (btrfs) { argv[4]="check"; argv[5]=option; argv[6]="--progress"; argv[7]="/proc/self/fd/198"; }
   dprintf(log,"\n$ %s %s%s /proc/self/fd/198\n",tool,btrfs ? "check " : "",option);
   GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
   g_subprocess_launcher_take_fd(launcher,dup(device),198);
   g_subprocess_launcher_take_stdout_fd(launcher,dup(log)); g_subprocess_launcher_take_stderr_fd(launcher,dup(log));
   GSubprocess *process=g_subprocess_launcher_spawnv(launcher,argv,error); g_object_unref(launcher);
   if (!process) return -1;
   gboolean ok=g_subprocess_wait(process,NULL,error);
   int code=ok && g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : -1;
   if (ok && code<0) fail(error,EIO,"The filesystem checker was interrupted. The drive remains blocked.");
   dprintf(log,"\nChecker exit status: %d\n",code); g_object_unref(process); return code;
}

/* Reopen only the boot-recorded relative directory beneath the verified filesystem root. */
static int repair_directory(const char *mount, const char *relative, GError **error)
{
   int fd=open(mount,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
   char **parts=g_strsplit(relative,"/",-1);
   for (char **part=parts;fd>=0 && *part;part++)
   {
      if (!**part || !strcmp(*part,".") || !strcmp(*part,"..")) { close(fd); fd=-1; break; }
      int child=openat(fd,*part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); close(fd); fd=child;
   }
   g_strfreev(parts);
   if (fd<0) fail(error,ESTALE,"The saved-changes directory is missing or linked. The drive remains blocked.");
   return fd;
}

/* Authenticate the pinned block node and its filesystem before every repair phase. */
static gboolean repair_identity(int fd, const char *device, const char *uuid, const char *filesystem, GError **error)
{
   struct stat pinned,current;
   gboolean same=!fstat(fd,&pinned) && !stat(device,&current) && S_ISBLK(current.st_mode);
   if (same) same=pinned.st_rdev==current.st_rdev && pinned.st_ino==current.st_ino && pinned.st_dev==current.st_dev;
   if (!same) return fail(error,ESTALE,"The selected drive changed or was disconnected.");
   const char *argv[]={"/sbin/blkid","-p","-o","export","/proc/self/fd/198",NULL};
   char *output=command_at(argv,fd,error);
   if (!output) return FALSE;
   char **lines=g_strsplit(output,"\n",-1); gboolean same_uuid=FALSE,same_type=FALSE;
   for (char **line=lines;*line;line++)
   {
      if (g_str_has_prefix(*line,"UUID=")) same_uuid=*uuid && !strcmp(*line+5,uuid);
      if (g_str_has_prefix(*line,"TYPE=")) same_type=!strcmp(*line+5,filesystem);
   }
   g_strfreev(lines); g_free(output);
   return same_uuid && same_type ? TRUE : fail(error,ESTALE,"The selected filesystem was replaced or cannot be identified.");
}

/* Hold the parent disk's standard lock, shared by udev and Slax's automounter. */
static int repair_lock(dev_t number, GArray *locks, GError **error)
{
   char *sys=g_strdup_printf("/sys/dev/block/%u:%u",major(number),minor(number)),*resolved=realpath(sys,NULL); g_free(sys);
   if (!resolved) { fail(error,errno,g_strerror(errno)); return -1; }
   char *partition=g_build_filename(resolved,"partition",NULL);
   char *parent=g_file_test(partition,G_FILE_TEST_EXISTS) ? g_path_get_dirname(resolved) : g_strdup(resolved);
   char *name=g_path_get_basename(parent),*path=g_build_filename("/dev",name,NULL);
   int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
   g_free(resolved); g_free(partition); g_free(parent); g_free(name); g_free(path);
   struct stat current,held;
   for (guint i=0;fd>=0 && i<locks->len;i++)
      if (!fstat(fd,&current) && !fstat(g_array_index(locks,int,i),&held) && current.st_rdev==held.st_rdev)
      { close(fd); return dup(g_array_index(locks,int,i)); }
   if (fd<0 || flock(fd,LOCK_EX|LOCK_NB))
   { if (fd>=0) close(fd); fail(error,EBUSY,"Another disk operation is using this drive. Try again after it finishes."); return -1; }
   return fd;
}

/* Unmount nested subvolumes before their parents, including every Btrfs member. */
static char *repair_mount(GArray *devices)
{
   char *text=NULL,*result=NULL;
   if (!g_file_get_contents("/proc/self/mountinfo",&text,NULL,NULL)) return NULL;
   char **lines=g_strsplit(text,"\n",-1);
   for (char **line=lines;*line;line++)
   {
      char *separator=strstr(*line," - "); if (!separator) continue;
      char **fields=g_strsplit(*line," ",-1),**tail=g_strsplit(separator+3," ",4);
      char *source=g_strcompress(tail[1]),*path=g_strcompress(fields[4]); struct stat node,member; struct statx visible;
      unsigned major_number=0,minor_number=0; sscanf(fields[2],"%u:%u",&major_number,&minor_number);
      gboolean source_block=!stat(source,&node) && S_ISBLK(node.st_mode);
      gboolean top=!statx(AT_FDCWD,path,AT_NO_AUTOMOUNT,STATX_MNT_ID,&visible) && (visible.stx_mask&STATX_MNT_ID) && visible.stx_mnt_id==g_ascii_strtoull(fields[0],NULL,10);
      for (guint i=0;top && i<devices->len;i++)
         if (!fstat(g_array_index(devices,int,i),&member) && (member.st_rdev==makedev(major_number,minor_number) || (source_block && member.st_rdev==node.st_rdev)))
            if (!result || strlen(path)>strlen(result)) { g_free(result); result=g_strdup(path); }
      g_free(source); g_free(path); g_strfreev(fields); g_strfreev(tail);
   }
   g_strfreev(lines); g_free(text); return result;
}

/* Check every pinned member again; an unplugged RAID member must never be ignored. */
static gboolean repair_members(GArray *devices, char **paths, const char *uuid, const char *filesystem, GError **error)
{
   for (guint i=0;i<devices->len;i++)
      if (!repair_identity(g_array_index(devices,int,i),paths[i],uuid,filesystem,error)) return FALSE;
   return TRUE;
}

/* Mount normally, allowing the kernel to replay a recoverable log. */
static gboolean repair_attach(int fd, const char *filesystem)
{
   if (g_mkdir_with_parents(RUNTIME "/repair",0700)) return FALSE;
   char source[32];g_snprintf(source,sizeof(source),"/proc/self/fd/%d",fd);
   return mount(source,RUNTIME "/repair",filesystem,MS_NODEV|MS_NOSUID|MS_NOEXEC,NULL)==0;
}

/* Detect writeback errors before clearing the marker or accepting the mount. */
static gboolean repair_flush(GError **error)
{
   int root=open(RUNTIME "/repair",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
   struct statvfs status;
   gboolean ok=root>=0 && !fstatvfs(root,&status);
   if (ok) ok=!(status.f_flag&ST_RDONLY) && !syncfs(root);
   if (root>=0) close(root);
   return ok || fail(error,EIO,"The filesystem reported a writeback error. The drive remains blocked.");
}

/* Verify the native clean state of the pinned device after checking or closing it. */
static gboolean repair_inspect(int fd, const char *filesystem, GError **error)
{
   const char *argv[]={"/run/initramfs/persistence","inspect","/proc/self/fd/198",filesystem,NULL};
   char *output=command_at(argv,fd,error);gboolean ok=output!=NULL;g_free(output);return ok;
}

/* Repair only an inactive filesystem and keep it quarantined until the check succeeds. */
static JsonObject *repair_filesystem(const char *id, gboolean btrfs_repair, GError **error)
{
   JsonObject *recovery=id ? repair_place(id,error) : repair_state();
   if (!recovery) { if (!error || !*error) fail(error,ENOENT,"There is no recorded drive to check."); return NULL; }
   if (!boolean(recovery,"canRepair")) { fail(error,ENOTSUP,string(recovery,"error")); json_object_unref(recovery); return NULL; }
   const char *device=string(recovery,"device"),*filesystem=string(recovery,"filesystem"),*tool=repair_tool(filesystem);
   gboolean interrupted=json_object_has_member(recovery,"issue");
   char *uuid=interrupted ? read_file(UNCLEAN "/uuid",error) : g_strdup(string(recovery,"uuid"));
   char *relative=interrupted && uuid ? read_file(UNCLEAN "/path",error) : g_strdup("");
   if (uuid) g_strchomp(uuid);
   int fd=-1,log=-1,folder=-1; gboolean ok=uuid && relative,mounted_here=FALSE,cleared=FALSE,needs_repair=FALSE;
   gboolean btrfs=!strcmp(filesystem,"btrfs");
   GArray *devices=g_array_new(FALSE,FALSE,sizeof(int)),*locks=g_array_new(FALSE,FALSE,sizeof(int));
   char **paths=NULL,*listing=NULL,*log_path=NULL;
   log=g_mkstemp_full(log_path=g_strdup(RUNTIME "/fsck.XXXXXX"),O_WRONLY|O_CLOEXEC,0644);
   if (log>=0 && !rename(log_path,RUNTIME "/fsck.log")) dprintf(log,"Filesystem check: %s (%s)\n",device,filesystem);
   else { if (ok) fail(error,errno,g_strerror(errno)); ok=FALSE; }
   if (log_path) { unlink(log_path); g_free(log_path); }
   char *kind=read_file(BOOT "/kind",NULL);
   gboolean ram=kind && !strcmp(g_strchomp(kind),"ram");
   gboolean active_changes=!ram || (active_path && *active_path);
   if (ok && interrupted && active_changes) ok=fail(error,EBUSY,"Filesystem repair is available only while changes are in RAM.");
   g_free(kind);
   if (ok && btrfs)
   {
      char *match=g_strconcat("UUID=",uuid,NULL);
      const char *argv[]={"/sbin/blkid","-c","/dev/null","-t",match,"-o","device",NULL};
      listing=command(argv,error); g_free(match); ok=listing!=NULL;
   }
   if (ok) paths=g_strsplit(btrfs ? g_strchomp(listing) : device,"\n",-1);
   for (guint i=0;ok && paths[i];i++)
   {
      int member=open(paths[i],O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW); struct stat st;
      if (member<0 || fstat(member,&st) || !S_ISBLK(st.st_mode))
      { if (member>=0) close(member); ok=fail(error,ENODEV,"A filesystem device is unavailable."); break; }
      g_array_append_val(devices,member);
      int lock=repair_lock(st.st_rdev,locks,error); ok=lock>=0;
      if (ok) g_array_append_val(locks,lock);
      if (!strcmp(paths[i],device)) fd=member;
   }
   if (ok && fd<0) ok=fail(error,ESTALE,"The selected drive is missing from its filesystem.");
   if (ok && id)
   {
      /* A replacement device must not inherit an approval for the previous catalogue entry. */
      JsonObject *current=repair_place(id,error);ok=current!=NULL;
      g_clear_pointer(&current,json_object_unref);
   }
   if (ok) ok=repair_members(devices,paths,uuid,filesystem,error);
   if (ok && btrfs)
   {
      const char *argv[]={tool,"inspect-internal","dump-super","/proc/self/fd/198",NULL};
      char *output=command_at(argv,fd,error); guint64 total=0;
      ok=output!=NULL;
      char **lines=g_strsplit(output ? output : "","\n",-1);
      for (char **line=lines;*line;line++) if (g_str_has_prefix(*line,"num_devices")) total=g_ascii_strtoull(*line+11,NULL,10);
      g_strfreev(lines); g_free(output);
      if (ok && (!total || total!=devices->len)) ok=fail(error,ENODEV,"Btrfs members are missing or ambiguous. Connect every member before checking this filesystem.");
   }
   while (ok)
   {
      char *path=repair_mount(devices);
      if (!path) break;
      if (!interrupted) ok=fail(error,EBUSY,"The filesystem is mounted or in use. Unmount it before checking it. Start Slax from another drive if this is its running system or saved changes.");
      else if (umount(path)) ok=fail(error,EBUSY,"The drive is still in use. Close its files and terminals, then try again. If Slax is running from this drive, start Slax from RAM or another medium first.");
      g_free(path);
   }
   if (ok) ok=repair_members(devices,paths,uuid,filesystem,error);
   if (ok && btrfs)
   {
      int code=repair_check(tool,"--readonly",fd,log,error);
      if (code>0 && !btrfs_repair) { needs_repair=TRUE; json_object_set_int_member(recovery,"checkExitCode",code); }
      else if (code>0)
      {
         ok=repair_members(devices,paths,uuid,filesystem,error);
         if (ok) ok=repair_check(tool,"--repair",fd,log,error)==0;
         for (guint i=0;ok && i<devices->len;i++) if (fsync(g_array_index(devices,int,i))) ok=fail(error,errno,g_strerror(errno));
         if (ok) ok=repair_check(tool,"--readonly",fd,log,error)==0;
      }
      else ok=code==0;
   }
   if (ok && !btrfs)
   {
      gboolean ext=!strcmp(tool,"/sbin/e2fsck");
      gboolean xfs=!strcmp(filesystem,"xfs");
      const char *mode=ext ? "-fp" : xfs ? "-e" : "-p";
      int code=repair_check(tool,mode,fd,log,error);
      if (xfs && code==2)
      {
         dprintf(log,"\nReplaying the XFS log through a normal mount and unmount.\n");
         ok=repair_attach(fd,filesystem);
         if (ok)
         {
            ok=repair_flush(error);
            if (umount(RUNTIME "/repair")) ok=FALSE;
         }
         if (ok && repair_members(devices,paths,uuid,filesystem,error)) code=repair_check(tool,mode,fd,log,error);
      }
      gboolean completed=code==0;
      if (ext && code>0 && code<4) completed=TRUE;
      if (xfs && code==4) completed=TRUE;
      /* FAT/exFAT can report corrected errors as 1; the independent check must still return 0. */
      if ((!strcmp(filesystem,"vfat") || !strcmp(filesystem,"exfat")) && code==1) completed=TRUE;
      ok=ok && completed;
      if (ok && fsync(fd)) ok=fail(error,errno,g_strerror(errno));
      if (ok) ok=repair_check(tool,ext ? "-fn" : "-n",fd,log,error)==0;
      if (!ok && (!error || !*error)) fail(error,EIO,"The filesystem check did not finish cleanly. The filesystem remains blocked. See /run/slax-status/fsck.log for the checker report.");
   }
   if (!ok && (!error || !*error)) fail(error,EIO,"The filesystem check did not finish cleanly. The filesystem remains blocked. Review the checker report below.");
   if (ok) ok=repair_members(devices,paths,uuid,filesystem,error);
   if (ok && !needs_repair)
   {
      ok=repair_inspect(fd,filesystem,error);
      if (ok)
      {
         mounted_here=repair_attach(fd,filesystem);
         ok=mounted_here ? repair_flush(error) : fail(error,errno,"The checked filesystem could not be mounted. The drive remains blocked.");
      }
   }
   if (ok && !needs_repair && *relative)
   {
      folder=repair_directory(RUNTIME "/repair",relative,error); ok=folder>=0;
      if (ok)
      {
         const char *argv[]={"/run/initramfs/persistence","enable","/proc/self/fd/198",NULL};
         char *output=command_at(argv,folder,error); ok=output!=NULL; cleared=ok; g_free(output);
      }
   }
   if (folder>=0) close(folder);
   if (mounted_here && umount(RUNTIME "/repair"))
   {
      /* A failed final unmount must not make the next boot trust an unfinished repair. */
      if (cleared)
      {
         folder=repair_directory(RUNTIME "/repair",relative,NULL);
         const char *argv[]={"/run/initramfs/persistence","open","/proc/self/fd/198",NULL};
         char *output=folder>=0 ? command_at(argv,folder,NULL) : NULL; g_free(output); if (folder>=0) close(folder);
      }
      if (ok) ok=fail(error,EBUSY,"The repaired filesystem could not be closed. Restart only after resolving the storage error.");
   }
   if (ok && !needs_repair)
   {
      ok=repair_inspect(fd,filesystem,error);
      /* Keep automount from stealing the restart offer after the disk lock is released. */
      if (ok && interrupted && rename(UNCLEAN "/device",UNCLEAN "/checked-device")) ok=fail(error,errno,g_strerror(errno));
      scanned=0;
   }
   for (guint i=0;i<devices->len;i++) close(g_array_index(devices,int,i));
   for (guint i=0;i<locks->len;i++) close(g_array_index(locks,int,i));
   /* Udev can recheck the repaired members only after the disk locks are released. */
   if (ok && !needs_repair)
      for (guint i=0;paths[i];i++)
      {
         const char *argv[]={"/usr/bin/udevadm","trigger","--action=change","--settle",paths[i],NULL};
         g_free(command(argv,NULL));
      }
   if (log>=0)
   {
      const char *status="Final check passed. You can mount the drive in File Manager.";
      if (needs_repair) status="Read-only check failed. The filesystem remains blocked; no repair was attempted.";
      if (!ok) status=error && *error ? (*error)->message : "The check failed. The filesystem remains blocked.";
      dprintf(log,"\n%s\n",status); close(log);
   }
   g_array_unref(devices); g_array_unref(locks); g_strfreev(paths); g_free(listing); g_free(uuid); g_free(relative);
   if (!ok) { json_object_unref(recovery); return NULL; }
   return recovery;
}
