/* Native GTK sidebar rows consume the same unprivileged catalogue as PCManFM. */
typedef struct
{
  GWeakRef sidebar,row;
  GSubprocess *process;
  GAsyncResult *result;
  char *path;
  gboolean unmount,open;
  GtkPlacesOpenFlags flags;
} SlaxPlacesJob;

/* Match catalogue identities without replacing a row under the pointer. */
static GtkWidget *slax_places_find(GtkPlacesSidebar *sidebar,const char *id)
{
  GList *rows=gtk_container_get_children(GTK_CONTAINER(sidebar->list_box));
  GtkWidget *found=NULL;
  for (GList *item=rows;item;item=item->next)
    {
      GVariant *location=g_object_get_data(G_OBJECT(item->data),"slax-location");
      const char *current;
      if (!location) continue;
      g_variant_get_child(location,0,"&s",&current);
      if (!strcmp(current,id)) { found=item->data; break; }
    }
  g_list_free(rows);
  return found;
}

/* Native Home, root and catalogue mounts must not be duplicated as bookmarks. */
static gboolean slax_places_duplicate(GtkPlacesSidebar *sidebar,GFile *file)
{
  char *path=g_file_get_path(file);
  gboolean duplicate=path && (!strcmp(path,"/") || !strcmp(path,g_get_home_dir()));
  if (path && !duplicate && sidebar->slax_snapshot)
    {
      GVariantIter iter;
      GVariant *location;
      g_variant_iter_init(&iter,sidebar->slax_snapshot);
      while ((location=g_variant_iter_next_value(&iter)))
        {
          const char *mount,*target;
          g_variant_get_child(location,6,"&s",&mount);
          g_variant_get_child(location,7,"&s",&target);
          duplicate=(*mount && !strcmp(path,mount)) || (*target && !strcmp(path,target));
          g_variant_unref(location);
          if (duplicate) break;
        }
    }
  g_free(path);
  return duplicate;
}

/* Keep native rows alive during ordinary GTK bookmark and volume refreshes. */
static void slax_places_clear(GtkWidget *row,gpointer data)
{
  if (!g_object_get_data(G_OBJECT(row),"slax-location")) gtk_widget_destroy(row);
}

/* Reconcile only presentation metadata; discovery and all mount policy remain in Slax helpers. */
static void slax_places_reconcile(GtkPlacesSidebar *sidebar)
{
  if (!sidebar->slax_snapshot) return;
  GHashTable *seen=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
  GVariantIter iter;
  GVariant *location;
  int index=0;
  g_variant_iter_init(&iter,sidebar->slax_snapshot);
  while ((location=g_variant_iter_next_value(&iter)))
    {
      const char *id,*name,*group,*icon,*path;
      g_variant_get_child(location,0,"&s",&id);
      g_variant_get_child(location,1,"&s",&name);
      g_variant_get_child(location,2,"&s",&group);
      g_variant_get_child(location,3,"&s",&icon);
      g_variant_get_child(location,6,"&s",&path);
      GtkPlacesSidebarSectionType section=SECTION_MOUNTS;
      if (!strcmp(group,"Shared folders")) section=SECTION_SLAX_SHARED;
      if (!strcmp(group,"Disk images")) section=SECTION_SLAX_IMAGES;
      GIcon *image=g_themed_icon_new(icon);
      GFile *file=*path?g_file_new_for_path(path):NULL;
      char *uri=file?g_file_get_uri(file):NULL;
      GtkWidget *row=slax_places_find(sidebar,id);
      if (!row) row=add_place(sidebar,PLACES_SLAX_LOCATION,section,name,image,NULL,uri,NULL,NULL,NULL,NULL,index,NULL);
      g_object_set(row,"label",name,"start-icon",image,"uri",uri,"order-index",index++,NULL);
      gtk_sidebar_row_set_slax(GTK_SIDEBAR_ROW(row),location,g_object_get_data(G_OBJECT(row),"slax-operation"));
      gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row));
      g_hash_table_add(seen,g_strdup(id));
      g_object_unref(image);g_clear_object(&file);g_free(uri);g_variant_unref(location);
    }
  GList *rows=gtk_container_get_children(GTK_CONTAINER(sidebar->list_box));
  for (GList *item=rows;item;item=item->next)
    {
      GVariant *current=g_object_get_data(G_OBJECT(item->data),"slax-location");
      const char *id;
      if (!current) continue;
      g_variant_get_child(current,0,"&s",&id);
      if (!g_hash_table_contains(seen,id)) gtk_widget_destroy(item->data);
    }
  g_list_free(rows);g_hash_table_unref(seen);
}

/* Keep operation text readable while kernel/provider notifications settle. */
static gboolean slax_places_finished(gpointer data)
{
  SlaxPlacesJob *job=data;
  GtkPlacesSidebar *sidebar=g_weak_ref_get(&job->sidebar);
  GtkSidebarRow *row=g_weak_ref_get(&job->row);
  if (sidebar && !sidebar->slax_process) g_clear_object(&sidebar);
  char *output=NULL,*message=NULL;
  GError *error=NULL;
  gboolean success=g_subprocess_communicate_utf8_finish(job->process,job->result,&output,&message,&error);
  success=success && g_subprocess_get_successful(job->process);
  if (sidebar && success)
    {
      if (job->unmount && sidebar->current_location)
        {
          GFile *root=g_file_new_for_path(job->path);
          gboolean current=g_file_equal(sidebar->current_location,root) || g_file_has_prefix(sidebar->current_location,root);
          g_object_unref(root);
          if (current)
            {
              GFile *home=g_file_new_for_path(g_get_home_dir());
              emit_open_location(sidebar,home,GTK_PLACES_OPEN_NORMAL);
              g_object_unref(home);
            }
        }
      else if (job->open)
        {
          GVariant *path=g_variant_parse(G_VARIANT_TYPE_STRING,output,NULL,NULL,&error);
          if (path)
            {
              GFile *file=g_file_new_for_path(g_variant_get_string(path,NULL));
              emit_open_location(sidebar,file,job->flags);
              g_object_unref(file);g_variant_unref(path);
            }
          else success=FALSE;
        }
    }
  if (sidebar && row && gtk_widget_get_parent(GTK_WIDGET(row))==sidebar->list_box)
    {
      g_object_set_data(G_OBJECT(row),"slax-operation",NULL);
      gtk_sidebar_row_set_slax(row,g_object_get_data(G_OBJECT(row),"slax-location"),NULL);
    }
  if (sidebar)
    {
      if (sidebar->current_location)
        {
          GFile *current=g_object_ref(sidebar->current_location);
          gtk_places_sidebar_set_location(sidebar,current);
          g_object_unref(current);
        }
      if (!success) emit_show_error_message(sidebar,job->unmount?_("Unable to unmount the filesystem"):_("Unable to mount the filesystem"),error?error->message:message);
    }
  g_free(output);g_free(message);g_clear_error(&error);
  g_clear_object(&row);g_clear_object(&sidebar);
  g_weak_ref_clear(&job->row);g_weak_ref_clear(&job->sidebar);
  g_object_unref(job->process);g_object_unref(job->result);g_free(job->path);g_free(job);
  return G_SOURCE_REMOVE;
}

/* The helper runs asynchronously; closing a chooser never keeps its sidebar alive. */
static void slax_places_mount_done(GObject *process,GAsyncResult *result,gpointer data)
{
  SlaxPlacesJob *job=data;
  job->result=g_object_ref(result);
  g_timeout_add(500,slax_places_finished,job);
}

/* A row opens its location; its real child button mounts or unmounts without selecting it. */
static void slax_places_activate(GtkSidebarRow *row,GtkPlacesOpenFlags flags,gboolean action)
{
  GVariant *location=g_object_get_data(G_OBJECT(row),"slax-location");
  const char *id,*path,*reason;
  if (g_object_get_data(G_OBJECT(row),"slax-operation")) return;
  g_variant_get_child(location,0,"&s",&id);
  g_variant_get_child(location,6,"&s",&path);
  g_variant_get_child(location,8,"&s",&reason);
  if (action && *reason) return;
  GtkPlacesSidebar *sidebar;
  g_object_get(row,"sidebar",&sidebar,NULL);
  if (!action && *path)
    {
      GFile *file=g_file_new_for_path(path);
      emit_open_location(sidebar,file,flags);
      g_object_unref(file);g_object_unref(sidebar);
      return;
    }
  const char *operation=action && *path?"unmount":"mount";
  const char *argv[]={"/usr/bin/pkexec","/usr/libexec/slax-places",operation,id,NULL};
  GError *error=NULL;
  GSubprocess *process=g_subprocess_newv(argv+(geteuid()==0),G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE,&error);
  if (!process)
    {
      emit_show_error_message(sidebar,_("Unable to access the filesystem"),error->message);
      g_error_free(error);g_object_unref(sidebar);
      return;
    }
  SlaxPlacesJob *job=g_new0(SlaxPlacesJob,1);
  g_weak_ref_init(&job->sidebar,sidebar);g_weak_ref_init(&job->row,row);
  job->process=process;job->path=g_strdup(path);job->unmount=action && *path;job->open=!action;job->flags=flags;
  g_object_set_data(G_OBJECT(row),"slax-operation",(gpointer)operation);
  gtk_sidebar_row_set_slax(row,location,operation);
  g_subprocess_communicate_utf8_async(process,NULL,NULL,slax_places_mount_done,job);
  g_object_unref(sidebar);
}

static void slax_places_read(GtkPlacesSidebar *sidebar);

/* Parse complete immutable snapshots on GTK's thread, keeping probing outside the application. */
static void slax_places_received(GObject *stream,GAsyncResult *result,gpointer data)
{
  GWeakRef *weak=data;
  GtkPlacesSidebar *sidebar=g_weak_ref_get(weak);
  GError *error=NULL;
  char *line=g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(stream),result,NULL,&error);
  g_weak_ref_clear(weak);g_free(weak);
  if (sidebar && sidebar->slax_process && line)
    {
      GVariant *snapshot=g_variant_parse(G_VARIANT_TYPE("a(sssssssss)"),line,NULL,NULL,&error);
      if (snapshot)
        {
          if (!sidebar->slax_snapshot || !g_variant_equal(sidebar->slax_snapshot,snapshot))
            {
              g_clear_pointer(&sidebar->slax_snapshot,g_variant_unref);
              sidebar->slax_snapshot=g_variant_ref(snapshot);
              update_places(sidebar);
            }
          g_variant_unref(snapshot);
        }
      slax_places_read(sidebar);
    }
  if (error && !g_error_matches(error,G_IO_ERROR,G_IO_ERROR_CANCELLED)) g_warning("Cannot read filesystem locations: %s",error->message);
  g_clear_error(&error);g_free(line);g_clear_object(&sidebar);
}

/* Weak asynchronous ownership permits immediate watcher cleanup when the chooser closes. */
static void slax_places_read(GtkPlacesSidebar *sidebar)
{
  GWeakRef *weak=g_new0(GWeakRef,1);
  g_weak_ref_init(weak,sidebar);
  g_data_input_stream_read_line_async(sidebar->slax_stream,G_PRIORITY_DEFAULT,sidebar->slax_cancel,slax_places_received,weak);
}

/* Only file-chooser sidebars use the Slax catalogue; other GTK sidebar clients stay native. */
void gtk_places_sidebar_enable_slax(GtkPlacesSidebar *sidebar)
{
  GError *error=NULL;
  sidebar->slax_process=g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE,&error,"/usr/libexec/slax-places","--watch",NULL);
  if (!sidebar->slax_process)
    {
      g_warning("Cannot start filesystem discovery: %s",error->message);g_error_free(error);
      return;
    }
  sidebar->slax_cancel=g_cancellable_new();
  sidebar->slax_stream=g_data_input_stream_new(g_subprocess_get_stdout_pipe(sidebar->slax_process));
  sidebar->show_recent=FALSE;sidebar->show_recent_set=TRUE;
  gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(sidebar)),"slax-places");
  gtk_widget_set_size_request(GTK_WIDGET(sidebar),230,-1);
  slax_places_read(sidebar);
  update_places(sidebar);
}
