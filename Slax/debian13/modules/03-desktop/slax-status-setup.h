/* Select saved-changes storage through the same persistent resume marker as early boot. */

/* Reopen a mounted filesystem root and reject replacement media before privileged writes. */
static int setup_drive_fd(Drive *drive, GError **error)
{
   if (!drive) { fail(error,ENOENT,"The selected drive is no longer available. Refresh the drive list."); return -1; }
   char *path=mounted(drive->number); struct stat root,node;
   gboolean same=!g_strcmp0(path,drive->path); g_free(path);
   int fd=same ? open(drive->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW) : -1;
   same=fd>=0 && !fstat(fd,&root) && root.st_dev==drive->filesystem && root.st_ino==drive->inode;
   if (same) same=!stat(drive->device,&node) && S_ISBLK(node.st_mode) && node.st_rdev==drive->number;
   if (!same) { if (fd>=0) close(fd); fail(error,ESTALE,"The selected drive changed or was disconnected."); return -1; }
   capacity(drive);
   const char *reason=setup_reason(drive);
   if (*reason) { close(fd); fail(error,ENOTSUP,reason); return -1; }
   const char *argv[]={"/sbin/blkid","-p","-s","UUID","-o","value",drive->device,NULL};
   char *uuid=command(argv,error);
   same=uuid && drive->uuid && !strcmp(g_strchomp(uuid),drive->uuid); g_free(uuid);
   if (!same) { close(fd); if (!error || !*error) fail(error,ESTALE,"The selected filesystem was replaced."); return -1; }
   return fd;
}

/* Open only canonical directories below the pinned drive; never follow links supplied by media. */
static int setup_directory(int root, gboolean create, GError **error)
{
   const char *names[]={"slax","changes"}; int fd=dup(root); struct stat volume;
   if (fstat(root,&volume)) { if (fd>=0) close(fd); fail(error,errno,g_strerror(errno)); return -1; }
   for (guint i=0;i<G_N_ELEMENTS(names);i++)
   {
      if (create && mkdirat(fd,names[i],0755) && errno!=EEXIST)
      { fail(error,errno,g_strerror(errno)); close(fd); return -1; }
      int child=openat(fd,names[i],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); struct stat st;
      gboolean same=child>=0 && !fstat(child,&st) && st.st_dev==volume.st_dev;
      if (!same || (create && fsync(fd)))
      { fail(error,errno ? errno : EXDEV,"The slax/changes path is unavailable, linked or on another filesystem."); if (child>=0) close(child); close(fd); return -1; }
      close(fd); fd=child;
   }
   return fd;
}

/* Resolve opaque drive handles only against the daemon's mounted-volume inventory. */
static Drive *setup_drive(const char *id)
{
   for (guint i=0;id && i<drives->len;i++)
   { Drive *drive=g_ptr_array_index(drives,i); if (!strcmp(drive->id,id)) return drive; }
   return NULL;
}

/* Ignore links like early boot, but reject timestamps that cannot be advanced safely. */
static gboolean resume_time(int folder, gint64 *now, GError **error)
{
   struct stat marker;
   if (fstatat(folder,"resume.txt",&marker,AT_SYMLINK_NOFOLLOW) || !S_ISREG(marker.st_mode)) return TRUE;
   if (marker.st_mtim.tv_sec>G_MAXINT64-4) return fail(error,EINVAL,"A saved-changes selection has an invalid timestamp.");
   *now=MAX(*now,marker.st_mtim.tv_sec+2); return TRUE;
}

/* Prefer the selected drive permanently; boot assumes all persistence media are already available. */
static gboolean mark_resume(int folder, GError **error)
{
   if (folder<0) return fail(error,errno,g_strerror(errno));
   gint64 now=g_get_real_time()/G_USEC_PER_SEC;
   for (guint i=0;i<drives->len;i++)
   {
      Drive *drive=g_ptr_array_index(drives,i); struct stat root;
      if (!drive->present) continue;
      int base=open(drive->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
      gboolean same=base>=0 && !fstat(base,&root) && root.st_dev==drive->filesystem && root.st_ino==drive->inode;
      int previous=same ? setup_directory(base,FALSE,NULL) : -1;
      if (base>=0) close(base);
      if (previous>=0)
      {
         gboolean ok=resume_time(previous,&now,error); close(previous); if (!ok) return FALSE;
      }
   }
   /* Explicit custom changes paths also keep their own timestamp; round up for FAT's two-second resolution. */
   if (!resume_time(folder,&now,error)) return FALSE;
   char *time=g_strdup_printf("%"G_GINT64_FORMAT,(now+1)&~(gint64)1);
   const char *argv[]={"/run/initramfs/persistence","mark","/proc/self/fd/198",time,NULL};
   char *output=command_at(argv,folder,error); gboolean ok=output!=NULL; g_free(output); g_free(time); return ok;
}

/* Create the canonical directory and make it the next automatic resume target without restoring a state. */
static gboolean prepare_setup(JsonObject *request, GError **error)
{
   char *kind=read_file(BOOT "/kind",error);
   if (!kind) return FALSE;
   gboolean ram=!strcmp(g_strchomp(kind),"ram"); g_free(kind);
   if (!ram || (active_path && *active_path)) return fail(error,EPERM,"Drive setup is available only while changes are in RAM.");
   Drive *drive=setup_drive(string(request,"driveId"));
   int root=setup_drive_fd(drive,error); if (root<0) return FALSE;
   int folder=setup_directory(root,TRUE,error);
   gboolean ok=folder>=0;
   if (ok && fsync(folder)) ok=fail(error,errno,g_strerror(errno));
   if (ok && fsync(root)) ok=fail(error,errno,g_strerror(errno));
   if (ok) ok=mark_resume(folder,error);
   if (ok)
   {
      const char *argv[]={"/run/initramfs/persistence","enable","/proc/self/fd/198",NULL};
      char *output=command_at(argv,folder,error); ok=output!=NULL; g_free(output);
   }
   if (folder>=0) close(folder);
   close(root); return ok;
}
