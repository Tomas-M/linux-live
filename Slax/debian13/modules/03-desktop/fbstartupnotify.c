/* Application startup feedback using the native themed progress cursor.
 * Build: cc -Os -s -o fbstartupnotify fbstartupnotify.c $(pkg-config --cflags --libs gtk+-2.0 gio-unix-2.0 libstartup-notification-1.0 xcursor x11 xfixes)
 * Usage: fbstartupnotify COMMAND [ARGS...] | --desktop FILE [FILES...] | --monitor | --complete
 */
#define SN_API_NOT_YET_FROZEN
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gio/gdesktopappinfo.h>
#include <libsn/sn.h>
#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xfixes.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct {
    SnStartupSequence *sequence;
    gint64 deadline;
    GHashTable *windows;
    Window active;
} Pending;

static Display *display;
static Window root;
static SnDisplay *sn_display;
static GList *pending;
static SnLauncherContext *launch;
static int child_status;
static gboolean monitor_mode;
static const char *arrow_names[]={"default", "left_ptr", "arrow", "top_left_arrow"};
static Cursor busy_cursors[G_N_ELEMENTS(arrow_names)];
static guint timer;
static int fixes_event;
static Atom monitor_selection;
static Atom clients_atom, active_atom, startup_atom, pid_atom, leader_atom;

/* libsn and window inspection share GDK's scoped handling of disappearing windows. */
static void push_error(SnDisplay *sn, Display *d) { gdk_error_trap_push(); }
static void pop_error(SnDisplay *sn, Display *d) { XSync(d, False); gdk_error_trap_pop(); }

/* Read one X property, checking its wire format before interpreting its data. */
static guchar *property(Window window, Atom atom, Atom type, int format, gulong *count)
{
    Atom actual;
    int bits;
    gulong remaining;
    guchar *data=NULL;
    *count=0;
    if (XGetWindowProperty(display, window, atom, 0, 65536, False, type,
            &actual, &bits, count, &remaining, &data) != Success || actual != type || bits != format)
    {
        if (data) XFree(data);
        *count=0;
        return NULL;
    }
    return data;
}

/* Single-value properties are used for the active window, process and group leader. */
static gulong number(Window window, Atom atom, Atom type)
{
    gulong count, result=0;
    gulong *data=(gulong *)property(window, atom, type, 32, &count);
    if (count) result=data[0];
    if (data) XFree(data);
    return result;
}

/* Match wrapper processes such as runuser, dbus-run-session and shell commands. */
static gboolean descendant(pid_t pid, pid_t parent)
{
    for (int depth=0; pid > 1 && depth < 64; depth++)
    {
        if (pid == parent) return TRUE;
        gchar *path=g_strdup_printf("/proc/%d/stat", pid), *stat=NULL;
        gboolean found=g_file_get_contents(path, &stat, NULL, NULL);
        g_free(path);
        if (!found) return FALSE;
        char *end=strrchr(stat, ')');
        int next=0;
        if (end) sscanf(end+1, " %*c %d", &next);
        g_free(stat);
        pid=next;
    }
    return FALSE;
}

/* Prefer the exact startup ID, then local process ancestry, then a new matching class. */
static int match_window(Pending *item, Window window, Window active)
{
    const char *id=sn_startup_sequence_get_id(item->sequence);
    gulong count;
    char *window_id=(char *)property(window, startup_atom, XInternAtom(display, "UTF8_STRING", False), 8, &count);
    gboolean leader_id=!window_id;
    if (!window_id)
    {
        Window leader=number(window, leader_atom, XA_WINDOW);
        if (leader) window_id=(char *)property(leader, startup_atom, XInternAtom(display, "UTF8_STRING", False), 8, &count);
    }
    gboolean match=window_id && !strcmp(id, window_id);
    gboolean other_id=window_id && *window_id && !match;
    if (window_id) XFree(window_id);
    if (match) return 3;
    gboolean activated=g_hash_table_contains(item->windows, (gpointer)window) && window == active && active != item->active;
    /* A shared leader can retain the startup ID of the application's first window. */
    if (other_id && !activated && !leader_id) return FALSE;

    int parent=0;
    if (sscanf(id, "fbstartupnotify-%d", &parent) == 1)
    {
        char *host=(char *)property(window, XA_WM_CLIENT_MACHINE, XA_STRING, 8, &count);
        gboolean local=!host || !strcmp(host, g_get_host_name());
        if (host) XFree(host);
        if (local && descendant(number(window, pid_atom, XA_CARDINAL), parent)) return 2;
    }

    if (g_hash_table_contains(item->windows, (gpointer)window) &&
            (window != active || active == item->active)) return FALSE;
    const char *wmclass=sn_startup_sequence_get_wmclass(item->sequence);
    const char *binary=sn_startup_sequence_get_binary_name(item->sequence);
    gchar *name=binary ? g_path_get_basename(binary) : NULL;
    XClassHint hint={0};
    if (XGetClassHint(display, window, &hint))
    {
        /* Xlib permits either string to be absent from an incomplete class hint. */
        const char *resource=hint.res_name ? hint.res_name : "";
        const char *klass=hint.res_class ? hint.res_class : "";
        gboolean declared_class=wmclass && (!strcmp(wmclass, resource) || !strcmp(wmclass, klass));
        gboolean executable_class=name && (!g_ascii_strcasecmp(name, resource) || !g_ascii_strcasecmp(name, klass));
        match=declared_class || executable_class;
        XFree(hint.res_name);
        XFree(hint.res_class);
    }
    g_free(name);
    return match;
}

/* Only ordinary arrows become busy; text, links, resize and custom cursors stay intact. */
static void set_feedback(gboolean busy)
{
    for (guint i=0; i < G_N_ELEMENTS(arrow_names); i++)
    {
        gchar *name=g_strconcat("_SLAX_STARTUP_", arrow_names[i], NULL);
        if (busy)
        {
            busy_cursors[i]=XcursorLibraryLoadCursor(display, "progress");
            XFixesSetCursorName(display, busy_cursors[i], name);
            XFixesChangeCursorByName(display, busy_cursors[i], arrow_names[i]);
        }
        else
        {
            Cursor normal=XcursorLibraryLoadCursor(display, arrow_names[i]);
            XFixesChangeCursorByName(display, normal, name);
            XFreeCursor(display, normal);
            if (busy_cursors[i]) XFreeCursor(display, busy_cursors[i]);
            busy_cursors[i]=None;
        }
        g_free(name);
    }
    XFlush(display);
}

/* Check readiness independently of the cursor animation, which runs in the X server. */
static gboolean tick(gpointer data)
{
    if (!pending)
    {
        set_feedback(FALSE);
        timer=0;
        return FALSE;
    }
    gint64 now=g_get_monotonic_time();
    gdk_error_trap_push();
    gulong count;
    Window *windows=(Window *)property(root, clients_atom, XA_WINDOW, 32, &count);
    Window active=number(root, active_atom, XA_WINDOW);
    GHashTable *ready=g_hash_table_new(g_direct_hash, g_direct_equal);
    /* Assign each new window to its strongest match, never to several launches. */
    for (gulong i=0; i < count; i++)
    {
        Pending *best=NULL;
        int score=0;
        for (GList *node=pending; node; node=node->next)
        {
            Pending *item=node->data;
            if (g_hash_table_contains(ready, item)) continue;
            int candidate=match_window(item, windows[i], active);
            if (candidate > score) { best=item; score=candidate; }
        }
        if (best) g_hash_table_add(ready, best);
    }
    for (GList *node=pending; node; node=node->next)
    {
        Pending *item=node->data;
        if (now >= item->deadline || g_hash_table_contains(ready, item))
            sn_startup_sequence_complete(item->sequence);
        for (gulong i=0; i < count; i++) g_hash_table_add(item->windows, (gpointer)windows[i]);
        item->active=active;
    }
    g_hash_table_destroy(ready);
    if (windows) XFree(windows);
    pop_error(sn_display, display);

    return TRUE;
}

/* Follow standard startup sequences; overlapping launches share a single indicator. */
static void notification(SnMonitorEvent *event, void *data)
{
    SnStartupSequence *sequence=sn_monitor_event_get_startup_sequence(event);
    const char *id=sn_startup_sequence_get_id(sequence);
    SnMonitorEventType type=sn_monitor_event_get_type(event);
    if (!monitor_mode)
    {
        if (type != SN_MONITOR_EVENT_COMPLETED || !launch) return;
        if (strcmp(id, sn_launcher_context_get_startup_id(launch))) return;
        sn_launcher_context_unref(launch);
        launch=NULL;
        if (gtk_main_level()) gtk_main_quit();
        return;
    }
    if (type == SN_MONITOR_EVENT_INITIATED)
    {
        /* Our launchers publish their baseline before the application can create windows. */
        Window snapshot=root;
        sscanf(id, "fbstartupnotify-%*d-%lx/", &snapshot);
        gdk_error_trap_push();
        Window active=number(snapshot, active_atom, XA_WINDOW);
        gulong count;
        Window *windows=(Window *)property(snapshot, clients_atom, XA_WINDOW, 32, &count);
        XSync(display, False);
        int error=gdk_error_trap_pop();
        if (error) { if (windows) XFree(windows); return; }

        Pending *item=g_new0(Pending, 1);
        item->sequence=sequence;
        sn_startup_sequence_ref(sequence);
        item->deadline=g_get_monotonic_time()+30*G_USEC_PER_SEC;
        item->windows=g_hash_table_new(g_direct_hash, g_direct_equal);
        item->active=active;
        for (gulong i=0; i < count; i++) g_hash_table_add(item->windows, (gpointer)windows[i]);
        if (windows) XFree(windows);
        pending=g_list_append(pending, item);
        if (!timer)
        {
            set_feedback(TRUE);
            timer=g_timeout_add(100, tick, NULL);
        }
    }
    if (type == SN_MONITOR_EVENT_COMPLETED || type == SN_MONITOR_EVENT_CANCELED)
        for (GList *node=pending; node; node=node->next)
        {
            Pending *item=node->data;
            if (strcmp(id, sn_startup_sequence_get_id(item->sequence))) continue;
            sn_startup_sequence_unref(item->sequence);
            g_hash_table_destroy(item->windows);
            g_free(item);
            pending=g_list_delete_link(pending, node);
            break;
        }
}

/* Feed startup broadcasts into libsn without consuming GTK's other X events. */
static GdkFilterReturn filter(GdkXEvent *event, GdkEvent *translated, gpointer data)
{
    XEvent *xevent=event;
    if (monitor_mode && pending && xevent->type == fixes_event+XFixesCursorNotify)
    {
        XFixesCursorNotifyEvent *cursor=(XFixesCursorNotifyEvent *)xevent;
        for (guint i=0; i < G_N_ELEMENTS(arrow_names); i++)
            if (cursor->cursor_name == XInternAtom(display, arrow_names[i], False))
                XFixesChangeCursorByName(display, busy_cursors[i], arrow_names[i]);
        XFlush(display);
    }
    /* Launchers restore arrows if the monitor crashes before completing their sequences. */
    if (!monitor_mode && xevent->type == fixes_event+XFixesSelectionNotify)
    {
        XFixesSelectionNotifyEvent *selection=(XFixesSelectionNotifyEvent *)xevent;
        if (selection->selection == monitor_selection && selection->owner == None &&
                XGetSelectionOwner(display, monitor_selection) == None) set_feedback(FALSE);
    }
    sn_display_process_event(sn_display, event);
    return GDK_FILTER_CONTINUE;
}

/* Restore native arrows on an ordinary monitor shutdown. */
static gboolean stop_monitor(gpointer data)
{
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

/* One unmapped owner window prevents racing launches from creating duplicate monitors. */
static void monitor(Atom selection)
{
    Window window=XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    XGrabServer(display);
    gboolean owner=XGetSelectionOwner(display, selection) == None;
    if (owner) XSetSelectionOwner(display, selection, window, CurrentTime);
    XUngrabServer(display);
    if (owner)
    {
        set_feedback(FALSE);
        XFixesSelectCursorInput(display, root, XFixesDisplayCursorNotifyMask);
        g_unix_signal_add(SIGTERM, stop_monitor, NULL);
        g_unix_signal_add(SIGINT, stop_monitor, NULL);
        gtk_main();
        set_feedback(FALSE);
    }
    XDestroyWindow(display, window);
    XFlush(display);
}

/* Each launcher owns its sequence until readiness, failure or the bounded timeout. */
static SnLauncherContext *begin(const char *name, const char *binary, const char *wmclass)
{
    /* Multiple file arguments are one user action, even if GIO spawns several children. */
    if (launch) return launch;
    SnLauncherContext *context=sn_launcher_context_new(sn_display, DefaultScreen(display));
    sn_launcher_context_set_name(context, name);
    sn_launcher_context_set_binary_name(context, binary);
    if (wmclass) sn_launcher_context_set_wmclass(context, wmclass);
    /* Keep the pre-launch snapshot on this X connection until its launcher exits. */
    Window snapshot=XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    gulong count;
    guchar *windows=property(root, clients_atom, XA_WINDOW, 32, &count);
    XChangeProperty(display, snapshot, clients_atom, XA_WINDOW, 32, PropModeReplace, windows, count);
    if (windows) XFree(windows);
    Window active=number(root, active_atom, XA_WINDOW);
    XChangeProperty(display, snapshot, active_atom, XA_WINDOW, 32, PropModeReplace, (guchar *)&active, 1);
    gchar *launcher=g_strdup_printf("fbstartupnotify-%d-%lx", getpid(), snapshot);
    Time time=gdk_x11_get_server_time(gdk_get_default_root_window());
    sn_launcher_context_initiate(context, launcher, binary, time);
    g_free(launcher);
    launch=context;
    return context;
}

typedef GAppLaunchContext LaunchContext;
typedef GAppLaunchContextClass LaunchContextClass;
G_DEFINE_TYPE(LaunchContext, launch_context, G_TYPE_APP_LAUNCH_CONTEXT)

/* GIO retains desktop-file quoting, field codes, terminal handling and D-Bus activation. */
static gchar *startup_id(GAppLaunchContext *context, GAppInfo *info, GList *files)
{
    const char *wmclass=g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(info));
    return g_strdup(sn_launcher_context_get_startup_id(begin(g_app_info_get_name(info),
        g_app_info_get_executable(info), wmclass)));
}

/* Tell the monitor immediately when GIO could not start a desktop entry. */
static void launch_failed(GAppLaunchContext *context, const char *id)
{
    gdk_notify_startup_complete_with_id(id);
}

/* Register the two startup-notification hooks on the ordinary GIO launch context. */
static void launch_context_class_init(LaunchContextClass *klass)
{
    klass->get_startup_notify_id=startup_id;
    klass->launch_failed=launch_failed;
}

/* No additional per-context state is needed. */
static void launch_context_init(LaunchContext *context) {}

/* A failed child ends feedback immediately; successful daemonization waits for readiness. */
static void child_exited(GPid pid, gint status, gpointer data)
{
    if (!WIFEXITED(status) || WEXITSTATUS(status))
    {
        child_status=WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
        if (launch) sn_launcher_context_complete(launch);
    }
    g_spawn_close_pid(pid);
}

/* Reap children while startup feedback is active, including desktop-file launches. */
static void child_started(GDesktopAppInfo *info, GPid pid, gpointer data)
{
    g_child_watch_add(pid, child_exited, NULL);
}

/* Entries without StartupNotify still get feedback through process/window matching. */
static void launch_started(GAppLaunchContext *context, GAppInfo *info, GVariant *platform, gpointer data)
{
    if (!g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(info), "StartupNotify"))
        begin(g_app_info_get_name(info), g_app_info_get_executable(info),
            g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(info)));
}

/* Also bound the launcher's lifetime if the monitor is stopped during a launch. */
static gboolean timeout(gpointer data)
{
    if (launch) sn_launcher_context_complete(launch);
    gtk_main_quit();
    return FALSE;
}

/* Initialize X once, then either monitor or launch while preserving the original argv. */
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        g_printerr("Usage: fbstartupnotify COMMAND [ARGS...] | --desktop FILE [FILES...] | --monitor | --complete\n");
        return 2;
    }
    monitor_mode=!strcmp(argv[1], "--monitor");
    gchar *inherited=g_strdup(g_getenv("DESKTOP_STARTUP_ID"));
    if (!inherited) inherited=g_strdup(g_getenv("XDG_ACTIVATION_TOKEN"));
    g_unsetenv("DESKTOP_STARTUP_ID");
    g_unsetenv("XDG_ACTIVATION_TOKEN");
    gtk_init(NULL, NULL);
    display=GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    root=DefaultRootWindow(display);
    if (inherited && !monitor_mode) gdk_notify_startup_complete_with_id(inherited);
    g_free(inherited);
    if (!strcmp(argv[1], "--complete")) { gdk_flush(); return 0; }
    clients_atom=XInternAtom(display, "_NET_CLIENT_LIST", False);
    active_atom=XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    startup_atom=XInternAtom(display, "_NET_STARTUP_ID", False);
    pid_atom=XInternAtom(display, "_NET_WM_PID", False);
    leader_atom=XInternAtom(display, "WM_CLIENT_LEADER", False);
    sn_display=sn_display_new(display, push_error, pop_error);
    sn_monitor_context_new(sn_display, DefaultScreen(display), notification, NULL, NULL);
    gdk_window_set_events(gdk_get_default_root_window(),
        gdk_window_get_events(gdk_get_default_root_window()) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(NULL, filter, NULL);
    int fixes_error;
    if (!XFixesQueryExtension(display, &fixes_event, &fixes_error)) return 1;
    Atom selection=monitor_selection=XInternAtom(display, "_SLAX_STARTUP_MONITOR", False);
    if (monitor_mode) { monitor(selection); return 0; }
    XFixesSelectSelectionInput(display, root, selection,
        XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask);

    if (XGetSelectionOwner(display, selection) == None)
    {
        gchar *args[]={"fbstartupnotify", "--monitor", NULL};
        g_spawn_async(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        for (int i=0; i < 100 && XGetSelectionOwner(display, selection) == None; i++) g_usleep(10000);
    }

    GError *error=NULL;
    gboolean success;
    if (!strcmp(argv[1], "--desktop"))
    {
        if (argc < 3) return 2;
        GDesktopAppInfo *info=g_desktop_app_info_new_from_filename(argv[2]);
        if (!info) { g_printerr("Cannot load desktop entry: %s\n", argv[2]); return 1; }
        GAppLaunchContext *context=g_object_new(launch_context_get_type(), NULL);
        g_signal_connect(context, "launch-started", G_CALLBACK(launch_started), NULL);
        GList *uris=NULL;
        for (int i=3; i < argc; i++)
        {
            GFile *file=g_file_new_for_commandline_arg(argv[i]);
            uris=g_list_append(uris, g_file_get_uri(file));
            g_object_unref(file);
        }
        success=g_desktop_app_info_launch_uris_as_manager(info, uris, context,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, child_started, NULL, &error);
        g_list_free_full(uris, g_free);
        g_object_unref(context);
        g_object_unref(info);
    }
    else
    {
        SnLauncherContext *context=begin(argv[1], argv[1], NULL);
        gchar **env=g_get_environ();
        env=g_environ_setenv(env, "DESKTOP_STARTUP_ID", sn_launcher_context_get_startup_id(context), TRUE);
        /* GTK4 may prefer this token; a nested installer must not pass the previous ID. */
        env=g_environ_setenv(env, "XDG_ACTIVATION_TOKEN", sn_launcher_context_get_startup_id(context), TRUE);
        GPid pid;
        success=g_spawn_async(NULL, argv+1, env, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL, &pid, &error);
        g_strfreev(env);
        if (success) g_child_watch_add(pid, child_exited, NULL);
    }
    if (!success)
    {
        g_printerr("%s\n", error->message);
        g_error_free(error);
        if (launch) sn_launcher_context_complete(launch);
    }
    else if (launch)
    {
        g_timeout_add_seconds(30, timeout, NULL);
        gtk_main();
    }
    gdk_flush();
    return success ? child_status : 1;
}
