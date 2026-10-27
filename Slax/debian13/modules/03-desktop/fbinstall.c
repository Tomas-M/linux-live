/* Build: cc -Os -s -o fbinstall fbinstall.c $(pkg-config --cflags --libs gtk+-2.0)
 * Usage: fbinstall [--user guest] ICON NAME "PACKAGE..." "COMMAND [ARGUMENT...]" [ARGUMENT...]
 * Commands use shell quoting for arguments, but are executed without a shell.
 */
#include <gtk/gtk.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *window, *progress, *status_label;
static gchar **packages, **command;
static GString *output;
static enum { INSTALL, UPDATE } phase=INSTALL;
static gboolean updated, guest, cancelled;
static GPid apt_child;
static guint pulse_timer;
static struct { GIOChannel *channel; guint watch; } pipes[3];

static gboolean start_apt(gpointer data);

/* Keep APT, dpkg and package scripts in one cancellable process group. */
static void setup_apt(gpointer data)
{
    (void)data;
    setpgid(0, 0);
}

/* Stop package processes that did not respond to the initial termination request. */
static gboolean kill_apt(gpointer data)
{
    (void)data;
    if (apt_child) kill(-apt_child, SIGKILL);
    return FALSE;
}

/* Closing the installer cancels every installation phase, including preparation. */
static gboolean cancel_install(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)event;
    (void)data;
    cancelled=TRUE;
    gtk_widget_hide(widget);
    if (apt_child)
    {
        kill(-apt_child, SIGTERM);
        g_timeout_add(1000, kill_apt, NULL);
    }
    else gtk_main_quit();
    return TRUE;
}

/* Show startup feedback, preserving file arguments and the selected application user. */
static void launch_command(void)
{
    if (window) gtk_widget_hide(window);
    GPtrArray *arguments=g_ptr_array_new();
    g_ptr_array_add(arguments, "fbstartupnotify");
    if (guest) g_ptr_array_add(arguments, "fbliveapp");
    for (guint i=0; command[i]; i++) g_ptr_array_add(arguments, command[i]);
    g_ptr_array_add(arguments, NULL);
    execvp("fbstartupnotify", (gchar **)arguments->pdata);
    g_ptr_array_free(arguments, TRUE);
}

/* Keep APT's diagnostic output readable in a bounded, scrollable error dialog. */
static void show_error(const gchar *message)
{
    GtkWidget *dialog=gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);
    gtk_window_set_title(GTK_WINDOW(dialog), "Installation error");
    gtk_window_set_gravity(GTK_WINDOW(dialog), GDK_GRAVITY_CENTER);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
    if (window) gtk_widget_hide(window);
    if (output && output->len)
    {
        GtkWidget *scroll=gtk_scrolled_window_new(NULL, NULL);
        GtkWidget *text=gtk_text_view_new();
        gchar *valid=g_utf8_make_valid(output->str, output->len);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), valid, -1);
        g_free(valid);
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), text);
        gtk_widget_set_size_request(scroll, 440, 160);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 8);
    }
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Pulse while updating or preparing, before APT has measurable progress. */
static gboolean pulse_progress(gpointer data)
{
    (void)data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress));
    return TRUE;
}

/* Read the documented status FD separately from stdout and stderr. */
static gboolean read_output(GIOChannel *channel, GIOCondition condition, gpointer data)
{
    guint index=GPOINTER_TO_UINT(data);
    GIOStatus state;
    gchar *line;
    gsize length;
    (void)condition;
    while ((state=g_io_channel_read_line(channel, &line, &length, NULL, NULL))==G_IO_STATUS_NORMAL)
    {
        if (index==2)
        {
            gchar **fields=g_strsplit(line, ":", 4);
            if (g_strv_length(fields)==4)
            {
                gboolean package_status=g_str_equal(fields[0], "pmstatus");
                gboolean download_status=g_str_equal(fields[0], "dlstatus");
                if (phase==INSTALL && (package_status || download_status))
                {
                    gdouble percent=g_ascii_strtod(fields[2], NULL);
                    gchar *text=g_strdup_printf("%s: %.0f%%", package_status ? "Installing" : "Downloading", percent);
                    if (pulse_timer) g_source_remove(pulse_timer);
                    pulse_timer=0;
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), CLAMP(percent/100, 0, 1));
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), text);
                    gtk_label_set_text(GTK_LABEL(status_label), g_strchomp(fields[3]));
                    g_free(text);
                }
                if (g_str_equal(fields[0], "pmerror")) g_string_append(output, fields[3]);
            }
            g_strfreev(fields);
        }
        else
        {
            g_string_append_len(output, line, length);
            if (output->len>65536) g_string_erase(output, 0, output->len-65536);
        }
        g_free(line);
    }
    if (state==G_IO_STATUS_AGAIN) return TRUE;
    pipes[index].watch=0;
    return FALSE;
}

/* Drain final diagnostics before testing APT's exit status or starting the application. */
static void apt_finished(GPid pid, gint status, gpointer data)
{
    GError *error=NULL;
    (void)data;
    if (cancelled) kill(-pid, SIGKILL);
    for (guint i=0; i<G_N_ELEMENTS(pipes); i++)
    {
        if (pipes[i].watch) g_source_remove(pipes[i].watch);
        pipes[i].watch=0;
        read_output(pipes[i].channel, G_IO_IN, GUINT_TO_POINTER(i));
        g_io_channel_shutdown(pipes[i].channel, FALSE, NULL);
        g_io_channel_unref(pipes[i].channel);
        pipes[i].channel=NULL;
    }
    g_spawn_close_pid(pid);
    apt_child=0;
    if (pulse_timer) g_source_remove(pulse_timer);
    pulse_timer=0;
    if (cancelled)
    {
        gtk_main_quit();
        return;
    }
    gboolean success=g_spawn_check_wait_status(status, &error);
    if (!success)
    {
        /* APT decides whether its existing lists can resolve and fetch the packages. */
        if (phase!=UPDATE && !updated)
        {
            gboolean missing_package=strstr(output->str, "Unable to locate package") || strstr(output->str, "has no installation candidate") || strstr(output->str, "was not found");
            gboolean missing_download=strstr(output->str, "Failed to fetch") && strstr(output->str, "404");
            if (missing_package || missing_download)
            {
                g_error_free(error);
                phase=UPDATE;
                start_apt(NULL);
                return;
            }
        }
        g_string_append_printf(output, "\n%s\n", error->message);
        show_error(phase==UPDATE ? "Could not update package lists." : "Package installation failed.");
        g_error_free(error);
        gtk_main_quit();
        return;
    }
    if (error) g_error_free(error);
    if (phase==UPDATE)
    {
        updated=TRUE;
        phase=INSTALL;
        start_apt(NULL);
        return;
    }
    launch_command();
    gchar *message=g_strdup_printf("Could not start %s: %s", command[0], g_strerror(errno));
    show_error(message);
    g_free(message);
    gtk_main_quit();
}

/* Run APT asynchronously, keeping fd 3 for progress and the original application environment. */
static gboolean start_apt(gpointer data)
{
    GError *error=NULL;
    GPid child;
    gint status_pipe[2], fds[3], target=3;
    gchar *common[]={"/usr/bin/apt-get", "--no-install-recommends",
        "-o", "APT::Status-Fd=3", "-o", "Dpkg::Use-Pty=0", "-o", "APT::Color=0",
        "-o", "Dpkg::Options::=--force-confdef", "-o", "Dpkg::Options::=--force-confold", NULL};
    GPtrArray *arguments=g_ptr_array_new();
    gchar **environment=g_get_environ();
    (void)data;
    environment=g_environ_setenv(environment, "DEBIAN_FRONTEND", "noninteractive", TRUE);
    environment=g_environ_setenv(environment, "LC_ALL", "C.UTF-8", TRUE);
    for (guint i=0; common[i]; i++) g_ptr_array_add(arguments, common[i]);
    g_ptr_array_add(arguments, "-qq");
    g_ptr_array_add(arguments, "-y");
    if (phase==INSTALL)
    {
        g_ptr_array_add(arguments, "--no-remove");
        g_ptr_array_add(arguments, "install");
        g_ptr_array_add(arguments, "--");
        for (guint i=0; packages[i]; i++) g_ptr_array_add(arguments, packages[i]);
    }
    else
    {
        g_ptr_array_add(arguments, "-o");
        g_ptr_array_add(arguments, "APT::Update::Error-Mode=any");
        g_ptr_array_add(arguments, "update");
    }
    g_ptr_array_add(arguments, NULL);
    g_string_truncate(output, 0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), phase==UPDATE ? "Updating package lists..." : "Preparing installation...");
    gtk_label_set_text(GTK_LABEL(status_label), "Please wait...");
    if (pipe(status_pipe)!=0)
    {
        show_error(g_strerror(errno));
        gtk_main_quit();
    }
    else
    {
        gboolean started=g_spawn_async_with_pipes_and_fds(NULL, (const gchar *const *)arguments->pdata,
            (const gchar *const *)environment, G_SPAWN_DO_NOT_REAP_CHILD, setup_apt, NULL,
            -1, -1, -1, &status_pipe[1], &target, 1, &child, NULL, &fds[0], &fds[1], &error);
        close(status_pipe[1]);
        if (!started)
        {
            close(status_pipe[0]);
            show_error(error->message);
            g_error_free(error);
            gtk_main_quit();
        }
        else
        {
            apt_child=child;
            fds[2]=status_pipe[0];
            for (guint i=0; i<G_N_ELEMENTS(pipes); i++)
            {
                pipes[i].channel=g_io_channel_unix_new(fds[i]);
                g_io_channel_set_encoding(pipes[i].channel, NULL, NULL);
                g_io_channel_set_flags(pipes[i].channel, G_IO_FLAG_NONBLOCK, NULL);
                pipes[i].watch=g_io_add_watch(pipes[i].channel, G_IO_IN|G_IO_HUP|G_IO_ERR, read_output, GUINT_TO_POINTER(i));
            }
            pulse_timer=g_timeout_add(100, pulse_progress, NULL);
            g_child_watch_add(child, apt_finished, NULL);
        }
    }
    g_strfreev(environment);
    g_ptr_array_free(arguments, TRUE);
    return FALSE;
}

/* Create the GTK2 installer window and start installation without confirmation. */
int main(int argc, char **argv)
{
    GError *error=NULL;
    GdkPixbuf *icon;
    if (argc>2 && g_str_equal(argv[1], "--user") && g_str_equal(argv[2], "guest"))
    {
        guest=TRUE;
        argc-=2;
        argv+=2;
    }
    if (argc<5)
    {
        fprintf(stderr, "Usage: fbinstall [--user guest] ICON NAME \"PACKAGE...\" \"COMMAND [ARGUMENT...]\" [ARGUMENT...]\n");
        return 2;
    }
    g_set_prgname("fbinstall");
    if (!g_shell_parse_argv(argv[4], NULL, &command, &error))
    {
        fprintf(stderr, "%s\n", error->message);
        return 2;
    }
    guint count=g_strv_length(command);
    command=g_realloc_n(command, count+argc-4, sizeof(gchar *));
    for (gint i=5; i<argc; i++) command[count++]=g_strdup(argv[i]);
    command[count]=NULL;
    gchar *program=g_find_program_in_path(command[0]);
    if (program)
    {
        g_free(program);
        launch_command();
        fprintf(stderr, "Could not start %s: %s\n", command[0], g_strerror(errno));
        return 1;
    }
    gtk_init(NULL, NULL);
    if (geteuid()!=0)
    {
        show_error("Package installation requires root privileges.");
        return 1;
    }
    if (!g_shell_parse_argv(argv[3], NULL, &packages, &error))
    {
        show_error(error->message);
        g_error_free(error);
        return 1;
    }
    if (strchr(argv[1], '/') || g_file_test(argv[1], G_FILE_TEST_IS_REGULAR)) icon=gdk_pixbuf_new_from_file_at_scale(argv[1], 64, 64, TRUE, &error);
    else icon=gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), argv[1], 64, GTK_ICON_LOOKUP_FORCE_SIZE, &error);
    if (!icon)
    {
        show_error(error->message);
        g_error_free(error);
        return 1;
    }
    window=gtk_dialog_new();
    gtk_dialog_set_has_separator(GTK_DIALOG(window), FALSE);
    gtk_window_set_title(GTK_WINDOW(window), "Installing software");
    /* Keep the window frame centered as progress text changes its size. */
    gtk_window_set_gravity(GTK_WINDOW(window), GDK_GRAVITY_CENTER);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_icon(GTK_WINDOW(window), icon);
    g_signal_connect(window, "delete-event", G_CALLBACK(cancel_install), NULL);
    GtkWidget *box=gtk_vbox_new(FALSE, 12);
    GtkWidget *header=gtk_hbox_new(FALSE, 16);
    GtkWidget *image=gtk_image_new_from_pixbuf(icon);
    gchar *message=g_strdup_printf("%s will be downloaded and installed, please wait...", argv[2]);
    GtkWidget *label=gtk_label_new(message);
    g_free(message);
    g_object_unref(icon);
    gtk_widget_set_size_request(image, 64, 64);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_widget_set_size_request(label, 320, -1);
    gtk_box_pack_start(GTK_BOX(header), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);
    status_label=gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(status_label), 48);
    gtk_misc_set_alignment(GTK_MISC(status_label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);
    progress=gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), progress, FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(window), 16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(window))), box, TRUE, TRUE, 0);
    output=g_string_new(NULL);
    gtk_widget_show_all(window);
    gtk_widget_hide(gtk_dialog_get_action_area(GTK_DIALOG(window)));
    g_idle_add(start_apt, NULL);
    gtk_main();
    return 1;
}
