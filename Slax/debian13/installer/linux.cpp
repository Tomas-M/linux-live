#include "installer.h"
#include "images.h"
#include <gtk/gtk.h>
#include <libudev.h>
#include <dirent.h>
#include <mntent.h>
#include <set>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>

static GtkWidget *window;
static GdkPixbuf *images[2];

// Read udev's optional strings without inventing identities for absent properties.
static std::string property(udev_device *device,const char *key,bool attribute=false)
{
   const char *value=attribute?udev_device_get_sysattr_value(device,key):udev_device_get_property_value(device,key);
   return value?value:"";
}

// Protect physical parents, including USB disks below device-mapper or RAID layers.
static void protect(udev *context,dev_t number,std::set<std::string> &paths)
{
   auto device=udev_device_new_from_devnum(context,'b',number);
   if (!device) return;
   const char *path=udev_device_get_syspath(device);
   if (!paths.insert(path).second) { udev_device_unref(device); return; }
   auto parent=udev_device_get_parent_with_subsystem_devtype(device,"block","disk");
   if (parent) paths.insert(udev_device_get_syspath(parent));
   auto slaves=opendir((std::string(path)+"/slaves").c_str());
   if (slaves)
   {
      while (auto entry=readdir(slaves))
      {
         if (entry->d_name[0]=='.') continue;
         auto slave=udev_device_new_from_subsystem_sysname(context,"block",entry->d_name);
         if (slave) { protect(context,udev_device_get_devnum(slave),paths); udev_device_unref(slave); }
      }
      closedir(slaves);
   }
   if (property(device,"DEVTYPE")=="disk" && property(device,"loop/backing_file",true)!="")
   {
      struct stat info;
      if (!stat(property(device,"loop/backing_file",true).c_str(),&info)) protect(context,info.st_dev,paths);
   }
   udev_device_unref(device);
}

// Enumerate physical USB storage only; all handles and filesystem queries are read-only.
std::vector<Device> scanDevices(const std::string &source)
{
   std::vector<Device> result;
   auto context=udev_new();
   if (!context) return result;
   std::set<std::string> system,origin;
   struct stat sourceInfo;
   if (!source.empty() && !stat(source.c_str(),&sourceInfo)) protect(context,sourceInfo.st_dev,origin);
   struct Mount { std::string source,path,type; };
   std::vector<Mount> mounts;
   auto table=fopen("/proc/self/mounts","r");
   if (table)
   {
      mntent record;
      char buffer[16384];
      while (getmntent_r(table,&record,buffer,sizeof(buffer)))
      {
         mounts.push_back({record.mnt_fsname,record.mnt_dir,record.mnt_type});
         std::string path=record.mnt_dir;
         bool active=path=="/" || path=="/boot" || path=="/boot/efi" || path=="/usr" || path=="/var" || path=="/home";
         bool live=path.find("/run/initramfs")==0 || path.find("/memory")==0;
         struct stat info;
         if ((active || live) && !stat(record.mnt_fsname,&info) && S_ISBLK(info.st_mode)) protect(context,info.st_rdev,system);
      }
      fclose(table);
   }
   auto enumeration=udev_enumerate_new(context);
   udev_enumerate_add_match_subsystem(enumeration,"block");
   udev_enumerate_add_match_property(enumeration,"DEVTYPE","disk");
   udev_enumerate_scan_devices(enumeration);
   udev_list_entry *entry;
   udev_list_entry_foreach(entry,udev_enumerate_get_list_entry(enumeration))
   {
      auto disk=udev_device_new_from_syspath(context,udev_list_entry_get_name(entry));
      if (!disk) continue;
      bool usb=udev_device_get_parent_with_subsystem_devtype(disk,"usb","usb_device")!=nullptr;
      std::string path=udev_device_get_syspath(disk);
      if (usb && property(disk,"ID_CDROM")!="1")
      {
         Device d;
         d.path=udev_device_get_devnode(disk);
         d.bytes=strtoull(property(disk,"size",true).c_str(),nullptr,10)*512;
         d.id=path+":"+property(disk,"USEC_INITIALIZED")+":"+property(disk,"ID_SERIAL")+":"+std::to_string(d.bytes);
         d.name=property(disk,"ID_MODEL_FROM_DATABASE");
         if (d.name.empty()) d.name=property(disk,"ID_MODEL");
         std::replace(d.name.begin(),d.name.end(),'_',' ');
         if (d.name.empty()) d.name="USB drive";
         d.label=d.path.substr(d.path.find_last_of('/')+1);
         d.detail=property(disk,"removable",true)=="1"?"Removable USB drive":"USB drive";
         if (property(disk,"ro",true)=="1") d.blocked="This drive is read-only.";
         if (system.count(path)) d.blocked="This drive contains the running system or live session.";
         if (origin.count(path)) d.blocked="This drive contains the selected ISO image.";
         for (const auto &mount:mounts)
         {
            struct stat info;
            if (stat(mount.source.c_str(),&info) || !S_ISBLK(info.st_mode)) continue;
            auto volume=udev_device_new_from_devnum(context,'b',info.st_rdev);
            if (!volume) continue;
            auto parent=udev_device_get_parent_with_subsystem_devtype(volume,"block","disk");
            std::string diskPath=parent?udev_device_get_syspath(parent):udev_device_get_syspath(volume);
            if (diskPath==path && mount.type=="vfat")
            {
               struct statvfs space;
               if (!statvfs(mount.path.c_str(),&space) && !(space.f_flag&ST_RDONLY))
               {
                  d.fat=true;
                  d.free=std::max(d.free,uint64_t(space.f_bavail)*space.f_frsize);
                  d.detail=sizeText(d.free)+" free · FAT32";
               }
            }
            udev_device_unref(volume);
         }
         if (d.bytes) result.push_back(d);
      }
      udev_device_unref(disk);
   }
   udev_enumerate_unref(enumeration);
   udev_unref(context);
   std::sort(result.begin(),result.end(),[](const Device &a,const Device &b) { return a.path<b.path; });
   return result;
}

// Keep ISO reads independent from GTK initialization for headless checks.
FILE *openImage(const std::string &path)
{
   struct stat info;
   if (stat(path.c_str(),&info) || !S_ISREG(info.st_mode)) return nullptr;
   return fopen(path.c_str(),"rb");
}

struct GtkUi
{
   GtkWidget *rail,*steps[4],*numbers[4],*step,*heading,*intro,*stack,*path,*browse,*message;
   GtkWidget *devices,*deviceScroll,*none,*erase,*target[2],*status,*percent,*progress,*result;
   GtkWidget *back,*next,*cancel,*footerLabel,*content;
   bool updating=false;
} ui;

// Use GTK's native text layout, font fallback and accessible labels.
static GtkWidget *label(const char *text,int size=14,bool bold=false)
{
   auto widget=gtk_label_new(text);
   gtk_label_set_xalign(GTK_LABEL(widget),0);
   auto attributes=pango_attr_list_new();
   pango_attr_list_insert(attributes,pango_attr_size_new_absolute(size*PANGO_SCALE));
   if (bold) pango_attr_list_insert(attributes,pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
   gtk_label_set_attributes(GTK_LABEL(widget),attributes);
   pango_attr_list_unref(attributes);
   return widget;
}

// Native wrapping keeps long paths, Unicode names and errors usable in narrow windows.
static void wrap(GtkWidget *widget)
{
   gtk_label_set_line_wrap(GTK_LABEL(widget),true);
   gtk_label_set_line_wrap_mode(GTK_LABEL(widget),PANGO_WRAP_WORD_CHAR);
}

// Refresh native rows only when discovery changes, preserving focus between scans.
static void updateWindow()
{
   ui.updating=true;
   gtk_label_set_text(GTK_LABEL(ui.step),("Step "+std::to_string(app.page+1)+" of 4").c_str());
   gtk_label_set_text(GTK_LABEL(ui.heading),app.heading().c_str());
   gtk_label_set_text(GTK_LABEL(ui.intro),app.introduction().c_str());
   for (int i=0; i<4; i++)
   {
      auto context=gtk_widget_get_style_context(ui.numbers[i]);
      gtk_style_context_remove_class(context,"current"); gtk_style_context_remove_class(context,"done");
      if (i==app.page) gtk_style_context_add_class(context,"current");
      if (i<app.page) gtk_style_context_add_class(context,"done");
      gtk_widget_set_sensitive(ui.steps[i],i<=app.page);
   }
   gtk_stack_set_visible_child_name(GTK_STACK(ui.stack),std::to_string(app.page).c_str());
   gtk_widget_set_margin_bottom(ui.content,app.page==1?12:29);
   if (app.iso.path!=gtk_entry_get_text(GTK_ENTRY(ui.path))) gtk_entry_set_text(GTK_ENTRY(ui.path),app.iso.path.c_str());
   gtk_label_set_text(GTK_LABEL(ui.message),app.message().c_str());
   gtk_widget_set_size_request(ui.message,-1,app.page==0?42:-1);
   gtk_widget_set_visible(ui.message,app.page==0 || (app.page==1 && !app.message().empty()));
   auto messageStyle=gtk_widget_get_style_context(ui.message);
   gtk_style_context_remove_class(messageStyle,"error");
   if (!app.error.empty() && app.pending==None) gtk_style_context_add_class(messageStyle,"error");
   auto currentRows=gtk_container_get_children(GTK_CONTAINER(ui.devices));
   bool changed=g_list_length(currentRows)!=app.devices.size();
   for (size_t i=0; !changed && i<app.devices.size(); i++)
   {
      auto row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(ui.devices),i);
      auto radio=G_OBJECT(g_object_get_data(G_OBJECT(row),"radio"));
      changed=app.devices[i].id!=static_cast<const char *>(g_object_get_data(radio,"device-id"));
   }
   g_list_free(currentRows);
   if (changed)
   {
      auto children=gtk_container_get_children(GTK_CONTAINER(ui.devices));
      for (auto item=children; item; item=item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
      g_list_free(children);
      for (const auto &device:app.devices)
      {
         auto row=gtk_list_box_row_new(),box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
         gtk_widget_set_margin_start(box,11); gtk_widget_set_margin_end(box,11);
         gtk_widget_set_margin_top(box,9); gtk_widget_set_margin_bottom(box,9);
         auto badge=label(device.label.c_str(),device.label.size()>3?13:20,true);
         gtk_label_set_xalign(GTK_LABEL(badge),0.5); gtk_widget_set_size_request(badge,42,42);
         gtk_style_context_add_class(gtk_widget_get_style_context(badge),"drive-badge");
         auto text=gtk_box_new(GTK_ORIENTATION_VERTICAL,1); gtk_widget_set_valign(text,GTK_ALIGN_CENTER);
         auto name=label(device.name.c_str(),14,true),detail=label("",13);
         gtk_label_set_ellipsize(GTK_LABEL(name),PANGO_ELLIPSIZE_END);
         gtk_label_set_ellipsize(GTK_LABEL(detail),PANGO_ELLIPSIZE_END);
         gtk_box_pack_start(GTK_BOX(text),name,false,false,0); gtk_box_pack_start(GTK_BOX(text),detail,false,false,0);
         auto radio=gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(ui.none)); gtk_widget_set_valign(radio,GTK_ALIGN_CENTER);
         g_object_set_data_full(G_OBJECT(radio),"device-id",g_strdup(device.id.c_str()),g_free);
         g_object_set_data(G_OBJECT(row),"radio",radio); g_object_set_data(G_OBJECT(row),"detail",detail); g_object_set_data(G_OBJECT(row),"name",name); g_object_set_data(G_OBJECT(row),"badge",badge);
         g_signal_connect(radio,"toggled",G_CALLBACK(+[](GtkToggleButton *button,gpointer)
         {
            if (!ui.updating && gtk_toggle_button_get_active(button)) app.activate(Select,static_cast<const char *>(g_object_get_data(G_OBJECT(button),"device-id")));
         }),nullptr);
         gtk_box_pack_start(GTK_BOX(box),badge,false,false,0); gtk_box_pack_start(GTK_BOX(box),text,true,true,0); gtk_box_pack_end(GTK_BOX(box),radio,false,false,0);
         gtk_container_add(GTK_CONTAINER(row),box); gtk_container_add(GTK_CONTAINER(ui.devices),row);
      }
      gtk_widget_show_all(ui.devices);
   }
   if (app.selected.empty()) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.none),true);
   for (size_t i=0; i<app.devices.size(); i++)
   {
      const auto &device=app.devices[i];
      auto row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(ui.devices),i);
      gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(row),"name")),device.name.c_str());
      gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(row),"badge")),device.label.c_str());
      auto radio=GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(row),"radio"));
      gtk_toggle_button_set_active(radio,device.id==app.selected);
      auto rowStyle=gtk_widget_get_style_context(GTK_WIDGET(row));
      gtk_style_context_remove_class(rowStyle,"selected");
      if (device.id==app.selected) gtk_style_context_add_class(rowStyle,"selected");
      std::string reason=app.eligibility(device);
      auto detail=GTK_LABEL(g_object_get_data(G_OBJECT(row),"detail"));
      gtk_label_set_text(detail,(sizeText(device.bytes)+" · "+(reason.empty()?device.detail:reason)).c_str());
      gtk_widget_set_tooltip_text(GTK_WIDGET(row),(device.path+"\n"+device.name+"\n"+sizeText(device.bytes)+" · "+reason).c_str());
   }
   gtk_widget_set_sensitive(ui.devices,app.pending==None);
   gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ui.deviceScroll),std::min(210,std::max(62,int(app.devices.size())*70-8)));
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.erase),app.erase);
   gtk_widget_set_sensitive(ui.erase,!app.devices.empty() && app.pending==None);
   for (auto target:ui.target) gtk_label_set_text(GTK_LABEL(target),app.target.c_str());
   gtk_label_set_text(GTK_LABEL(ui.status),app.progressText().c_str());
   gtk_label_set_text(GTK_LABEL(ui.percent),(std::to_string(app.percent)+"%").c_str());
   gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress),app.percent/100.0);
   gtk_image_set_from_icon_name(GTK_IMAGE(ui.result),app.error.empty()?"dialog-information":"dialog-error",GTK_ICON_SIZE_DIALOG);
   gtk_widget_set_visible(ui.back,app.page<3); gtk_widget_set_sensitive(ui.back,app.page==1);
   gtk_widget_set_visible(ui.cancel,app.page<3); gtk_widget_set_sensitive(ui.cancel,app.page!=2);
   gtk_widget_set_sensitive(ui.browse,app.pending==None);
   gtk_widget_set_sensitive(ui.next,app.canContinue());
   gtk_button_set_label(GTK_BUTTON(ui.next),app.page==3?(app.error.empty()?"Finish":"Try again"):"Next");
   ui.updating=false;
}

// All callers run on GTK's owning thread; GTK performs ordinary widget invalidation.
void invalidate()
{
   if (window) updateWindow();
}

// Native closing follows the same path as the window-manager close button.
void windowAction(Action action)
{
   if (action==Close) { ui.updating=true; gtk_widget_destroy(window); }
}

// Keep Browse native, modal and independent of shell quoting.
std::string chooseImage()
{
   auto dialog=gtk_file_chooser_native_new("Choose a Slax ISO image",GTK_WINDOW(window),GTK_FILE_CHOOSER_ACTION_OPEN,"Open","Cancel");
   gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog),true);
   auto filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"ISO images (*.iso)");
   // Native chooser backends support glob filters; enumerate the ASCII extension's case variants.
   for (char i:{'i','I'}) for (char s:{'s','S'}) for (char o:{'o','O'})
      gtk_file_filter_add_pattern(filter,("*."+std::string({i,s,o})).c_str());
   gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog),filter);
   if (!app.iso.path.empty()) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog),app.iso.path.c_str());
   std::string path;
   if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog))==GTK_RESPONSE_ACCEPT)
   {
      char *filename=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)); path=filename; g_free(filename);
   }
   g_object_unref(dialog);
   return path;
}

// Confirm with a normal GTK dialog and default to Cancel.
bool confirmInstall(const std::string &target,bool erase)
{
   auto dialog=gtk_message_dialog_new(GTK_WINDOW(window),GTK_DIALOG_MODAL,GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,"Simulate installation?");
   std::string text=target+"\n\n"+(erase?"Drive preparation, file copying and boot setup will only be simulated. No data will be erased.":"File copying and boot setup will only be simulated. No data will be changed.");
   gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",text.c_str());
   gtk_dialog_add_buttons(GTK_DIALOG(dialog),"Simulate",GTK_RESPONSE_ACCEPT,"Cancel",GTK_RESPONSE_CANCEL,nullptr);
   gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
   bool accepted=gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT;
   gtk_widget_destroy(dialog);
   return accepted;
}

// Build the reference layout from native GTK widgets in an ordinary decorated window.
int runWindow()
{
   if (!gtk_init_check(nullptr,nullptr)) { fprintf(stderr,"Open the installer in a Slax graphical session.\n"); return 1; }
   for (int i=0; i<2; i++)
   {
      auto loader=gdk_pixbuf_loader_new();
      gdk_pixbuf_loader_write(loader,i?wordmark_png:clover_png,i?sizeof(wordmark_png):sizeof(clover_png),nullptr);
      gdk_pixbuf_loader_close(loader,nullptr); images[i]=GDK_PIXBUF(g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader))); g_object_unref(loader);
   }
   gboolean dark=false; gchar *theme=nullptr;
   g_object_get(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",&dark,"gtk-theme-name",&theme,nullptr);
   dark=dark || (theme && (strstr(theme,"dark") || strstr(theme,"Dark"))); g_free(theme);
   std::string css=".slax-installer {font-size:14px;} #installer-rail {background:"+std::string(dark?"#263125":"#ecf3e7")+";} .step-number {border-radius:12px; border:1px solid "+(dark?"#434b42":"#d9e0d8")+";} .step-number.current {background:"+(dark?"#a8de89":"#397721")+";color:"+(dark?"#182810":"white")+";} .step-number.done,.drive-badge {color:"+(dark?"#a8de89":"#397721")+";} #installer-next {font-weight:600;} .installer-message.error {color:"+(dark?"#ffafaf":"#a22929")+";}";
   css+="#installer-devices {background:transparent;} #installer-devices row {padding:0; margin-bottom:8px; border:1px solid "+std::string(dark?"#434b42":"#d9e0d8")+"; border-radius:7px;} #installer-devices row:last-child {margin-bottom:0;} #installer-devices row.selected {background:"+(dark?"#293d25":"#eef7e7")+"; border-color:"+(dark?"#a8de89":"#397721")+";} .drive-badge {background:"+(dark?"#263125":"#ecf3e7")+"; border-radius:6px;} .installer-message {color:"+(dark?"#b4bfb2":"#546056")+";}";
   auto provider=gtk_css_provider_new(); gtk_css_provider_load_from_data(provider,css.c_str(),-1,nullptr);
   gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(provider),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(provider);
   window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
   gtk_window_set_title(GTK_WINDOW(window),"Slax USB Installer"); gtk_window_set_icon(GTK_WINDOW(window),images[0]);
   gtk_style_context_add_class(gtk_widget_get_style_context(window),"slax-installer");
   gtk_window_set_wmclass(GTK_WINDOW(window),"slax-usb-installer","SlaxUsbInstaller");
   auto monitor=gdk_display_get_primary_monitor(gdk_display_get_default());
   if (!monitor) monitor=gdk_display_get_monitor(gdk_display_get_default(),0);
   GdkRectangle workarea; gdk_monitor_get_workarea(monitor,&workarea);
   gtk_window_set_default_size(GTK_WINDOW(window),std::min(832,workarea.width-32),std::min(446,workarea.height-70));
   gtk_window_set_position(GTK_WINDOW(window),GTK_WIN_POS_CENTER);
   GdkGeometry geometry={}; geometry.min_width=720; geometry.min_height=400;
   gtk_window_set_geometry_hints(GTK_WINDOW(window),nullptr,&geometry,GDK_HINT_MIN_SIZE);
   auto root=gtk_box_new(GTK_ORIENTATION_VERTICAL,0),body=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
   gtk_container_add(GTK_CONTAINER(window),root); gtk_box_pack_start(GTK_BOX(root),body,true,true,0);
   ui.rail=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_widget_set_name(ui.rail,"installer-rail");
   gtk_box_pack_start(GTK_BOX(body),ui.rail,false,false,0);
   auto brand=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,9);
   gtk_widget_set_margin_top(brand,28); gtk_widget_set_margin_start(brand,25); gtk_widget_set_margin_bottom(brand,22);
   for (int i=0; i<2; i++)
   {
      int width=i?83:31,height=i?30:33;
      auto pixbuf=gdk_pixbuf_scale_simple(images[i],width,height,GDK_INTERP_BILINEAR);
      gtk_box_pack_start(GTK_BOX(brand),gtk_image_new_from_pixbuf(pixbuf),false,false,0); g_object_unref(pixbuf);
   }
   gtk_box_pack_start(GTK_BOX(ui.rail),brand,false,false,0);
   const char *steps[]={"ISO image","USB drive","Installation","Finish"};
   for (int i=0; i<4; i++)
   {
      auto row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10); gtk_widget_set_size_request(row,-1,31); gtk_widget_set_margin_bottom(row,9); gtk_widget_set_margin_start(row,22); gtk_widget_set_margin_end(row,10);
      ui.numbers[i]=label(std::to_string(i+1).c_str(),13); gtk_label_set_xalign(GTK_LABEL(ui.numbers[i]),0.5);
      gtk_widget_set_size_request(ui.numbers[i],24,24); gtk_widget_set_valign(ui.numbers[i],GTK_ALIGN_CENTER); gtk_style_context_add_class(gtk_widget_get_style_context(ui.numbers[i]),"step-number");
      ui.steps[i]=label(steps[i]); gtk_box_pack_start(GTK_BOX(row),ui.numbers[i],false,false,0); gtk_box_pack_start(GTK_BOX(row),ui.steps[i],true,true,0);
      gtk_box_pack_start(GTK_BOX(ui.rail),row,false,false,0);
   }
   auto scroller=gtk_scrolled_window_new(nullptr,nullptr); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
   gtk_box_pack_start(GTK_BOX(body),scroller,true,true,0);
   auto main=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); ui.content=main;
   gtk_widget_set_margin_top(main,29); gtk_widget_set_margin_bottom(main,29); gtk_widget_set_margin_start(main,30); gtk_widget_set_margin_end(main,30);
   gtk_container_add(GTK_CONTAINER(scroller),main);
   ui.step=label("",13,true); ui.heading=label("",28,true); ui.intro=label("",16); wrap(ui.heading); wrap(ui.intro);
   gtk_box_pack_start(GTK_BOX(main),ui.step,false,false,0); gtk_widget_set_margin_bottom(ui.step,6);
   gtk_box_pack_start(GTK_BOX(main),ui.heading,false,false,0); gtk_widget_set_margin_bottom(ui.heading,9);
   gtk_box_pack_start(GTK_BOX(main),ui.intro,false,false,0); gtk_widget_set_margin_bottom(ui.intro,22);
   ui.stack=gtk_stack_new(); gtk_stack_set_hhomogeneous(GTK_STACK(ui.stack),true); gtk_stack_set_vhomogeneous(GTK_STACK(ui.stack),false);
   gtk_box_pack_start(GTK_BOX(main),ui.stack,false,false,0);
   auto iso=gtk_box_new(GTK_ORIENTATION_VERTICAL,0),isoRow=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
   auto pathLabel=label("ISO file path",14,true); gtk_box_pack_start(GTK_BOX(iso),pathLabel,false,false,0); gtk_widget_set_margin_bottom(pathLabel,6);
   ui.path=gtk_entry_new(); gtk_editable_set_editable(GTK_EDITABLE(ui.path),false); gtk_entry_set_width_chars(GTK_ENTRY(ui.path),8);
   gtk_entry_set_activates_default(GTK_ENTRY(ui.path),true);
   gtk_label_set_mnemonic_widget(GTK_LABEL(pathLabel),ui.path);
   ui.browse=gtk_button_new_with_label("Browse…"); gtk_box_pack_start(GTK_BOX(isoRow),ui.path,true,true,0); gtk_box_pack_start(GTK_BOX(isoRow),ui.browse,false,false,0);
   gtk_widget_set_size_request(ui.path,-1,35); gtk_widget_set_size_request(ui.browse,-1,35);
   gtk_box_pack_start(GTK_BOX(iso),isoRow,false,false,0);
   gtk_stack_add_named(GTK_STACK(ui.stack),iso,"0");
   auto usb=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
   auto empty=label("No USB drives detected. Connect a USB drive to continue."); wrap(empty);
   ui.deviceScroll=gtk_scrolled_window_new(nullptr,nullptr); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui.deviceScroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ui.deviceScroll),210); gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ui.deviceScroll),true);
   ui.devices=gtk_list_box_new(); gtk_widget_set_name(ui.devices,"installer-devices"); gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.devices),GTK_SELECTION_NONE); gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(ui.devices),true);
   gtk_list_box_set_placeholder(GTK_LIST_BOX(ui.devices),empty); gtk_container_add(GTK_CONTAINER(ui.deviceScroll),ui.devices); gtk_box_pack_start(GTK_BOX(usb),ui.deviceScroll,false,false,0);
   ui.none=gtk_radio_button_new(nullptr); g_object_ref_sink(ui.none);
   g_signal_connect(ui.devices,"row-activated",G_CALLBACK(+[](GtkListBox *,GtkListBoxRow *row,gpointer) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(row),"radio")),true); }),nullptr);
   ui.erase=gtk_check_button_new_with_label("Erase all data on the selected device"); gtk_widget_set_size_request(ui.erase,-1,34); gtk_box_pack_start(GTK_BOX(usb),ui.erase,false,false,0);
   gtk_stack_add_named(GTK_STACK(ui.stack),usb,"1");
   auto writing=gtk_box_new(GTK_ORIENTATION_VERTICAL,6),result=gtk_box_new(GTK_ORIENTATION_VERTICAL,13);
   for (int i=0; i<2; i++) { ui.target[i]=label("",14); wrap(ui.target[i]); gtk_box_pack_start(GTK_BOX(i?result:writing),ui.target[i],false,false,0); }
   ui.result=gtk_image_new(); gtk_widget_set_halign(ui.result,GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(result),ui.result,false,false,0); gtk_box_reorder_child(GTK_BOX(result),ui.result,0);
   auto status=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_widget_set_margin_top(status,15);
   ui.status=label(""),ui.percent=label(""); gtk_box_pack_start(GTK_BOX(status),ui.status,true,true,0); gtk_box_pack_end(GTK_BOX(status),ui.percent,false,false,0);
   gtk_box_pack_start(GTK_BOX(writing),status,false,false,0); ui.progress=gtk_progress_bar_new(); gtk_box_pack_start(GTK_BOX(writing),ui.progress,false,false,0);
   gtk_stack_add_named(GTK_STACK(ui.stack),writing,"2"); gtk_stack_add_named(GTK_STACK(ui.stack),result,"3");
   ui.message=label(""); wrap(ui.message); gtk_label_set_yalign(GTK_LABEL(ui.message),0);
   gtk_style_context_add_class(gtk_widget_get_style_context(ui.message),"installer-message");
   gtk_widget_set_margin_top(ui.message,13); gtk_widget_set_size_request(ui.message,-1,42); gtk_box_pack_start(GTK_BOX(main),ui.message,false,false,0);
   gtk_box_pack_start(GTK_BOX(root),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),false,false,0);
   auto footer=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_container_set_border_width(GTK_CONTAINER(footer),11); gtk_widget_set_margin_start(footer,4); gtk_widget_set_margin_end(footer,4);
   gtk_box_pack_start(GTK_BOX(root),footer,false,false,0);
   ui.footerLabel=label("Slax USB Installer · Simulation",13); gtk_box_pack_start(GTK_BOX(footer),ui.footerLabel,true,true,0);
   gtk_label_set_ellipsize(GTK_LABEL(ui.footerLabel),PANGO_ELLIPSIZE_END);
   ui.cancel=gtk_button_new_with_label("Cancel"),ui.back=gtk_button_new_with_label("Back"),ui.next=gtk_button_new_with_label("Next");
   gtk_widget_set_name(ui.next,"installer-next"); gtk_style_context_add_class(gtk_widget_get_style_context(ui.next),"suggested-action");
   gtk_widget_set_size_request(ui.next,92,35); gtk_widget_set_can_default(ui.next,true); gtk_window_set_default(GTK_WINDOW(window),ui.next);
   gtk_box_pack_end(GTK_BOX(footer),ui.next,false,false,0); gtk_box_pack_end(GTK_BOX(footer),ui.back,false,false,0); gtk_box_pack_end(GTK_BOX(footer),ui.cancel,false,false,0);
   g_signal_connect(ui.browse,"clicked",G_CALLBACK(+[](GtkButton *,gpointer) { app.activate(Browse); }),nullptr);
   g_signal_connect(ui.next,"clicked",G_CALLBACK(+[](GtkButton *,gpointer) { app.activate(Next); }),nullptr);
   g_signal_connect(ui.back,"clicked",G_CALLBACK(+[](GtkButton *,gpointer) { app.activate(Back); }),nullptr);
   g_signal_connect(ui.cancel,"clicked",G_CALLBACK(+[](GtkButton *,gpointer) { app.activate(Cancel); }),nullptr);
   g_signal_connect(ui.erase,"toggled",G_CALLBACK(+[](GtkToggleButton *button,gpointer) { if (!ui.updating && gtk_toggle_button_get_active(button)!=app.erase) app.activate(Erase); }),nullptr);
   g_signal_connect(window,"delete-event",G_CALLBACK(+[](GtkWidget *,GdkEvent *,gpointer)->gboolean { app.activate(Close); return true; }),nullptr);
   g_signal_connect(window,"destroy",G_CALLBACK(+[](GtkWidget *,gpointer)
   {
      ui.updating=true; window=nullptr;
      if (gtk_main_level()) gtk_main_quit();
   }),nullptr);
   g_signal_connect(window,"key-press-event",G_CALLBACK(+[](GtkWidget *,GdkEventKey *event,gpointer)->gboolean { if (event->keyval==GDK_KEY_Escape && app.page!=2) { app.activate(Close); return true; } return false; }),nullptr);
   g_signal_connect(body,"size-allocate",G_CALLBACK(+[](GtkWidget *,GtkAllocation *size,gpointer)
   {
      gtk_widget_set_size_request(ui.rail,std::max(175,int(size->width*0.3)),0);
   }),nullptr);
   guint timer=g_timeout_add(100,+[](gpointer)->gboolean { app.tick(); return G_SOURCE_CONTINUE; },nullptr);
   gtk_widget_show_all(window); updateWindow(); app.startScan(); gtk_main();
   g_source_remove(timer); app.stop();
   if (window) gtk_widget_destroy(window);
   g_object_unref(ui.none);
   for (auto image:images) g_object_unref(image);
   return 0;
}

