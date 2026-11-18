/* The RAM introduction shares the confirmed Clear guide layout and native GTK controls. */
static GtkWidget *setup_drives,*setup_button,*setup_refresh,*setup_empty,*setup_none,*intro_title,*intro_footer;
static char *setup_rows_signature;

/* Installed symbolic SVGs use GTK's foreground and the same artwork as Partition Manager. */
static GtkWidget *setup_icon(const char *name, int size, gboolean badge)
{
    char *icon=g_strdup_printf("slax-status-%s-symbolic",name);
    GtkWidget *image=gtk_image_new_from_icon_name(icon,GTK_ICON_SIZE_MENU);g_free(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image),size);gtk_widget_set_valign(image,GTK_ALIGN_CENTER);
    if (!badge) return image;
    GtkWidget *holder=box(GTK_ORIENTATION_HORIZONTAL,0);pack(holder,image,FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(holder),"setup-icon");gtk_widget_set_valign(holder,GTK_ALIGN_START);return holder;
}

/* Wrap explanations within each column rather than letting a paragraph widen the window. */
static GtkWidget *setup_paragraph(const char *text)
{
    GtkWidget *widget=paragraph(text);gtk_label_set_max_width_chars(GTK_LABEL(widget),43);return widget;
}

/* The list's selected row is the only source of the proposed destination identity. */
static JsonObject *setup_selected(void)
{
    GtkListBoxRow *row=gtk_list_box_get_selected_row(GTK_LIST_BOX(setup_drives));
    return row ? find(array(state,"drives"),"id",g_object_get_data(G_OBJECT(row),"drive-id")) : NULL;
}

/* Native radio buttons and list selection describe the same user choice. */
static void setup_selection(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;(void)data;
    GtkWidget *radio=row ? g_object_get_data(G_OBJECT(row),"radio") : setup_none;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio),TRUE);
    gtk_widget_set_sensitive(setup_button,row && flag(setup_selected(),"canSetup") && !mutating && !restart_request);
}

/* Radio keyboard navigation also updates the enclosing row and restart action. */
static void setup_radio(GtkToggleButton *toggle, gpointer data)
{
    if (gtk_toggle_button_get_active(toggle)) gtk_list_box_select_row(GTK_LIST_BOX(setup_drives),GTK_LIST_BOX_ROW(data));
}

/* Rebuild only inventory changes; live free-space counters update existing labels in place. */
static void render_setup_drives(void)
{
    JsonArray *items=array(state,"drives");GString *key=g_string_new(NULL);
    const char *fields[]={"id","device","label","path","filesystem","icon","setupError"};
    for (guint i=0;i<count(items);i++)
    {
        JsonObject *drive=json_array_get_object_element(items,i);
        for (guint j=0;j<G_N_ELEMENTS(fields);j++) g_string_append_printf(key,"%s\n",str(drive,fields[j]));
        g_string_append_c(key,flag(drive,"canSetup") ? '1' : '0');
    }
    if (g_strcmp0(key->str,setup_rows_signature))
    {
        char *chosen=g_strdup(str(setup_selected(),"id"));clear_box(setup_drives);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(setup_none),TRUE);
        for (guint i=0;i<count(items);i++)
        {
            JsonObject *drive=json_array_get_object_element(items,i);GtkWidget *row=gtk_list_box_row_new();
            GtkWidget *line=box(GTK_ORIENTATION_HORIZONTAL,10),*text=box(GTK_ORIENTATION_VERTICAL,4);
            GtkWidget *radio=gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(setup_none));
            gtk_widget_set_valign(radio,GTK_ALIGN_CENTER);gtk_widget_set_sensitive(radio,flag(drive,"canSetup"));
            pack(line,radio,FALSE);pack(line,setup_icon(str(drive,"icon"),22,TRUE),FALSE);pack(line,text,TRUE);
            GtkWidget *name=label("","strong"),*details=label("","muted");
            gtk_label_set_max_width_chars(GTK_LABEL(name),42);gtk_label_set_max_width_chars(GTK_LABEL(details),42);
            /* Keep the access flag visible even when a long path is ellipsized. */
            GtkWidget *metadata=box(GTK_ORIENTATION_HORIZONTAL,8),*readonly=label("RO","muted");
            gtk_widget_set_tooltip_text(readonly,"Read-only");pack(metadata,details,TRUE);pack(metadata,readonly,FALSE);
            pack(text,name,FALSE);pack(text,metadata,FALSE);
            if (flag(drive,"writable") && *str(drive,"setupError"))
            {
                GtkWidget *reason=label(str(drive,"setupError"),"muted");gtk_label_set_max_width_chars(GTK_LABEL(reason),42);pack(text,reason,FALSE);
            }
            gtk_container_add(GTK_CONTAINER(row),line);gtk_container_add(GTK_CONTAINER(setup_drives),row);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row),flag(drive,"canSetup"));
            g_object_set_data_full(G_OBJECT(row),"drive-id",g_strdup(str(drive,"id")),g_free);
            g_object_set_data(G_OBJECT(row),"radio",radio);g_object_set_data(G_OBJECT(row),"name",name);g_object_set_data(G_OBJECT(row),"details",details);g_object_set_data(G_OBJECT(row),"readonly",readonly);
            g_signal_connect(radio,"toggled",G_CALLBACK(setup_radio),row);gtk_widget_show_all(row);
            if (flag(drive,"canSetup") && !strcmp(chosen,str(drive,"id"))) gtk_list_box_select_row(GTK_LIST_BOX(setup_drives),GTK_LIST_BOX_ROW(row));
        }
        g_free(chosen);g_free(setup_rows_signature);setup_rows_signature=g_strdup(key->str);
    }
    g_string_free(key,TRUE);
    for (guint i=0;i<count(items);i++)
    {
        JsonObject *drive=json_array_get_object_element(items,i);
        GtkListBoxRow *row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(setup_drives),i);
        char *name=*str(drive,"label") ? g_strdup_printf("%s (%s)",str(drive,"device"),str(drive,"label")) : g_strdup(str(drive,"device"));
        char *free=size_text(number(drive,"freeBytes"));
        char *details=g_strdup_printf("%s · %s · %s free",str(drive,"path"),str(drive,"filesystem"),free);
        set_label(g_object_get_data(G_OBJECT(row),"name"),name);set_label(g_object_get_data(G_OBJECT(row),"details"),details);
        gtk_widget_set_visible(g_object_get_data(G_OBJECT(row),"readonly"),!flag(drive,"writable"));
        gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(row),"radio"),flag(drive,"canSetup") && !mutating && !restart_request);
        gtk_list_box_row_set_selectable(row,flag(drive,"canSetup") && !mutating && !restart_request);
        g_free(name);g_free(free);g_free(details);
    }
    set_label(setup_empty,connection_error ? connection_error : !state ? "Loading mounted drives…" : "No mounted drives found. Mount a writable drive, then click Refresh.");
    gtk_widget_set_visible(setup_empty,!count(items));
    gtk_widget_set_sensitive(setup_refresh,!mutating && !restart_request);
    gtk_widget_set_sensitive(setup_button,flag(setup_selected(),"canSetup") && !mutating && !restart_request);
    const char *caption="Set up and restart…";
    if (restart_request) caption=mutating ? "Preparing…" : "Restarting…";
    if (strcmp(gtk_button_get_label(GTK_BUTTON(setup_button)),caption)) gtk_button_set_label(GTK_BUTTON(setup_button),caption);
    set_label(intro_title,!state ? "Loading storage…" : !strcmp(str(run(),"source"),"RAM") ? "This Slax is running in RAM" : "This run keeps changes in RAM");
}

/* Build the RAM page once; drive discovery and preparation remain in the broker's worker. */
static GtkWidget *make_ram_intro(void)
{
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    intro=box(GTK_ORIENTATION_VERTICAL,20);gtk_container_set_border_width(GTK_CONTAINER(intro),28);
    gtk_widget_set_valign(intro,GTK_ALIGN_FILL);gtk_container_add(GTK_CONTAINER(scroll),intro);
    GtkWidget *heading=box(GTK_ORIENTATION_HORIZONTAL,15);pack(heading,setup_icon("memory",28,TRUE),FALSE);
    intro_title=label("Loading storage…","heading");pack(heading,intro_title,TRUE);pack(intro,heading,FALSE);
    intro_columns=box(GTK_ORIENTATION_HORIZONTAL,18);pack(intro,intro_columns,TRUE);
    GtkWidget *left=box(GTK_ORIENTATION_VERTICAL,16);pack(intro_columns,left,TRUE);
    GtkWidget *benefits=box(GTK_ORIENTATION_VERTICAL,15);pack(left,benefits,FALSE);
    pack(benefits,label("For future runs","title"),FALSE);
    const char *items[][3]={{"hard-drive","Saved changes","Keep installed apps, settings and files between restarts."},{"clock-rotate-left","Saved states","Return to a working state or recover saved files. Open windows and unsaved work are not included."}};
    for (guint i=0;i<G_N_ELEMENTS(items);i++)
    {
        GtkWidget *row=box(GTK_ORIENTATION_HORIZONTAL,11),*text=box(GTK_ORIENTATION_VERTICAL,4);
        pack(row,setup_icon(items[i][0],21,FALSE),FALSE);pack(text,label(items[i][1],"strong"),FALSE);
        pack(text,setup_paragraph(items[i][2]),FALSE);pack(row,text,TRUE);pack(benefits,row,FALSE);
    }
    GtkWidget *loss=box(GTK_ORIENTATION_HORIZONTAL,10),*warning=box(GTK_ORIENTATION_VERTICAL,5);
    gtk_style_context_add_class(gtk_widget_get_style_context(loss),"ram-loss");pack(loss,setup_icon("circle-info",18,FALSE),FALSE);
    pack(warning,label("This run cannot be saved.","strong"),FALSE);
    pack(warning,setup_paragraph("Restart discards this run’s apps, settings and files. Copy important files to a drive first."),FALSE);
    pack(loss,warning,TRUE);pack(left,loss,FALSE);
    intro_example=box(GTK_ORIENTATION_VERTICAL,12);gtk_widget_set_size_request(intro_example,420,-1);
    gtk_style_context_add_class(gtk_widget_get_style_context(intro_example),"ram-setup");
    gtk_style_context_add_class(gtk_widget_get_style_context(intro_example),"inspector-panel");pack(intro_columns,intro_example,TRUE);
    heading=box(GTK_ORIENTATION_HORIZONTAL,11);pack(heading,setup_icon("hard-drive",21,FALSE),FALSE);pack(heading,label("Choose a drive","title"),TRUE);pack(intro_example,heading,FALSE);
    pack(intro_example,setup_paragraph("Existing saved changes will be resumed. Other files on the drive will be kept."),FALSE);
    pack(intro_example,label("Mounted drives","muted"),FALSE);
    setup_none=gtk_radio_button_new(NULL);gtk_widget_set_no_show_all(setup_none,TRUE);pack(intro_example,setup_none,FALSE);
    setup_drives=gtk_list_box_new();gtk_list_box_set_selection_mode(GTK_LIST_BOX(setup_drives),GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(setup_drives),"setup-drives");pack(intro_example,setup_drives,FALSE);
    g_signal_connect(setup_drives,"row-selected",G_CALLBACK(setup_selection),NULL);
    setup_empty=setup_paragraph("Loading mounted drives…");pack(intro_example,setup_empty,FALSE);
    setup_refresh=link_button("Refresh","storage.refresh");gtk_widget_set_halign(setup_refresh,GTK_ALIGN_END);pack(intro_example,setup_refresh,FALSE);
    pack(intro_example,gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE);
    setup_button=primary_button("Set up and restart…","storage.setup");gtk_widget_set_sensitive(setup_button,FALSE);pack(intro_example,setup_button,FALSE);
    return scroll;
}

/* Confirm the captured drive identity; Cancel is the native keyboard default. */
static void setup_restart(void)
{
    JsonObject *drive=setup_selected();if (!drive || !flag(drive,"canSetup")) return;
    char *id=g_strdup(str(drive,"id"));
    char *question=g_strdup_printf("Set up %s and restart?",*str(drive,"label") ? str(drive,"label") : str(drive,"device"));
    GtkWidget *dialog=gtk_message_dialog_new(GTK_WINDOW(manager),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"%s",question);g_free(question);
    gtk_window_set_titlebar(GTK_WINDOW(dialog),NULL);gtk_window_set_title(GTK_WINDOW(dialog),"Set up saved changes");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"Enable saved changes on %s. Existing saved changes and other files will be kept. Automatic resume will use this drive until you select another one.\n\nThis RAM run will be discarded. Current apps, settings and files will not be transferred. Copy files you need to a drive before continuing.",str(drive,"device"));
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),"Set up and restart",GTK_RESPONSE_ACCEPT,"Cancel",GTK_RESPONSE_CANCEL,NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
    gboolean accepted=gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT;gtk_widget_destroy(dialog);
    if (accepted && !mutating && !restart_request)
    {
        JsonObject *request=command("storage.prepare",FALSE);json_object_set_string_member(request,"driveId",id);
        json_object_set_boolean_member(request,"refresh",TRUE);restart_request=json_object_ref(request);send_request(request,TRUE);
    }
    g_free(id);
}
