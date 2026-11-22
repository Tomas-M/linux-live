/* Native interaction and presentation for Slax's filesystem rows. */
#include "slax-places-labels.h"
enum { PLACE_ICON_PADDING=17 };
typedef struct
{
    GWeakRef view,model;
    char *id;
    guint button;
    guint redraw;
    GSubprocess *process;
    GAsyncResult *result;
} NativeMountJob;

typedef GtkCellAreaBox FmPlacesArea;
typedef GtkCellAreaBoxClass FmPlacesAreaClass;
G_DEFINE_TYPE(FmPlacesArea,_fm_places_area,GTK_TYPE_CELL_AREA_BOX);

/* Paint behind the whole row once, including the spaces between its native cells. */
static void native_area_render(GtkCellArea *area,GtkCellAreaContext *context,GtkWidget *widget,cairo_t *cr,
                               const GdkRectangle *background,const GdkRectangle *cells,GtkCellRendererState flags,gboolean focus)
{
    GtkTreeView *view=GTK_TREE_VIEW(widget);
    GtkTreeRowReference *reference=g_object_get_data(G_OBJECT(view),"slax-hover-row");
    GtkTreePath *path=reference?gtk_tree_row_reference_get_path(reference):NULL;
    if (path && !(flags&GTK_CELL_RENDERER_SELECTED))
    {
        GtkTreeModel *model=gtk_tree_view_get_model(view);
        GtkTreeIter iter;
        FmPlacesItem *item=NULL;
        GdkRectangle row;
        gtk_tree_model_get_iter(model,&iter,path);
        gtk_tree_model_get(model,&iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
        gtk_tree_view_get_background_area(view,path,NULL,&row);
        gboolean location=item && fm_places_item_get_type(item)!=FM_PLACES_ITEM_NONE;
        if (location && row.y==background->y)
        {
            cairo_save(cr);
            cairo_set_source_rgb(cr,242.0/255,246.0/255,249.0/255);
            cairo_rectangle(cr,background->x,row.y,background->width,row.height);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }
    if (path) gtk_tree_path_free(path);
    GTK_CELL_AREA_CLASS(_fm_places_area_parent_class)->render(area,context,widget,cr,background,cells,flags,focus);
}

/* Keep normal GTK cell layout and add only the row background layer. */
static void _fm_places_area_class_init(FmPlacesAreaClass *klass)
{
    GTK_CELL_AREA_CLASS(klass)->render=native_area_render;
}

/* Hover identity remains owned by the view rather than duplicated in its cell area. */
static void _fm_places_area_init(FmPlacesArea *area)
{
}

/* Bookmark actions and drag boundaries follow item identity rather than later device rows. */
static gboolean native_bookmark_at(FmPlacesModel *model,GtkTreeIter *iter)
{
    FmPlacesItem *item=NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(model),iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
    return item && fm_places_item_get_bookmark_item(item)!=NULL;
}

/* A header is a non-location row, not a separator or bookmark. */
static gboolean native_select(GtkTreeSelection *selection,GtkTreeModel *model,GtkTreePath *path,gboolean selected,gpointer unused)
{
    GtkTreeIter iter;
    FmPlacesItem *item=NULL;
    if (!gtk_tree_model_get_iter(model,&iter,path)) return FALSE;
    gtk_tree_model_get(model,&iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
    return item && fm_places_item_get_type(item)!=FM_PLACES_ITEM_NONE;
}

/* Reuse the ordinary GTK label renderer and escape names before adding secondary text. */
static void native_label(GtkTreeViewColumn *column,GtkCellRenderer *renderer,GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
    FmPlacesItem *item=NULL;
    char *label=NULL;
    gtk_tree_model_get(model,iter,FM_PLACES_MODEL_COL_INFO,&item,FM_PLACES_MODEL_COL_LABEL,&label,-1);
    char *markup=g_markup_escape_text(label?label:"",-1);
    GVariant *location=item?fm_places_item_get_native(item):NULL;
    if (location)
    {
        const char *operation=fm_places_item_get_operation(item);
        char *detail=slax_places_detail(location,operation);
        char *text=g_strdup_printf("%s\n<span size=\"9984\" foreground=\"#697079\">%s</span>",markup,detail);
        g_free(markup); markup=text;
        g_free(detail);
    }
    gboolean header=item && fm_places_item_get_type(item)==FM_PLACES_ITEM_NONE;
    int height=36;
    if (location) height=48;
    if (header) height=!g_strcmp0(label,"Places")?34:38;
    g_object_set(renderer,"markup",markup,"visible",item!=NULL,"xpad",header?12:0,"ypad",item && !header?4:0,"height",item?height-6:0,"yalign",header && g_strcmp0(label,"Places")?.75f:.5f,"sensitive",TRUE,NULL);
    if (header) g_object_set(renderer,"foreground","#74797d","size",9984,NULL);
    else g_object_set(renderer,"foreground-set",FALSE,"size-set",FALSE,NULL);
    g_free(markup); g_free(label);
}

/* Unmounted media keep their device icon, with the theme's disabled treatment. */
static void native_icon(GtkTreeViewColumn *column,GtkCellRenderer *renderer,GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
    FmPlacesItem *item=NULL;
    GdkPixbuf *icon=NULL;
    gtk_tree_model_get(model,iter,FM_PLACES_MODEL_COL_INFO,&item,FM_PLACES_MODEL_COL_ICON,&icon,-1);
    g_object_set(renderer,"pixbuf",icon,"visible",item && fm_places_item_get_type(item)!=FM_PLACES_ITEM_NONE,"sensitive",TRUE,NULL);
    g_clear_object(&icon);
}

/* A completed request navigates only when the user activated the volume name. */
static gboolean native_mount_finished(gpointer data)
{
    NativeMountJob *job=data;
    FmPlacesView *view=g_weak_ref_get(&job->view);
    FmPlacesModel *model=g_weak_ref_get(&job->model);
    char *output=NULL,*message=NULL;
    GError *error=NULL;
    gboolean success=g_subprocess_communicate_utf8_finish(job->process,job->result,&output,&message,&error);
    if (model) fm_places_model_set_busy(model,job->id,NULL);
    if (job->redraw) g_source_remove(job->redraw);
    if (view)
    {
        if (!success || !g_subprocess_get_successful(job->process))
            fm_show_error(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view))),NULL,error?error->message:message);
        else if (job->button)
        {
            GVariant *path=g_variant_parse(G_VARIANT_TYPE_STRING,output,NULL,NULL,&error);
            if (path)
            {
                FmPath *folder=fm_path_new_for_path(g_variant_get_string(path,NULL));
                g_signal_emit(view,signals[CHDIR],0,job->button,folder);
                fm_path_unref(folder); g_variant_unref(path);
            }
            else fm_show_error(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view))),NULL,error->message);
        }
    }
    g_clear_error(&error); g_free(output); g_free(message);
    g_clear_object(&view); g_clear_object(&model);
    g_weak_ref_clear(&job->view); g_weak_ref_clear(&job->model);
    g_clear_object(&job->result); g_clear_object(&job->process);
    g_free(job->id); g_free(job);
    return G_SOURCE_REMOVE;
}

/* Keep the operation readable for 500 ms while mount notifications settle to their latest state. */
static void native_mount_done(GObject *process,GAsyncResult *result,gpointer data)
{
    NativeMountJob *job=data;
    job->process=g_object_ref(G_SUBPROCESS(process));
    job->result=g_object_ref(result);
    g_timeout_add(500,native_mount_finished,job);
}

/* Redraw only while an operation is active; the cell's themed spinner supplies progress. */
static gboolean native_redraw(gpointer data)
{
    NativeMountJob *job=data;
    GtkWidget *view=g_weak_ref_get(&job->view);
    if (!view) { job->redraw=0; return G_SOURCE_REMOVE; }
    gtk_widget_queue_draw(view); g_object_unref(view);
    return G_SOURCE_CONTINUE;
}

/* The privileged helper accepts only the catalogue identity, never caller-selected paths. */
static void native_mount(FmPlacesView *view,FmPlacesItem *item,gboolean unmount,guint open_button)
{
    if (fm_places_item_get_operation(item)) return;
    GVariant *location=fm_places_item_get_native(item);
    const char *id,*reason;
    g_variant_get_child(location,0,"&s",&id);
    g_variant_get_child(location,8,"&s",&reason);
    if (*reason)
    {
        fm_show_error(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view))),NULL,reason);
        return;
    }
    const char *action=unmount?"unmount":"mount";
    const char *argv[]={"/usr/bin/pkexec","/usr/libexec/slax-places",action,id,NULL};
    GError *error=NULL;
    GSubprocess *process=g_subprocess_newv(argv+(geteuid()==0?1:0),G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE,&error);
    if (!process)
    {
        fm_show_error(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view))),NULL,error->message);
        g_error_free(error);
        return;
    }
    NativeMountJob *job=g_new0(NativeMountJob,1);
    g_weak_ref_init(&job->view,view); g_weak_ref_init(&job->model,model);
    job->id=g_strdup(id); job->button=open_button;
    fm_places_model_set_busy(model,id,action);
    job->redraw=g_timeout_add(80,native_redraw,job);
    g_subprocess_communicate_utf8_async(process,NULL,NULL,native_mount_done,job);
    g_object_unref(process);
}

/* A native context menu makes mount actions available from both mouse and keyboard. */
static void native_menu_action(GtkMenuItem *menu,gpointer data)
{
    FmPlacesView *view=FM_PLACES_VIEW(data);
    GVariant *location=g_object_get_data(G_OBJECT(menu),"location");
    const char *id,*path;
    g_variant_get_child(location,0,"&s",&id);
    g_variant_get_child(location,6,"&s",&path);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model),&iter)) return;
    do
    {
        FmPlacesItem *item=NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,FM_PLACES_MODEL_COL_INFO,&item,-1);
        GVariant *current=item?fm_places_item_get_native(item):NULL;
        const char *current_id=NULL;
        if (current) g_variant_get_child(current,0,"&s",&current_id);
        if (g_strcmp0(id,current_id)) continue;
        native_mount(view,item,fm_places_item_is_mounted(item),0);
        break;
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(model),&iter));
}

/* Snapshot metadata is owned by the menu, so hot removal cannot leave a dangling item. */
static GtkWidget *native_menu(FmPlacesView *view,FmPlacesItem *item)
{
    GtkWidget *menu=gtk_menu_new();
    GVariant *location=fm_places_item_get_native(item);
    const char *path,*reason;
    g_variant_get_child(location,6,"&s",&path);
    g_variant_get_child(location,8,"&s",&reason);
    GtkWidget *action=gtk_menu_item_new_with_label(*reason?reason:(*path?"Unmount":"Mount"));
    gtk_widget_set_sensitive(action,!*reason && !fm_places_item_get_operation(item));
    g_object_set_data_full(G_OBJECT(action),"location",g_variant_ref(location),(GDestroyNotify)g_variant_unref);
    g_signal_connect_object(action,"activate",G_CALLBACK(native_menu_action),view,0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),action);
    gtk_menu_attach_to_widget(GTK_MENU(menu),GTK_WIDGET(view),NULL);
    g_object_weak_ref(G_OBJECT(view),(GWeakNotify)gtk_menu_detach,menu);
    g_signal_connect(menu,"selection-done",G_CALLBACK(on_selection_done),NULL);
    gtk_widget_show_all(menu);
    return menu;
}

/* Restore the current-folder highlight after a location or bookmark reconciliation. */
static void native_selection_changed(FmPlacesModel *model,FmPlacesView *view)
{
    FmPath *path=g_object_get_data(G_OBJECT(view),"slax-current-path");
    if (path) fm_places_view_chdir(view,path);
}
