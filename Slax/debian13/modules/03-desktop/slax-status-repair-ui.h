/* The broker publishes a new root-owned log inode for each serialized check. */
static GtkWidget *repair_window,*repair_text,*repair_status,*repair_spinner,*repair_close,*repair_attempt,*repair_restart;
static JsonObject *repair_result,*repair_device;
static GString *repair_bytes;
static guint repair_timer;
static gboolean repair_confirming;
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

/* Closing the viewer cannot interrupt a filesystem tool halfway through its writes. */
static gboolean repair_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)event;(void)data;if (!repair_timer) {repair_reply(FALSE);gtk_widget_hide(widget);}return TRUE;
}
/* Retain the finished report when its viewer is dismissed. */
static void repair_dismiss(GtkButton *button, gpointer data) { (void)button;(void)data;repair_reply(FALSE);gtk_widget_hide(repair_window); }

/* Btrfs's general repair mode requires a separate decision after reviewing its check. */
static void repair_confirm_attempt(GtkButton *button, gpointer data)
{
    (void)button;(void)data;
    repair_confirming=TRUE;
    gboolean accepted=confirm("Attempt Btrfs repair","Attempt Btrfs repair?","The read-only check did not finish cleanly. Review its report first: a tool or I/O failure does not necessarily mean filesystem corruption. Btrfs repair can cause further damage and unrecoverable data loss. Back up important data before continuing; expert guidance is recommended. The filesystem stays blocked unless the repair and final read-only check both succeed.","Attempt repair");
    repair_confirming=FALSE;
    if (accepted) repair_start(TRUE);
}

/* Keep the completed report available while the user decides when to restart. */
static void repair_confirm_restart(GtkButton *button, gpointer data)
{
    (void)button;(void)data;
    char *text=g_strdup_printf("The filesystem on %s passed its final check. Slax can use the drive after restarting.\n\nThis RAM run will not be saved. Copy important files to another drive before restarting.",str(repair_result,"device"));
    if (!restart_request && confirm("Filesystem check completed","Restart Slax?",text,"Restart")) {restart_request=command("storage.repair",FALSE);start_restart();}
    g_free(text);
}

/* Use an ordinary resizable GTK window for the merged stdout and stderr stream. */
static void repair_viewer(void)
{
    repair_window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(repair_window),"Filesystem check and repair");
    gtk_window_set_default_size(GTK_WINDOW(repair_window),800,480);gtk_widget_set_size_request(repair_window,540,320);gtk_window_set_position(GTK_WINDOW(repair_window),GTK_WIN_POS_CENTER);
    g_signal_connect(repair_window,"delete-event",G_CALLBACK(repair_delete),NULL);
    GtkWidget *body=box(GTK_ORIENTATION_VERTICAL,12);gtk_container_set_border_width(GTK_CONTAINER(body),16);gtk_container_add(GTK_CONTAINER(repair_window),body);
    GtkWidget *heading=box(GTK_ORIENTATION_HORIZONTAL,10);repair_spinner=gtk_spinner_new();pack(heading,repair_spinner,FALSE);
    repair_status=paragraph("");pack(heading,repair_status,TRUE);pack(body,heading,FALSE);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),GTK_SHADOW_IN);pack(body,scroll,TRUE);
    repair_text=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(repair_text),FALSE);gtk_text_view_set_monospace(GTK_TEXT_VIEW(repair_text),TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(repair_text),8);gtk_text_view_set_right_margin(GTK_TEXT_VIEW(repair_text),8);gtk_container_add(GTK_CONTAINER(scroll),repair_text);
    pack(body,paragraph("Checker output (stdout and stderr). Full report: /run/slax-status/fsck.log"),FALSE);
    GtkWidget *footer=box(GTK_ORIENTATION_HORIZONTAL,10);pack(body,footer,FALSE);
    repair_attempt=button("Attempt Btrfs repair…",NULL,NULL);g_signal_connect(repair_attempt,"clicked",G_CALLBACK(repair_confirm_attempt),NULL);pack(footer,repair_attempt,FALSE);
    repair_restart=button("Restart…",NULL,NULL);g_signal_connect(repair_restart,"clicked",G_CALLBACK(repair_confirm_restart),NULL);pack(footer,repair_restart,FALSE);
    GtkWidget *spacer=box(GTK_ORIENTATION_HORIZONTAL,0);pack(footer,spacer,TRUE);
    repair_close=button("Close",NULL,NULL);g_signal_connect(repair_close,"clicked",G_CALLBACK(repair_dismiss),NULL);pack(footer,repair_close,FALSE);
    gtk_widget_show_all(body);repair_bytes=g_string_new(NULL);
}

/* Reset the viewer before queueing the job, without displaying the previous job's log. */
static void repair_start(gboolean attempt)
{
    if (repair_timer || mutating || restart_request) return;
    if (!repair_window) repair_viewer();
    g_clear_pointer(&repair_result,json_object_unref);g_string_truncate(repair_bytes,0);
    repair_previous=open("/run/slax-status/fsck.log",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(repair_text)),"",0);
    char *text=g_strdup_printf("%s %s…",attempt ? "Repairing" : "Checking",str(repair_device,"device"));set_label(repair_status,text);g_free(text);
    gtk_widget_hide(repair_attempt);gtk_widget_hide(repair_restart);gtk_widget_set_sensitive(repair_close,FALSE);gtk_spinner_start(GTK_SPINNER(repair_spinner));
    hide_popup();gtk_widget_hide(notice.window);gtk_window_present(GTK_WINDOW(repair_window));
    repair_timer=g_timeout_add(75,repair_output,NULL);
    JsonObject *request=command("storage.repair",FALSE);json_object_set_boolean_member(request,"repair",attempt);
    if (*str(repair_device,"placeId")) json_object_set_string_member(request,"placeId",str(repair_device,"placeId"));
    send_request(request,TRUE);
}

/* Every entry point uses this confirmation before the shared checker can write. */
static gboolean repair_offer(JsonObject *device)
{
    if (repair_timer) {gtk_window_present(GTK_WINDOW(repair_window));return FALSE;}
    if (!device) return FALSE;
    if (mutating || restart_request || repair_confirming) return FALSE;
    if (!flag(device,"canRepair")) {message("Filesystem check",str(device,"error"));return FALSE;}
    JsonObject *target=json_object_ref(device);
    gtk_widget_hide(notice.window);
    char *question=g_strdup_printf("Check and repair %s?",str(device,"device"));
    gboolean btrfs=!strcmp(str(device,"filesystem"),"btrfs"),interrupted=*str(device,"issue");
    const char *check=btrfs
        ? "Slax will run a read-only Btrfs check. If it reports errors, review the log before deciding whether to attempt repair."
        : "Slax will check the filesystem and repair errors it can fix safely. Repairs may recover or truncate damaged files.";
    char *text=g_strdup_printf("%s\n\n%s",check,interrupted
        ? "Close files and terminals using this drive so Slax can unmount it. Saved changes will be re-enabled only after a successful final check. Your current run stays in RAM."
        : "The drive must remain unmounted during the check.");
    repair_confirming=TRUE;
    gboolean accepted=confirm("Check and repair filesystem",question,text,"Check and repair");g_free(question);g_free(text);
    repair_confirming=FALSE;
    if (accepted && !mutating && !restart_request)
    {
        g_clear_pointer(&repair_device,json_object_unref);repair_device=target;repair_start(FALSE);return TRUE;
    }
    json_object_unref(target);return FALSE;
}

/* Interactive requests queue behind polling and use the broker's fresh catalogue. */
static gboolean repair_select(const char *id,Window client)
{
    if (repair_timer) {gtk_window_present(GTK_WINDOW(repair_window));return FALSE;}
    if (repair_client) return FALSE;
    if (mutating || restart_request || repair_confirming) return FALSE;
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
    gtk_spinner_stop(GTK_SPINNER(repair_spinner));gtk_widget_set_sensitive(repair_close,TRUE);
}

/* A failed job keeps its report visible and never offers a successful restart. */
static void repair_failed(const char *detail)
{
    repair_finished();repair_reply(FALSE);set_label(repair_status,"Check or repair failed. The filesystem remains blocked.");repair_append("\n");repair_append(detail);repair_append("\n");
}

/* The broker distinguishes a failed read-only Btrfs check from completed recovery. */
static void repair_completed(JsonObject *result)
{
    repair_finished();repair_result=json_object_ref(result);
    gboolean needs_repair=number(result,"checkExitCode")!=0;
    set_label(repair_status,needs_repair ? "Read-only check did not finish cleanly. Review the report before attempting repair. The filesystem remains blocked." : "Final check passed. You can open the drive in the file browser.");
    gboolean interrupted=*str(result,"issue");
    gtk_widget_set_visible(repair_attempt,needs_repair);gtk_widget_set_visible(repair_restart,!needs_repair && interrupted);
    if (!needs_repair && interrupted) {json_object_remove_member(state,"recovery");gtk_widget_hide(notice.window);update_ui();}
    if (!needs_repair) repair_reply(!interrupted);
}
