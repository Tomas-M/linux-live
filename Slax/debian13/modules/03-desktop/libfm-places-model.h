/* Slax's native location rows share libfm's existing Places model and bookmark area. */

/* Keep the approved unmounted opacity without changing shared theme-cache pixels. */
static GdkPixbuf *native_pixbuf(FmPlacesItem *item)
{
    GdkPixbuf *pixbuf=fm_pixbuf_from_icon(item->icon,fm_config->pane_icon_size);
    if (!item->location || item->mounted) return pixbuf;
    GdkPixbuf *faded=gdk_pixbuf_add_alpha(pixbuf,FALSE,0,0,0);
    g_object_unref(pixbuf);
    guchar *pixels=gdk_pixbuf_get_pixels(faded);
    int stride=gdk_pixbuf_get_rowstride(faded);
    for (int y=0;y<gdk_pixbuf_get_height(faded);y++)
        for (int x=0;x<gdk_pixbuf_get_width(faded);x++)
        {
            guchar *alpha=pixels+y*stride+x*4+3;
            *alpha=(*alpha*58+50)/100;
        }
    return faded;
}

/* Find by stable identity so a refresh never replaces a row being clicked or dragged. */
static FmPlacesItem *native_find(FmPlacesModel *model,const char *id,GtkTreeIter *result)
{
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model),&iter)) return NULL;
    do
    {
        FmPlacesItem *item=NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
        if (item && !g_strcmp0(item->native_id,id)) { if (result) *result=iter; return item; }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(model),&iter));
    return NULL;
}

/* Headers use the existing non-location item type and cannot be selected or activated. */
static void native_header(FmPlacesModel *model,const char *name,GHashTable *seen)
{
    char *id=g_strconcat("header:",name,NULL);
    g_hash_table_add(seen,g_strdup(id));
    GtkTreeIter iter;
    FmPlacesItem *item=native_find(model,id,&iter);
    if (!item)
    {
        item=add_new_item(GTK_LIST_STORE(model),FM_PLACES_ITEM_NONE,&iter,NULL);
        item->native_id=g_strdup(id);
        gtk_list_store_set(GTK_LIST_STORE(model),&iter,FM_PLACES_MODEL_COL_LABEL,name,-1);
    }
    if (!strcmp(name,"Places")) gtk_list_store_move_after(GTK_LIST_STORE(model),&iter,NULL);
    else gtk_list_store_move_before(GTK_LIST_STORE(model),&iter,NULL);
    g_free(id);
}

/* Reconcile mount metadata in place and let libfm perform ordinary directory-info jobs. */
static void native_reconcile(FmPlacesModel *model,GVariant *locations)
{
    if (model->native_snapshot && g_variant_equal(model->native_snapshot,locations)) return;
    g_clear_pointer(&model->native_snapshot,g_variant_unref);
    model->native_snapshot=g_variant_ref(locations);
    GHashTable *seen=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    FmFileInfoJob *job=fm_file_info_job_new(NULL,FM_FILE_INFO_JOB_FOLLOW_SYMLINK);
    native_header(model,"Places",seen);
    const char *previous="";
    GVariantIter rows;
    GVariant *row;
    g_variant_iter_init(&rows,locations);
    while ((row=g_variant_iter_next_value(&rows)))
    {
        const char *id,*name,*group,*icon,*type,*source,*path,*target,*reason;
        g_variant_get(row,"(&s&s&s&s&s&s&s&s&s)",&id,&name,&group,&icon,&type,&source,&path,&target,&reason);
        if (strcmp(group,previous)) native_header(model,group,seen);
        previous=group;
        g_hash_table_add(seen,g_strdup(id));
        GtkTreeIter iter;
        FmPlacesItem *item=native_find(model,id,&iter);
        if (!item)
        {
            item=add_new_item(GTK_LIST_STORE(model),FM_PLACES_ITEM_LOCATION,&iter,NULL);
            item->native_id=g_strdup(id);
        }
        g_clear_pointer(&item->location,g_variant_unref);
        item->location=row;
        item->mounted=*path!=0;
        fm_file_info_unref(item->fi);
        item->fi=fm_file_info_new();
        if (*path)
        {
            FmPath *folder=fm_path_new_for_path(path);
            fm_file_info_set_path(item->fi,folder);
            fm_file_info_job_add(job,folder);
            fm_path_unref(folder);
        }
        g_clear_object(&item->icon);
        item->icon=fm_icon_from_name(icon);
        GdkPixbuf *pixbuf=native_pixbuf(item);
        gtk_list_store_set(GTK_LIST_STORE(model),&iter,FM_PLACES_MODEL_COL_ICON,pixbuf,FM_PLACES_MODEL_COL_LABEL,name,-1);
        g_clear_object(&pixbuf);
        gtk_list_store_move_before(GTK_LIST_STORE(model),&iter,NULL);
    }
    GtkTreeIter iter;
    gboolean valid=gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model),&iter);
    while (valid)
    {
        FmPlacesItem *item=NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
        if (item && item->native_id && !g_hash_table_contains(seen,item->native_id))
        {
            valid=gtk_list_store_remove(GTK_LIST_STORE(model),&iter);
            place_item_free(item);
        }
        else valid=gtk_tree_model_iter_next(GTK_TREE_MODEL(model),&iter);
    }
    g_hash_table_unref(seen);
    on_bookmarks_changed(model->bookmarks,model);
    g_signal_connect(job,"finished",G_CALLBACK(on_file_info_job_finished),model);
    model->jobs=g_slist_prepend(model->jobs,job);
    if (!fm_job_run_async(FM_JOB(job))) { model->jobs=g_slist_remove(model->jobs,job); g_object_unref(job); }
}

static void native_read(FmPlacesModel *model);

/* Weak ownership lets closing the last Places view cancel its private watcher immediately. */
static void native_received(GObject *stream,GAsyncResult *result,gpointer data)
{
    GWeakRef *weak=data;
    FmPlacesModel *model=g_weak_ref_get(weak);
    GError *error=NULL;
    char *line=g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(stream),result,NULL,&error);
    g_weak_ref_clear(weak); g_free(weak);
    if (model && line)
    {
        GVariant *locations=g_variant_parse(G_VARIANT_TYPE("a(sssssssss)"),line,NULL,NULL,&error);
        if (locations) { native_reconcile(model,locations); g_variant_unref(locations); }
        native_read(model);
    }
    if (error && !g_error_matches(error,G_IO_ERROR,G_IO_ERROR_CANCELLED)) g_warning("Cannot read filesystem locations: %s",error->message);
    g_clear_error(&error); g_free(line); g_clear_object(&model);
}

/* Keep all discovery and probing outside GTK's thread. */
static void native_read(FmPlacesModel *model)
{
    GWeakRef *weak=g_new0(GWeakRef,1);
    g_weak_ref_init(weak,model);
    g_data_input_stream_read_line_async(model->native_stream,G_PRIORITY_DEFAULT,model->native_cancel,native_received,weak);
}

/* Busy state belongs to the transient operation, never to persisted volume metadata. */
void fm_places_model_set_busy(FmPlacesModel *model,const char *id,const char *operation)
{
    GtkTreeIter iter;
    FmPlacesItem *item=native_find(model,id,&iter);
    if (!item) return;
    item->pending=operation;
    GtkTreePath *path=gtk_tree_model_get_path(GTK_TREE_MODEL(model),&iter);
    gtk_tree_model_row_changed(GTK_TREE_MODEL(model),path,&iter);
    gtk_tree_path_free(path);
}

/* Native metadata is immutable until the next kernel or provider notification. */
GVariant *fm_places_item_get_native(FmPlacesItem *item)
{
    return item->location;
}

/* All views sharing this model show the same in-flight operation. */
const char *fm_places_item_get_operation(FmPlacesItem *item)
{
    return item->pending;
}
