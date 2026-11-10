/* Build: cc -Os -shared -fPIC -s -o libslax-app-style.so gtk-app-style.c $(pkg-config --cflags --libs gtk+-3.0) */
#include <gtk/gtk.h>
#include <gmodule.h>

/* Keep standard message dialogs spacious without expanding their action buttons. */
static gboolean configure_message_dialog(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    GtkWidget *widget=g_value_get_object(values);
    if (!GTK_IS_MESSAGE_DIALOG(widget)) return TRUE;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget *actions=gtk_dialog_get_action_area(GTK_DIALOG(widget));
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (gtk_button_box_get_layout(GTK_BUTTON_BOX(actions))==GTK_BUTTONBOX_EXPAND)
        gtk_button_box_set_layout(GTK_BUTTON_BOX(actions), GTK_BUTTONBOX_END);
    GtkContainer *content=GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(widget)));
    gtk_container_set_border_width(content, MAX(14, gtk_container_get_border_width(content)));
    return TRUE;
}

/* Keep the ordinary pointer while GDK continues tracking the actual drop action. */
static void keep_drag_pointer(GdkDragContext *context, GdkDragAction action, gpointer data)
{
    g_signal_stop_emission_by_name(context, "action-changed");
}

/* Keep each pane's native drag preview while using the ordinary pointer. */
static void begin_file_drag(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
    gdk_window_set_cursor(gtk_widget_get_window(widget), NULL);
    g_signal_connect(context, "action-changed", G_CALLBACK(keep_drag_pointer), NULL);
}

/* Scope drag styling to the file-manager panes, leaving desktop and other apps alone. */
static gboolean configure_file_drag(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    GtkWidget *widget=g_value_get_object(values);
    gboolean places=g_strcmp0(G_OBJECT_TYPE_NAME(widget), "FmPlacesView")==0;
    if (places) gtk_style_context_add_class(gtk_widget_get_style_context(widget), "slax-places");
    gboolean icon_view=g_strcmp0(G_OBJECT_TYPE_NAME(widget), "ExoIconView")==0;
    if (!places && !icon_view && !GTK_IS_TREE_VIEW(widget)) return TRUE;
    GtkWidget *parent=widget;
    while (parent && g_strcmp0(G_OBJECT_TYPE_NAME(parent), "FmStandardView")) parent=gtk_widget_get_parent(parent);
    if (!places && !parent) return TRUE;
    if (!g_signal_handler_find(widget, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, G_CALLBACK(begin_file_drag), NULL))
        g_signal_connect_after(widget, "drag-begin", G_CALLBACK(begin_file_drag), NULL);
    return TRUE;
}

/* Hide only PCManFM's root indicator and its separator before the toolbar is mapped. */
static gboolean hide_root_warning(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    GtkWidget *widget=g_value_get_object(values);
    if (!GTK_IS_TOOLBAR(widget)) return TRUE;
    if (g_strcmp0(G_OBJECT_TYPE_NAME(gtk_widget_get_toplevel(widget)), "FmMainWin")) return TRUE;

    GtkToolItem *warning=gtk_toolbar_get_nth_item(GTK_TOOLBAR(widget), 0);
    GtkToolItem *separator=gtk_toolbar_get_nth_item(GTK_TOOLBAR(widget), 1);
    if (!warning || !GTK_IS_SEPARATOR_TOOL_ITEM(separator)) return TRUE;
    GtkWidget *image=gtk_bin_get_child(GTK_BIN(warning));
    if (!GTK_IS_IMAGE(image)) return TRUE;

    char *stock=NULL;
    g_object_get(image, "stock", &stock, NULL);
    gboolean root_warning=g_strcmp0(stock, "gtk-dialog-warning")==0;
    g_free(stock);
    if (root_warning)
    {
        gtk_widget_set_no_show_all(GTK_WIDGET(warning), TRUE);
        gtk_widget_set_no_show_all(GTK_WIDGET(separator), TRUE);
        gtk_widget_hide(GTK_WIDGET(warning));
        gtk_widget_hide(GTK_WIDGET(separator));
    }
    return TRUE;
}

/* Apply shared message-dialog spacing and optional application styles. */
G_MODULE_EXPORT void gtk_module_init(int *argc, char ***argv)
{
    g_signal_add_emission_hook(g_signal_lookup("show", GTK_TYPE_WIDGET), 0, configure_message_dialog, NULL, NULL);
    if (g_strcmp0(g_get_prgname(), "pcmanfm")==0)
    {
        g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, hide_root_warning, NULL, NULL);
        g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, configure_file_drag, NULL, NULL);
    }

    char *path=g_strdup_printf("/usr/share/slax/gtk-3.0/apps/%s.css", g_get_prgname());
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
    {
        GtkCssProvider *style=gtk_css_provider_new();
        gtk_css_provider_load_from_path(style, path, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(style);
    }
    g_free(path);
}
