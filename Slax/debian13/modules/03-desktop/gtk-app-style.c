/* Build: cc -Os -shared -fPIC -s -o libslax-app-style.so gtk-app-style.c $(pkg-config --cflags --libs gtk+-3.0) */
#include <gtk/gtk.h>
#include <gmodule.h>

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

/* GTK calls this after opening the default display; only named applications get CSS. */
G_MODULE_EXPORT void gtk_module_init(int *argc, char ***argv)
{
    if (g_strcmp0(g_get_prgname(), "pcmanfm")==0)
        g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, hide_root_warning, NULL, NULL);

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
