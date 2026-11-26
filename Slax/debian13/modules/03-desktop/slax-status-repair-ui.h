/* The broker publishes a new root-owned log inode for each serialized check. */
typedef enum {REPAIR_NONE,REPAIR_CHECK,REPAIR_REVIEW_BTRFS,REPAIR_WRITE_BTRFS,REPAIR_RESTART} RepairAction;
static GtkWidget *repair_window,*repair_text,*repair_heading,*repair_status,*repair_progress,*repair_details,*repair_primary,*repair_close;
static JsonObject *repair_device;
static RepairAction repair_action;
static GString *repair_bytes;
static guint repair_timer;
static int repair_log=-1,repair_previous=-1;
static void repair_start(gboolean attempt);

/* Keep the latest output visible without rebuilding the text buffer on every poll. */
static void repair_append(const char *text)
{
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(repair_text));GtkTextIter end,start;
    gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_insert(buffer,&end,text,-1);
    if (gtk_text_buffer_get_char_count(buffer)>1048576)
    {
        gtk_text_buffer_get_start_iter(buffer,&start);gtk_text_buffer_get_iter_at_offset(buffer,&end,524288);
        gtk_text_buffer_delete(buffer,&start,&end);
    }
    gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_move_mark(buffer,gtk_text_buffer_get_insert(buffer),&end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(repair_text),gtk_text_buffer_get_insert(buffer));
}

/* Preserve UTF-8 split across reads; invalid tool bytes are shown as replacement characters. */
static gboolean repair_output(gpointer data)
{
    (void)data;
    if (repair_timer) gtk_progress_bar_pulse(GTK_PROGRESS_BAR(repair_progress));
    if (repair_log<0)
    {
        int fd=open("/run/slax-status/fsck.log",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat next,old;
        gboolean fresh=fd>=0 && !fstat(fd,&next) && S_ISREG(next.st_mode) && next.st_uid==0;
        if (fresh && repair_previous>=0 && !fstat(repair_previous,&old)) fresh=next.st_dev!=old.st_dev || next.st_ino!=old.st_ino;
        if (fresh) repair_log=fd;else if (fd>=0) close(fd);
    }
    char bytes[65536];ssize_t size=repair_log<0 ? 0 : read(repair_log,bytes,sizeof(bytes));
    if (size>0) g_string_append_len(repair_bytes,bytes,size);
    gsize complete=repair_bytes->len;
    if (complete && repair_timer)
    {
        char *last=g_utf8_find_prev_char(repair_bytes->str,repair_bytes->str+complete);
        if (last && g_utf8_get_char_validated(last,repair_bytes->str+complete-last)==(gunichar)-2) complete=last-repair_bytes->str;
    }
    if (complete)
    {
        char *valid=g_utf8_make_valid(repair_bytes->str,complete);repair_append(valid);g_free(valid);g_string_erase(repair_bytes,0,complete);
    }
    return G_SOURCE_CONTINUE;
}

/* Preserve the chosen width and let GTK fit the height to the current step. */
static void repair_fit(void)
{
    int width;gtk_window_get_size(GTK_WINDOW(repair_window),&width,NULL);
    gtk_window_resize(GTK_WINDOW(repair_window),width,1);
}

/* Keep the safe choice as the keyboard default whenever a decision is offered. */
static void repair_controls(RepairAction action, const char *primary, const char *secondary)
{
    repair_action=action;
    if (primary) gtk_button_set_label(GTK_BUTTON(repair_primary),primary);
    if (secondary) gtk_button_set_label(GTK_BUTTON(repair_close),secondary);
    gtk_widget_set_visible(repair_primary,primary!=NULL);gtk_widget_set_visible(repair_close,secondary!=NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(repair_window),secondary ? GTK_RESPONSE_CANCEL : GTK_RESPONSE_NONE);
    gtk_widget_set_visible(gtk_dialog_get_action_area(GTK_DIALOG(repair_window)),primary || secondary);
    gtk_widget_set_margin_bottom(gtk_bin_get_child(GTK_BIN(repair_details)),action==REPAIR_RESTART ? 28 : 0);
    if (secondary) gtk_widget_grab_focus(repair_close);
    if (!gtk_expander_get_expanded(GTK_EXPANDER(repair_details))) repair_fit();
}

/* Return to the read-only report when the user cancels the destructive Btrfs step. */
static void repair_btrfs_choice(void)
{
    set_label(repair_heading,"Btrfs check found a problem");
    set_label(repair_status,"The read-only check did not finish cleanly. The filesystem remains blocked. Review the details before attempting repair.");
    repair_controls(REPAIR_REVIEW_BTRFS,"Attempt Btrfs repair…","Close");
}

/* A recovered drive can be used after restarting, but this session still runs in RAM. */
static void repair_restart_choice(const char *error)
{
    if (error) set_label(repair_heading,"Could not restart Slax");
    char *text=g_strdup_printf("%s%sThe drive with your saved changes (%s) is ready to use again. Restart Slax to continue from the changes already saved there.\n\nSlax started this session in memory (RAM) because the drive needed checking. Changes made in this temporary session will be lost when you restart or shut down. Copy any files you want to keep to a drive first.",
        error ? error : "",error ? "\n\n" : "",str(repair_device,"device"));
    set_label(repair_status,text);g_free(text);
    repair_controls(REPAIR_RESTART,"Restart Slax","Continue in RAM");
}

/* Advance the same dialog from confirmation through results, without nesting another window. */
static void repair_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)data;
    if (repair_timer || restart_request) return;
    if (response!=GTK_RESPONSE_ACCEPT)
    {
        if (response==GTK_RESPONSE_CANCEL && repair_action==REPAIR_WRITE_BTRFS) {repair_btrfs_choice();return;}
        repair_reply(FALSE);gtk_widget_hide(GTK_WIDGET(dialog));return;
    }
    if (mutating) return;
    if (repair_action==REPAIR_CHECK || repair_action==REPAIR_WRITE_BTRFS)
        repair_start(repair_action==REPAIR_WRITE_BTRFS);
    else if (repair_action==REPAIR_REVIEW_BTRFS)
    {
        set_label(repair_heading,"Attempt Btrfs repair?");
        set_label(repair_status,"A tool or I/O failure does not necessarily mean filesystem corruption. Btrfs repair can cause further damage and unrecoverable data loss. Back up important data before continuing. Expert guidance is recommended. The filesystem stays blocked unless repair and a final read-only check both succeed.");
        repair_controls(REPAIR_WRITE_BTRFS,"Attempt repair","Cancel");
    }
    else if (repair_action==REPAIR_RESTART)
    {
        restart_request=command("storage.repair",FALSE);
        set_label(repair_heading,"Restarting Slax…");repair_controls(REPAIR_NONE,NULL,NULL);
        start_restart();
    }
}

/* GtkDialog emits a delete response before its default handler destroys the window. */
static gboolean repair_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget;(void)event;(void)data;return TRUE;
}

/* Keep the native details affordance clear in both its collapsed and expanded forms. */
static void repair_details_changed(GtkExpander *details, GParamSpec *property, gpointer data)
{
    (void)property;(void)data;
    gboolean expanded=gtk_expander_get_expanded(details);
    gtk_expander_set_label(details,expanded ? "Hide details" : "Show details");
    if (!expanded) repair_fit();
}

/* Use a compact GTK dialog with a resizable, scrollable log on demand. */
static void repair_viewer(void)
{
    repair_window=gtk_dialog_new();gtk_window_set_titlebar(GTK_WINDOW(repair_window),NULL);
    gtk_window_set_title(GTK_WINDOW(repair_window),"Filesystem check and repair");
    gtk_window_set_default_size(GTK_WINDOW(repair_window),540,-1);gtk_window_set_position(GTK_WINDOW(repair_window),GTK_WIN_POS_CENTER);
    /* Measure wrapped text at a usable minimum width to keep the dialog compact. */
    gtk_widget_set_size_request(repair_window,480,-1);
    g_signal_connect(repair_window,"response",G_CALLBACK(repair_response),NULL);
    g_signal_connect(repair_window,"delete-event",G_CALLBACK(repair_delete),NULL);
    GtkWidget *body=gtk_dialog_get_content_area(GTK_DIALOG(repair_window));
    gtk_container_set_border_width(GTK_CONTAINER(body),16);gtk_box_set_spacing(GTK_BOX(body),12);
    repair_heading=paragraph("");gtk_style_context_add_class(gtk_widget_get_style_context(repair_heading),"title");pack(body,repair_heading,FALSE);
    repair_status=paragraph("");gtk_label_set_max_width_chars(GTK_LABEL(repair_status),54);pack(body,repair_status,FALSE);
    repair_progress=gtk_progress_bar_new();pack(body,repair_progress,FALSE);
    repair_details=gtk_expander_new("Show details");gtk_expander_set_resize_toplevel(GTK_EXPANDER(repair_details),TRUE);pack(body,repair_details,TRUE);
    g_signal_connect(repair_details,"notify::expanded",G_CALLBACK(repair_details_changed),NULL);
    GtkWidget *log=box(GTK_ORIENTATION_VERTICAL,8);gtk_container_add(GTK_CONTAINER(repair_details),log);
    /* Let the scroll area use spare dialog height instead of leaving it above the buttons. */
    gtk_widget_set_margin_top(log,6);gtk_widget_set_vexpand(log,TRUE);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),180);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),GTK_SHADOW_IN);pack(log,scroll,TRUE);
    repair_text=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(repair_text),FALSE);gtk_text_view_set_monospace(GTK_TEXT_VIEW(repair_text),TRUE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(repair_text),6);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(repair_text),8);gtk_text_view_set_right_margin(GTK_TEXT_VIEW(repair_text),8);gtk_container_add(GTK_CONTAINER(scroll),repair_text);
    pack(log,paragraph("Full report: /run/slax-status/fsck.log"),FALSE);
    repair_primary=gtk_dialog_add_button(GTK_DIALOG(repair_window),"Check and repair",GTK_RESPONSE_ACCEPT);
    repair_close=gtk_dialog_add_button(GTK_DIALOG(repair_window),"Cancel",GTK_RESPONSE_CANCEL);
    gtk_widget_show_all(body);repair_bytes=g_string_new(NULL);
}

/* Reset the report before queueing the job, so it can never show an earlier job's log. */
static void repair_start(gboolean attempt)
{
    g_string_truncate(repair_bytes,0);
    repair_previous=open("/run/slax-status/fsck.log",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(repair_text)),"",0);
    char *text=g_strdup_printf("%s %s…",attempt ? "Repairing" : "Checking",str(repair_device,"device"));
    set_label(repair_heading,text);g_free(text);
    set_label(repair_status,"Keep the drive connected until the check finishes.");
    gtk_expander_set_expanded(GTK_EXPANDER(repair_details),FALSE);
    gtk_widget_show(repair_details);gtk_widget_show(repair_progress);
    repair_controls(REPAIR_NONE,NULL,NULL);
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(repair_progress));
    repair_timer=g_timeout_add(75,repair_output,NULL);
    JsonObject *request=command("storage.repair",FALSE);json_object_set_boolean_member(request,"repair",attempt);
    if (*str(repair_device,"placeId")) json_object_set_string_member(request,"placeId",str(repair_device,"placeId"));
    send_request(request,TRUE);
}

/* Preserve the current check or pending confirmation if another entry point is invoked. */
static gboolean repair_in_use(void)
{
    if (!repair_window || !gtk_widget_get_visible(repair_window)) return FALSE;
    gboolean awaiting_check=repair_action==REPAIR_CHECK;
    gboolean awaiting_btrfs=repair_action==REPAIR_REVIEW_BTRFS || repair_action==REPAIR_WRITE_BTRFS;
    if (!repair_timer && !awaiting_check && !awaiting_btrfs) return FALSE;
    gtk_window_present(GTK_WINDOW(repair_window));return TRUE;
}

/* Every entry point shows the same confirmation before the checker can write. */
static gboolean repair_offer(JsonObject *device)
{
    if (!device || mutating || restart_request || repair_in_use()) return FALSE;
    if (!flag(device,"canRepair")) {message("Filesystem check",str(device,"error"));return FALSE;}
    if (!repair_window) repair_viewer();
    g_clear_pointer(&repair_device,json_object_unref);repair_device=json_object_ref(device);
    hide_popup();gtk_widget_hide(notice.window);
    char *question=g_strdup_printf("Check and repair %s?",str(device,"device"));
    set_label(repair_heading,question);g_free(question);
    gboolean btrfs=!strcmp(str(device,"filesystem"),"btrfs"),interrupted=*str(device,"issue");
    const char *check=btrfs
        ? "Slax will run a read-only Btrfs check. If it reports errors, review the log before deciding whether to attempt repair."
        : "Slax will check the filesystem and repair errors it can fix safely. Repairs may recover or truncate damaged files.";
    char *text=g_strdup_printf("%s\n\n%s",check,interrupted
        ? "Close files and terminals using this drive so Slax can unmount it. Your current session stays in RAM."
        : "The drive must remain unmounted during the check.");
    set_label(repair_status,text);g_free(text);
    gtk_expander_set_expanded(GTK_EXPANDER(repair_details),FALSE);
    gtk_widget_hide(repair_details);gtk_widget_hide(repair_progress);
    repair_controls(REPAIR_CHECK,"Check and repair","Cancel");
    gtk_window_set_transient_for(GTK_WINDOW(repair_window),gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL);
    gtk_window_present(GTK_WINDOW(repair_window));return TRUE;
}

/* Interactive requests queue behind polling and use the broker's fresh catalogue. */
static gboolean repair_select(const char *id,Window client)
{
    if (repair_client || mutating || restart_request || repair_in_use()) return FALSE;
    repair_client=client;
    JsonObject *request=command("storage.check",FALSE);json_object_set_string_member(request,"placeId",id);send_request(request,TRUE);
    return TRUE;
}

/* Drain the final output before enabling Close or any follow-up action. */
static void repair_finished(void)
{
    if (repair_timer) g_source_remove(repair_timer);
    repair_timer=0;
    off_t before;
    do {before=repair_log<0 ? -1 : lseek(repair_log,0,SEEK_CUR);repair_output(NULL);} while (repair_log>=0 && lseek(repair_log,0,SEEK_CUR)!=before);
    if (repair_log>=0) close(repair_log);
    if (repair_previous>=0) close(repair_previous);
    repair_log=repair_previous=-1;
    gtk_widget_hide(repair_progress);
}

/* A failed job keeps its report available and leaves the device blocked. */
static void repair_failed(const char *detail)
{
    repair_finished();repair_reply(FALSE);
    set_label(repair_heading,"Filesystem check failed");set_label(repair_status,detail);
    repair_append("\n");repair_append(detail);repair_append("\n");
    repair_controls(REPAIR_NONE,NULL,"Close");
}

/* The broker distinguishes a failed read-only Btrfs check from completed recovery. */
static void repair_completed(JsonObject *result)
{
    repair_finished();
    if (number(result,"checkExitCode")!=0) {repair_btrfs_choice();return;}
    gboolean read_only=!strcmp(str(result,"filesystem"),"btrfs") && !flag(result,"repaired");
    set_label(repair_heading,read_only ? "Disk check succeeded" : "Disk check and repair succeeded");
    gboolean interrupted=*str(result,"issue");
    if (interrupted)
    {
        repair_restart_choice(NULL);
        json_object_remove_member(state,"recovery");gtk_widget_hide(notice.window);update_ui();
    }
    else
    {
        set_label(repair_status,"The drive is ready to use again. You can open it in File Manager.");
        repair_controls(REPAIR_NONE,NULL,"Close");
    }
    repair_reply(!interrupted);
}
