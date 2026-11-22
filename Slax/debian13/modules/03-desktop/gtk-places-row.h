#include "gtkicontheme.h"
#include "gtkdrawingarea.h"
#include "slax-places-labels.h"

/* Render the bundled stroke icons directly at device resolution, retaining their alpha. */
static gboolean slax_places_symbol(GtkWidget *area,cairo_t *cr,gpointer data)
{
  GtkSidebarRow *row=data;
  const char *name=g_object_get_data(G_OBJECT(row),"slax-symbol");
  int size=!strcmp(name,"slax-places-lock-symbolic")?13:14;
  int scale=gtk_widget_get_scale_factor(area);
  GtkIconTheme *theme=gtk_icon_theme_get_for_screen(gtk_widget_get_screen(area));
  GtkIconInfo *info=gtk_icon_theme_lookup_icon_for_scale(theme,name,size,scale,GTK_ICON_LOOKUP_FORCE_SIZE);
  GdkPixbuf *pixbuf=gtk_icon_info_load_icon(info,NULL);
  g_object_unref(info);
  if (!pixbuf) return FALSE;
  cairo_surface_t *surface=gdk_cairo_surface_create_from_pixbuf(pixbuf,scale,gtk_widget_get_window(area));
  GdkRGBA color;
  gtk_style_context_get_color(gtk_widget_get_style_context(row->eject_button),gtk_widget_get_state_flags(row->eject_button),&color);
  gdk_cairo_set_source_rgba(cr,&color);
  int x=(gtk_widget_get_allocated_width(area)-size)*scale/2,y=(gtk_widget_get_allocated_height(area)-size)*scale/2;
  cairo_mask_surface(cr,surface,x/(double)scale,y/(double)scale);
  cairo_surface_destroy(surface);g_object_unref(pixbuf);
  return FALSE;
}

/* Keep ordinary GTK rows and their native input, adding the shared Slax presentation. */
void gtk_sidebar_row_set_slax(GtkSidebarRow *row,GVariant *location,const char *operation)
{
  gtk_widget_set_margin_top(GTK_WIDGET(row),0);
  gtk_widget_set_margin_bottom(GTK_WIDGET(row),0);
  gtk_widget_set_margin_end(row->start_icon_widget,8);
  gtk_image_set_pixel_size(GTK_IMAGE(row->start_icon_widget),24);
  gtk_widget_set_has_tooltip(GTK_WIDGET(row),FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(row),-1,location?48:36);
  if (!location) return;
  g_object_set_data_full(G_OBJECT(row),"slax-location",g_variant_ref(location),(GDestroyNotify)g_variant_unref);
  const char *path,*reason;
  g_variant_get_child(location,6,"&s",&path);
  g_variant_get_child(location,8,"&s",&reason);
  gtk_widget_set_opacity(row->start_icon_widget,*path?1.0:.58);
  char *detail=slax_places_detail(location,operation);
  char *label=g_markup_escape_text(row->label,-1);
  char *markup=g_strdup_printf("%s\n<span size=\"9984\" foreground=\"#697079\">%s</span>",label,detail);
  gtk_label_set_markup(GTK_LABEL(row->label_widget),markup);
  g_free(markup);g_free(label);g_free(detail);
  if (!g_object_get_data(G_OBJECT(row),"slax-symbol-area"))
    {
      GtkWidget *area=gtk_drawing_area_new();
      gtk_widget_set_size_request(area,14,14);
      gtk_widget_set_halign(area,GTK_ALIGN_CENTER);gtk_widget_set_valign(area,GTK_ALIGN_CENTER);
      gtk_container_remove(GTK_CONTAINER(row->eject_button),gtk_bin_get_child(GTK_BIN(row->eject_button)));
      gtk_container_add(GTK_CONTAINER(row->eject_button),area);
      g_signal_connect(area,"draw",G_CALLBACK(slax_places_symbol),row);
      gtk_widget_show(area);
      g_object_set_data(G_OBJECT(row),"slax-symbol-area",area);
    }
  const char *symbol=*path?"slax-places-unmount-symbolic":"slax-places-mount-symbolic";
  if (*reason) symbol="slax-places-lock-symbolic";
  g_object_set_data(G_OBJECT(row),"slax-symbol",(gpointer)symbol);
  GtkStyleContext *style=gtk_widget_get_style_context(row->eject_button);
  if (*reason) gtk_style_context_add_class(style,"protected-place");
  else gtk_style_context_remove_class(style,"protected-place");
  if (!*path) gtk_style_context_add_class(style,"mount-place");
  else gtk_style_context_remove_class(style,"mount-place");
  gtk_widget_set_can_focus(row->eject_button,FALSE);
  gtk_widget_set_focus_on_click(row->eject_button,FALSE);
  gtk_widget_set_margin_start(row->eject_button,3);
  gtk_widget_set_sensitive(row->eject_button,!*reason && !operation);
  gtk_widget_set_tooltip_text(row->eject_button,*reason?reason:(*path?"Unmount":"Mount"));
  g_object_set(row,"ejectable",TRUE,NULL);
  gtk_sidebar_row_set_busy(row,operation!=NULL);
  gtk_widget_queue_draw(row->eject_button);
}
