/* Slax's native status tray and profile manager. Storage operations belong to slax-statusd. */
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <X11/Xatom.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

typedef struct { GtkWidget *window, *body; int arrow; gboolean bottom; } Bubble;
typedef struct { JsonObject *point; int depth; gboolean path; GtkWidget *widget; } HistoryRow;
typedef struct { JsonObject *request; char *op, *profile; gboolean mutation; } Request;
static JsonObject *state, *history, *pending_request;
static char *selected, *selected_point, *history_profile, *profiles_signature, *history_signature;
static char *connection_error, *last_warning;
static gboolean requesting, mutating, updating, manager_dirty=TRUE, compact, stacked;
static gboolean discovery_pending, new_profile_pending;
static gint64 snooze_until;
static GtkWidget *tray, *tray_icon, *manager, *manager_body, *sidebar, *profile_list, *profile_scroll, *profile_combo;
static GtkWidget *profile_actions, *profile_rename, *profile_delete, *profile_new, *profile_heading;
static GtkWidget *selected_profile_row;
static GtkWidget *history_box, *history_scroll, *history_list, *inspector, *inspector_scroll, *history_layout, *main_layout, *context_box;
static GtkWidget *manager_source, *manager_storage, *manager_next, *manager_cancel, *manager_space, *manager_status;
static GtkWidget *popup_title, *popup_source, *popup_storage, *popup_space, *popup_meter, *popup_save, *popup_choice;
static GtkWidget *popup_help, *popup_next, *popup_cancel, *popup_status, *notice_title, *notice_message, *show_removed;
static Bubble popup, notice;
static GtkStyleContext *bubble_style;
static GPtrArray *rows;
static HistoryRow *hover_row;
static gboolean removed_visible;
static gboolean reveal_current=TRUE;
static Display *display;
static Window command_window;
static Atom tray_selection, manager_atom, command_atom;

static void refresh(void);
static void update_ui(void);
static void show_manager(void);
static void render_history(void);
static void render_inspector(void);
static void dock_tray(void);
static void send_request(JsonObject *request, gboolean mutation);
static void action(GtkButton *button, gpointer data);
static void name_dialog(const char *op);

/* Optional JSON values stay absent instead of acquiring guessed UI state. */
static const char *str(JsonObject *object, const char *key)
{
    JsonNode *node=object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node)==G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gboolean flag(JsonObject *object, const char *key) { return object && json_object_has_member(object, key) && json_object_get_boolean_member(object, key); }
static gint64 number(JsonObject *object, const char *key) { return object && json_object_has_member(object, key) ? json_object_get_int_member(object, key) : 0; }
static JsonObject *object(JsonObject *parent, const char *key)
{
    JsonNode *node=parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const char *key)
{
    JsonNode *node=parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static guint count(JsonArray *items) { return items ? json_array_get_length(items) : 0; }

/* Look up exact opaque identities; numeric profile directory names are not globally unique. */
static JsonObject *find(JsonArray *items, const char *key, const char *value)
{
    for (guint i=0; value && i<count(items); i++)
    {
        JsonObject *item=json_array_get_object_element(items, i);
        if (!strcmp(str(item,key),value)) return item;
    }
    return NULL;
}
static JsonObject *profile(void) { return find(array(state,"profiles"),"id",selected); }
static JsonObject *point(void) { return find(array(history,"snapshots"),"id",selected_point); }
static JsonObject *run(void) { return object(state,"run"); }

/* UI labels use the established numeric fallback, while all actions keep the opaque ID. */
static char *profile_name(JsonObject *item)
{
    return *str(item,"name") ? g_strdup(str(item,"name")) : g_strdup_printf("Profile #%s",str(item,"numericId"));
}
/* Linux device identities remain visible before optional, potentially long filesystem labels. */
static char *drive_name(JsonObject *drive)
{
    return *str(drive,"label") ? g_strdup_printf("%s (%s)",str(drive,"device"),str(drive,"label")) : g_strdup(str(drive,"device"));
}
static const char *point_name(JsonObject *item) { return flag(item,"automatic") ? "Automatic restore point" : str(item,"name"); }
static char *size_text(gint64 bytes) { return g_format_size_full(MAX((gint64)0,bytes),G_FORMAT_SIZE_DEFAULT); }
static char *date_text(gint64 seconds)
{
    GDateTime *date=g_date_time_new_from_unix_local(seconds);
    char *text=date ? g_date_time_format(date,"%e %b · %H:%M") : g_strdup("");
    if (date) g_date_time_unref(date);
    return text;
}

/* Use plain GTK text, never treat profile names or backend errors as markup. */
static GtkWidget *label(const char *text, const char *css)
{
    GtkWidget *widget=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget),0);
    gtk_label_set_ellipsize(GTK_LABEL(widget),PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(widget),TRUE);
    if (css) gtk_style_context_add_class(gtk_widget_get_style_context(widget),css);
    return widget;
}
static void set_label(GtkWidget *widget, const char *text)
{
    gtk_label_set_text(GTK_LABEL(widget),text ? text : "");
    gtk_widget_set_tooltip_text(widget,text && *text ? text : NULL);
}
static void clear_box(GtkWidget *widget)
{
    GList *children=gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item=children; item; item=item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}
static GtkWidget *box(GtkOrientation orientation, int spacing) { return gtk_box_new(orientation,spacing); }
static void pack(GtkWidget *parent, GtkWidget *child, gboolean expand) { gtk_box_pack_start(GTK_BOX(parent),child,expand,expand,0); }

/* Release only the explicitly opened status popup's input grab. Notifications never grab. */
static void hide_popup(void)
{
    if (popup.window && gtk_widget_get_visible(popup.window))
    {
        gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(popup.window)));
        if (gtk_widget_has_grab(popup.window)) gtk_grab_remove(popup.window);
        gtk_widget_hide(popup.window);
    }
}

/* Dialogs appear only after explicit actions and use the ordinary native window frame. */
static void message(const char *title, const char *text)
{
    hide_popup();
    GtkWidget *dialog=gtk_message_dialog_new(gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",text);
    gtk_dialog_run(GTK_DIALOG(dialog));gtk_widget_destroy(dialog);
}

/* Send one complete JSON command; no shell or UI-supplied filesystem paths cross the boundary. */
static JsonObject *command(const char *op, gboolean with_profile)
{
    JsonObject *request=json_object_new();
    json_object_set_string_member(request,"op",op);
    if (with_profile && selected) json_object_set_string_member(request,"profileId",selected);
    return request;
}
static void request_free(gpointer data)
{
    Request *request=data;
    json_object_unref(request->request);g_free(request->op);g_free(request->profile);g_free(request);
}

/* Blocking socket work runs exclusively in GTask's worker, including JSON decoding. */
static void request_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    (void)source;
    Request *request=task_data;
    GError *error=NULL;
    GSocketClient *client=g_socket_client_new();
    g_socket_client_set_timeout(client,120);
    GSocketAddress *address=g_unix_socket_address_new("/run/slax-status/socket");
    GSocketConnection *connection=g_socket_client_connect(client,G_SOCKET_CONNECTABLE(address),cancel,&error);
    g_object_unref(address);g_object_unref(client);
    if (!connection) {g_task_return_error(task,error);return;}
    JsonNode *node=json_node_new(JSON_NODE_OBJECT);json_node_set_object(node,request->request);
    char *json=json_to_string(node,FALSE),*line=g_strconcat(json,"\n",NULL);
    json_node_free(node);g_free(json);
    gboolean written=g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),line,strlen(line),NULL,cancel,&error);
    g_free(line);
    JsonParser *parser=json_parser_new();
    if (written)
    {
        GDataInputStream *input=g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
        gsize length=0;
        char *reply=g_data_input_stream_read_line_utf8(input,&length,cancel,&error);
        if (reply) json_parser_load_from_data(parser,reply,length,&error);
        else if (!error) g_set_error_literal(&error,G_IO_ERROR,G_IO_ERROR_CLOSED,"The storage service closed the connection.");
        g_free(reply);g_object_unref(input);
    }
    g_io_stream_close(G_IO_STREAM(connection),NULL,NULL);g_object_unref(connection);
    JsonNode *root=json_parser_get_root(parser);
    if (!error && (!root || !JSON_NODE_HOLDS_OBJECT(root)))
        g_set_error_literal(&error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"The storage service returned an invalid reply.");
    if (error) g_task_return_error(task,error);
    else g_task_return_pointer(task,json_object_ref(json_node_get_object(root)),(GDestroyNotify)json_object_unref);
    g_object_unref(parser);
}

/* Keep JSON signatures only to avoid replacing rows during unchanged background refreshes. */
static char *signature(JsonNode *node) { return node ? json_to_string(node,FALSE) : g_strdup(""); }

/* Apply confirmed results on GTK's thread and reload authoritative state after every mutation. */
static void request_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;(void)data;
    Request *request=g_task_get_task_data(G_TASK(result));
    GError *error=NULL;
    JsonObject *reply=g_task_propagate_pointer(G_TASK(result),&error);
    requesting=FALSE;mutating=pending_request!=NULL;
    gboolean discovered=!strcmp(request->op,"state") && flag(request->request,"refresh");
    gboolean succeeded=!error && flag(reply,"ok");
    if (error || !flag(reply,"ok"))
    {
        const char *detail=error ? error->message : str(object(reply,"error"),"message");
        if (request->mutation) message("The action could not be completed",detail);
        else if (discovered && new_profile_pending)
        {
            new_profile_pending=FALSE;message("Cannot find profile drives",detail);
        }
        else if (!strcmp(request->op,"state")) {g_free(connection_error);connection_error=g_strdup(detail);}
        else if (!strcmp(request->op,"history") && !g_strcmp0(selected,request->profile))
        {
            hover_row=NULL;g_clear_pointer(&rows,g_ptr_array_unref);
            clear_box(history_list);gtk_container_add(GTK_CONTAINER(history_list),label(detail,"muted"));gtk_widget_show_all(history_list);
        }
        g_clear_error(&error);
    }
    else
    {
        JsonObject *value=object(reply,"result");
        if (!strcmp(request->op,"state"))
        {
            if (state) json_object_unref(state);
            state=json_object_ref(value);g_clear_pointer(&connection_error,g_free);
            if (!find(array(state,"profiles"),"id",selected))
            {
                g_free(selected);selected=g_strdup(str(run(),"profileId"));
                if (!*selected && count(array(state,"profiles"))) {g_free(selected);selected=g_strdup(str(json_array_get_object_element(array(state,"profiles"),0),"id"));}
                g_clear_pointer(&selected_point,g_free);manager_dirty=TRUE;reveal_current=TRUE;
                render_history();render_inspector();
            }
        }
        else if (!strcmp(request->op,"history") && !g_strcmp0(selected,request->profile))
        {
            char *current=signature(json_object_get_member(reply,"result"));
            gboolean changed=g_strcmp0(current,history_signature) || g_strcmp0(history_profile,selected);
            if (history) json_object_unref(history);
            history=json_object_ref(value);
            g_free(history_profile);history_profile=g_strdup(selected);
            g_free(history_signature);history_signature=current;
            if (changed || manager_dirty) {render_history();render_inspector();manager_dirty=FALSE;}
        }
        else if (request->mutation)
        {
            if (!strcmp(request->op,"profile.create")) {g_free(selected);selected=g_strdup(str(value,"profileId"));g_clear_pointer(&selected_point,g_free);}
            if (!strcmp(request->op,"point.create")) {g_free(selected_point);selected_point=g_strdup(str(value,"pointId"));}
            if (!strcmp(request->op,"point.delete")) g_clear_pointer(&selected_point,g_free);
            if (!strcmp(request->op,"files.open") && *str(value,"path"))
            {
                const char *argv[]={"pcmanfm",str(value,"path"),NULL};
                GSubprocess *process=g_subprocess_newv(argv,G_SUBPROCESS_FLAGS_NONE,&error);
                if (process) g_object_unref(process);
                else {message("Cannot open the file manager",error->message);g_clear_error(&error);}
            }
            manager_dirty=TRUE;
        }
    }
    if (reply) json_object_unref(reply);
    update_ui();
    if (discovered && succeeded && !discovery_pending && new_profile_pending)
    {
        new_profile_pending=FALSE;update_ui();name_dialog("profile.create");
    }
    if (pending_request) {JsonObject *queued=pending_request;pending_request=NULL;send_request(queued,TRUE);}
    else if (discovery_pending) refresh();
    else if (request->mutation) refresh();
    else if (!strcmp(request->op,"state") && gtk_widget_get_visible(manager) && profile())
        send_request(command("history",TRUE),FALSE);
    else if (!strcmp(request->op,"history") && g_strcmp0(request->profile,selected) && gtk_widget_get_visible(manager) && profile())
        send_request(command("history",TRUE),FALSE);
}

/* One request at a time also prevents conflicting UI mutations and stale poll replies. */
static void send_request(JsonObject *value, gboolean mutation)
{
    if (requesting)
    {
        if (mutation && !pending_request) {pending_request=value;mutating=TRUE;update_ui();}
        else json_object_unref(value);
        return;
    }
    Request *request=g_new0(Request,1);
    request->request=value;request->op=g_strdup(str(value,"op"));request->profile=g_strdup(str(value,"profileId"));request->mutation=mutation;
    requesting=TRUE;mutating=mutation;update_ui();
    GTask *task=g_task_new(NULL,NULL,request_done,NULL);
    g_task_set_task_data(task,request,request_free);g_task_run_in_thread(task,request_worker);g_object_unref(task);
}
/* Only an explicit manager/new-profile action authorizes discovering unmounted drives. */
static void refresh(void)
{
    if (requesting) return;
    JsonObject *request=command("state",FALSE);
    if (discovery_pending) {json_object_set_boolean_member(request,"refresh",TRUE);discovery_pending=FALSE;}
    send_request(request,FALSE);
}

/* Main-loop polling reads counters only; catalogue work is requested while the manager is visible. */
static gboolean poll_state(gpointer data) { (void)data;refresh();return G_SOURCE_CONTINUE; }

/* Give controls the same native relief and separate accessible labels as the mockup. */
static GtkWidget *button(const char *text, const char *icon, const char *op)
{
    GtkWidget *widget=icon ? gtk_button_new_from_icon_name(icon,GTK_ICON_SIZE_BUTTON) : gtk_button_new_with_label(text);
    if (icon) {gtk_widget_set_tooltip_text(widget,text);atk_object_set_name(gtk_widget_get_accessible(widget),text);gtk_style_context_add_class(gtk_widget_get_style_context(widget),"icon-button");}
    if (op) g_signal_connect(widget,"clicked",G_CALLBACK(action),(gpointer)op);
    return widget;
}
static GtkWidget *link_button(const char *text, const char *op)
{
    GtkWidget *widget=button(text,NULL,op);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),"link");
    return widget;
}
static GtkWidget *primary_button(const char *text, const char *op)
{
    GtkWidget *widget=button(text,NULL,op);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),"suggested-action");
    return widget;
}

/* Restore choices can refer to any profile; the main context always describes the running system. */
static char *next_text(void)
{
    JsonObject *next=object(state,"next");
    if (next)
    {
        JsonObject *target=find(array(state,"profiles"),"id",str(next,"profileId"));
        char *name=g_strdup(str(next,"label"));
        if (!*name) {g_free(name);name=target ? profile_name(target) : g_strdup("Selected profile");}
        char *text=NULL;
        if (*str(next,"pointId"))
        {
            char *date=date_text(number(next,"created"));
            text=g_strdup_printf("%s · %s · %s",name,*str(next,"pointName") ? str(next,"pointName") : "selected restore point",date);g_free(date);
        }
        else text=g_strdup_printf("%s · current state",name);
        g_free(name);return text;
    }
    if (!strcmp(str(run(),"kind"),"ram")) return g_strdup("Fresh temporary run");
    return g_strdup(flag(run(),"save") ? "Continue current state" : "Return to the start of this run");
}

/* Hold a stable width and reserve a single line for changing descriptions. */
static GtkWidget *location_row(const char *heading, GtkWidget **value)
{
    GtkWidget *row=box(GTK_ORIENTATION_HORIZONTAL,12),*name=label(heading,"muted");
    gtk_widget_set_size_request(name,98,-1);pack(row,name,FALSE);
    *value=label("Loading…",NULL);pack(row,*value,TRUE);return row;
}

/* The Save switch is derived from backend policy and never changes optimistically. */
static void save_changed(GObject *widget, GParamSpec *spec, gpointer data)
{
    (void)spec;(void)data;
    if (updating || !flag(run(),"canSetSave")) return;
    JsonObject *request=command("run.save",FALSE);
    json_object_set_boolean_member(request,"save",gtk_switch_get_active(GTK_SWITCH(widget)));
    send_request(request,TRUE);
}
static GtkWidget *save_control(GtkWidget **toggle, GtkWidget **choice, GtkWidget **help)
{
    GtkWidget *outer=box(GTK_ORIENTATION_VERTICAL,8),*row=box(GTK_ORIENTATION_HORIZONTAL,10);
    GtkWidget *heading=label("Save this run",NULL);gtk_label_set_ellipsize(GTK_LABEL(heading),PANGO_ELLIPSIZE_NONE);
    pack(row,heading,TRUE);*choice=label("Save","muted");pack(row,*choice,FALSE);
    *toggle=gtk_switch_new();gtk_widget_set_valign(*toggle,GTK_ALIGN_CENTER);pack(row,*toggle,FALSE);
    g_signal_connect(*toggle,"notify::active",G_CALLBACK(save_changed),NULL);
    pack(outer,row,FALSE);*help=label("Keep changes after restart.","muted");pack(outer,*help,FALSE);
    return outer;
}
static void update_save(GtkWidget *toggle, GtkWidget *choice, GtkWidget *help)
{
    gboolean save=flag(run(),"save");
    updating=TRUE;gtk_switch_set_active(GTK_SWITCH(toggle),save);updating=FALSE;
    gtk_widget_set_sensitive(toggle,flag(run(),"canSetSave") && !mutating && !new_profile_pending);
    set_label(choice,save ? "Save" : "Don’t save");
    const char *text=save ? "Keep changes after restart." : "Return to the start of this run after restart.";
    if (!strcmp(str(run(),"kind"),"ram")) text="Changes stay in RAM until shutdown.";
    else if (!strcmp(str(run(),"kind"),"native")) text="This profile always saves changes.";
    else if (!flag(run(),"canSetSave")) text="This run cannot change its save setting.";
    set_label(help,text);
}

/* Match the keyboard applet's continuous themed outline around the popover tail. */
static gboolean bubble_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Bubble *bubble=data;
    int width=gtk_widget_get_allocated_width(widget),height=gtk_widget_get_allocated_height(widget);
    int top=bubble->bottom ? 0 : 12,edge=bubble->bottom ? height-12 : 12,tip=bubble->bottom ? height-1 : 1;
    cairo_save(cr);cairo_set_operator(cr,CAIRO_OPERATOR_SOURCE);cairo_set_source_rgba(cr,0,0,0,0);cairo_paint(cr);
    cairo_set_operator(cr,CAIRO_OPERATOR_OVER);
    gtk_render_background(bubble_style,cr,0,top,width,height-12);
    gtk_render_frame_gap(bubble_style,cr,0,top,width,height-12,bubble->bottom ? GTK_POS_BOTTOM : GTK_POS_TOP,bubble->arrow-10,bubble->arrow+10);
    GdkRGBA background,border;
    gtk_style_context_get_background_color(bubble_style,GTK_STATE_FLAG_NORMAL,&background);
    gtk_style_context_get_border_color(bubble_style,GTK_STATE_FLAG_NORMAL,&border);
    cairo_move_to(cr,bubble->arrow-10,edge);cairo_line_to(cr,bubble->arrow,tip);cairo_line_to(cr,bubble->arrow+10,edge);cairo_close_path(cr);
    gdk_cairo_set_source_rgba(cr,&background);cairo_fill(cr);
    cairo_move_to(cr,bubble->arrow-10,edge);cairo_line_to(cr,bubble->arrow,tip);cairo_line_to(cr,bubble->arrow+10,edge);
    gdk_cairo_set_source_rgba(cr,&border);cairo_set_line_width(cr,1);cairo_stroke(cr);cairo_restore(cr);return FALSE;
}

/* Anchor against the actual tray allocation and its current monitor, including a top panel. */
static void position_bubble(Bubble *bubble)
{
    if (!tray || !gtk_widget_get_realized(tray)) return;
    int x,y;GtkAllocation anchor;gtk_widget_get_allocation(tray,&anchor);
    gdk_window_get_origin(gtk_widget_get_window(tray),&x,&y);
    GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(tray),gtk_widget_get_window(tray));
    GdkRectangle area;gdk_monitor_get_geometry(monitor,&area);
    int width=MIN(bubble==&popup ? 500 : 480,area.width-20);
    gtk_widget_set_size_request(bubble->body,MAX(260,width-40),-1);
    GtkRequisition size;gtk_widget_get_preferred_size(bubble->body,NULL,&size);
    int height=size.height+36;
    bubble->bottom=y-area.y>=height;
    gtk_widget_set_margin_top(bubble->body,bubble->bottom ? 16 : 28);
    gtk_widget_set_margin_bottom(bubble->body,bubble->bottom ? 28 : 16);
    int left=CLAMP(x+anchor.width/2-width+32,area.x+10,MAX(area.x+10,area.x+area.width-width-10));
    int top=bubble->bottom ? y-height : y+anchor.height;
    bubble->arrow=CLAMP(x+anchor.width/2-left,20,width-20);
    gtk_window_resize(GTK_WINDOW(bubble->window),width,height);
    gtk_window_move(GTK_WINDOW(bubble->window),left,CLAMP(top,area.y,MAX(area.y,area.y+area.height-height)));
    gtk_widget_queue_draw(bubble->window);
}
static gboolean popup_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)data;int x,y;gdk_window_get_origin(gtk_widget_get_window(widget),&x,&y);
    gboolean outside_x=event->x_root<x || event->x_root>=x+gtk_widget_get_allocated_width(widget);
    gboolean outside_y=event->y_root<y || event->y_root>=y+gtk_widget_get_allocated_height(widget);
    if (!outside_x && !outside_y) return FALSE;
    hide_popup();return TRUE;
}
static gboolean popup_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;(void)data;if (event->keyval!=GDK_KEY_Escape) return FALSE;hide_popup();return TRUE;
}

/* Keep passive notifications separate: never activate, focus or grab the user's applications. */
static void make_bubble(Bubble *bubble, const char *role)
{
    bubble->window=gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(bubble->window),"Slax Status");gtk_window_set_role(GTK_WINDOW(bubble->window),role);
    gtk_window_set_accept_focus(GTK_WINDOW(bubble->window),FALSE);gtk_window_set_focus_on_map(GTK_WINDOW(bubble->window),FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(bubble->window),bubble==&notice ? GDK_WINDOW_TYPE_HINT_NOTIFICATION : GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_widget_set_app_paintable(bubble->window,TRUE);gtk_widget_set_name(bubble->window,"slax-status-bubble");
    GdkVisual *visual=gdk_screen_get_rgba_visual(gtk_widget_get_screen(bubble->window));
    if (visual) gtk_widget_set_visual(bubble->window,visual);
    bubble->body=box(GTK_ORIENTATION_VERTICAL,14);
    gtk_widget_set_margin_start(bubble->body,20);gtk_widget_set_margin_end(bubble->body,20);
    gtk_container_add(GTK_CONTAINER(bubble->window),bubble->body);
    g_signal_connect(bubble->window,"draw",G_CALLBACK(bubble_draw),bubble);
}

/* The fixed status layout shares its fields with the manager instead of duplicating backend policy. */
static void make_popups(void)
{
    GtkWidgetPath *path=gtk_widget_path_new();gtk_widget_path_append_type(path,GTK_TYPE_POPOVER);
    gtk_widget_path_iter_set_object_name(path,-1,"popover");bubble_style=gtk_style_context_new();gtk_style_context_set_path(bubble_style,path);
    gtk_style_context_add_class(bubble_style,GTK_STYLE_CLASS_BACKGROUND);gtk_widget_path_unref(path);
    make_bubble(&popup,"slax-status-popup");
    GtkWidget *header=box(GTK_ORIENTATION_HORIZONTAL,12),*titles=box(GTK_ORIENTATION_VERTICAL,2);
    pack(header,gtk_image_new_from_icon_name("slax-status",GTK_ICON_SIZE_LARGE_TOOLBAR),FALSE);
    popup_title=label("Slax Status","title");pack(titles,popup_title,FALSE);pack(titles,label("Slax Status","muted"),FALSE);
    pack(header,titles,TRUE);pack(header,button("Close popup","window-close-symbolic","close-popup"),FALSE);pack(popup.body,header,FALSE);
    pack(popup.body,location_row("Running from",&popup_source),FALSE);pack(popup.body,location_row("Changes on",&popup_storage),FALSE);
    GtkWidget *capacity=box(GTK_ORIENTATION_HORIZONTAL,8);popup_space=label("Loading…","metric");pack(capacity,popup_space,FALSE);
    GtkWidget *available_label=label("available",NULL);gtk_widget_set_valign(available_label,GTK_ALIGN_END);gtk_widget_set_margin_bottom(available_label,4);pack(capacity,available_label,TRUE);
    pack(capacity,link_button("Details","space-run"),FALSE);pack(popup.body,capacity,FALSE);
    popup_meter=gtk_progress_bar_new();pack(popup.body,popup_meter,FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    pack(popup.body,save_control(&popup_save,&popup_choice,&popup_help),FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *next=box(GTK_ORIENTATION_HORIZONTAL,12);pack(next,label("Next start","muted"),FALSE);popup_next=label("",NULL);pack(next,popup_next,TRUE);
    popup_cancel=link_button("Cancel","next-clear");pack(next,popup_cancel,FALSE);pack(popup.body,next,FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *footer=box(GTK_ORIENTATION_HORIZONTAL,10);popup_status=label("","muted");pack(footer,popup_status,TRUE);
    pack(footer,button("Profiles & restore points",NULL,"manager"),FALSE);pack(popup.body,footer,FALSE);
    g_signal_connect(popup.window,"button-press-event",G_CALLBACK(popup_press),NULL);
    g_signal_connect(popup.window,"key-press-event",G_CALLBACK(popup_key),NULL);
    gtk_widget_add_events(popup.window,GDK_BUTTON_PRESS_MASK|GDK_KEY_PRESS_MASK);
    make_bubble(&notice,"slax-status-notification");
    header=box(GTK_ORIENTATION_HORIZONTAL,10);pack(header,gtk_image_new_from_icon_name("dialog-warning-symbolic",GTK_ICON_SIZE_BUTTON),FALSE);
    notice_title=label("Space is running low","notice-title");pack(header,notice_title,TRUE);pack(header,button("Dismiss notification","window-close-symbolic","dismiss"),FALSE);pack(notice.body,header,FALSE);
    notice_message=gtk_label_new("");gtk_label_set_xalign(GTK_LABEL(notice_message),0);gtk_label_set_line_wrap(GTK_LABEL(notice_message),TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(notice_message),PANGO_WRAP_WORD_CHAR);gtk_label_set_lines(GTK_LABEL(notice_message),3);
    gtk_label_set_ellipsize(GTK_LABEL(notice_message),PANGO_ELLIPSIZE_END);gtk_widget_set_size_request(notice_message,400,42);pack(notice.body,notice_message,FALSE);
    footer=box(GTK_ORIENTATION_HORIZONTAL,10);pack(footer,button("Review storage",NULL,"review"),FALSE);pack(footer,button("Remind in 15 min",NULL,"snooze"),FALSE);pack(notice.body,footer,FALSE);
    gtk_widget_show_all(popup.body);gtk_widget_show_all(notice.body);
}

/* Group side subtrees before the green continuation, with timestamp/ID ordering among siblings. */
static gint compare_points(gconstpointer first, gconstpointer second, gpointer data)
{
    JsonObject *a=*(JsonObject **)first,*b=*(JsonObject **)second;GHashTable *path=data;
    gboolean pa=g_hash_table_contains(path,str(a,"id")),pb=g_hash_table_contains(path,str(b,"id"));
    if (pa!=pb) return pa-pb;
    gint64 delta=number(a,"created")-number(b,"created");
    return delta ? (delta<0 ? -1 : 1) : strcmp(str(a,"id"),str(b,"id"));
}
static void row_free(gpointer data) { HistoryRow *row=data;json_object_unref(row->point);g_free(row); }

/* Internal lifecycle guards are not user history: collapse their ancestry before assigning lanes. */
static const char *graph_parent(JsonObject *point)
{
    const char *parent=str(point,"parent");JsonArray *catalog=array(history,"snapshots");
    for (guint i=0;*parent && i<count(catalog);i++)
    {
        JsonObject *ancestor=find(catalog,"id",parent);
        if (!ancestor || !flag(ancestor,"hidden")) break;
        parent=str(ancestor,"parent");
    }
    return parent;
}

/* Lane assignment uses the full catalog; filtering later cannot invent or flatten a fork. */
static void append_history(GPtrArray *points, GHashTable *path, const char *parent, int depth)
{
    GPtrArray *children=g_ptr_array_new();
    for (guint i=0;i<points->len;i++)
    {
        JsonObject *point=g_ptr_array_index(points,i);
        if (!strcmp(graph_parent(point),parent)) g_ptr_array_add(children,point);
    }
    g_ptr_array_sort_with_data(children,compare_points,path);
    for (guint i=0;i<children->len;i++)
    {
        JsonObject *point=g_ptr_array_index(children,i);
        HistoryRow *row=g_new0(HistoryRow,1);row->point=json_object_ref(point);
        row->path=g_hash_table_contains(path,str(point,"id"));
        gboolean branch=*parent && !row->path && (g_hash_table_contains(path,parent) || children->len>1);
        row->depth=depth+branch;g_ptr_array_add(rows,row);
        append_history(points,path,str(point,"id"),row->depth);
    }
    g_ptr_array_unref(children);
}

/* Hidden guards are never UI restore points; their visible descendants retain truthful ancestry. */
static HistoryRow *visible_parent(HistoryRow *row)
{
    const char *parent=graph_parent(row->point);
    for (guint attempts=0;*parent && attempts<rows->len;attempts++)
    {
        HistoryRow *found=NULL;
        for (guint i=0;i<rows->len;i++)
        {
            HistoryRow *candidate=g_ptr_array_index(rows,i);
            if (!strcmp(str(candidate->point,"id"),parent)) {found=candidate;break;}
        }
        if (!found) return NULL;
        if (found->widget) return found;
        parent=graph_parent(found->point);
    }
    return NULL;
}

/* Draw exact parent-column elbows. Node hover/focus belongs to each real selectable GTK row. */
static gboolean draw_history(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget;(void)data;
    if (!rows) return FALSE;
    cairo_set_line_join(cr,CAIRO_LINE_JOIN_MITER);cairo_set_line_cap(cr,CAIRO_LINE_CAP_BUTT);
    for (guint i=0;i<rows->len;i++)
    {
        HistoryRow *row=g_ptr_array_index(rows,i),*parent=visible_parent(row);
        if (!row->widget || !parent) continue;
        GtkAllocation a,b;gtk_widget_get_allocation(row->widget,&a);gtk_widget_get_allocation(parent->widget,&b);
        double x=18+row->depth*22,px=18+parent->depth*22,y=a.y+a.height/2.,py=b.y+b.height/2.;
        cairo_set_source_rgb(cr,row->path ? .34 : .73,row->path ? .56 : .77,row->path ? .27 : .70);
        cairo_set_line_width(cr,row->path ? 2.5 : 2);
        cairo_move_to(cr,px,py);cairo_line_to(cr,px,y);cairo_line_to(cr,x,y);cairo_stroke(cr);
    }
    JsonObject *next=object(state,"next");
    for (guint i=0;i<rows->len;i++)
    {
        HistoryRow *row=g_ptr_array_index(rows,i);if (!row->widget) continue;
        gboolean current=!strcmp(str(row->point,"id"),"current");
        gboolean chosen=current ? !selected_point : !g_strcmp0(selected_point,str(row->point,"id"));
        gboolean highlighted=chosen || hover_row==row || gtk_widget_has_focus(row->widget) || (gtk_widget_get_state_flags(row->widget)&GTK_STATE_FLAG_PRELIGHT);
        gboolean queued=next && !g_strcmp0(selected,str(next,"profileId")) && !strcmp(str(row->point,"id"),str(next,"pointId"));
        GtkAllocation a;gtk_widget_get_allocation(row->widget,&a);double x=18+row->depth*22,y=a.y+a.height/2.;
        double red=queued ? .16 : .34,green=queued ? .47 : .56,blue=queued ? .72 : .27;
        if (!row->path && !queued && !highlighted) {red=.53;green=.60;blue=.49;}
        if (highlighted) {cairo_set_source_rgba(cr,red,green,blue,.22);cairo_arc(cr,x,y,10,0,2*G_PI);cairo_fill(cr);}
        cairo_arc(cr,x,y,current ? 6 : 4,0,2*G_PI);
        if (current || highlighted) cairo_set_source_rgb(cr,red,green,blue);else cairo_set_source_rgb(cr,.97,.98,.96);
        cairo_fill_preserve(cr);cairo_set_source_rgb(cr,red,green,blue);cairo_set_line_width(cr,highlighted ? 3 : 2);
        if (flag(row->point,"deleted")) {double dash[]={2,2};cairo_set_dash(cr,dash,2,0);}
        cairo_stroke(cr);cairo_set_dash(cr,NULL,0,0);
    }
    return FALSE;
}
static gboolean history_hover(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    (void)widget;if (event->detail==GDK_NOTIFY_INFERIOR) return FALSE;
    hover_row=event->type==GDK_ENTER_NOTIFY ? data : NULL;gtk_widget_queue_draw(history_list);return FALSE;
}
static void history_state_changed(GtkWidget *widget, GtkStateFlags previous, gpointer data)
{
    (void)widget;(void)previous;(void)data;gtk_widget_queue_draw(history_list);
}

/* ScrolledWindow finishes child allocation and adjustment ranges before revealing the row. */
static void reveal_history(GtkWidget *widget, GtkAllocation *size, gpointer data)
{
    (void)widget;(void)size;(void)data;if (!reveal_current || updating) return;
    GtkListBoxRow *row=gtk_list_box_get_selected_row(GTK_LIST_BOX(history_list));if (!row) return;
    GtkAllocation allocation;gtk_widget_get_allocation(GTK_WIDGET(row),&allocation);
    GtkAdjustment *adjustment=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(history_scroll));
    double position=gtk_adjustment_get_value(adjustment),page=gtk_adjustment_get_page_size(adjustment);
    if (allocation.height<=1 || page<=0 || gtk_adjustment_get_upper(adjustment)<allocation.y+allocation.height) return;
    if (allocation.y<position) gtk_adjustment_set_value(adjustment,allocation.y);
    else if (allocation.y+allocation.height>position+page) gtk_adjustment_set_value(adjustment,allocation.y+allocation.height-page);
    reveal_current=FALSE;
}
static void history_selected(GtkListBox *list, GtkListBoxRow *widget, gpointer data)
{
    (void)list;(void)data;if (updating || !widget) return;
    const char *id=g_object_get_data(G_OBJECT(widget),"point-id");
    g_free(selected_point);selected_point=strcmp(id,"current") ? g_strdup(id) : NULL;
    render_inspector();gtk_widget_queue_draw(history_list);
}
static void removed_changed(GtkToggleButton *toggle, gpointer data)
{
    (void)data;removed_visible=gtk_toggle_button_get_active(toggle);render_history();
}

/* Rebuild changed history only, preserving its scroll position and exact selected identity. */
static void render_history(void)
{
    GtkAdjustment *adjustment=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(history_scroll));
    double scroll=gtk_adjustment_get_value(adjustment);updating=TRUE;hover_row=NULL;
    clear_box(history_list);if (rows) g_ptr_array_unref(rows);rows=g_ptr_array_new_with_free_func(row_free);
    const char *placeholder=NULL;
    if (!profile()) placeholder=!state || discovery_pending ? "Loading profiles…" : "Create a profile to get started.";
    else if (strcmp(str(profile(),"kind"),"container")) placeholder="Changes are saved directly. This profile has no restore points.";
    else if (!history || g_strcmp0(history_profile,selected)) placeholder="Loading restore points…";
    if (placeholder)
    {
        gtk_container_add(GTK_CONTAINER(history_list),label(placeholder,"muted"));gtk_widget_show_all(history_list);updating=FALSE;return;
    }
    if (selected_point && !point()) g_clear_pointer(&selected_point,g_free);
    GPtrArray *points=g_ptr_array_new();JsonArray *catalog=array(history,"snapshots");
    for (guint i=0;i<count(catalog);i++)
    {
        JsonObject *point=json_array_get_object_element(catalog,i);
        if (!flag(point,"hidden")) g_ptr_array_add(points,point);
    }
    JsonObject *current=json_object_new();json_object_set_string_member(current,"id","current");json_object_set_string_member(current,"parent",str(history,"currentParent"));g_ptr_array_add(points,current);
    GHashTable *path=g_hash_table_new(g_str_hash,g_str_equal);g_hash_table_add(path,"current");
    const char *parent=str(history,"currentParent");
    while (*parent && !g_hash_table_contains(path,parent))
    {
        g_hash_table_add(path,(gpointer)parent);JsonObject *ancestor=find(catalog,"id",parent);if (!ancestor) break;parent=str(ancestor,"parent");
    }
    append_history(points,path,"",0);
    /* Retain only tombstones that connect visible descendants, unless Show removed is enabled. */
    GHashTable *visible=g_hash_table_new(g_str_hash,g_str_equal);
    for (gint i=(gint)rows->len-1;i>=0;i--)
    {
        HistoryRow *row=g_ptr_array_index(rows,i);const char *id=str(row->point,"id");
        if (!flag(row->point,"deleted") || removed_visible || row->path || g_hash_table_contains(visible,id))
        {g_hash_table_add(visible,(gpointer)id);g_hash_table_add(visible,(gpointer)graph_parent(row->point));}
    }
    JsonObject *next=object(state,"next");
    for (guint i=0;i<rows->len;i++)
    {
        HistoryRow *row=g_ptr_array_index(rows,i);const char *id=str(row->point,"id");
        if (flag(row->point,"hidden") || !g_hash_table_contains(visible,id)) continue;
        gboolean live=!strcmp(id,"current");
        row->widget=gtk_list_box_row_new();gtk_widget_set_size_request(row->widget,-1,36);
        GtkWidget *line=box(GTK_ORIENTATION_HORIZONTAL,8);gtk_widget_set_margin_start(line,36+row->depth*22);gtk_widget_set_margin_end(line,8);
        GtkWidget *name=label(live ? "Current state" : point_name(row->point),live ? "strong" : NULL);pack(line,name,TRUE);
        const char *badge=flag(row->point,"deleted") ? "Removed" : !strcmp(id,str(history,"currentParent")) ? "Current restore point" : "";
        if (next && !g_strcmp0(selected,str(next,"profileId")) && !strcmp(id,str(next,"pointId"))) badge="Next start";
        if (live) badge=flag(profile(),"active") ? "You are here" : "Latest state";
        if (*badge) pack(line,label(badge,flag(row->point,"deleted") ? "muted" : "badge"),FALSE);
        if (flag(row->point,"deleted")) gtk_style_context_add_class(gtk_widget_get_style_context(name),"muted");
        if (!live) {char *date=date_text(number(row->point,"created"));GtkWidget *time=label(date,"muted");gtk_widget_set_size_request(time,100,-1);pack(line,time,FALSE);g_free(date);}
        gtk_container_add(GTK_CONTAINER(row->widget),line);gtk_container_add(GTK_CONTAINER(history_list),row->widget);
        g_object_set_data_full(G_OBJECT(row->widget),"point-id",g_strdup(id),g_free);
        g_signal_connect(row->widget,"enter-notify-event",G_CALLBACK(history_hover),row);g_signal_connect(row->widget,"leave-notify-event",G_CALLBACK(history_hover),row);
        g_signal_connect(row->widget,"state-flags-changed",G_CALLBACK(history_state_changed),NULL);
        gtk_widget_add_events(row->widget,GDK_ENTER_NOTIFY_MASK|GDK_LEAVE_NOTIFY_MASK);
        gtk_widget_set_tooltip_text(row->widget,live ? "Current state" : point_name(row->point));
        if (live ? !selected_point : !g_strcmp0(selected_point,id)) gtk_list_box_select_row(GTK_LIST_BOX(history_list),GTK_LIST_BOX_ROW(row->widget));
    }
    g_hash_table_unref(visible);g_hash_table_unref(path);g_ptr_array_unref(points);json_object_unref(current);
    gtk_widget_show_all(history_list);gtk_adjustment_set_value(adjustment,scroll);updating=FALSE;
    gtk_widget_queue_resize(history_scroll);
}

/* Changing a profile drops only that profile's point selection; pending next-start policy is separate. */
static void select_profile(const char *id)
{
    if (!id || !g_strcmp0(id,selected)) return;
    g_free(selected);selected=g_strdup(id);g_clear_pointer(&selected_point,g_free);manager_dirty=TRUE;reveal_current=TRUE;
    if (history) {json_object_unref(history);history=NULL;}
    render_history();render_inspector();update_ui();
    if (!requesting) send_request(command("history",TRUE),FALSE);
}
static void profile_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;(void)data;if (!updating && row) select_profile(g_object_get_data(G_OBJECT(row),"profile-id"));
}
static void combo_changed(GtkComboBox *combo, gpointer data)
{
    (void)data;if (!updating) select_profile(gtk_combo_box_get_active_id(combo));
}

/* Move the same profile controls beside the selected sidebar row or the narrow selector. */
static void position_profile_actions(void)
{
    GtkWidget *target=compact || !selected_profile_row ? sidebar : selected_profile_row;
    GtkWidget *parent=gtk_widget_get_parent(profile_actions);if (parent==target) return;
    g_object_ref(profile_actions);if (parent) gtk_container_remove(GTK_CONTAINER(parent),profile_actions);
    pack(target,profile_actions,FALSE);g_object_unref(profile_actions);
}

/* Changing capacity counters must not replace the profile widgets or interrupt their focus. */
static char *profile_signature(void)
{
    GString *text=g_string_new("");
    g_string_append(text,str(object(state,"next"),"profileId"));
    JsonArray *profiles=array(state,"profiles");
    for (guint i=0;i<count(profiles);i++)
    {
        JsonObject *item=json_array_get_object_element(profiles,i);
        const char *keys[]={"id","numericId","name","driveId","device"};
        for (guint j=0;j<G_N_ELEMENTS(keys);j++) {const char *value=str(item,keys[j]);g_string_append_printf(text,"%zu:%s;",strlen(value),value);}
        const char *name=str(find(array(state,"drives"),"id",str(item,"driveId")),"label");g_string_append_printf(text,"%zu:%s;",strlen(name),name);
        g_string_append_c(text,flag(item,"active") ? '1' : '0');
    }
    return g_string_free(text,FALSE);
}

/* Keep rows and the combo model alive while GTK finishes dispatching selection events. */
static void sync_profile_selection(void)
{
    updating=TRUE;selected_profile_row=NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(profile_list));
    for (GList *item=children;item;item=item->next)
        if (!g_strcmp0(selected,g_object_get_data(G_OBJECT(item->data),"profile-id")))
        {
            gtk_list_box_select_row(GTK_LIST_BOX(profile_list),GTK_LIST_BOX_ROW(item->data));
            selected_profile_row=gtk_bin_get_child(GTK_BIN(item->data));break;
        }
    g_list_free(children);position_profile_actions();gtk_combo_box_set_active_id(GTK_COMBO_BOX(profile_combo),selected);updating=FALSE;
}

/* Rebuild profile widgets only when their names, device or status markings change. */
static void render_profiles(void)
{
    updating=TRUE;selected_profile_row=NULL;position_profile_actions();clear_box(profile_list);gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(profile_combo));
    JsonArray *profiles=array(state,"profiles");
    for (guint i=0;i<count(profiles);i++)
    {
        JsonObject *item=json_array_get_object_element(profiles,i);char *name=profile_name(item);
        GtkWidget *row=gtk_list_box_row_new(),*line=box(GTK_ORIENTATION_HORIZONTAL,6),*titles=box(GTK_ORIENTATION_VERTICAL,4);
        gtk_container_set_border_width(GTK_CONTAINER(line),6);pack(line,gtk_image_new_from_icon_name("folder-symbolic",GTK_ICON_SIZE_BUTTON),FALSE);
        pack(titles,label(name,NULL),FALSE);
        JsonObject *drive=find(array(state,"drives"),"id",str(item,"driveId"));char *drive_text=drive_name(drive);
        GtkWidget *device_label=label(drive_text,"muted");gtk_widget_set_tooltip_text(device_label,drive_text);pack(titles,device_label,FALSE);pack(line,titles,TRUE);
        JsonObject *next=object(state,"next");
        if (flag(item,"active")) pack(line,label("●","running"),FALSE);
        else if (!strcmp(str(item,"id"),str(next,"profileId"))) pack(line,label("●","queued"),FALSE);
        gtk_container_add(GTK_CONTAINER(row),line);gtk_container_add(GTK_CONTAINER(profile_list),row);
        g_object_set_data_full(G_OBJECT(row),"profile-id",g_strdup(str(item,"id")),g_free);gtk_widget_set_tooltip_text(row,name);
        char *option=g_strdup_printf("%s · %s",name,drive_text);gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(profile_combo),str(item,"id"),option);g_free(option);
        g_free(name);g_free(drive_text);
    }
    gtk_widget_show_all(profile_list);updating=FALSE;
}

/* A fixed inspector exposes actions without expanding or moving history rows. */
static void render_inspector(void)
{
    clear_box(inspector);JsonObject *item=profile(),*saved=point();if (!item) return;
    gboolean writable=flag(item,"writable") && !mutating;
    const char *eyebrow=saved ? flag(saved,"deleted") ? "REMOVED RESTORE POINT" : "SAVED RESTORE POINT" : flag(item,"active") ? "YOU ARE HERE" : "LATEST STATE";
    pack(inspector,label(eyebrow,"eyebrow"),FALSE);pack(inspector,label(saved ? point_name(saved) : "Current state","title"),FALSE);
    if (saved)
    {
        char *date=date_text(number(saved,"created"));pack(inspector,label(date,"muted"),FALSE);g_free(date);
        JsonObject *parent=find(array(history,"snapshots"),"id",graph_parent(saved));
        pack(inspector,label("Previous","muted"),FALSE);pack(inspector,label(parent ? point_name(parent) : "First restore point",NULL),FALSE);
        pack(inspector,label(flag(saved,"automatic") ? "Saved automatically" : "Saved by you","muted"),FALSE);
        if (flag(saved,"deleted"))
        {
            GtkWidget *note=gtk_label_new("Its place in history remains; its files are no longer available.");gtk_label_set_line_wrap(GTK_LABEL(note),TRUE);gtk_label_set_xalign(GTK_LABEL(note),0);pack(inspector,note,FALSE);
        }
        else
        {
            JsonObject *next=object(state,"next");gboolean queued=!g_strcmp0(selected,str(next,"profileId")) && !strcmp(str(saved,"id"),str(next,"pointId"));
            GtkWidget *use=primary_button(queued ? "Selected for next start" : "Use at next start","use-point");gtk_widget_set_sensitive(use,writable && !queued);pack(inspector,use,FALSE);
            GtkWidget *open=button("Open files",NULL,"open-files");gtk_widget_set_sensitive(open,!mutating);pack(inspector,open,FALSE);
            if (flag(saved,"filesOpen")) {GtkWidget *close=button("Close files",NULL,"close-files");gtk_widget_set_sensitive(close,!mutating);pack(inspector,close,FALSE);}
            GtkWidget *remove=button("Delete restore point",NULL,"delete-point");gtk_widget_set_sensitive(remove,writable && !flag(saved,"protected") && !flag(saved,"filesOpen"));
            gtk_widget_set_tooltip_text(remove,flag(saved,"protected") ? "Needed by this run or a pending restore" : "Delete restore point");pack(inspector,remove,FALSE);
        }
    }
    else
    {
        if (flag(item,"active"))
        {
            GtkWidget *toggle,*choice,*help,*control=save_control(&toggle,&choice,&help);pack(inspector,control,FALSE);update_save(toggle,choice,help);
        }
        else if (number(item,"lastUsed")) {char *date=date_text(number(item,"lastUsed"));char *text=g_strconcat("Last used ",date,NULL);pack(inspector,label(text,"muted"),FALSE);g_free(date);g_free(text);}
        if (!strcmp(str(item,"kind"),"container")) {GtkWidget *create=primary_button("Create restore point","create-point");gtk_widget_set_sensitive(create,writable);pack(inspector,create,FALSE);}
        if (!flag(item,"active")) {GtkWidget *use=button("Use at next start",NULL,"use-profile");gtk_widget_set_sensitive(use,writable);pack(inspector,use,FALSE);}
    }
    gtk_widget_show_all(inspector);
}

/* Choose the layout before GTK allocates children; changing orientation during allocation is too late. */
static gboolean manager_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    (void)widget;(void)data;gboolean narrow=event->width<900,vertical=event->width<650;
    if (narrow!=compact)
    {
        compact=narrow;gtk_orientable_set_orientation(GTK_ORIENTABLE(main_layout),compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(sidebar),compact ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
        GtkStyleContext *style=gtk_widget_get_style_context(sidebar);
        if (compact) gtk_style_context_add_class(style,"compact");else gtk_style_context_remove_class(style,"compact");
        gtk_widget_set_size_request(sidebar,compact ? -1 : 250,-1);gtk_widget_set_visible(profile_scroll,!compact);gtk_widget_set_visible(profile_combo,compact);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(context_box),compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
        position_profile_actions();
    }
    if (vertical!=stacked)
    {
        stacked=vertical;gtk_orientable_set_orientation(GTK_ORIENTABLE(history_layout),stacked ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
        GtkStyleContext *style=gtk_widget_get_style_context(inspector_scroll);
        if (stacked) gtk_style_context_add_class(style,"stacked");else gtk_style_context_remove_class(style,"stacked");
        gtk_box_set_spacing(GTK_BOX(history_layout),stacked ? 8 : 18);
        gtk_widget_set_size_request(inspector_scroll,stacked ? -1 : 264,stacked ? 160 : -1);
    }
    return FALSE;
}
static gboolean manager_close(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)event;(void)data;new_profile_pending=FALSE;gtk_widget_hide(widget);return TRUE;
}
static gboolean manager_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)data;if (event->keyval!=GDK_KEY_Escape) return FALSE;new_profile_pending=FALSE;gtk_widget_hide(widget);return TRUE;
}

/* Ordinary GTK windows get the system's decoration; only the tray surfaces paint their own outline. */
static void make_manager(void)
{
    manager=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(manager),"Slax profiles");
    gtk_window_set_wmclass(GTK_WINDOW(manager),"slax-status","slax-status");gtk_window_set_icon_name(GTK_WINDOW(manager),"slax-status");
    gtk_window_set_default_size(GTK_WINDOW(manager),1140,640);gtk_window_set_position(GTK_WINDOW(manager),GTK_WIN_POS_CENTER);
    gtk_widget_set_size_request(manager,540,440);
    g_signal_connect(manager,"delete-event",G_CALLBACK(manager_close),NULL);g_signal_connect(manager,"key-press-event",G_CALLBACK(manager_key),NULL);
    manager_body=box(GTK_ORIENTATION_VERTICAL,0);gtk_container_add(GTK_CONTAINER(manager),manager_body);
    context_box=box(GTK_ORIENTATION_HORIZONTAL,12);gtk_container_set_border_width(GTK_CONTAINER(context_box),16);
    pack(context_box,location_row("Running from",&manager_source),TRUE);pack(context_box,location_row("Changes on",&manager_storage),TRUE);pack(manager_body,context_box,FALSE);
    pack(manager_body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    main_layout=box(GTK_ORIENTATION_HORIZONTAL,0);pack(manager_body,main_layout,TRUE);
    /* CSS padding stays inside the painted panels; container borders leave unpainted gutters. */
    sidebar=box(GTK_ORIENTATION_VERTICAL,10);gtk_widget_set_size_request(sidebar,250,-1);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar),"sidebar");pack(main_layout,sidebar,FALSE);
    GtkWidget *side_header=box(GTK_ORIENTATION_HORIZONTAL,10);gtk_style_context_add_class(gtk_widget_get_style_context(side_header),"sidebar-heading");pack(side_header,label("Profiles","strong"),TRUE);
    profile_new=button("New profile","list-add-symbolic","new-profile");gtk_widget_set_valign(profile_new,GTK_ALIGN_CENTER);pack(side_header,profile_new,FALSE);pack(sidebar,side_header,FALSE);
    profile_combo=gtk_combo_box_text_new();gtk_widget_set_hexpand(profile_combo,TRUE);pack(sidebar,profile_combo,TRUE);g_signal_connect(profile_combo,"changed",G_CALLBACK(combo_changed),NULL);
    GList *cells=gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(profile_combo));
    for (GList *cell=cells;cell;cell=cell->next) g_object_set(cell->data,"ellipsize",PANGO_ELLIPSIZE_END,"width-chars",12,NULL);
    g_list_free(cells);
    profile_list=gtk_list_box_new();gtk_list_box_set_selection_mode(GTK_LIST_BOX(profile_list),GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(profile_list),"profile-list");
    profile_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(profile_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(profile_scroll),profile_list);pack(sidebar,profile_scroll,TRUE);
    g_signal_connect(profile_list,"row-selected",G_CALLBACK(profile_selected),NULL);
    profile_actions=box(GTK_ORIENTATION_HORIZONTAL,6);
    profile_rename=button("Rename profile","document-edit-symbolic","rename-profile");profile_delete=button("Delete profile","user-trash-symbolic","delete-profile");
    pack(profile_actions,profile_rename,FALSE);pack(profile_actions,profile_delete,FALSE);pack(sidebar,profile_actions,FALSE);
    GtkWidget *detail=box(GTK_ORIENTATION_VERTICAL,12);gtk_style_context_add_class(gtk_widget_get_style_context(detail),"session-detail");pack(main_layout,detail,TRUE);
    profile_heading=label("Profiles","heading");pack(detail,profile_heading,FALSE);
    history_layout=box(GTK_ORIENTATION_HORIZONTAL,18);pack(detail,history_layout,TRUE);
    history_box=box(GTK_ORIENTATION_VERTICAL,10);pack(history_layout,history_box,TRUE);
    GtkWidget *heading=box(GTK_ORIENTATION_HORIZONTAL,12);pack(heading,label("Restore points","strong"),TRUE);
    show_removed=gtk_check_button_new_with_label("Show removed");pack(heading,show_removed,FALSE);pack(history_box,heading,FALSE);g_signal_connect(show_removed,"toggled",G_CALLBACK(removed_changed),NULL);
    history_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(history_scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);pack(history_box,history_scroll,TRUE);
    g_signal_connect_after(history_scroll,"size-allocate",G_CALLBACK(reveal_history),NULL);
    history_list=gtk_list_box_new();gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list),GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(history_list),"history-list");gtk_container_add(GTK_CONTAINER(history_scroll),history_list);
    g_signal_connect(history_list,"row-selected",G_CALLBACK(history_selected),NULL);g_signal_connect_after(history_list,"draw",G_CALLBACK(draw_history),NULL);
    GtkWidget *legend=box(GTK_ORIENTATION_HORIZONTAL,12);gtk_widget_set_margin_bottom(legend,10);pack(legend,label("━ Current path","running"),TRUE);pack(legend,link_button("Go to now ↓","current"),FALSE);pack(history_box,legend,FALSE);
    inspector=box(GTK_ORIENTATION_VERTICAL,12);gtk_style_context_add_class(gtk_widget_get_style_context(inspector),"inspector");
    inspector_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inspector_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_style_context_add_class(gtk_widget_get_style_context(inspector_scroll),"inspector-panel");
    gtk_widget_set_size_request(inspector_scroll,264,-1);gtk_container_add(GTK_CONTAINER(inspector_scroll),inspector);pack(history_layout,inspector_scroll,FALSE);
    pack(manager_body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *footer=box(GTK_ORIENTATION_HORIZONTAL,12);gtk_container_set_border_width(GTK_CONTAINER(footer),14);pack(footer,label("Next start:","muted"),FALSE);
    manager_next=label("Loading…",NULL);pack(footer,manager_next,TRUE);manager_cancel=link_button("Cancel","next-clear");pack(footer,manager_cancel,FALSE);
    manager_space=link_button("","space-profile");gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(manager_space))),PANGO_ELLIPSIZE_END);
    pack(footer,manager_space,FALSE);pack(manager_body,footer,FALSE);
    manager_status=label("","muted");gtk_widget_set_margin_start(manager_status,14);gtk_widget_set_margin_end(manager_status,14);pack(manager_body,manager_status,FALSE);
    g_signal_connect(manager,"configure-event",G_CALLBACK(manager_configure),NULL);
    gtk_widget_show_all(manager_body);gtk_widget_hide(profile_combo);
}

/* A manual manager request may focus its normal window; passive alerts never call this. */
static void show_manager(void)
{
    hide_popup();gtk_widget_hide(notice.window);gtk_widget_show(manager);gtk_window_present(GTK_WINDOW(manager));manager_dirty=TRUE;
    discovery_pending=TRUE;render_history();render_inspector();update_ui();refresh();
}

/* Hysteresis avoids warning flicker while allocations cross the low-space boundary. */
static const char *warning_kind(void)
{
    if (connection_error) return "service";
    if (*str(run(),"error")) return "error";
    JsonObject *active=find(array(state,"profiles"),"id",str(run(),"profileId"));
    if (active && !flag(active,"writable")) return "readonly";
    gint64 available=number(run(),"freeBytes")+number(run(),"reusableBytes");
    gboolean already_low=last_warning && (!strcmp(last_warning,"ram") || !strcmp(last_warning,"disk"));
    if (state && available<(already_low ? 650 : 500)*G_GINT64_CONSTANT(1000000))
        return !strcmp(str(run(),"kind"),"ram") ? "ram" : "disk";
    return "";
}

/* Update live counters in place; catalogue rows are rebuilt only when their data or selection changes. */
static void update_ui(void)
{
    if (!manager || !popup.window) return;
    JsonObject *active=find(array(state,"profiles"),"id",str(run(),"profileId"));
    char *name=active ? profile_name(active) : g_strdup(state ? "Temporary run" : "Slax Status");
    set_label(popup_title,name);set_label(popup_source,str(run(),"source"));set_label(popup_storage,str(run(),"storage"));
    set_label(manager_source,str(run(),"source"));set_label(manager_storage,str(run(),"storage"));
    gint64 available=number(run(),"freeBytes")+number(run(),"reusableBytes");
    char *size=size_text(available);
    set_label(popup_space,state ? size : "Loading…");
    double total=number(run(),"totalBytes");gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(popup_meter),total>0 ? CLAMP(available/total,0,1) : 0);
    update_save(popup_save,popup_choice,popup_help);
    char *next=next_text();set_label(popup_next,next);set_label(manager_next,next);g_free(next);
    gboolean queued=object(state,"next")!=NULL;gtk_widget_set_visible(popup_cancel,queued);gtk_widget_set_visible(manager_cancel,queued);
    gtk_widget_set_sensitive(popup_cancel,!mutating);gtk_widget_set_sensitive(manager_cancel,!mutating);
    JsonObject *item=profile();char *heading=item ? profile_name(item) : g_strdup("Profiles");set_label(profile_heading,heading);gtk_widget_set_tooltip_text(profile_combo,heading);g_free(heading);
    gtk_widget_set_sensitive(profile_rename,item && flag(item,"writable") && !mutating && !new_profile_pending);
    gtk_widget_set_sensitive(profile_delete,item && flag(item,"writable") && !flag(item,"active") && !mutating && !new_profile_pending);
    gtk_widget_set_sensitive(profile_new,state && !mutating && !new_profile_pending);
    gtk_widget_set_sensitive(inspector,!mutating && !new_profile_pending);
    if (item)
    {
        char *free=size_text(number(item,"freeBytes")+number(item,"reusableBytes"));
        JsonObject *drive=find(array(state,"drives"),"id",str(item,"driveId"));
        char *device=drive_name(drive),*text=g_strdup_printf("%s available · %s",free,device);
        gtk_button_set_label(GTK_BUTTON(manager_space),text);gtk_widget_set_tooltip_text(manager_space,text);g_free(free);g_free(device);g_free(text);
    }
    else gtk_button_set_label(GTK_BUTTON(manager_space),"");
    const char *warning=warning_kind();
    GtkStyleContext *meter_style=gtk_widget_get_style_context(popup_meter);
    gtk_style_context_remove_class(meter_style,"capacity-warning");gtk_style_context_remove_class(meter_style,"capacity-temporary");
    if (*warning) gtk_style_context_add_class(meter_style,"capacity-warning");
    else if (!flag(run(),"save")) gtk_style_context_add_class(meter_style,"capacity-temporary");
    const char *status=mutating ? "Working…" : connection_error ? "Storage service unavailable" : !strcmp(warning,"readonly") ? "Storage is read-only" : *warning ? "Storage needs attention" : snooze_until>g_get_monotonic_time() ? "Alerts snoozed · 15 min" : "";
    set_label(popup_status,status);set_label(manager_status,connection_error ? connection_error : mutating ? "Working…" : "");gtk_widget_set_visible(manager_status,connection_error || mutating);
    char *tooltip=g_strdup_printf("Slax Status · %s · %s available",name,size);if (tray) gtk_widget_set_tooltip_text(tray,tooltip);
    g_free(tooltip);g_free(name);g_free(size);if (tray_icon) gtk_widget_queue_draw(tray_icon);
    if (state)
    {
        char *current=profile_signature();
        if (g_strcmp0(current,profiles_signature)) {g_free(profiles_signature);profiles_signature=current;render_profiles();}else g_free(current);
        sync_profile_selection();
        if (!item && manager_dirty) {render_history();render_inspector();manager_dirty=FALSE;}
    }
    gboolean anchored=tray && gtk_plug_get_embedded(GTK_PLUG(tray));
    if (anchored && *warning && g_strcmp0(warning,last_warning) && snooze_until<=g_get_monotonic_time() && !gtk_widget_get_visible(popup.window))
    {
        char *free=size_text(available),*text=NULL;const char *title="Space is running low";
        if (!strcmp(warning,"ram")) {title="Available memory is low";text=g_strdup_printf("About %s of RAM is available. Save important files to a drive and close unused applications.",free);}
        else if (!strcmp(warning,"readonly")) {title="Changes cannot be saved";text=g_strdup("The profile drive became read-only. Save important files to another writable drive and check the storage device.");}
        else if (!strcmp(warning,"service")) {title="Storage status is unavailable";text=g_strdup("The storage service could not be reached. Open Slax Status to review the error.");}
        else if (!strcmp(warning,"error")) {title="Storage needs attention";text=g_strdup(str(run(),"error"));}
        else text=g_strdup_printf("About %s is available for changes. Free drive space or remove unneeded restore points.",free);
        /* Pango limits lines per paragraph, so flatten separators in the preview, retaining the full tooltip. */
        GString *preview=g_string_new(NULL);
        for (const char *cursor=text;*cursor;cursor=g_utf8_next_char(cursor))
        {
            gunichar character=g_utf8_get_char(cursor);g_string_append_unichar(preview,g_unichar_isspace(character) ? ' ' : character);
        }
        set_label(notice_title,title);gtk_label_set_text(GTK_LABEL(notice_message),preview->str);gtk_widget_set_tooltip_text(notice_message,text);
        g_string_free(preview,TRUE);g_free(text);g_free(free);
        gtk_widget_show(notice.window);position_bubble(&notice);
    }
    if (!*warning) gtk_widget_hide(notice.window);
    if (snooze_until<=g_get_monotonic_time() && (anchored || !*warning)) {g_free(last_warning);last_warning=g_strdup(warning);}
}

/* Confirmation defaults to Cancel and may carry the explicit keep-current-state choice. */
static gboolean confirm(const char *title, const char *text, const char *accept, gboolean *keep)
{
    hide_popup();GtkWidget *dialog=gtk_message_dialog_new(gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"%s",title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",text);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),"Cancel",GTK_RESPONSE_CANCEL,accept,GTK_RESPONSE_ACCEPT,NULL);gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
    GtkWidget *check=NULL;
    if (keep)
    {
        check=gtk_check_button_new_with_label("Keep this profile’s current state as a named restore point");gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),TRUE);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),check);gtk_widget_show(check);
    }
    gboolean accepted=gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT;
    if (keep) *keep=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));
    gtk_widget_destroy(dialog);return accepted;
}

/* Reflect DynFileFS's UTF-8 name rules before making an expensive freeze/create request. */
static const char *name_error(const char *name)
{
    if (!*name) return "Enter a name for this restore point.";
    if (strlen(name)>250) return "Use a shorter name (at most 250 UTF-8 bytes).";
    if (g_str_has_prefix(name,"auto-") || g_str_has_prefix(name,"live-start-") || g_str_has_prefix(name,"live-end-")) return "This prefix is reserved for Slax’s automatic restore points.";
    if (name[strlen(name)-1]==' ' || name[strlen(name)-1]=='.') return "Do not end the name with a dot or space.";
    for (const char *cursor=name;*cursor;cursor=g_utf8_next_char(cursor))
    {
        gunichar character=g_utf8_get_char(cursor);
        if (g_unichar_iscntrl(character) || (character<128 && strchr("<>:\"/\\|?*",character))) return "Do not use control characters, path separators or < > : \" | ? *.";
    }
    JsonArray *points=array(history,"snapshots");
    for (guint i=0;i<count(points);i++) {JsonObject *item=json_array_get_object_element(points,i);if (!flag(item,"deleted") && !strcmp(str(item,"name"),name)) return "A restore point with this name already exists.";}
    return NULL;
}

/* Name/profile creation use ordinary GTK forms, with errors retained beside the editable input. */
static void name_dialog(const char *op)
{
    hide_popup();gboolean create_profile=!strcmp(op,"profile.create"),create_point=!strcmp(op,"point.create");
    g_autofree char *target=g_strdup(selected);
    const char *title=create_profile ? "New profile" : create_point ? "Create restore point" : "Rename profile";
    GtkWidget *dialog=gtk_dialog_new_with_buttons(title,GTK_WINDOW(manager),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel",GTK_RESPONSE_CANCEL,create_profile ? "Create profile" : create_point ? "Create restore point" : "Save name",GTK_RESPONSE_ACCEPT,NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog),480,-1);gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);
    GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(dialog));gtk_container_set_border_width(GTK_CONTAINER(content),18);gtk_box_set_spacing(GTK_BOX(content),12);
    const char *note=create_profile ? "Start with a clean profile." : create_point && flag(profile(),"active") && !flag(run(),"save") ? "This restore point stays even though this run will not be saved." : create_point ? "Save a point you can return to later." : "";
    if (*note) pack(content,label(note,"muted"),FALSE);
    pack(content,label(create_profile ? "Profile name" : "Name",NULL),FALSE);GtkWidget *entry=gtk_entry_new();gtk_entry_set_activates_default(GTK_ENTRY(entry),TRUE);
    if (!create_profile && !create_point) gtk_entry_set_text(GTK_ENTRY(entry),str(profile(),"name"));
    if (create_point) gtk_entry_set_placeholder_text(GTK_ENTRY(entry),"e.g. Before upgrade");
    pack(content,entry,FALSE);
    GtkWidget *drives=NULL;
    if (create_profile)
    {
        pack(content,label("Drive",NULL),FALSE);drives=gtk_combo_box_text_new();JsonArray *items=array(state,"drives");
        for (guint i=0;i<count(items);i++)
        {
            JsonObject *drive=json_array_get_object_element(items,i);if (!flag(drive,"writable")) continue;
            char *free=size_text(number(drive,"freeBytes")),*device=drive_name(drive),*name=g_strdup_printf("%s · %s free",device,free);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(drives),str(drive,"id"),name);g_free(free);g_free(device);g_free(name);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(drives),0);pack(content,drives,FALSE);
        if (!gtk_combo_box_get_active_id(GTK_COMBO_BOX(drives))) gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT,FALSE);
    }
    GtkWidget *error=label("","error");pack(content,error,FALSE);gtk_widget_show_all(content);gtk_widget_grab_focus(entry);
    while (gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT)
    {
        const char *name=gtk_entry_get_text(GTK_ENTRY(entry)),*invalid=create_point ? name_error(name) : NULL;
        if (invalid) {set_label(error,invalid);continue;}
        JsonObject *request=command(op,!create_profile);json_object_set_string_member(request,"name",name);
        if (!create_profile) json_object_set_string_member(request,"profileId",target);
        if (create_profile) json_object_set_string_member(request,"driveId",gtk_combo_box_get_active_id(GTK_COMBO_BOX(drives)));
        gtk_widget_destroy(dialog);send_request(request,TRUE);return;
    }
    gtk_widget_destroy(dialog);
}

/* Capacity explains the single estimate without promising byte-for-byte filesystem growth. */
static void space_dialog(gboolean current)
{
    hide_popup();JsonObject *item=current ? run() : profile();if (!item) return;
    char *free=size_text(number(item,"freeBytes")),*reusable=size_text(number(item,"reusableBytes")),*available=size_text(number(item,"freeBytes")+number(item,"reusableBytes"));
    gboolean ram=current && !strcmp(str(item,"kind"),"ram");
    GtkWidget *dialog=gtk_message_dialog_new(gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL,GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,"%s",ram ? "Available memory" : "Space for changes");
    if (ram) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s available in system memory.",available);
    else gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"Free on drive: %s\nReusable in this profile: %s\nAvailable for changes: %s\n\nAvailable space is an estimate.",free,reusable,available);
    gtk_dialog_run(GTK_DIALOG(dialog));gtk_widget_destroy(dialog);g_free(free);g_free(reusable);g_free(available);
}

/* Open the click-invoked bubble without activating a managed window; the keyboard grab enables Escape. */
static void show_popup(GdkEvent *event)
{
    new_profile_pending=FALSE;gtk_widget_hide(manager);gtk_widget_hide(notice.window);update_ui();gtk_widget_show(popup.window);position_bubble(&popup);
    GdkSeat *seat=gdk_display_get_default_seat(gtk_widget_get_display(popup.window));
    if (gdk_seat_grab(seat,gtk_widget_get_window(popup.window),GDK_SEAT_CAPABILITY_ALL,TRUE,NULL,event,NULL,NULL)==GDK_GRAB_SUCCESS)
    {gtk_grab_add(popup.window);gtk_widget_child_focus(popup.body,GTK_DIR_TAB_FORWARD);}
    else gtk_widget_hide(popup.window);
    refresh();
}

/* Map visible actions directly to the backend contract; confirmation never mutates storage. */
static void action(GtkButton *button_widget, gpointer data)
{
    (void)button_widget;const char *op=data;
    if (!strcmp(op,"close-popup")) {hide_popup();return;}
    if (!strcmp(op,"manager")) {show_manager();return;}
    if (!strcmp(op,"dismiss")) {gtk_widget_hide(notice.window);return;}
    if (!strcmp(op,"snooze")) {snooze_until=g_get_monotonic_time()+15*60*G_USEC_PER_SEC;g_clear_pointer(&last_warning,g_free);gtk_widget_hide(notice.window);update_ui();return;}
    if (!strcmp(op,"review")) {show_popup(NULL);return;}
    if (!strcmp(op,"space-run") || !strcmp(op,"space-profile")) {space_dialog(!strcmp(op,"space-run"));return;}
    if (!strcmp(op,"current"))
    {
        g_clear_pointer(&selected_point,g_free);reveal_current=TRUE;render_history();render_inspector();return;
    }
    if (mutating || new_profile_pending || !state) return;
    if (!strcmp(op,"next-clear")) {send_request(command("next.clear",FALSE),TRUE);return;}
    if (!strcmp(op,"new-profile")) {new_profile_pending=TRUE;discovery_pending=TRUE;update_ui();refresh();return;}
    JsonObject *item=profile(),*saved=point();if (!item) return;
    g_autofree char *target=g_strdup(selected);
    if (!strcmp(op,"rename-profile")) {name_dialog("profile.rename");return;}
    if (!strcmp(op,"create-point")) {name_dialog("point.create");return;}
    if (!strcmp(op,"delete-profile"))
    {
        char *name=profile_name(item),*title=g_strdup_printf("Delete %s?",name);g_free(name);
        gboolean accepted=confirm(title,"Remove this profile and its entire history from the drive.","Delete profile",NULL);g_free(title);
        if (accepted) {JsonObject *request=command("profile.delete",TRUE);json_object_set_string_member(request,"profileId",target);send_request(request,TRUE);}
        return;
    }
    if (!strcmp(op,"use-profile"))
    {
        JsonObject *request=command("next.set",TRUE);json_object_set_null_member(request,"pointId");json_object_set_boolean_member(request,"keepCurrent",FALSE);send_request(request,TRUE);return;
    }
    if (!saved) return;
    g_autofree char *saved_id=g_strdup(str(saved,"id"));
    if (!strcmp(op,"use-point"))
    {
        char *title=g_strdup_printf("Use “%s” at next start?",point_name(saved));gboolean keep=TRUE;
        gboolean accepted=confirm(title,"Other restore points stay available. The running desktop continues until restart.","Use at next start",&keep);g_free(title);
        if (accepted) {JsonObject *request=command("next.set",TRUE);json_object_set_string_member(request,"profileId",target);json_object_set_string_member(request,"pointId",saved_id);json_object_set_boolean_member(request,"keepCurrent",keep);send_request(request,TRUE);}return;
    }
    if (!strcmp(op,"delete-point"))
    {
        char *title=g_strdup_printf("Delete “%s”?",point_name(saved));
        gboolean accepted=confirm(title,"Its files will no longer be available. Other restore points and your current state stay unchanged. Its next-start selection, if any, will be cancelled.","Delete restore point",NULL);g_free(title);
        if (accepted) {JsonObject *request=command("point.delete",TRUE);json_object_set_string_member(request,"profileId",target);json_object_set_string_member(request,"pointId",saved_id);send_request(request,TRUE);}return;
    }
    if (!strcmp(op,"open-files")) {JsonObject *request=command("files.open",TRUE);json_object_set_string_member(request,"pointId",str(saved,"id"));send_request(request,TRUE);return;}
    if (!strcmp(op,"close-files"))
    {
        JsonArray *views=array(state,"views");
        for (guint i=0;i<count(views);i++)
        {
            JsonObject *view=json_array_get_object_element(views,i);
            if (strcmp(str(view,"profileId"),selected) || strcmp(str(view,"pointId"),str(saved,"id"))) continue;
            JsonObject *request=command("files.close",FALSE);json_object_set_string_member(request,"viewId",str(view,"id"));send_request(request,TRUE);break;
        }
    }
}

/* The icon is drawn at native tray resolution with a separate, legible state dot. */
static gboolean draw_tray(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;double width=gtk_widget_get_allocated_width(widget),height=gtk_widget_get_allocated_height(widget);
    cairo_save(cr);cairo_set_operator(cr,CAIRO_OPERATOR_SOURCE);cairo_set_source_rgba(cr,0,0,0,0);cairo_paint(cr);cairo_set_operator(cr,CAIRO_OPERATOR_OVER);
    /* Integral filled bars stay crisp at GTK's device scale; only the round status dot is antialiased. */
    cairo_translate(cr,floor((width-22)/2),floor((height-22)/2));cairo_set_source_rgb(cr,.95,.96,.95);
    cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);
    for (int y=3;y<=12;y+=9)
    {
        cairo_rectangle(cr,3,y,15,1);cairo_rectangle(cr,3,y+5,15,1);
        cairo_rectangle(cr,3,y+1,1,4);cairo_rectangle(cr,17,y+1,1,4);
        cairo_rectangle(cr,6,y+2,2,2);cairo_rectangle(cr,10,y+3,y==3 ? 5 : 4,1);
    }
    cairo_fill(cr);cairo_set_antialias(cr,CAIRO_ANTIALIAS_DEFAULT);
    const char *warning=warning_kind();
    if (*warning) cairo_set_source_rgb(cr,1,.38,.34);
    else if (flag(run(),"save")) cairo_set_source_rgb(cr,.49,.80,.38);
    else cairo_set_source_rgb(cr,.30,.67,.97);
    cairo_arc(cr,18,18,3.5,0,2*G_PI);cairo_fill_preserve(cr);cairo_set_source_rgb(cr,.17,.19,.17);cairo_set_line_width(cr,1.4);cairo_stroke(cr);cairo_restore(cr);return FALSE;
}
static gboolean tray_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;(void)data;if (event->button!=1 && event->button!=3) return FALSE;
    if (gtk_widget_get_visible(popup.window)) hide_popup();else show_popup((GdkEvent *)event);return TRUE;
}
static void tray_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget;(void)data;hide_popup();gtk_widget_hide(notice.window);tray=tray_icon=NULL;
}

/* Use the standard XEmbed tray protocol and reconnect when the panel replaces its tray manager. */
static void dock_tray(void)
{
    Window owner=XGetSelectionOwner(display,tray_selection);if (!owner) return;
    if (!tray)
    {
        tray=gtk_plug_new(0);gtk_window_set_title(GTK_WINDOW(tray),"Slax Status");gtk_window_set_wmclass(GTK_WINDOW(tray),"slax-status","slax-status");
        gtk_widget_set_name(tray,"slax-status-tray");gtk_widget_set_app_paintable(tray,TRUE);gtk_widget_set_size_request(tray,24,24);
        GdkScreen *screen=gtk_widget_get_screen(tray);GdkVisual *visual=gdk_screen_get_rgba_visual(screen);
        Atom type;int format;unsigned long count,remaining;unsigned char *property=NULL;
        XGetWindowProperty(display,owner,XInternAtom(display,"_NET_SYSTEM_TRAY_VISUAL",False),0,1,False,AnyPropertyType,&type,&format,&count,&remaining,&property);
        if (property && format==32 && count==1) visual=gdk_x11_screen_lookup_visual(screen,*(unsigned long *)property);
        if (property) XFree(property);
        if (visual) gtk_widget_set_visual(tray,visual);
        tray_icon=gtk_drawing_area_new();gtk_widget_set_size_request(tray_icon,24,24);gtk_container_add(GTK_CONTAINER(tray),tray_icon);
        gtk_widget_add_events(tray_icon,GDK_BUTTON_PRESS_MASK);g_signal_connect(tray_icon,"draw",G_CALLBACK(draw_tray),NULL);
        g_signal_connect(tray_icon,"button-press-event",G_CALLBACK(tray_press),NULL);g_signal_connect(tray,"destroy",G_CALLBACK(tray_destroyed),NULL);gtk_widget_show_all(tray);
    }
    XEvent event={0};event.xclient.type=ClientMessage;event.xclient.window=owner;
    event.xclient.message_type=XInternAtom(display,"_NET_SYSTEM_TRAY_OPCODE",False);event.xclient.format=32;
    event.xclient.data.l[0]=CurrentTime;event.xclient.data.l[1]=0;event.xclient.data.l[2]=gdk_x11_window_get_xid(gtk_widget_get_window(tray));
    XSendEvent(display,owner,False,NoEventMask,&event);XFlush(display);
}

/* A second --manage invocation asks this user's existing tray to show its manager. */
static gboolean x_events(gint fd, GIOCondition condition, gpointer data)
{
    (void)fd;(void)condition;(void)data;
    while (XPending(display))
    {
        XEvent event;XNextEvent(display,&event);
        if (event.type==ClientMessage && event.xclient.message_type==manager_atom && (Atom)event.xclient.data.l[1]==tray_selection) dock_tray();
        else if (event.type==ClientMessage && event.xclient.message_type==command_atom) show_manager();
        else if (event.type==SelectionClear && event.xselectionclear.window==command_window) gtk_main_quit();
    }
    return G_SOURCE_CONTINUE;
}

/* Keep styling local to this application; native controls and the desktop's font settings remain intact. */
static void load_style(void)
{
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#slax-status-tray, #slax-status-bubble { background: transparent; border: 0; box-shadow: none; }"
        "window { font-size: 14px; } .muted { color: #65705f; font-size: 13px; }"
        ".title { font-size: 20px; font-weight: 600; } .heading { font-size: 24px; font-weight: 400; } .metric { font-size: 32px; }"
        ".notice-title { font-size: 16px; font-weight: 600; }"
        ".strong { font-weight: 600; } .eyebrow { font-size: 13px; color: #65705f; }"
        ".running { color: #397b35; } .queued { color: #2784d8; } .error { color: #ba3932; }"
        ".sidebar { background: #eff1eb; border-right: 1px solid #d8ddd3; padding: 18px 10px; }"
        ".sidebar-heading { padding: 0 8px; } .sidebar.compact { border-right: 0; border-bottom: 1px solid #d8ddd3; padding: 8px 12px; } .sidebar.compact .sidebar-heading { padding: 0; }"
        ".session-detail { padding: 18px 18px 0; }"
        ".profile-list, .history-list { background: transparent; }"
        ".profile-list row { border-radius: 5px; } .profile-list row:selected { background: #e4ecdd; color: #292d29; }"
        ".history-list row { min-height: 34px; padding: 0; border: 1px solid transparent; border-radius: 4px; color: #292d29; }"
        ".history-list row:selected { background: #e7eee0; border-color: #c3d4b7; } .history-list row:hover { background: #eef2e9; }"
        ".badge { color: #507343; font-size: 13px; } .inspector-panel { border-left: 1px solid #d8ddd3; } .inspector { padding: 4px 0 18px 18px; }"
        ".inspector-panel.stacked { border-left: 0; border-top: 1px solid #d8ddd3; } .inspector-panel.stacked .inspector { padding: 12px 0; }"
        "button.link { background: transparent; box-shadow: none; border: 0; color: #306c99; padding: 5px 0; }"
        "button.icon-button { min-width: 24px; min-height: 24px; padding: 0; border: 0; background: transparent; box-shadow: none; }"
        "button.icon-button:hover { background: #e2e8dc; }"
        "button.suggested-action { background: #397b35; color: white; border-color: #397b35; }"
        "button.suggested-action:hover { background: #2c6729; } progressbar progress { background: #579044; border: 0; min-height: 6px; } progressbar trough { min-height: 6px; }"
        "progressbar.capacity-warning progress { background: #ce413a; } progressbar.capacity-temporary progress { background: #2784d8; }",
        -1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
}

/* One tray per X display, screen and login UID; root and guest use the same unprivileged frontend. */
int main(int argc, char **argv)
{
    gboolean manage=argc==2 && !strcmp(argv[1],"--manage");
    if (argc>1 && !manage) {g_printerr("Usage: slax-status [--manage]\n");return 1;}
    gtk_init(&argc,&argv);display=XOpenDisplay(NULL);if (!display) return 1;
    char *name=g_strdup_printf("_SLAX_STATUS_%lu_S%d",(unsigned long)getuid(),DefaultScreen(display));Atom selection=XInternAtom(display,name,False);g_free(name);
    command_atom=XInternAtom(display,"_SLAX_STATUS_MANAGE",False);XGrabServer(display);Window owner=XGetSelectionOwner(display,selection);
    if (owner)
    {
        XUngrabServer(display);
        if (manage) {XEvent event={0};event.xclient.type=ClientMessage;event.xclient.window=owner;event.xclient.message_type=command_atom;event.xclient.format=32;XSendEvent(display,owner,False,NoEventMask,&event);XSync(display,False);}
        XCloseDisplay(display);return 0;
    }
    command_window=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1,1,0,0,0);XSetSelectionOwner(display,selection,command_window,CurrentTime);XUngrabServer(display);
    name=g_strdup_printf("_NET_SYSTEM_TRAY_S%d",DefaultScreen(display));tray_selection=XInternAtom(display,name,False);g_free(name);
    manager_atom=XInternAtom(display,"MANAGER",False);XSelectInput(display,DefaultRootWindow(display),StructureNotifyMask);XFlush(display);
    g_unix_fd_add(ConnectionNumber(display),G_IO_IN|G_IO_HUP,x_events,NULL);
    load_style();make_manager();make_popups();dock_tray();update_ui();refresh();g_timeout_add_seconds(3,poll_state,NULL);
    if (manage) show_manager();
    gtk_main();XDestroyWindow(display,command_window);XCloseDisplay(display);return 0;
}
