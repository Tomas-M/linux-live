/* Keyboard tray and shared layout selection for the Slax desktop. */
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *code, *name, *flag; } Layout;
static GArray *layouts;
static Display *display;
static Window window;
static Atom command_atom, rules_atom;
static GtkWidget *tray, *label, *popup, *pages, *body;
static Atom tray_selection, manager_atom;
static GtkStyleContext *bubble_style;
static int arrow_x;
static gboolean arrow_bottom;
static int current, preferred=-1, input_opcode;
static guint remember_timer;
static gboolean pressed[256], chord, cancelled, super_tap;
static char *saved_path;

/* Find the canonical XKB identifier in the shared catalog. */
static int find_layout(const char *code)
{
    for (guint i=0; i<layouts->len; i++)
        if (!strcmp(g_array_index(layouts, Layout, i).code, code)) return i;
    return -1;
}

/* Keep existing labels and flags first, then include every installed XKB layout. */
static void load_layouts(void)
{
    layouts=g_array_new(FALSE, FALSE, sizeof(Layout));
    char *contents=NULL;
    GError *error=NULL;
    if (!g_file_get_contents("/usr/share/slax/keyboard-layouts.tsv", &contents, NULL, &error))
        g_error("%s", error->message);
    char **lines=g_strsplit(contents, "\n", -1);
    for (char **line=lines; *line; line++)
    {
        if (!**line) continue;
        char **fields=g_strsplit(*line, "\t", 3);
        Layout layout={g_strdup(fields[0]), g_strdup(fields[1]), g_strdup(fields[2])};
        g_array_append_val(layouts, layout);
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
    if (!g_file_get_contents("/usr/share/X11/xkb/rules/base.lst", &contents, NULL, &error))
        g_error("%s", error->message);
    lines=g_strsplit(contents, "\n", -1);
    gboolean section=FALSE;
    for (char **line=lines; *line; line++)
    {
        char *text=g_strstrip(*line);
        if (*text=='!') { section=!strcmp(g_strstrip(text+1), "layout"); continue; }
        if (!section || !*text) continue;
        char *name=text+strcspn(text, " \t");
        if (!*name) continue;
        *name++=0;
        if (find_layout(text)>=0) continue;
        Layout layout={g_strdup(text), g_strdup(g_strstrip(name)), g_strdup("")};
        g_array_append_val(layouts, layout);
    }
    g_strfreev(lines);
    g_free(contents);
    /* Rules can advertise a custom layout even when no symbols file exists. */
    for (guint i=0; i<layouts->len;)
    {
        Layout *layout=&g_array_index(layouts, Layout, i);
        char *path=g_build_filename("/usr/share/X11/xkb/symbols", layout->code, NULL);
        gboolean available=g_file_test(path, G_FILE_TEST_IS_REGULAR);
        g_free(path);
        if (available) { i++; continue; }
        g_free(layout->code);
        g_free(layout->name);
        g_free(layout->flag);
        g_array_remove_index(layouts, i);
    }
}

/* Let GTK render text directly at the panel's native resolution. */
static void draw_label(void)
{
    if (!label) return;
    Layout *layout=&g_array_index(layouts, Layout, current);
    char text[3]={g_ascii_toupper(layout->code[0]), g_ascii_toupper(layout->code[1]), 0};
    if (!strcmp(layout->code, "us")) strcpy(text, "EN");
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_tooltip_text(tray, layout->name);
}

/* Persist only the alternate; English remains the initial layout on every login. */
static void remember_layout(void)
{
    if (!current) return;
    preferred=current;
    GError *error=NULL;
    if (!g_file_set_contents(saved_path, g_array_index(layouts, Layout, preferred).code, -1, &error))
        g_warning("%s", error->message);
    g_clear_error(&error);
}

/* An uninterrupted thirty seconds on a non-English layout ends exploration. */
static gboolean remember_timeout(gpointer data)
{
    (void)data;
    remember_timer=0;
    remember_layout();
    return G_SOURCE_REMOVE;
}

/* Update the indicator and restart dwell timing only when the actual layout changes. */
static void show_layout(int index)
{
    current=index;
    if (remember_timer) g_source_remove(remember_timer);
    remember_timer=0;
    if (preferred<0 && current) remember_timer=g_timeout_add_seconds(30, remember_timeout, NULL);
    draw_label();
}

/* Apply a catalog entry without shell quoting or resetting unrelated XKB options. */
static gboolean select_layout(int index, gboolean remember)
{
    Layout *layout=&g_array_index(layouts, Layout, index);
    char *argv[]={"setxkbmap", "-layout", layout->code, "-variant", "", NULL};
    int status;
    GError *error=NULL;
    gboolean ok=g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &error);
    if (ok) ok=g_spawn_check_wait_status(status, &error);
    if (!ok)
    {
        g_warning("Cannot select %s: %s", layout->code, error->message);
        g_clear_error(&error);
        return FALSE;
    }
    current=index;
    if (remember) remember_layout();
    show_layout(index);
    return TRUE;
}

/* Follow XKB changes made outside this applet, including the active group. */
static void read_layout(void)
{
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data=NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display), rules_atom, 0, 1024, False,
                           XA_STRING, &type, &format, &count, &remaining, &data)!=Success || !data) return;
    char *field=(char *)data, *end=field+count;
    for (int i=0; i<2 && field<end; i++) field+=strnlen(field, end-field)+1;
    if (field<end && memchr(field, 0, end-field))
    {
        char **groups=g_strsplit(field, ",", -1);
        XkbStateRec state;
        if (XkbGetState(display, XkbUseCoreKbd, &state)==Success && state.group<g_strv_length(groups))
        {
            int index=find_layout(groups[state.group]);
            if (index>=0 && index!=current) show_layout(index);
        }
        g_strfreev(groups);
    }
    XFree(data);
}

/* Recognize complete layout chords without stealing extended application shortcuts. */
static void key_event(XIRawEvent *event)
{
    if (event->detail<8 || event->detail>=256) return;
    const unsigned int space_key=1u<<8;
    gboolean press=event->evtype==XI_RawKeyPress, other=FALSE;
    pressed[event->detail]=press;
    unsigned int held=0;
    for (int key=8; key<256; key++)
    {
        if (!pressed[key]) continue;
        KeySym symbol=XkbKeycodeToKeysym(display, key, 0, 0);
        if (symbol==XK_Control_L || symbol==XK_Control_R) held|=ControlMask;
        else if (symbol==XK_Shift_L || symbol==XK_Shift_R) held|=ShiftMask;
        /* AltGr remains a character modifier, not an Alt+Shift shortcut. */
        else if (symbol==XK_Alt_L || symbol==XK_Alt_R) held|=Mod1Mask;
        else if (symbol==XK_Super_L || symbol==XK_Super_R) held|=Mod4Mask;
        else if (symbol==XK_space) held|=space_key;
        else other=TRUE;
    }
    gboolean switch_keys=held==(ControlMask|ShiftMask) || held==(Mod1Mask|ShiftMask) || held==(Mod4Mask|space_key);
    gboolean single_key=(held&(held-1))==0;
    if (other || (!single_key && !switch_keys)) cancelled=TRUE;
    if (switch_keys && !cancelled) chord=TRUE;
    if (press)
    {
        KeySym symbol=XkbKeycodeToKeysym(display, event->detail, 0, 0);
        /* Only a lone left Super press can become a launcher tap. */
        super_tap=symbol==XK_Super_L && held==Mod4Mask && !other && !chord;
    }
    if (!held)
    {
        if (chord && !cancelled)
        {
            int next=(current+1)%layouts->len;
            if (preferred>=0) next=current ? 0 : preferred;
            else
            {
                /* Explore only the flagged layouts from the main menu. */
                while (!*g_array_index(layouts, Layout, next).flag)
                    next=(next+1)%layouts->len;
            }
            select_layout(next, FALSE);
        }
        else if (super_tap && !cancelled)
            g_spawn_command_line_async("fbappselect", NULL);
        chord=super_tap=FALSE;
        cancelled=other;
    }
}

static void dock_tray(void);

/* Process selection requests, real layout changes and global modifier events without grabs. */
static gboolean x_events(GIOChannel *channel, GIOCondition condition, gpointer data)
{
    (void)channel; (void)data;
    if (condition&(G_IO_HUP|G_IO_ERR)) { gtk_main_quit(); return G_SOURCE_REMOVE; }
    while (XPending(display))
    {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type==ClientMessage && event.xclient.message_type==command_atom)
        {
            char *code=XGetAtomName(display, event.xclient.data.l[0]);
            int index=find_layout(code);
            if (index>=0) select_layout(index, TRUE);
            XFree(code);
        }
        else if (event.type==ClientMessage && event.xclient.message_type==manager_atom &&
                 (Atom)event.xclient.data.l[1]==tray_selection) dock_tray();
        else if (event.type==PropertyNotify && event.xproperty.atom==rules_atom) read_layout();
        else if (event.type==GenericEvent && event.xcookie.extension==input_opcode)
        {
            if (XGetEventData(display, &event.xcookie))
            {
                if (event.xcookie.evtype==XI_RawKeyPress || event.xcookie.evtype==XI_RawKeyRelease)
                    key_event(event.xcookie.data);
                XFreeEventData(display, &event.xcookie);
            }
        }
        else if (event.type==SelectionClear) gtk_main_quit();
        else read_layout();
    }
    return G_SOURCE_CONTINUE;
}

/* Release the popup grab before returning input to applications. */
static void close_popup(void)
{
    gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(popup)));
    if (gtk_widget_has_grab(popup)) gtk_grab_remove(popup);
    gtk_widget_hide(popup);
}

/* A bubble selection immediately fixes the alternate and dismisses the popup. */
static void menu_select(GtkButton *button, gpointer data)
{
    (void)button;
    if (select_layout(GPOINTER_TO_INT(data), TRUE)) close_popup();
}

/* Fit each menu page to the tray's monitor and keep the arrow over its text. */
static void position_popup(void)
{
    int x, y;
    GtkAllocation anchor;
    gtk_widget_get_allocation(tray, &anchor);
    gdk_window_get_origin(gtk_widget_get_window(tray), &x, &y);
    GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(tray), gtk_widget_get_window(tray));
    GdkRectangle area;
    gdk_monitor_get_geometry(monitor, &area);
    GtkRequisition natural;
    gtk_widget_get_preferred_size(pages, NULL, &natural);
    int width=natural.width+24, height=natural.height+36;
    arrow_bottom=y-area.y>=height;
    gtk_widget_set_margin_top(body, arrow_bottom ? 12 : 24);
    gtk_widget_set_margin_bottom(body, arrow_bottom ? 24 : 12);
    int left=CLAMP(x+anchor.width/2-width/2, area.x, MAX(area.x, area.x+area.width-width));
    int top=arrow_bottom ? y-height : y+anchor.height;
    arrow_x=CLAMP(x+anchor.width/2-left, 20, width-20);
    gtk_window_resize(GTK_WINDOW(popup), width, height);
    gtk_window_move(GTK_WINDOW(popup), left, top);
    gtk_widget_queue_draw(popup);
}

/* Model-button navigation can change the bubble's natural width and height. */
static void page_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object; (void)spec; (void)data;
    if (gtk_widget_get_visible(popup)) position_popup();
}

/* Render the theme's rounded popover body and a matching triangular tail. */
static gboolean bubble_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    int width=gtk_widget_get_allocated_width(widget), height=gtk_widget_get_allocated_height(widget);
    int top=arrow_bottom ? 0 : 12, edge=arrow_bottom ? height-12 : 12;
    int tip=arrow_bottom ? height-1 : 1;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    gtk_render_background(bubble_style, cr, 0, top, width, height-12);
    /* Leave the body outline open where the tail joins it. */
    gtk_render_frame_gap(bubble_style, cr, 0, top, width, height-12,
                         arrow_bottom ? GTK_POS_BOTTOM : GTK_POS_TOP, arrow_x-10, arrow_x+10);
    GdkRGBA background, border;
    gtk_style_context_get_background_color(bubble_style, GTK_STATE_FLAG_NORMAL, &background);
    gtk_style_context_get_border_color(bubble_style, GTK_STATE_FLAG_NORMAL, &border);
    cairo_move_to(cr, arrow_x-10, edge);
    cairo_line_to(cr, arrow_x, tip);
    cairo_line_to(cr, arrow_x+10, edge);
    cairo_close_path(cr);
    gdk_cairo_set_source_rgba(cr, &background);
    cairo_fill(cr);
    cairo_move_to(cr, arrow_x-10, edge);
    cairo_line_to(cr, arrow_x, tip);
    cairo_line_to(cr, arrow_x+10, edge);
    gdk_cairo_set_source_rgba(cr, &border);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_restore(cr);
    return FALSE;
}

/* A pointer grab delivers outside clicks to the popup for dismissal. */
static gboolean popup_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)data;
    int x, y;
    gdk_window_get_origin(gtk_widget_get_window(widget), &x, &y);
    gboolean outside_x=event->x_root<x || event->x_root>=x+gtk_widget_get_allocated_width(widget);
    gboolean outside_y=event->y_root<y || event->y_root>=y+gtk_widget_get_allocated_height(widget);
    if (!outside_x && !outside_y) return FALSE;
    close_popup();
    return TRUE;
}

/* Escape dismisses the bubble without changing the selected keyboard. */
static gboolean popup_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget; (void)data;
    if (event->keyval!=GDK_KEY_Escape) return FALSE;
    close_popup();
    return TRUE;
}

/* Anchor the bubble above or below the native tray widget on either mouse button. */
static gboolean popup_menu(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; (void)data;
    if (event->button!=1 && event->button!=3) return FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(pages), "main");
    position_popup();
    gtk_widget_show(popup);
    GdkSeat *seat=gdk_display_get_default_seat(gtk_widget_get_display(popup));
    if (gdk_seat_grab(seat, gtk_widget_get_window(popup), GDK_SEAT_CAPABILITY_ALL, TRUE,
                      NULL, (GdkEvent *)event, NULL, NULL)!=GDK_GRAB_SUCCESS)
    {
        gtk_widget_hide(popup);
        return TRUE;
    }
    gtk_grab_add(popup);
    gtk_widget_child_focus(pages, GTK_DIR_TAB_FORWARD);
    return TRUE;
}

/* Recreate the embedded widget when the panel's tray manager is replaced. */
static void tray_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (popup) { close_popup(); gtk_widget_destroy(popup); }
    g_clear_object(&bubble_style);
    tray=label=popup=pages=body=NULL;
}

/* Clear the RGBA plug, leaving the panel's real background visible. */
static gboolean tray_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget; (void)data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

/* Embed a real GTK label using the standard X11 tray protocol, never a scaled icon. */
static void dock_tray(void)
{
    Window manager=XGetSelectionOwner(display, tray_selection);
    if (!manager) return;
    if (!tray)
    {
        tray=gtk_plug_new(0);
        gtk_window_set_title(GTK_WINDOW(tray), "Keyboard layout");
        gtk_window_set_wmclass(GTK_WINDOW(tray), "fbkeyboard", "fbkeyboard");
        gtk_widget_set_name(tray, "slax-keyboard-tray");
        gtk_widget_set_app_paintable(tray, TRUE);
        gtk_widget_set_size_request(tray, 22, 22);
        GdkScreen *screen=gtk_widget_get_screen(tray);
        GdkVisual *visual=gdk_screen_get_rgba_visual(screen);
        Atom type;
        int format;
        unsigned long count, remaining;
        unsigned char *data=NULL;
        XGetWindowProperty(display, manager, XInternAtom(display, "_NET_SYSTEM_TRAY_VISUAL", False),
                           0, 1, False, AnyPropertyType, &type, &format, &count, &remaining, &data);
        if (data && format==32 && count==1)
            visual=gdk_x11_screen_lookup_visual(screen, *(unsigned long *)data);
        if (data) XFree(data);
        if (visual) gtk_widget_set_visual(tray, visual);
        GtkWidget *anchor=gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(anchor), FALSE);
        gtk_widget_add_events(anchor, GDK_BUTTON_PRESS_MASK);
        label=gtk_label_new("");
        gtk_widget_set_name(label, "slax-keyboard-label");
        gtk_container_add(GTK_CONTAINER(anchor), label);
        gtk_container_add(GTK_CONTAINER(tray), anchor);
        GtkCssProvider *css=gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#slax-keyboard-tray, #slax-keyboard-popup { background: transparent; border: 0; box-shadow: none; padding: 0; }"
            "#slax-keyboard-label { color: white; font: 14px Sans; padding: 0; margin: 0; }", -1, NULL);
        gtk_style_context_add_provider(gtk_widget_get_style_context(tray), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER+1);
        gtk_style_context_add_provider(gtk_widget_get_style_context(label), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER+1);

        g_signal_connect(tray, "draw", G_CALLBACK(tray_draw), NULL);
        g_signal_connect(tray, "destroy", G_CALLBACK(tray_destroyed), NULL);
        g_signal_connect(anchor, "button-press-event", G_CALLBACK(popup_menu), NULL);
        popup=gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_title(GTK_WINDOW(popup), "Keyboard layouts");
        gtk_window_set_role(GTK_WINDOW(popup), "slax-keyboard-popup");
        gtk_window_set_type_hint(GTK_WINDOW(popup), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
        gtk_widget_set_name(popup, "slax-keyboard-popup");
        gtk_widget_set_app_paintable(popup, TRUE);
        GdkVisual *rgba=gdk_screen_get_rgba_visual(screen);
        if (rgba) gtk_widget_set_visual(popup, rgba);
        gtk_style_context_add_provider(gtk_widget_get_style_context(popup), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER+1);
        g_object_unref(css);
        GtkWidgetPath *path=gtk_widget_path_new();
        gtk_widget_path_append_type(path, GTK_TYPE_POPOVER);
        gtk_widget_path_iter_set_object_name(path, -1, "popover");
        bubble_style=gtk_style_context_new();
        gtk_style_context_set_path(bubble_style, path);
        gtk_style_context_set_screen(bubble_style, screen);
        gtk_style_context_add_class(bubble_style, GTK_STYLE_CLASS_BACKGROUND);
        gtk_widget_path_unref(path);
        g_signal_connect(popup, "draw", G_CALLBACK(bubble_draw), NULL);
        g_signal_connect(popup, "button-press-event", G_CALLBACK(popup_press), NULL);
        g_signal_connect(popup, "key-press-event", G_CALLBACK(popup_key), NULL);
        gtk_widget_add_events(popup, GDK_BUTTON_PRESS_MASK|GDK_KEY_PRESS_MASK);
        body=gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_start(body, 12);
        gtk_widget_set_margin_end(body, 12);
        gtk_container_add(GTK_CONTAINER(popup), body);
        pages=gtk_stack_new();
        gtk_stack_set_homogeneous(GTK_STACK(pages), FALSE);
        GtkWidget *main=gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *other=gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *other_page=gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *scroll=gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_container_add(GTK_CONTAINER(scroll), other);
        gtk_stack_add_named(GTK_STACK(pages), main, "main");
        gtk_stack_add_named(GTK_STACK(pages), other_page, "other");
        gtk_container_add(GTK_CONTAINER(body), pages);
        g_signal_connect(pages, "notify::visible-child", G_CALLBACK(page_changed), NULL);
        GtkWidget *back=gtk_model_button_new();
        g_object_set(back, "text", "Keyboard layout", "menu-name", "main", "inverted", TRUE, NULL);
        gtk_box_pack_start(GTK_BOX(other_page), back, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(other_page), scroll, TRUE, TRUE, 0);
        for (guint i=0; i<layouts->len; i++)
        {
            Layout *layout=&g_array_index(layouts, Layout, i);
            GtkWidget *entry=gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(entry), GTK_RELIEF_NONE);
            GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_container_add(GTK_CONTAINER(entry), row);
            if (*layout->flag)
                gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_file(layout->flag), FALSE, FALSE, 0);
            GtkWidget *name=gtk_label_new(layout->name);
            gtk_label_set_xalign(GTK_LABEL(name), 0);
            gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(*layout->flag ? main : other), entry, FALSE, FALSE, 0);
            g_signal_connect(entry, "clicked", G_CALLBACK(menu_select), GINT_TO_POINTER(i));
        }
        GtkWidget *more=gtk_model_button_new();
        g_object_set(more, "text", "Other", "menu-name", "other", NULL);
        gtk_box_pack_start(GTK_BOX(main), more, FALSE, FALSE, 0);
        gtk_widget_show_all(body);
        draw_label();
        gtk_widget_show_all(tray);
    }
    XEvent event={0};
    event.xclient.type=ClientMessage;
    event.xclient.window=manager;
    event.xclient.message_type=XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
    event.xclient.format=32;
    event.xclient.data.l[0]=CurrentTime;
    event.xclient.data.l[1]=0;
    event.xclient.data.l[2]=gdk_x11_window_get_xid(gtk_widget_get_window(tray));
    XSendEvent(display, manager, False, NoEventMask, &event);
    XFlush(display);
}

/* Run one tray per X screen, or send a canonical selection to that tray. */
int main(int argc, char **argv)
{
    load_layouts();
    display=XOpenDisplay(NULL);
    if (!display) { g_printerr("Cannot open X display.\n"); return 1; }
    char *selection_name=g_strdup_printf("_SLAX_KEYBOARD_S%d", DefaultScreen(display));
    Atom selection=XInternAtom(display, selection_name, False);
    g_free(selection_name);
    command_atom=XInternAtom(display, "_SLAX_KEYBOARD_SELECT", False);
    Window owner=XGetSelectionOwner(display, selection);
    if (argc==3 && !strcmp(argv[1], "--select"))
    {
        if (find_layout(argv[2])<0 || !owner) { g_printerr("Unknown layout or keyboard tray is not running.\n"); return 1; }
        XEvent event={0};
        event.xclient.type=ClientMessage;
        event.xclient.window=owner;
        event.xclient.message_type=command_atom;
        event.xclient.format=32;
        event.xclient.data.l[0]=XInternAtom(display, argv[2], False);
        XSendEvent(display, owner, False, NoEventMask, &event);
        XSync(display, False);
        XCloseDisplay(display);
        return 0;
    }
    if (argc!=1) { g_printerr("Usage: fbkeyboard [--select LAYOUT]\n"); return 1; }
    if (owner) return 0;
    gtk_init(&argc, &argv);
    window=XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    XSetSelectionOwner(display, selection, window, CurrentTime);
    saved_path=g_build_filename(g_get_home_dir(), ".fluxbox", "kblayout", NULL);
    char *saved=NULL;
    if (g_file_get_contents(saved_path, &saved, NULL, NULL))
    {
        preferred=find_layout(g_strstrip(saved));
        if (!preferred) preferred=-1;
        g_free(saved);
    }
    if (!select_layout(0, FALSE)) return 1;
    rules_atom=XInternAtom(display, "_XKB_RULES_NAMES", False);
    XSelectInput(display, DefaultRootWindow(display), PropertyChangeMask|StructureNotifyMask);
    char *tray_name=g_strdup_printf("_NET_SYSTEM_TRAY_S%d", DefaultScreen(display));
    tray_selection=XInternAtom(display, tray_name, False);
    g_free(tray_name);
    manager_atom=XInternAtom(display, "MANAGER", False);
    dock_tray();
    XkbSelectEventDetails(display, XkbUseCoreKbd, XkbStateNotify, XkbGroupStateMask, XkbGroupStateMask);
    /* XI 2.1 keeps modifier events flowing while a popup owns the keyboard. */
    int event, error, major=2, minor=1;
    if (!XQueryExtension(display, "XInputExtension", &input_opcode, &event, &error) ||
        XIQueryVersion(display, &major, &minor)!=Success) return 1;
    unsigned char mask[XIMaskLen(XI_RawKeyRelease)]={0};
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XIEventMask events={XIAllMasterDevices, sizeof(mask), mask};
    XISelectEvents(display, DefaultRootWindow(display), &events, 1);
    char keys[32];
    XQueryKeymap(display, keys);
    for (int key=0; key<256; key++) pressed[key]=(keys[key/8]&(1<<(key%8)))!=0;
    GIOChannel *channel=g_io_channel_unix_new(ConnectionNumber(display));
    g_io_add_watch(channel, G_IO_IN|G_IO_HUP|G_IO_ERR, x_events, NULL);
    x_events(channel, G_IO_IN, NULL);
    gtk_main();
    XCloseDisplay(display);
    return 0;
}
