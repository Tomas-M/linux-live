/* Keep chooser navigation local to each dialog, using native folders and controls. */
typedef struct
{
  GtkWidget *entry,*back,*forward,*history_button,*up;
  GList *history,*current,*pending;
  GFile *selection;
  GCancellable *query;
} SlaxChooserNavigation;

typedef struct { GWeakRef chooser;GCancellable *cancel; } SlaxChooserPath;
static void set_busy_cursor(GtkFileChooserWidget *impl,gboolean busy);

/* An unavailable selection has no folder, file model or actionable stale filename. */
static void slax_chooser_clear_location(GtkFileChooserWidget *impl)
{
  GtkFileChooserWidgetPrivate *priv=impl->priv;
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  if (priv->update_current_folder_cancellable) g_cancellable_cancel(priv->update_current_folder_cancellable);
  priv->update_current_folder_cancellable=NULL;
  if (navigation->query) g_cancellable_cancel(navigation->query);
  g_clear_object(&navigation->query);g_clear_object(&navigation->selection);navigation->pending=NULL;
  g_clear_object(&priv->current_folder);priv->reload_state=RELOAD_EMPTY;
  operation_mode_set(impl,OPERATION_MODE_BROWSE);stop_loading_and_clear_list_model(impl,TRUE);set_busy_cursor(impl,FALSE);
  g_slist_free_full(priv->pending_select_files,g_object_unref);priv->pending_select_files=NULL;
  gtk_entry_set_text(GTK_ENTRY(navigation->entry),"");
  _gtk_file_chooser_entry_set_base_folder(GTK_FILE_CHOOSER_ENTRY(navigation->entry),NULL);
  if (priv->location_entry) _gtk_file_chooser_entry_set_base_folder(GTK_FILE_CHOOSER_ENTRY(priv->location_entry),NULL);
  gtk_widget_set_sensitive(navigation->back,navigation->current!=NULL);gtk_widget_set_sensitive(navigation->up,FALSE);
  gtk_widget_set_sensitive(priv->browse_new_folder_button,FALSE);
  gtk_places_sidebar_set_location(GTK_PLACES_SIDEBAR(priv->places_sidebar),NULL);
  g_signal_emit_by_name(impl,"current-folder-changed");g_signal_emit_by_name(impl,"selection-changed");
}

/* Release only navigation state; GTK owns the toolbar's widgets. */
static void slax_chooser_navigation_free(gpointer data)
{
  SlaxChooserNavigation *navigation=data;
  if (navigation->query) g_cancellable_cancel(navigation->query);
  g_clear_object(&navigation->query);
  g_list_free_full(navigation->history,g_object_unref);
  g_clear_object(&navigation->selection);g_free(navigation);
}

/* A menu entry points at an existing visit, including repeated visits to a folder. */
static void slax_chooser_history_activate(GtkMenuItem *item,GtkFileChooserWidget *impl)
{
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  navigation->pending=g_object_get_data(G_OBJECT(item),"slax-visit");
  change_folder_and_display_error(impl,navigation->pending->data,FALSE);
}

/* Record successful folder changes and discard the forward branch after a new visit. */
static void slax_chooser_folder_changed(GtkFileChooserWidget *impl,gpointer unused)
{
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  GFile *folder=impl->priv->current_folder;
  if (!folder) return;
  gtk_widget_set_sensitive(impl->priv->browse_new_folder_button,TRUE);
  if (navigation->query) g_cancellable_cancel(navigation->query);
  g_clear_object(&navigation->query);
  if (navigation->selection)
    {
      GFile *parent=g_file_get_parent(navigation->selection);
      if (!parent || !g_file_equal(parent,folder)) g_clear_object(&navigation->selection);
      g_clear_object(&parent);
    }
  if (navigation->pending && g_file_equal(navigation->pending->data,folder)) navigation->current=navigation->pending;
  else if (!navigation->current || !g_file_equal(navigation->current->data,folder))
    {
      GList *forward=navigation->current?navigation->current->next:NULL;
      if (forward) { forward->prev=NULL;navigation->current->next=NULL;g_list_free_full(forward,g_object_unref); }
      navigation->history=g_list_append(navigation->history,g_object_ref(folder));
      navigation->current=g_list_last(navigation->history);
    }
  navigation->pending=NULL;
  if (gtk_widget_has_focus(navigation->entry)) gtk_widget_grab_focus(impl->priv->browse_files_tree_view);
  char *path=g_file_get_parse_name(folder);
  gtk_entry_set_text(GTK_ENTRY(navigation->entry),path);g_free(path);
  _gtk_file_chooser_entry_set_base_folder(GTK_FILE_CHOOSER_ENTRY(navigation->entry),folder);
  gtk_widget_set_sensitive(navigation->back,navigation->current && navigation->current->prev);
  gtk_widget_set_sensitive(navigation->forward,navigation->current && navigation->current->next);
  GFile *parent=g_file_get_parent(folder);
  gtk_widget_set_sensitive(navigation->up,parent!=NULL);g_clear_object(&parent);
  GtkWidget *menu=gtk_menu_new();
  for (GList *visit=navigation->history;visit;visit=visit->next)
    {
      path=g_file_get_parse_name(visit->data);
      GtkWidget *item=gtk_check_menu_item_new_with_label(path);g_free(path);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),visit==navigation->current);
      g_object_set_data(G_OBJECT(item),"slax-visit",visit);
      g_signal_connect(item,"activate",G_CALLBACK(slax_chooser_history_activate),impl);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),item);
    }
  gtk_widget_show_all(menu);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(navigation->history_button),menu);
}

/* Back and Forward use the same native asynchronous folder change as sidebar navigation. */
static void slax_chooser_history_step(GtkButton *button,GtkFileChooserWidget *impl)
{
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  if (!navigation->current) return;
  GList *visit=navigation->current->next;
  if (GTK_WIDGET(button)==navigation->back) visit=impl->priv->current_folder?navigation->current->prev:navigation->current;
  if (!visit) return;
  navigation->pending=visit;
  change_folder_and_display_error(impl,visit->data,FALSE);
}

/* Keep the save filename while navigating to the parent directory. */
static void slax_chooser_up(GtkButton *button,GtkFileChooserWidget *impl)
{
  if (!impl->priv->current_folder) return;
  GFile *parent=g_file_get_parent(impl->priv->current_folder);
  if (parent) { change_folder_and_display_error(impl,parent,FALSE);g_object_unref(parent); }
}

/* Let the dialog update its native Open button before activating a confirmed selection. */
static gboolean slax_chooser_accept(gpointer data)
{
  GWeakRef *weak=data;
  GtkFileChooserWidget *impl=g_weak_ref_get(weak);
  if (impl && gtk_widget_get_mapped(GTK_WIDGET(impl)))
    {
      SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
      GFile *file=gtk_file_chooser_get_file(GTK_FILE_CHOOSER(impl));
      gboolean selected=file && navigation->selection && g_file_equal(file,navigation->selection);
      g_clear_object(&file);g_clear_object(&navigation->selection);
      if (selected) g_signal_emit_by_name(impl,"file-activated");
    }
  g_clear_object(&impl);g_weak_ref_clear(weak);g_free(weak);
  return G_SOURCE_REMOVE;
}

/* Accept a typed file only after GTK has actually selected it in the loaded folder. */
static void slax_chooser_selection_changed(GtkFileChooserWidget *impl,gpointer unused)
{
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  if (!navigation->selection) return;
  GFile *file=gtk_file_chooser_get_file(GTK_FILE_CHOOSER(impl));
  gboolean selected=file && g_file_equal(file,navigation->selection);
  g_clear_object(&file);
  if (!selected) return;
  GWeakRef *weak=g_new0(GWeakRef,1);g_weak_ref_init(weak,impl);
  g_idle_add(slax_chooser_accept,weak);
}

/* Resolve a typed path without blocking GTK or retaining a closed dialog. */
static void slax_chooser_path_ready(GObject *file,GAsyncResult *result,gpointer data)
{
  SlaxChooserPath *request=data;
  GtkFileChooserWidget *impl=g_weak_ref_get(&request->chooser);
  GError *error=NULL;
  GFileInfo *info=g_file_query_info_finish(G_FILE(file),result,&error);
  if (impl && gtk_widget_get_mapped(GTK_WIDGET(impl)) && !g_cancellable_is_cancelled(request->cancel))
    {
      SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
      g_clear_object(&navigation->query);
      if (!info) error_changing_folder_dialog(impl,G_FILE(file),g_steal_pointer(&error));
      else if (g_file_info_get_file_type(info)==G_FILE_TYPE_DIRECTORY) change_folder_and_display_error(impl,G_FILE(file),FALSE);
      else
        {
          g_set_object(&navigation->selection,G_FILE(file));
          if (!gtk_file_chooser_select_file(GTK_FILE_CHOOSER(impl),G_FILE(file),&error))
            { g_clear_object(&navigation->selection);error_changing_folder_dialog(impl,G_FILE(file),g_steal_pointer(&error)); }
          else slax_chooser_selection_changed(impl,NULL);
        }
    }
  g_clear_error(&error);g_clear_object(&info);g_clear_object(&impl);
  g_weak_ref_clear(&request->chooser);g_object_unref(request->cancel);g_free(request);
}

/* Reuse GTK's completion parser for local, relative, international and URI paths. */
static void slax_chooser_path_activate(GtkEntry *entry,GtkFileChooserWidget *impl)
{
  GtkFileChooserEntry *completion=GTK_FILE_CHOOSER_ENTRY(entry);
  GFile *folder=_gtk_file_chooser_entry_get_current_folder(completion);
  if (!folder) return;
  const char *part=_gtk_file_chooser_entry_get_file_part(completion);
  GError *error=NULL;
  GFile *file=*part?g_file_get_child_for_display_name(folder,part,&error):g_object_ref(folder);
  g_object_unref(folder);
  if (!file) { error_building_filename_dialog(impl,error);return; }
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  if (navigation->query) g_cancellable_cancel(navigation->query);
  g_clear_object(&navigation->query);g_clear_object(&navigation->selection);
  navigation->query=g_cancellable_new();
  SlaxChooserPath *request=g_new0(SlaxChooserPath,1);g_weak_ref_init(&request->chooser,impl);request->cancel=g_object_ref(navigation->query);
  g_file_query_info_async(file,G_FILE_ATTRIBUTE_STANDARD_TYPE,G_FILE_QUERY_INFO_NONE,G_PRIORITY_DEFAULT,navigation->query,slax_chooser_path_ready,request);
  g_object_unref(file);
}

/* The entry's Go icon has the same action as Enter. */
static void slax_chooser_path_icon(GtkEntry *entry,GtkEntryIconPosition position,GdkEvent *event,GtkFileChooserWidget *impl)
{
  if (position==GTK_ENTRY_ICON_SECONDARY) slax_chooser_path_activate(entry,impl);
}

/* Offer native completion without silently appending another folder to the submitted path. */
static gboolean slax_chooser_path_prefix(GtkEntryCompletion *completion,const char *prefix,gpointer unused)
{
  return TRUE;
}

/* Ctrl+L must select the full path even while the entry itself already has focus. */
static gboolean slax_chooser_path_key(GtkWidget *entry,GdkEventKey *event,gpointer unused)
{
  if ((event->state&gtk_accelerator_get_default_mod_mask())!=GDK_CONTROL_MASK || gdk_keyval_to_lower(event->keyval)!=GDK_KEY_l) return FALSE;
  gtk_editable_select_region(GTK_EDITABLE(entry),0,-1);return TRUE;
}

/* Keep the file manager's location and history shortcuts inside the chooser. */
static gboolean slax_chooser_navigation_key(GtkWidget *widget,GdkEventKey *event,gpointer unused)
{
  GtkFileChooserWidget *impl=GTK_FILE_CHOOSER_WIDGET(widget);
  SlaxChooserNavigation *navigation=g_object_get_data(G_OBJECT(impl),"slax-navigation");
  GdkModifierType modifiers=event->state&gtk_accelerator_get_default_mod_mask();
  if (modifiers!=GDK_MOD1_MASK) return FALSE;
  if (event->keyval==GDK_KEY_Left) slax_chooser_history_step(GTK_BUTTON(navigation->back),impl);
  else if (event->keyval==GDK_KEY_Right) slax_chooser_history_step(GTK_BUTTON(navigation->forward),impl);
  else if (event->keyval==GDK_KEY_Up) slax_chooser_up(NULL,impl);
  else return FALSE;
  return TRUE;
}

/* Move the native header above both panes and keep its search and save controls intact. */
static void slax_chooser_navigation_init(GtkFileChooserWidget *impl)
{
  GtkFileChooserWidgetPrivate *priv=impl->priv;
  SlaxChooserNavigation *navigation=g_new0(SlaxChooserNavigation,1);
  g_object_set_data_full(G_OBJECT(impl),"slax-navigation",navigation,slax_chooser_navigation_free);
  GtkWidget *header=priv->browse_header_revealer;
  GtkWidget *content=gtk_widget_get_parent(priv->browse_widgets_hpaned);
  g_object_ref(header);gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(header)),header);
  gtk_box_pack_start(GTK_BOX(content),header,FALSE,FALSE,0);gtk_box_reorder_child(GTK_BOX(content),header,0);g_object_unref(header);
  GtkWidget *box=gtk_bin_get_child(GTK_BIN(header));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(box),GTK_ORIENTATION_HORIZONTAL);gtk_box_set_spacing(GTK_BOX(box),0);
  GtkWidget *controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  gtk_widget_set_valign(controls,GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(controls),"slax-chooser-navigation");
  const char *icons[]={"slax-go-previous-symbolic","slax-go-next-symbolic","pan-down-symbolic","slax-go-up-symbolic"};
  const char *hints[]={"Back","Forward","History","Go to the parent folder"};
  const char *names[]={"slax_chooser_back","slax_chooser_forward","slax_chooser_history","slax_chooser_up"};
  GtkWidget **buttons[]={&navigation->back,&navigation->forward,&navigation->history_button,&navigation->up};
  for (int i=0;i<4;i++)
    {
      GtkWidget *button=i==2?gtk_menu_button_new():gtk_button_new();*buttons[i]=button;
      gtk_widget_set_name(button,names[i]);
      gtk_button_set_image(GTK_BUTTON(button),gtk_image_new_from_icon_name(icons[i],GTK_ICON_SIZE_MENU));
      gtk_button_set_relief(GTK_BUTTON(button),GTK_RELIEF_NONE);gtk_widget_set_tooltip_text(button,hints[i]);
      gtk_widget_set_focus_on_click(button,FALSE);gtk_box_pack_start(GTK_BOX(controls),button,FALSE,FALSE,0);
    }
  gtk_box_pack_start(GTK_BOX(box),controls,FALSE,FALSE,0);gtk_box_reorder_child(GTK_BOX(box),controls,0);
  gtk_widget_set_hexpand(priv->browse_header_stack,TRUE);
  gtk_stack_set_vhomogeneous(GTK_STACK(priv->browse_header_stack),FALSE);
  gtk_box_set_child_packing(GTK_BOX(box),priv->browse_header_stack,TRUE,TRUE,0,GTK_PACK_START);
  box=gtk_stack_get_child_by_name(GTK_STACK(priv->browse_header_stack),"pathbar");
  gtk_container_set_border_width(GTK_CONTAINER(box),4);
  gtk_widget_set_no_show_all(priv->browse_path_bar,TRUE);gtk_widget_hide(priv->browse_path_bar);
  navigation->entry=_gtk_file_chooser_entry_new(TRUE,FALSE);
  gtk_widget_set_name(navigation->entry,"slax_chooser_path");gtk_widget_set_hexpand(navigation->entry,TRUE);
  _gtk_file_chooser_entry_set_action(GTK_FILE_CHOOSER_ENTRY(navigation->entry),GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  g_signal_connect(gtk_entry_get_completion(GTK_ENTRY(navigation->entry)),"insert-prefix",G_CALLBACK(slax_chooser_path_prefix),NULL);
  gtk_entry_set_icon_from_stock(GTK_ENTRY(navigation->entry),GTK_ENTRY_ICON_SECONDARY,GTK_STOCK_JUMP_TO);
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(navigation->entry),GTK_ENTRY_ICON_SECONDARY,_("Go to this location"));
  gtk_box_pack_start(GTK_BOX(box),navigation->entry,TRUE,TRUE,0);gtk_box_reorder_child(GTK_BOX(box),navigation->entry,0);
  g_signal_connect(navigation->entry,"activate",G_CALLBACK(slax_chooser_path_activate),impl);
  g_signal_connect(navigation->entry,"key-press-event",G_CALLBACK(slax_chooser_path_key),NULL);
  g_signal_connect(navigation->entry,"icon-release",G_CALLBACK(slax_chooser_path_icon),impl);
  g_signal_connect(navigation->back,"clicked",G_CALLBACK(slax_chooser_history_step),impl);
  g_signal_connect(navigation->forward,"clicked",G_CALLBACK(slax_chooser_history_step),impl);
  g_signal_connect(navigation->up,"clicked",G_CALLBACK(slax_chooser_up),impl);
  g_signal_connect(impl,"current-folder-changed",G_CALLBACK(slax_chooser_folder_changed),NULL);
  g_signal_connect(impl,"selection-changed",G_CALLBACK(slax_chooser_selection_changed),NULL);
  g_signal_connect(impl,"key-press-event",G_CALLBACK(slax_chooser_navigation_key),NULL);
  gtk_widget_show_all(controls);gtk_widget_show(navigation->entry);
  gtk_widget_set_sensitive(navigation->back,FALSE);gtk_widget_set_sensitive(navigation->forward,FALSE);
  slax_chooser_folder_changed(impl,NULL);
}
