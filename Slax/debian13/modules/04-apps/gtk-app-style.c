/* Build: cc -Os -shared -fPIC -s -o libslax-app-style.so gtk-app-style.c $(pkg-config --cflags --libs gtk+-3.0) */
#include <gtk/gtk.h>
#include <gmodule.h>

/* GTK calls this after opening the default display; only named applications get CSS. */
G_MODULE_EXPORT void gtk_module_init(int *argc, char ***argv)
{
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
