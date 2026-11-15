/* Slax's native status tray and restore-point manager. Storage operations belong to slax-statusd. */
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#define QUEUED_COLOR "#2978b8"

typedef struct { GtkWidget *window, *body; int arrow; gboolean bottom; } Bubble;
typedef struct { JsonObject *point; int depth; gboolean path; GtkWidget *widget, *size; } HistoryRow;
typedef struct { JsonObject *request; char *op, *store; gboolean mutation; } Request;
static Request *active_request; /* Borrowed from the running GTask. */
static JsonObject *state, *history, *pending_request;
static char *selected, *selected_point, *history_store, *history_signature;
static char *connection_error, *history_error, *last_warning;
static gboolean requesting, mutating, updating, manager_dirty=TRUE, compact, stacked;
static GSubprocess *restart_process;
static gint64 snooze_until;
static GtkWidget *tray, *tray_icon, *manager, *manager_body;
static GtkWidget *history_box, *history_scroll, *history_list, *inspector, *inspector_scroll, *history_layout, *main_layout, *context_box;
static GtkWidget *manager_source, *manager_storage, *manager_next, *manager_cancel, *manager_cancel_slot, *manager_restart, *manager_space, *manager_status;
static GtkWidget *popup_title, *popup_source, *popup_storage, *popup_space, *popup_meter, *popup_save, *popup_choice;
static GtkWidget *popup_help, *popup_next, *popup_cancel, *popup_status, *notice_title, *notice_message, *show_removed;
static GtkWidget *inspector_save, *inspector_choice, *inspector_help, *inspector_reclaim, *manager_feedback;
static GtkWidget *inspector_eyebrow, *inspector_title, *inspector_saved, *inspector_current, *inspector_date, *inspector_origin, *inspector_removed;
static GtkWidget *inspector_actions, *inspector_use, *inspector_open, *inspector_rename, *inspector_delete, *inspector_create;
static GtkWidget *manager_content, *intro, *intro_columns, *intro_example, *history_footer, *history_latest;
static Bubble popup, notice;
static GtkStyleContext *bubble_style;
static GPtrArray *rows;
static HistoryRow *hover_row;
static gboolean removed_visible;
static gboolean reveal_current=TRUE;
static Display *display;
static Window command_window;
static Atom tray_selection, manager_atom, command_atom;
static GdkRGBA queued_color;

static void refresh(void);
static void update_ui(void);
static void show_manager(void);
static void render_history(void);
static void render_inspector(void);
static void update_history_sizes(void);
static void dock_tray(void);
static void send_request(JsonObject *request, gboolean mutation);
static void action(GtkButton *button, gpointer data);
static void name_dialog(gboolean rename);

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

/* Storage and restore-point actions use exact opaque identities. */
static JsonObject *find(JsonArray *items, const char *key, const char *value)
{
    for (guint i=0; value && i<count(items); i++)
    {
        JsonObject *item=json_array_get_object_element(items, i);
        if (!strcmp(str(item,key),value)) return item;
    }
    return NULL;
}
static JsonObject *store(void) { return find(array(state,"stores"),"id",selected); }
static JsonObject *point(void) { return find(array(history,"snapshots"),"id",selected_point); }
static JsonObject *run(void) { return object(state,"run"); }

/* Save intents may replace one another while the serialized policy write finishes. */
static gboolean saving(void)
{
    if (!mutating) return FALSE;
    if (active_request && active_request->mutation && strcmp(active_request->op,"run.save")) return FALSE;
    return !pending_request || !strcmp(str(pending_request,"op"),"run.save");
}

static const char *point_name(JsonObject *item) { return flag(item,"automatic") ? "Automatic restore point" : str(item,"name"); }
static char *size_text(gint64 bytes) { return g_format_size_full(MAX((gint64)0,bytes),G_FORMAT_SIZE_DEFAULT); }
/* Only deletable points can promise reusable capacity; unknown amounts stay blank. */
static char *reclaimable_text(JsonObject *item)
{
    if (!item) return NULL;
    if (flag(item,"deleted") || flag(item,"protected") || flag(item,"filesOpen")) return NULL;
    JsonNode *value=json_object_get_member(item,"reclaimableBytes");
    if (!value || JSON_NODE_HOLDS_NULL(value)) return NULL;
    gint64 bytes=json_node_get_int(value);
    if (!bytes) return g_strdup("0 MB");
    return bytes<1000000 ? g_strdup("<1 MB") : size_text(bytes);
}
static char *date_text(gint64 seconds)
{
    GDateTime *date=g_date_time_new_from_unix_local(seconds);
    char *text=date ? g_date_time_format(date,"%e %b · %H:%M") : g_strdup("");
    if (date) g_date_time_unref(date);
    return text;
}

/* Use plain GTK text, never treat restore-point names or backend errors as markup. */
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
    if (!g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)),text ? text : "")) return;
    gtk_label_set_text(GTK_LABEL(widget),text ? text : "");
    if (gtk_label_get_ellipsize(GTK_LABEL(widget))!=PANGO_ELLIPSIZE_NONE)
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

/* Help text wraps within the current window instead of forcing a wider layout. */
static GtkWidget *paragraph(const char *text)
{
    GtkWidget *widget=gtk_label_new(text);gtk_label_set_xalign(GTK_LABEL(widget),0);gtk_label_set_line_wrap(GTK_LABEL(widget),TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget),PANGO_WRAP_WORD_CHAR);gtk_label_set_max_width_chars(GTK_LABEL(widget),74);return widget;
}

/* Informational history messages are placeholders, never selectable restore-point rows. */
static void history_placeholder(const char *text)
{
    GtkWidget *message=label(text,"muted");gtk_widget_set_size_request(message,-1,36);gtk_widget_set_valign(message,GTK_ALIGN_START);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(history_list),message);gtk_widget_show(message);
}

/* Hide the explicit popup; no tray bubble takes a device grab. */
static void hide_popup(void)
{
    if (popup.window) gtk_widget_hide(popup.window);
    if (tray) gtk_widget_set_has_tooltip(tray,TRUE);
}

/* Dialogs appear only after explicit actions and use the ordinary native window frame. */
static void message(const char *title, const char *text)
{
    hide_popup();
    GtkWidget *dialog=gtk_message_dialog_new(gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",title);
    gtk_window_set_titlebar(GTK_WINDOW(dialog),NULL);gtk_window_set_title(GTK_WINDOW(dialog),title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",text);
    gtk_dialog_run(GTK_DIALOG(dialog));gtk_widget_destroy(dialog);
}

/* Send one complete JSON command; no shell or UI-supplied filesystem paths cross the boundary. */
static JsonObject *command(const char *op, gboolean with_store)
{
    JsonObject *request=json_object_new();
    json_object_set_string_member(request,"op",op);
    if (with_store && selected) json_object_set_string_member(request,"storeId",selected);
    return request;
}
static void request_free(gpointer data)
{
    Request *request=data;
    json_object_unref(request->request);g_free(request->op);g_free(request->store);g_free(request);
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

/* Copy structural fields without the live capacity counter. */
static void copy_history_field(JsonObject *source, const char *name, JsonNode *value, gpointer data)
{
    (void)source;
    if (strcmp(name,"reclaimableBytes")) json_object_set_member(data,name,json_node_copy(value));
}

/* Capacity changes update labels in place without replacing hovered or selected rows. */
static char *signature(JsonNode *node)
{
    if (!node) return g_strdup("");
    JsonObject *source=json_node_get_object(node),*copy=json_object_new();
    json_object_foreach_member(source,copy_history_field,copy);
    JsonArray *catalog=array(source,"snapshots"),*points=json_array_new();
    for (guint i=0;i<count(catalog);i++)
    {
        JsonObject *point=json_object_new();json_object_foreach_member(json_array_get_object_element(catalog,i),copy_history_field,point);
        json_array_add_object_element(points,point);
    }
    json_object_set_array_member(copy,"snapshots",points);
    JsonNode *root=json_node_new(JSON_NODE_OBJECT);json_node_take_object(root,copy);
    char *text=json_to_string(root,FALSE);json_node_free(root);return text;
}

/* Apply confirmed results on GTK's thread and reload authoritative state after every mutation. */
static void request_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;(void)data;
    Request *request=g_task_get_task_data(G_TASK(result));
    GError *error=NULL;
    JsonObject *reply=g_task_propagate_pointer(G_TASK(result),&error);
    requesting=FALSE;active_request=NULL;mutating=pending_request!=NULL;
    if (error || !flag(reply,"ok"))
    {
        const char *detail=error ? error->message : str(object(reply,"error"),"message");
        if (request->mutation) message("The action could not be completed",detail);
        else if (!strcmp(request->op,"state")) {g_free(connection_error);connection_error=g_strdup(detail);}
        else if (!strcmp(request->op,"history") && !g_strcmp0(selected,request->store))
        {
            g_free(history_error);history_error=g_strdup(detail);
            /* Retain the confirmed tree during transient failures; retry an initial placeholder. */
            if (!history || g_strcmp0(history_store,selected)) {history_placeholder(detail);manager_dirty=TRUE;}
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
            if (g_strcmp0(selected,str(run(),"storeId")))
            {
                g_free(selected);selected=g_strdup(str(run(),"storeId"));
                g_clear_pointer(&history_error,g_free);
                g_clear_pointer(&selected_point,g_free);manager_dirty=TRUE;reveal_current=TRUE;
                render_history();render_inspector();
            }
        }
        else if (!strcmp(request->op,"history") && !g_strcmp0(selected,request->store))
        {
            g_clear_pointer(&history_error,g_free);
            char *current=signature(json_object_get_member(reply,"result"));
            gboolean changed=g_strcmp0(current,history_signature) || g_strcmp0(history_store,selected);
            if (history) json_object_unref(history);
            history=json_object_ref(value);
            g_free(history_store);history_store=g_strdup(selected);
            g_free(history_signature);history_signature=current;
            if (changed || manager_dirty) {render_history();render_inspector();manager_dirty=FALSE;}
            else update_history_sizes();
        }
        else if (request->mutation)
        {
            if (!strcmp(request->op,"point.create")) {g_free(selected_point);selected_point=g_strdup(str(value,"pointId"));}
            if (!strcmp(request->op,"point.delete")) g_clear_pointer(&selected_point,g_free);
            if (!strcmp(request->op,"run.save")) json_object_set_boolean_member(run(),"save",flag(request->request,"save"));
            else manager_dirty=TRUE;
        }
    }
    if (reply) json_object_unref(reply);
    update_ui();
    if (pending_request) {JsonObject *queued=pending_request;pending_request=NULL;send_request(queued,TRUE);}
    else if (request->mutation) refresh();
    else if (!strcmp(request->op,"state") && gtk_widget_get_visible(manager) && store())
        send_request(command("history",TRUE),FALSE);
    else if (!strcmp(request->op,"history") && g_strcmp0(request->store,selected) && gtk_widget_get_visible(manager) && store())
        send_request(command("history",TRUE),FALSE);
}

/* One request at a time also prevents conflicting UI mutations and stale poll replies. */
static void send_request(JsonObject *value, gboolean mutation)
{
    if (requesting)
    {
        /* Keep the newest native switch intent instead of dropping rapid changes. */
        if (mutation && !strcmp(str(value,"op"),"run.save") && saving()) g_clear_pointer(&pending_request,json_object_unref);
        if (mutation && !pending_request) {pending_request=value;mutating=TRUE;update_ui();}
        else json_object_unref(value);
        return;
    }
    Request *request=g_new0(Request,1);
    request->request=value;request->op=g_strdup(str(value,"op"));request->store=g_strdup(str(value,"storeId"));request->mutation=mutation;
    requesting=TRUE;active_request=request;mutating=mutation;update_ui();
    GTask *task=g_task_new(NULL,NULL,request_done,NULL);
    g_task_set_task_data(task,request,request_free);g_task_run_in_thread(task,request_worker);g_object_unref(task);
}
/* The application inspects the running storage only; drive selection belongs to boot. */
static void refresh(void)
{
    if (requesting) return;
    send_request(command("state",FALSE),FALSE);
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
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),"primary-action");
    return widget;
}

/* The next-start summary describes the chosen state on the current drive. */
static char *next_text(void)
{
    JsonObject *next=object(state,"next");
    if (next && *str(next,"pointId"))
    {
        char *date=date_text(number(next,"created"));
        char *text=g_strdup_printf("%s · %s",*str(next,"pointName") ? str(next,"pointName") : "Selected restore point",date);
        g_free(date);return text;
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

/* Let GTK's requested position wait for the asynchronous, durable policy update. */
static gboolean save_changed(GtkSwitch *widget, gboolean save, gpointer data)
{
    (void)widget;(void)data;
    if (updating) return FALSE;
    if ((mutating && !saving()) || restart_process || !flag(run(),"canSetSave")) return TRUE;
    JsonObject *request=command("run.save",FALSE);
    json_object_set_boolean_member(request,"save",save);
    send_request(request,TRUE);
    return TRUE;
}
static GtkWidget *save_control(GtkWidget **toggle, GtkWidget **choice, GtkWidget **help)
{
    GtkWidget *outer=box(GTK_ORIENTATION_VERTICAL,8),*row=box(GTK_ORIENTATION_HORIZONTAL,10);
    GtkWidget *heading=label("Save this run",NULL);gtk_label_set_ellipsize(GTK_LABEL(heading),PANGO_ELLIPSIZE_NONE);
    pack(row,heading,TRUE);*choice=label("Save","muted");pack(row,*choice,FALSE);
    *toggle=gtk_switch_new();gtk_widget_set_valign(*toggle,GTK_ALIGN_CENTER);pack(row,*toggle,FALSE);
    g_signal_connect(*toggle,"state-set",G_CALLBACK(save_changed),NULL);
    pack(outer,row,FALSE);*help=label("Keep changes after restart.","muted");pack(outer,*help,FALSE);
    return outer;
}
static void update_save(GtkWidget *toggle, GtkWidget *choice, GtkWidget *help)
{
    gboolean save=flag(run(),"save");
    /* Describe the latest intent while acknowledgements catch up with the native thumb. */
    if (saving()) save=flag(pending_request ? pending_request : active_request->request,"save");
    if (!mutating)
    {
        /* Setting an unchanged active value cancels GTK's animation and can strand its slider. */
        updating=TRUE;
        if (gtk_switch_get_active(GTK_SWITCH(toggle))!=save) gtk_switch_set_active(GTK_SWITCH(toggle),save);
        gtk_switch_set_state(GTK_SWITCH(toggle),save);updating=FALSE;
    }
    gtk_widget_set_sensitive(toggle,flag(run(),"canSetSave") && (!mutating || saving()) && !restart_process);
    set_label(choice,save ? "Save" : "Don’t save");
    const char *text=save ? "Keep changes after restart." : "Discard changes from this run after restart.";
    if (!strcmp(str(run(),"kind"),"ram")) text="Changes stay in RAM until shutdown.";
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
    if (width<24 || height<12) {cairo_restore(cr);return FALSE;}
    int arrow=CLAMP(bubble->arrow,12,width-12);
    cairo_set_operator(cr,CAIRO_OPERATOR_OVER);
    gtk_render_background(bubble_style,cr,0,top,width,height-12);
    gtk_render_frame_gap(bubble_style,cr,0,top,width,height-12,bubble->bottom ? GTK_POS_BOTTOM : GTK_POS_TOP,arrow-10,arrow+10);
    GdkRGBA background,border;
    gtk_style_context_get_background_color(bubble_style,GTK_STATE_FLAG_NORMAL,&background);
    gtk_style_context_get_border_color(bubble_style,GTK_STATE_FLAG_NORMAL,&border);
    cairo_move_to(cr,arrow-10,edge);cairo_line_to(cr,arrow,tip);cairo_line_to(cr,arrow+10,edge);cairo_close_path(cr);
    gdk_cairo_set_source_rgba(cr,&background);cairo_fill(cr);
    cairo_move_to(cr,arrow-10,edge);cairo_line_to(cr,arrow,tip);cairo_line_to(cr,arrow+10,edge);
    gdk_cairo_set_source_rgba(cr,&border);cairo_set_line_width(cr,1);cairo_stroke(cr);cairo_restore(cr);return FALSE;
}

/* Re-anchor the actual window size after GTK and the window manager finish placing it. */
static void anchor_bubble(Bubble *bubble, int width, int height)
{
    if (!tray || !gtk_widget_get_realized(tray)) return;
    int x,y;GtkAllocation anchor;gtk_widget_get_allocation(tray,&anchor);
    gdk_window_get_origin(gtk_widget_get_window(tray),&x,&y);
    GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(tray),gtk_widget_get_window(tray));
    GdkRectangle area;gdk_monitor_get_geometry(monitor,&area);
    int left=CLAMP(x+anchor.width/2-width+32,area.x+10,MAX(area.x+10,area.x+area.width-width-10));
    int top=bubble->bottom ? y-height : y+anchor.height;
    bubble->arrow=CLAMP(x+anchor.width/2-left,20,width-20);
    int current_x,current_y;gtk_window_get_position(GTK_WINDOW(bubble->window),&current_x,&current_y);
    top=CLAMP(top,area.y,MAX(area.y,area.y+area.height-height));
    if (current_x!=left || current_y!=top) gtk_window_move(GTK_WINDOW(bubble->window),left,top);
    gtk_widget_queue_draw(bubble->window);
}
/* Window-manager remapping must not retain an earlier size or position. */
static gboolean bubble_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    if (gtk_widget_get_visible(widget)) anchor_bubble(data,event->width,event->height);
    return FALSE;
}

/* Measure the body without its tail, then choose the side of the tray with enough room. */
static void position_bubble(Bubble *bubble)
{
    if (!tray || !gtk_widget_get_realized(tray)) return;
    GdkWindow *window=gtk_widget_get_window(tray);int x,y;gdk_window_get_origin(window,&x,&y);
    GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(tray),window);
    GdkRectangle area;gdk_monitor_get_geometry(monitor,&area);
    int width=MIN(bubble==&popup ? 500 : 480,area.width-20);
    gtk_widget_set_size_request(bubble->body,MAX(260,width-40),-1);
    gtk_widget_set_margin_top(bubble->body,16);gtk_widget_set_margin_bottom(bubble->body,16);
    GtkRequisition size;gtk_widget_get_preferred_size(bubble->body,NULL,&size);
    int height=size.height+12;
    bubble->bottom=y-area.y>=height;
    gtk_widget_set_margin_top(bubble->body,bubble->bottom ? 16 : 28);
    gtk_widget_set_margin_bottom(bubble->body,bubble->bottom ? 28 : 16);
    gtk_window_resize(GTK_WINDOW(bubble->window),width,height);
    anchor_bubble(bubble,width,height);
}
/* Ordinary window focus dismisses the interactive bubble without intercepting outside input. */
static gboolean popup_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
    (void)event;(void)data;gtk_widget_hide(widget);return FALSE;
}
static gboolean popup_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;(void)data;if (event->keyval!=GDK_KEY_Escape) return FALSE;hide_popup();return TRUE;
}

/* Keep passive notifications separate: never activate, focus or grab the user's applications. */
static void make_bubble(Bubble *bubble, const char *role)
{
    bubble->window=gtk_window_new(bubble==&notice ? GTK_WINDOW_POPUP : GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(bubble->window),"Slax Status");gtk_window_set_role(GTK_WINDOW(bubble->window),role);
    gtk_window_set_accept_focus(GTK_WINDOW(bubble->window),bubble==&popup);gtk_window_set_focus_on_map(GTK_WINDOW(bubble->window),bubble==&popup);
    gtk_window_set_decorated(GTK_WINDOW(bubble->window),FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(bubble->window),TRUE);gtk_window_set_skip_pager_hint(GTK_WINDOW(bubble->window),TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(bubble->window),TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(bubble->window),bubble==&notice ? GDK_WINDOW_TYPE_HINT_NOTIFICATION : GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_widget_set_app_paintable(bubble->window,TRUE);gtk_widget_set_name(bubble->window,"slax-status-bubble");
    GdkVisual *visual=gdk_screen_get_rgba_visual(gtk_widget_get_screen(bubble->window));
    if (visual) gtk_widget_set_visual(bubble->window,visual);
    bubble->body=box(GTK_ORIENTATION_VERTICAL,12);
    gtk_widget_set_margin_start(bubble->body,20);gtk_widget_set_margin_end(bubble->body,20);
    gtk_container_add(GTK_CONTAINER(bubble->window),bubble->body);
    g_signal_connect(bubble->window,"draw",G_CALLBACK(bubble_draw),bubble);
    g_signal_connect(bubble->window,"configure-event",G_CALLBACK(bubble_configure),bubble);
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
    popup_title=label("Slax Status","title");pack(titles,popup_title,FALSE);
    pack(header,titles,TRUE);pack(header,button("Close popup","window-close-symbolic","close-popup"),FALSE);pack(popup.body,header,FALSE);
    GtkWidget *locations=box(GTK_ORIENTATION_VERTICAL,6);pack(locations,location_row("Running from",&popup_source),FALSE);
    pack(locations,location_row("Changes on",&popup_storage),FALSE);pack(popup.body,locations,FALSE);
    GtkWidget *capacity=box(GTK_ORIENTATION_HORIZONTAL,8);popup_space=label("Loading…","metric");pack(capacity,popup_space,FALSE);
    GtkWidget *available_label=label("available",NULL);gtk_widget_set_valign(available_label,GTK_ALIGN_END);gtk_widget_set_margin_bottom(available_label,4);pack(capacity,available_label,TRUE);
    GtkWidget *storage=box(GTK_ORIENTATION_VERTICAL,8);pack(storage,capacity,FALSE);
    popup_meter=gtk_progress_bar_new();gtk_widget_set_tooltip_text(popup_meter,"Space used");pack(storage,popup_meter,FALSE);pack(popup.body,storage,FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    pack(popup.body,save_control(&popup_save,&popup_choice,&popup_help),FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *next=box(GTK_ORIENTATION_HORIZONTAL,12);pack(next,label("Next start","muted"),FALSE);popup_next=label("",NULL);pack(next,popup_next,TRUE);
    popup_cancel=link_button("Cancel","next-clear");pack(next,popup_cancel,FALSE);pack(popup.body,next,FALSE);
    pack(popup.body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *footer=box(GTK_ORIENTATION_HORIZONTAL,10);popup_status=label("","muted");pack(footer,popup_status,TRUE);
    pack(footer,button("Restore points",NULL,"manager"),FALSE);pack(popup.body,footer,FALSE);
    g_signal_connect(popup.window,"focus-out-event",G_CALLBACK(popup_focus_out),NULL);
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
        gboolean queued=next && !g_strcmp0(selected,str(next,"storeId")) && !strcmp(str(row->point,"id"),str(next,"pointId"));
        GtkAllocation a;gtk_widget_get_allocation(row->widget,&a);double x=18+row->depth*22,y=a.y+a.height/2.;
        double red=queued ? queued_color.red : .34,green=queued ? queued_color.green : .56,blue=queued ? queued_color.blue : .27;
        if (!row->path && !queued && !highlighted) {red=.53;green=.60;blue=.49;}
        if (highlighted) {cairo_set_source_rgba(cr,red,green,blue,.22);cairo_arc(cr,x,y,10,0,2*G_PI);cairo_fill(cr);}
        cairo_arc(cr,x,y,current ? 6 : 4,0,2*G_PI);
        if (current || highlighted || queued) cairo_set_source_rgb(cr,red,green,blue);else cairo_set_source_rgb(cr,.97,.98,.96);
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
    (void)data;removed_visible=gtk_toggle_button_get_active(toggle);render_history();render_inspector();
}

/* Reusable slots increase the capacity for future changes, without shrinking data files. */
static void update_history_sizes(void)
{
    for (guint i=0;rows && i<rows->len;i++)
    {
        HistoryRow *row=g_ptr_array_index(rows,i);if (!row->size) continue;
        char *text=reclaimable_text(find(array(history,"snapshots"),"id",str(row->point,"id")));
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(row->size)),text ? text : "")) gtk_label_set_text(GTK_LABEL(row->size),text ? text : "");
        g_free(text);
    }
}

/* Rebuild changed history only, preserving its scroll position and exact selected identity. */
static void render_history(void)
{
    GtkAdjustment *adjustment=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(history_scroll));
    double scroll=gtk_adjustment_get_value(adjustment);updating=TRUE;hover_row=NULL;
    gtk_list_box_set_placeholder(GTK_LIST_BOX(history_list),NULL);
    clear_box(history_list);if (rows) g_ptr_array_unref(rows);rows=g_ptr_array_new_with_free_func(row_free);
    gtk_widget_hide(show_removed);gtk_widget_hide(history_footer);
    gtk_button_set_label(GTK_BUTTON(history_latest),"Go to now ↓");
    const char *placeholder=NULL;
    if (!store()) placeholder=state ? "Restore points are available when changes are saved on a drive." : "Loading storage…";
    else if (!history || g_strcmp0(history_store,selected)) placeholder="Loading restore points…";
    if (placeholder)
    {
        history_placeholder(placeholder);updating=FALSE;return;
    }
    JsonObject *saved=point();
    if (selected_point && (!saved || (flag(saved,"deleted") && !removed_visible))) g_clear_pointer(&selected_point,g_free);
    GPtrArray *points=g_ptr_array_new();JsonArray *catalog=array(history,"snapshots");
    for (guint i=0;i<count(catalog);i++)
    {
        JsonObject *point=json_array_get_object_element(catalog,i);
        if (!flag(point,"hidden")) g_ptr_array_add(points,point);
    }
    gboolean has_removed=FALSE;
    for (guint i=0;i<points->len;i++) if (flag(g_ptr_array_index(points,i),"deleted")) has_removed=TRUE;
    gtk_widget_set_visible(show_removed,has_removed);
    JsonObject *current=json_object_new();gtk_widget_show(history_footer);json_object_set_string_member(current,"id","current");
    json_object_set_string_member(current,"parent",str(history,"currentParent"));g_ptr_array_add(points,current);
    GHashTable *path=g_hash_table_new(g_str_hash,g_str_equal);g_hash_table_add(path,"current");
    const char *parent=str(history,"currentParent");
    while (*parent && !g_hash_table_contains(path,parent))
    {
        g_hash_table_add(path,(gpointer)parent);JsonObject *ancestor=find(catalog,"id",parent);if (!ancestor) break;parent=str(ancestor,"parent");
    }
    append_history(points,path,"",0);
    JsonObject *next=object(state,"next");
    for (guint i=0;i<rows->len;i++)
    {
        HistoryRow *row=g_ptr_array_index(rows,i);const char *id=str(row->point,"id");
        /* Keep full ancestry for branch lanes; the checkbox controls deleted rows, not their records. */
        if (flag(row->point,"hidden") || (flag(row->point,"deleted") && !removed_visible)) continue;
        gboolean live=!strcmp(id,"current");
        row->widget=gtk_list_box_row_new();gtk_widget_set_size_request(row->widget,-1,36);
        GtkWidget *line=box(GTK_ORIENTATION_HORIZONTAL,8);gtk_widget_set_margin_start(line,36+row->depth*22);gtk_widget_set_margin_end(line,8);
        GtkWidget *name=label(live ? "Current state" : point_name(row->point),live ? "strong" : NULL);pack(line,name,TRUE);
        const char *badge=flag(row->point,"deleted") ? "Removed" : !strcmp(id,str(history,"currentParent")) ? "Latest restore point" : "";
        gboolean queued=next && !g_strcmp0(selected,str(next,"storeId")) && !strcmp(id,str(next,"pointId"));
        if (queued) badge="Next start";
        if (live) badge="You are here";
        if (*badge)
        {
            GtkWidget *status=label(badge,flag(row->point,"deleted") ? "muted" : "badge");
            if (queued) gtk_style_context_add_class(gtk_widget_get_style_context(status),"queued");
            pack(line,status,FALSE);
        }
        if (flag(row->point,"deleted")) gtk_style_context_add_class(gtk_widget_get_style_context(name),"muted");
        if (!live)
        {
            row->size=label("","muted");gtk_label_set_xalign(GTK_LABEL(row->size),1);gtk_widget_set_size_request(row->size,76,-1);pack(line,row->size,FALSE);
        }
        if (!live) {char *date=date_text(number(row->point,"created"));GtkWidget *time=label(date,"muted");gtk_widget_set_size_request(time,100,-1);pack(line,time,FALSE);g_free(date);}
        gtk_container_add(GTK_CONTAINER(row->widget),line);gtk_container_add(GTK_CONTAINER(history_list),row->widget);
        g_object_set_data_full(G_OBJECT(row->widget),"point-id",g_strdup(id),g_free);
        g_signal_connect(row->widget,"enter-notify-event",G_CALLBACK(history_hover),row);g_signal_connect(row->widget,"leave-notify-event",G_CALLBACK(history_hover),row);
        g_signal_connect(row->widget,"state-flags-changed",G_CALLBACK(history_state_changed),NULL);
        gtk_widget_add_events(row->widget,GDK_ENTER_NOTIFY_MASK|GDK_LEAVE_NOTIFY_MASK);
        if (live ? !selected_point : !g_strcmp0(selected_point,id)) gtk_list_box_select_row(GTK_LIST_BOX(history_list),GTK_LIST_BOX_ROW(row->widget));
    }
    g_hash_table_unref(path);g_ptr_array_unref(points);json_object_unref(current);
    gtk_widget_show_all(history_list);gtk_adjustment_set_value(adjustment,scroll);updating=FALSE;
    update_history_sizes();
    gtk_widget_queue_resize(history_scroll);
}

/* Build controls once so reports never replace a switch, focus target or text widget. */
static void make_inspector(void)
{
    inspector_eyebrow=label("","eyebrow");pack(inspector,inspector_eyebrow,FALSE);
    inspector_title=paragraph("");gtk_label_set_max_width_chars(GTK_LABEL(inspector_title),22);
    gtk_style_context_add_class(gtk_widget_get_style_context(inspector_title),"title");pack(inspector,inspector_title,FALSE);
    inspector_saved=box(GTK_ORIENTATION_VERTICAL,12);pack(inspector,inspector_saved,FALSE);
    inspector_date=label("","muted");pack(inspector_saved,inspector_date,FALSE);
    inspector_origin=paragraph("");pack(inspector_saved,inspector_origin,FALSE);
    inspector_removed=paragraph("This restore point can no longer be used to start Slax or browse saved files. The point stays in the tree to show where later restore points came from.");pack(inspector_saved,inspector_removed,FALSE);
    inspector_actions=box(GTK_ORIENTATION_VERTICAL,12);pack(inspector_saved,inspector_actions,FALSE);
    inspector_use=primary_button("Use at next start","use-point");pack(inspector_actions,inspector_use,FALSE);
    inspector_open=button("Open files",NULL,"open-files");pack(inspector_actions,inspector_open,FALSE);
    inspector_rename=button("Rename restore point",NULL,"rename-point");pack(inspector_actions,inspector_rename,FALSE);
    inspector_delete=button("Delete restore point",NULL,"delete-point");pack(inspector_actions,inspector_delete,FALSE);
    inspector_reclaim=paragraph("");pack(inspector_actions,inspector_reclaim,FALSE);
    inspector_current=box(GTK_ORIENTATION_VERTICAL,12);pack(inspector,inspector_current,FALSE);
    pack(inspector_current,save_control(&inspector_save,&inspector_choice,&inspector_help),FALSE);
    inspector_create=primary_button("Create restore point","create-point");pack(inspector_current,inspector_create,FALSE);
}

/* Refresh only changed properties; busy actions must not fade the inspector's text. */
static void render_inspector(void)
{
    JsonObject *item=store(),*saved=point();if (!item) return;
    gboolean busy=(mutating && !saving()) || restart_process,writable=flag(item,"writable") && !busy;
    gboolean initial=flag(saved,"initial"),removed=flag(saved,"deleted"),files_open=flag(saved,"filesOpen");
    JsonObject *next=object(state,"next");
    gboolean queued=saved && !g_strcmp0(selected,str(next,"storeId")) && !strcmp(str(saved,"id"),str(next,"pointId"));
    const char *eyebrow="YOU ARE HERE";
    if (saved) eyebrow=removed ? "REMOVED RESTORE POINT" : "SAVED RESTORE POINT";
    set_label(inspector_eyebrow,eyebrow);set_label(inspector_title,saved ? point_name(saved) : "Current state");
    gtk_widget_set_visible(inspector_saved,saved!=NULL);gtk_widget_set_visible(inspector_current,saved==NULL);
    char *date=date_text(number(saved,"created"));set_label(inspector_date,date);g_free(date);
    const char *origin="Kept until you delete it";
    if (initial) origin="Start with Slax’s original apps, settings and files. Other restore points stay available.";
    else if (flag(saved,"automatic")) origin="Saved automatically";
    set_label(inspector_origin,origin);
    GtkStyleContext *origin_style=gtk_widget_get_style_context(inspector_origin);
    if (initial) gtk_style_context_remove_class(origin_style,"muted");else gtk_style_context_add_class(origin_style,"muted");
    gtk_widget_set_visible(inspector_removed,removed);gtk_widget_set_visible(inspector_actions,!removed);
    const char *use_text=initial ? "Fresh start" : "Use at next start";
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(inspector_use)),queued ? "Cancel next-start selection" : use_text))
        gtk_button_set_label(GTK_BUTTON(inspector_use),queued ? "Cancel next-start selection" : use_text);
    GtkStyleContext *use_style=gtk_widget_get_style_context(inspector_use);
    if (queued) gtk_style_context_add_class(use_style,"queued-action");else gtk_style_context_remove_class(use_style,"queued-action");
    gtk_widget_set_sensitive(inspector_use,queued ? !busy : writable);gtk_widget_set_sensitive(inspector_open,!busy);
    gtk_widget_set_visible(inspector_rename,!initial);gtk_widget_set_sensitive(inspector_rename,writable);
    gtk_widget_set_visible(inspector_delete,!initial);gtk_widget_set_sensitive(inspector_delete,writable && !flag(saved,"protected") && !files_open);
    char *size=reclaimable_text(saved),*text=size ? g_strdup_printf("Deleting makes %s available for new changes.",size) : NULL;
    set_label(inspector_reclaim,text);gtk_widget_set_visible(inspector_reclaim,!initial && text!=NULL);g_free(size);g_free(text);
    update_save(inspector_save,inspector_choice,inspector_help);gtk_widget_set_sensitive(inspector_create,writable);
}

/* Choose the layout before GTK allocates children; changing orientation during allocation is too late. */
static gboolean manager_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    (void)widget;(void)data;gboolean narrow=event->width<900,vertical=event->width<650;
    if (narrow!=compact)
    {
        compact=narrow;
        gtk_orientable_set_orientation(GTK_ORIENTABLE(context_box),compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(intro_columns),compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
        gtk_container_set_border_width(GTK_CONTAINER(intro),compact ? 12 : 28);
        gtk_widget_set_halign(intro,compact ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(intro_example,compact ? -1 : 320,-1);
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
    (void)event;(void)data;gtk_widget_hide(widget);return TRUE;
}
static gboolean manager_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)data;if (event->keyval!=GDK_KEY_Escape) return FALSE;gtk_widget_hide(widget);return TRUE;
}

/* Draw the static example with the same nodes as history, without creating selectable records. */
static gboolean draw_intro_example(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;GList *children=gtk_container_get_children(GTK_CONTAINER(widget));GtkAllocation first,last;
    gtk_widget_get_allocation(children->data,&first);gtk_widget_get_allocation(g_list_last(children)->data,&last);
    int x,first_y,last_y;gtk_widget_translate_coordinates(children->data,widget,0,0,&x,&first_y);
    gtk_widget_translate_coordinates(g_list_last(children)->data,widget,0,0,&x,&last_y);
    cairo_set_source_rgb(cr,.34,.56,.27);cairo_set_line_width(cr,2);
    cairo_move_to(cr,16,first_y+first.height/2.);cairo_line_to(cr,16,last_y+last.height/2.);cairo_stroke(cr);
    guint index=0;
    for (GList *item=children;item;item=item->next,index++)
    {
        GtkAllocation row;int row_y;gtk_widget_get_allocation(item->data,&row);gtk_widget_translate_coordinates(item->data,widget,0,0,&x,&row_y);double y=row_y+row.height/2.;
        if (index==1) {cairo_set_source_rgba(cr,.34,.56,.27,.22);cairo_arc(cr,16,y,9,0,2*G_PI);cairo_fill(cr);}
        cairo_arc(cr,16,y,4,0,2*G_PI);cairo_set_source_rgb(cr,index==1 ? .34 : .97,index==1 ? .56 : .98,index==1 ? .27 : .96);
        cairo_fill_preserve(cr);cairo_set_source_rgb(cr,.34,.56,.27);cairo_set_line_width(cr,2);cairo_stroke(cr);
    }
    g_list_free(children);return FALSE;
}

/* Ordinary GTK windows get the system's decoration; only the tray surfaces paint their own outline. */
static void make_manager(void)
{
    manager=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(manager),"Restore points");
    gtk_window_set_wmclass(GTK_WINDOW(manager),"slax-status","slax-status");gtk_window_set_icon_name(GTK_WINDOW(manager),"slax-status");
    gtk_window_set_default_size(GTK_WINDOW(manager),1040,640);gtk_window_set_position(GTK_WINDOW(manager),GTK_WIN_POS_CENTER);
    gtk_widget_set_size_request(manager,540,440);
    g_signal_connect(manager,"delete-event",G_CALLBACK(manager_close),NULL);g_signal_connect(manager,"key-press-event",G_CALLBACK(manager_key),NULL);
    manager_body=box(GTK_ORIENTATION_VERTICAL,0);gtk_container_add(GTK_CONTAINER(manager),manager_body);
    context_box=box(GTK_ORIENTATION_HORIZONTAL,12);gtk_container_set_border_width(GTK_CONTAINER(context_box),16);
    pack(context_box,location_row("Running from",&manager_source),TRUE);pack(context_box,location_row("Changes on",&manager_storage),TRUE);pack(manager_body,context_box,FALSE);
    pack(manager_body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    manager_content=gtk_stack_new();gtk_stack_set_homogeneous(GTK_STACK(manager_content),FALSE);pack(manager_body,manager_content,TRUE);
    GtkWidget *intro_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(intro_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    intro=box(GTK_ORIENTATION_VERTICAL,14);gtk_container_set_border_width(GTK_CONTAINER(intro),28);
    gtk_widget_set_halign(intro,GTK_ALIGN_CENTER);gtk_widget_set_valign(intro,GTK_ALIGN_CENTER);gtk_container_add(GTK_CONTAINER(intro_scroll),intro);
    pack(intro,label("Keep changes after restart","heading"),FALSE);
    intro_columns=box(GTK_ORIENTATION_HORIZONTAL,28);pack(intro,intro_columns,FALSE);
    GtkWidget *explanation=box(GTK_ORIENTATION_VERTICAL,12);pack(intro_columns,explanation,TRUE);
    const char *texts[]={
        "This run uses RAM. Apps, settings and files changed inside Slax disappear when you shut down.",
        "Undo a change that went wrong",
        "Before installing an app or changing settings, make a restore point. You can restart from it to undo later changes, or open it to recover files.",
        "At next start, press Esc to open the boot menu. Choose “Choose a restore point”, select a drive and “Fresh start” to keep changes on that drive.",
        "You will start with a clean Slax. Changes made in this run are not copied."
    };
    for (guint i=0;i<G_N_ELEMENTS(texts);i++)
    {
        GtkWidget *text=i==1 ? label(texts[i],"strong") : paragraph(texts[i]);
        gtk_label_set_max_width_chars(GTK_LABEL(text),48);pack(explanation,text,FALSE);
    }
    intro_example=box(GTK_ORIENTATION_VERTICAL,10);gtk_widget_set_size_request(intro_example,320,-1);gtk_widget_set_valign(intro_example,GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(intro_example),"intro-example");pack(intro_columns,intro_example,FALSE);
    pack(intro_example,label("EXAMPLE","eyebrow"),FALSE);pack(intro_example,label("My Slax","title"),FALSE);
    pack(intro_example,label("Restore points on your drive","muted"),FALSE);
    GtkWidget *example_tree=box(GTK_ORIENTATION_VERTICAL,0);pack(intro_example,example_tree,FALSE);
    const char *examples[][2]={{"Initial state","A clean Slax"},{"Before installing an app","Your apps, settings and files"},{"After installing the app","The new app causes problems"}};
    for (guint i=0;i<G_N_ELEMENTS(examples);i++)
    {
        GtkWidget *row=box(GTK_ORIENTATION_VERTICAL,3);gtk_style_context_add_class(gtk_widget_get_style_context(row),"intro-example-row");
        if (i==1) gtk_style_context_add_class(gtk_widget_get_style_context(row),"intro-example-choice");
        pack(row,label(examples[i][0],i==1 ? "strong" : NULL),FALSE);pack(row,label(examples[i][1],"muted"),FALSE);pack(example_tree,row,FALSE);
    }
    g_signal_connect_after(example_tree,"draw",G_CALLBACK(draw_intro_example),NULL);
    GtkWidget *recovery=paragraph("Restart from “Before installing an app” to get your working Slax back.");
    gtk_label_set_max_width_chars(GTK_LABEL(recovery),30);pack(intro_example,recovery,FALSE);
    gtk_stack_add_named(GTK_STACK(manager_content),intro_scroll,"intro");
    main_layout=box(GTK_ORIENTATION_VERTICAL,12);gtk_stack_add_named(GTK_STACK(manager_content),main_layout,"stores");
    gtk_style_context_add_class(gtk_widget_get_style_context(main_layout),"storage-detail");
    history_layout=box(GTK_ORIENTATION_HORIZONTAL,18);pack(main_layout,history_layout,TRUE);
    history_box=box(GTK_ORIENTATION_VERTICAL,10);pack(history_layout,history_box,TRUE);
    pack(history_box,label("Restore points","heading"),FALSE);
    history_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(history_scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);pack(history_box,history_scroll,TRUE);
    g_signal_connect_after(history_scroll,"size-allocate",G_CALLBACK(reveal_history),NULL);
    history_list=gtk_list_box_new();gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list),GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(history_list),"history-list");gtk_container_add(GTK_CONTAINER(history_scroll),history_list);
    g_signal_connect(history_list,"row-selected",G_CALLBACK(history_selected),NULL);g_signal_connect_after(history_list,"draw",G_CALLBACK(draw_history),NULL);
    history_footer=box(GTK_ORIENTATION_HORIZONTAL,12);gtk_widget_set_margin_bottom(history_footer,10);
    show_removed=gtk_check_button_new_with_label("Show removed");gtk_widget_set_halign(show_removed,GTK_ALIGN_START);
    g_signal_connect(show_removed,"toggled",G_CALLBACK(removed_changed),NULL);
    history_latest=link_button("Go to now ↓","current");
    pack(history_footer,show_removed,TRUE);pack(history_footer,history_latest,FALSE);pack(history_box,history_footer,FALSE);
    inspector=box(GTK_ORIENTATION_VERTICAL,12);gtk_style_context_add_class(gtk_widget_get_style_context(inspector),"inspector");
    make_inspector();
    inspector_scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inspector_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_style_context_add_class(gtk_widget_get_style_context(inspector_scroll),"inspector-panel");
    gtk_widget_set_size_request(inspector_scroll,264,-1);gtk_container_add(GTK_CONTAINER(inspector_scroll),inspector);pack(history_layout,inspector_scroll,FALSE);
    pack(manager_body,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    GtkWidget *footer=box(GTK_ORIENTATION_HORIZONTAL,14);gtk_container_set_border_width(GTK_CONTAINER(footer),14);
    GtkWidget *next=box(GTK_ORIENTATION_HORIZONTAL,10);gtk_widget_set_halign(next,GTK_ALIGN_START);pack(footer,next,TRUE);
    pack(next,label("Next start:","muted"),FALSE);manager_next=label("Loading…",NULL);pack(next,manager_next,FALSE);
    /* Keep the native button's full height while an empty child hides only its contents. */
    manager_cancel_slot=gtk_stack_new();gtk_stack_set_hhomogeneous(GTK_STACK(manager_cancel_slot),FALSE);
    gtk_stack_add_named(GTK_STACK(manager_cancel_slot),box(GTK_ORIENTATION_HORIZONTAL,0),"empty");
    manager_cancel=link_button("Cancel","next-clear");gtk_stack_add_named(GTK_STACK(manager_cancel_slot),manager_cancel,"cancel");pack(next,manager_cancel_slot,FALSE);
    /* Capacity and operation feedback share one fixed footer slot, without moving the history. */
    manager_feedback=gtk_stack_new();manager_space=label("","muted");manager_status=label("","muted");
    gtk_stack_add_named(GTK_STACK(manager_feedback),manager_space,"capacity");gtk_stack_add_named(GTK_STACK(manager_feedback),manager_status,"status");
    pack(footer,manager_feedback,FALSE);manager_restart=button("Restart Slax","system-reboot-symbolic","restart");pack(footer,manager_restart,FALSE);
    pack(manager_body,footer,FALSE);
    g_signal_connect(manager,"configure-event",G_CALLBACK(manager_configure),NULL);
    gtk_widget_show_all(manager_body);
}

/* A manual manager request may focus its normal window; passive alerts never call this. */
static void show_manager(void)
{
    hide_popup();gtk_widget_hide(notice.window);gtk_widget_show(manager);gtk_window_present(GTK_WINDOW(manager));manager_dirty=TRUE;
    render_history();render_inspector();update_ui();refresh();
}

/* Hysteresis avoids warning flicker while allocations cross the low-space boundary. */
static const char *warning_kind(void)
{
    if (connection_error) return "service";
    if (*str(run(),"error")) return "error";
    JsonObject *active=find(array(state,"stores"),"id",str(run(),"storeId"));
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
    JsonObject *active=find(array(state,"stores"),"id",str(run(),"storeId"));
    const char *mode=state ? "Temporary run" : "Slax Status";
    if (active) mode=flag(run(),"save") ? "Saving changes" : "Changes will be discarded";
    set_label(popup_title,"Slax Status");set_label(popup_source,str(run(),"source"));set_label(popup_storage,str(run(),"storage"));
    set_label(manager_source,str(run(),"source"));set_label(manager_storage,str(run(),"storage"));
    gint64 available=number(run(),"freeBytes")+number(run(),"reusableBytes");
    char *size=size_text(available);
    set_label(popup_space,state ? size : "Loading…");
    double total=number(run(),"totalBytes");gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(popup_meter),total>0 ? CLAMP(1-available/total,0,1) : 0);
    update_save(popup_save,popup_choice,popup_help);
    render_inspector();
    char *next=next_text();set_label(popup_next,next);set_label(manager_next,next);g_free(next);
    gboolean queued=object(state,"next")!=NULL;gtk_widget_set_visible(popup_cancel,queued);
    gtk_stack_set_visible_child_name(GTK_STACK(manager_cancel_slot),queued ? "cancel" : "empty");
    gtk_widget_set_sensitive(popup_cancel,!mutating && !restart_process);gtk_widget_set_sensitive(manager_cancel,!mutating && !restart_process);
    gtk_widget_set_sensitive(manager_restart,!mutating && !restart_process);
    JsonObject *item=store();
    gtk_stack_set_visible_child_name(GTK_STACK(manager_content),item ? "stores" : "intro");
    if (item)
    {
        char *free=size_text(number(item,"freeBytes")+number(item,"reusableBytes"));
        char *text=g_strdup_printf("%s available",free);set_label(manager_space,text);g_free(free);g_free(text);
    }
    else set_label(manager_space,"");
    const char *warning=warning_kind();
    GtkStyleContext *meter_style=gtk_widget_get_style_context(popup_meter);
    gtk_style_context_remove_class(meter_style,"capacity-warning");gtk_style_context_remove_class(meter_style,"capacity-temporary");
    if (*warning) gtk_style_context_add_class(meter_style,"capacity-warning");
    else if (!flag(run(),"save")) gtk_style_context_add_class(meter_style,"capacity-temporary");
    const char *status=mutating ? "Working…" : connection_error ? "Storage service unavailable" : !strcmp(warning,"readonly") ? "Storage is read-only" : *warning ? "Storage needs attention" : snooze_until>g_get_monotonic_time() ? "Alerts snoozed · 15 min" : "";
    const char *operation="";
    if (mutating) operation="Working…";
    if (restart_process) operation="Restarting…";
    const char *feedback=connection_error;
    if (!feedback) feedback=*operation ? operation : history_error;
    set_label(popup_status,status);set_label(manager_status,feedback);
    gtk_stack_set_visible_child(GTK_STACK(manager_feedback),feedback ? manager_status : manager_space);
    char *tooltip=g_strdup_printf("Slax Status · %s · %s available",mode,size);
    if (tray) {gtk_widget_set_tooltip_text(tray,tooltip);gtk_widget_set_has_tooltip(tray,!gtk_widget_get_visible(popup.window));}
    g_free(tooltip);g_free(size);if (tray_icon) gtk_widget_queue_draw(tray_icon);
    if (!item && manager_dirty) {render_history();render_inspector();manager_dirty=FALSE;}
    gboolean anchored=tray && gtk_plug_get_embedded(GTK_PLUG(tray));
    if (anchored && *warning && g_strcmp0(warning,last_warning) && snooze_until<=g_get_monotonic_time() && !gtk_widget_get_visible(popup.window))
    {
        char *free=size_text(available),*text=NULL;const char *title="Space is running low";
        if (!strcmp(warning,"ram")) {title="Available memory is low";text=g_strdup_printf("About %s of RAM is available. Save important files to a drive and close unused applications.",free);}
        else if (!strcmp(warning,"readonly")) {title="Changes cannot be saved";text=g_strdup("The changes drive became read-only. Save important files to another writable drive and check the storage device.");}
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

/* Use the normal window-manager frame; destructive confirmations keep Cancel on the right as default. */
static gboolean confirm(const char *title, const char *question, const char *text, const char *accept)
{
    hide_popup();GtkWidget *dialog=gtk_message_dialog_new(gtk_widget_get_visible(manager) ? GTK_WINDOW(manager) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"%s",question);
    gtk_window_set_titlebar(GTK_WINDOW(dialog),NULL);gtk_window_set_title(GTK_WINDOW(dialog),title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",text);
    GtkWidget *area=gtk_widget_get_parent(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog)));
    gtk_widget_set_margin_start(area,0);gtk_widget_set_margin_end(area,0);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),accept,GTK_RESPONSE_ACCEPT,"Cancel",GTK_RESPONSE_CANCEL,NULL);gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
    gboolean accepted=gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT;gtk_widget_destroy(dialog);return accepted;
}

/* Report a refused normal reboot without blocking GTK or bypassing logind authorization. */
static void restart_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;GError *error=NULL;char *details=NULL;
    gboolean completed=g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source),result,NULL,&details,&error);
    gboolean successful=completed && g_subprocess_get_successful(G_SUBPROCESS(source));
    g_clear_object(&restart_process);update_ui();
    if (!completed) message("Cannot restart Slax",error->message);
    else if (!successful) message("Cannot restart Slax",details && *details ? details : "Save your open documents and try Restart from the desktop menu.");
    g_clear_error(&error);g_free(details);
}

/* Reflect DynFileFS's UTF-8 name rules before making an expensive freeze/create request. */
static const char *name_error(const char *name, const char *renamed)
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
    for (guint i=0;i<count(points);i++) {JsonObject *item=json_array_get_object_element(points,i);if (!flag(item,"deleted") && g_strcmp0(str(item,"id"),renamed) && !strcmp(str(item,"name"),name)) return "A restore point with this name already exists.";}
    return NULL;
}

/* Capture stable identities before a modal dialog while background reports keep refreshing. */
static void name_dialog(gboolean rename)
{
    hide_popup();g_autofree char *target=g_strdup(selected);
    g_autofree char *id=rename ? g_strdup(selected_point) : NULL,*original=rename ? g_strdup(point_name(point())) : NULL;
    const char *title=rename ? "Rename restore point" : "Create restore point";
    GtkWidget *dialog=gtk_dialog_new_with_buttons(title,GTK_WINDOW(manager),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        title,GTK_RESPONSE_ACCEPT,"Cancel",GTK_RESPONSE_CANCEL,NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog),480,-1);gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);
    GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(dialog));gtk_container_set_border_width(GTK_CONTAINER(content),18);gtk_box_set_spacing(GTK_BOX(content),12);
    const char *note="Save your current apps, settings and files so you can return to them later.";
    if (flag(store(),"active") && !flag(run(),"save")) note="Save your current apps, settings and files in a restore point. This point will be kept even though Don’t save is selected.";
    if (rename) note="Give this restore point a name you will recognize.";
    if (rename && flag(point(),"automatic")) note="Give this automatic restore point a name to keep it until you choose to delete it.";
    pack(content,paragraph(note),FALSE);pack(content,label("Name",NULL),FALSE);
    GtkWidget *entry=gtk_entry_new();gtk_entry_set_activates_default(GTK_ENTRY(entry),TRUE);gtk_entry_set_placeholder_text(GTK_ENTRY(entry),"e.g. Before upgrade");pack(content,entry,FALSE);
    if (rename) {gtk_entry_set_text(GTK_ENTRY(entry),original);gtk_editable_select_region(GTK_EDITABLE(entry),0,-1);}
    GtkWidget *error=label("","error");pack(content,error,FALSE);gtk_widget_show_all(content);gtk_widget_grab_focus(entry);
    while (gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT)
    {
        const char *name=gtk_entry_get_text(GTK_ENTRY(entry));
        if (rename && !strcmp(name,original)) break;
        const char *invalid=name_error(name,id);
        if (invalid) {set_label(error,invalid);continue;}
        JsonObject *request=command(rename ? "point.rename" : "point.create",FALSE);json_object_set_string_member(request,"name",name);json_object_set_string_member(request,"storeId",target);
        if (rename) json_object_set_string_member(request,"pointId",id);
        gtk_widget_destroy(dialog);send_request(request,TRUE);return;
    }
    gtk_widget_destroy(dialog);
}

/* Focus only a user-requested bubble; normal window input leaves global shortcuts available. */
static void show_popup(void)
{
    gtk_widget_hide(notice.window);update_ui();gtk_widget_show(popup.window);position_bubble(&popup);
    gtk_widget_set_has_tooltip(tray,FALSE);
    gtk_window_present(GTK_WINDOW(popup.window));gtk_widget_child_focus(popup.body,GTK_DIR_TAB_FORWARD);
    refresh();
}

/* The independent helper keeps the browser and its authenticated mount owner on one PID. */
static int browse_files(const char *store_id, const char *point_id)
{
    JsonObject *value=command("files.open",FALSE);json_object_set_string_member(value,"storeId",store_id);json_object_set_string_member(value,"pointId",point_id);
    Request request={.request=value};GTask *task=g_task_new(NULL,NULL,NULL,NULL);g_task_set_task_data(task,&request,NULL);
    g_task_run_in_thread_sync(task,request_worker);GError *error=NULL;JsonObject *reply=g_task_propagate_pointer(task,&error);g_object_unref(task);json_object_unref(value);
    if (!reply || !flag(reply,"ok"))
    {
        g_printerr("%s\n",error ? error->message : str(object(reply,"error"),"message"));g_clear_error(&error);
        if (reply) json_object_unref(reply);
        return 1;
    }
    char *path=g_strdup(str(object(reply,"result"),"path"));json_object_unref(reply);
    /* Browser diagnostics must not retain a pipe to a tray process that may exit. */
    int output=open("/dev/null",O_WRONLY|O_CLOEXEC);
    if (output<0 || dup2(output,STDERR_FILENO)<0) {g_printerr("%s\n",g_strerror(errno));g_free(path);if (output>=0) close(output);return 1;}
    close(output);
    execlp("pcmanfm","pcmanfm","--standalone","--new-win",path,NULL);
    g_printerr("Cannot start the file manager: %s\n",g_strerror(errno));g_free(path);return 1;
}

/* Closing the browser is ordinary success; launch failures remain visible in its parent UI. */
static void files_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;GError *error=NULL;char *err=NULL;
    gboolean ok=g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source),result,NULL,&err,&error);
    if (!ok || !g_subprocess_get_successful(G_SUBPROCESS(source)))
    {
        const char *detail=error ? error->message : "The file manager stopped unexpectedly.";
        if (!error && err && *err) detail=err;
        message("Cannot open restore point files",detail);
    }
    g_clear_error(&error);g_free(err);refresh();
}

/* Map visible actions directly to the backend contract; confirmation never mutates storage. */
static void action(GtkButton *button_widget, gpointer data)
{
    (void)button_widget;const char *op=data;
    if (!strcmp(op,"close-popup")) {hide_popup();return;}
    if (!strcmp(op,"manager")) {show_manager();return;}
    if (!strcmp(op,"dismiss")) {gtk_widget_hide(notice.window);return;}
    if (!strcmp(op,"snooze")) {snooze_until=g_get_monotonic_time()+15*60*G_USEC_PER_SEC;g_clear_pointer(&last_warning,g_free);gtk_widget_hide(notice.window);update_ui();return;}
    if (!strcmp(op,"review")) {show_popup();return;}
    if (!strcmp(op,"current"))
    {
        g_clear_pointer(&selected_point,g_free);reveal_current=TRUE;render_history();render_inspector();return;
    }
    if (mutating || restart_process) return;
    if (!strcmp(op,"restart"))
    {
        char *text=g_strdup("Save your open documents before restarting. Slax will close running applications.");
        if (state && !connection_error && !*str(run(),"error"))
        {
            const char *saving="Your changes will be saved.";
            if (!strcmp(str(run(),"kind"),"ram")) saving="Changes made in this RAM run will be lost.";
            else if (!flag(run(),"save")) saving="Changes from this run will be discarded.";
            else if (object(state,"next")) saving="Your current apps, settings and files will be kept in another restore point.";
            char *next=next_text();g_free(text);text=g_strdup_printf("Save your open documents before restarting.\n%s\n\nNext start: %s",saving,next);g_free(next);
        }
        gboolean accepted=confirm("Restart Slax","Restart Slax now?",text,"Restart Slax");g_free(text);
        if (accepted && !mutating)
        {
            GError *error=NULL;
            restart_process=g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE|G_SUBPROCESS_FLAGS_STDERR_PIPE,&error,"systemctl","reboot",NULL);
            if (restart_process) {update_ui();g_subprocess_communicate_utf8_async(restart_process,NULL,NULL,restart_done,NULL);}
            else {message("Cannot restart Slax",error->message);g_clear_error(&error);}
        }
        return;
    }
    if (!state) return;
    if (!strcmp(op,"next-clear")) {send_request(command("next.clear",FALSE),TRUE);return;}
    JsonObject *item=store(),*saved=point();if (!item) return;
    g_autofree char *target=g_strdup(selected);
    if (!strcmp(op,"create-point")) {name_dialog(FALSE);return;}
    if (!saved) return;
    if (!strcmp(op,"rename-point")) {name_dialog(TRUE);return;}
    g_autofree char *saved_id=g_strdup(str(saved,"id"));
    if (!strcmp(op,"use-point"))
    {
        JsonObject *next=object(state,"next");
        if (!g_strcmp0(target,str(next,"storeId")) && !strcmp(saved_id,str(next,"pointId")))
        {send_request(command("next.clear",FALSE),TRUE);return;}
        gboolean initial=flag(saved,"initial");char *question=initial ? g_strdup("Start with Slax’s original apps, settings and files?") : g_strdup_printf("Start Slax from “%s”?",point_name(saved));
        const char *saving="Other restore points will stay available.";
        if (flag(item,"active")) saving=flag(run(),"save") ? "Your current apps, settings and files will be kept in another restore point." : "Changes from this run will be discarded. Other restore points will stay available.";
        char *text=g_strdup_printf("%s\n\n%s",initial ? "At the next start, Slax will use the original apps, settings and files." : "At the next start, Slax will use the apps, settings and files saved in this restore point.",saving);
        gboolean accepted=confirm(initial ? "Fresh start" : "Use restore point at next start",question,text,initial ? "Fresh start" : "Use at next start");g_free(question);g_free(text);
        if (accepted) {JsonObject *request=command("next.set",TRUE);json_object_set_string_member(request,"storeId",target);json_object_set_string_member(request,"pointId",saved_id);send_request(request,TRUE);}return;
    }
    if (!strcmp(op,"delete-point"))
    {
        char *question=g_strdup_printf("Delete “%s”?",point_name(saved));
        JsonObject *next=object(state,"next");gboolean queued=!g_strcmp0(target,str(next,"storeId")) && !strcmp(saved_id,str(next,"pointId"));
        char *text=g_strconcat("You will no longer be able to start Slax from this restore point or open files saved in this restore point. Your current apps, settings and files and all other restore points will be kept.",queued ? "\n\nSlax will no longer use this restore point at the next start." : "",NULL);
        gboolean accepted=confirm("Delete restore point",question,text,"Delete restore point");g_free(question);g_free(text);
        if (accepted) {JsonObject *request=command("point.delete",TRUE);json_object_set_string_member(request,"storeId",target);json_object_set_string_member(request,"pointId",saved_id);send_request(request,TRUE);}return;
    }
    if (!strcmp(op,"open-files"))
    {
        GError *error=NULL;
        GSubprocess *process=g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE|G_SUBPROCESS_FLAGS_STDERR_PIPE,&error,
            "/usr/bin/slax-status","--open-files",selected,str(saved,"id"),NULL);
        if (process) {g_subprocess_communicate_utf8_async(process,NULL,NULL,files_finished,NULL);g_object_unref(process);}
        else {message("Cannot open restore point files",error->message);g_clear_error(&error);}
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
    (void)widget;(void)data;if (event->type!=GDK_BUTTON_PRESS || (event->button!=1 && event->button!=3)) return FALSE;
    if (gtk_widget_get_visible(popup.window)) hide_popup();else show_popup();return TRUE;
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
    gdk_rgba_parse(&queued_color,QUEUED_COLOR);
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#slax-status-tray, #slax-status-bubble { background: transparent; border: 0; box-shadow: none; }"
        "window { font-size: 14px; } .muted { color: #65705f; font-size: 13px; }"
        ".title { font-size: 20px; font-weight: 600; } .heading { font-size: 24px; font-weight: 400; } .metric { font-size: 32px; }"
        ".notice-title { font-size: 16px; font-weight: 600; }"
        ".intro-example { background: #eff1eb; border: 1px solid #d8ddd3; border-radius: 6px; padding: 16px; }"
        ".intro-example-row { padding: 8px 8px 8px 32px; border: 1px solid transparent; } .intro-example-choice { background: #e4ecdd; border-color: #c3d4b7; border-radius: 4px; }"
        ".strong { font-weight: 600; } .eyebrow { font-size: 13px; color: #65705f; }"
        ".running { color: #397b35; } .badge.queued { color: " QUEUED_COLOR "; } .error { color: #ba3932; }"
        ".storage-detail { padding: 18px 18px 0; } .history-list { background: transparent; }"
        ".history-list row { min-height: 34px; padding: 0; border: 1px solid transparent; border-radius: 4px; color: #292d29; box-shadow: none; background-image: none; }"
        ".history-list row:active { box-shadow: none; background-image: none; }"
        ".history-list row:selected { background: #e7eee0; border-color: #c3d4b7; } .history-list row:hover { background: #eef2e9; }"
        ".badge { color: #507343; font-size: 13px; } .inspector-panel { border-left: 1px solid #d8ddd3; } .inspector { padding: 4px 0 18px 18px; }"
        ".inspector-panel.stacked { border-left: 0; border-top: 1px solid #d8ddd3; } .inspector-panel.stacked .inspector { padding: 12px 0; }"
        "button.link { background: transparent; box-shadow: none; border: 0; color: #306c99; padding: 5px 0; }"
        "button.icon-button { min-width: 24px; min-height: 24px; padding: 0; border: 0; background: transparent; box-shadow: none; }"
        "button.icon-button:hover { background: #e2e8dc; }"
        "button.primary-action:not(:backdrop) { background: linear-gradient(#43863e,#397b35); color: white; border-color: #2e642a; border-bottom-color: #245320; box-shadow: inset 0 1px rgba(255,255,255,.2), 0 1px 2px rgba(0,0,0,.07); }"
        "button.primary-action:hover:not(:disabled):not(:backdrop) { background: linear-gradient(#367632,#2c6729); }"
        "button.primary-action:active:not(:disabled):not(:backdrop) { background: linear-gradient(#2b6027,#326e2e); border-bottom-color: #2e642a; box-shadow: inset 0 1px 2px rgba(0,0,0,.2); }"
        "button.primary-action.queued-action:not(:backdrop) { background: linear-gradient(shade(" QUEUED_COLOR ",1.08)," QUEUED_COLOR "); border-color: shade(" QUEUED_COLOR ",.8); border-bottom-color: shade(" QUEUED_COLOR ",.65); }"
        "button.primary-action.queued-action:hover:not(:disabled):not(:backdrop) { background: linear-gradient(" QUEUED_COLOR ",shade(" QUEUED_COLOR ",.85)); }"
        "button.primary-action.queued-action:active:not(:disabled):not(:backdrop) { background: linear-gradient(shade(" QUEUED_COLOR ",.75),shade(" QUEUED_COLOR ",.9)); border-bottom-color: shade(" QUEUED_COLOR ",.8); }"
        "progressbar progress { background: #579044; border: 0; min-height: 6px; } progressbar trough { min-height: 6px; }"
        "progressbar.capacity-warning progress { background: #ce413a; } progressbar.capacity-temporary progress { background: #2784d8; }",
        -1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
}

/* One tray per X display, screen and login UID; root and guest use the same unprivileged frontend. */
int main(int argc, char **argv)
{
    if (argc==4 && !strcmp(argv[1],"--open-files")) return browse_files(argv[2],argv[3]);
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
