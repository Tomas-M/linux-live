/*
 * GRUB extension: countdown keys and a menu-only Tab shortcut for command editing.
 * Build with matching GRUB sources and installed platform flags:
 *   sh tools/grub-anykey.create GRUB_SOURCE Slax/debian13/bootfiles/grub/extra i386-pc x86_64-efi
 * No GRUB source changes are required. Editors regain the original keyboard input.
 */
#define GRUB_FILE __FILE__
#include <grub/dl.h>
#include <grub/command.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/menu_viewer.h>
#include <grub/normal.h>
#include <grub/term.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/api.h>
#endif

GRUB_MOD_LICENSE("GPLv3+");
GRUB_MOD_NAME(anykey);
static const char dependencies[] __attribute__((section(".moddeps"),used))="normal\0gfxmenu";
struct input_hook {
   grub_term_input_t term;
   int (*getkey)(grub_term_input_t);
   int status;
#ifdef GRUB_MACHINE_EFI
   void *notify;
#endif
   struct input_hook *next;
};
static struct input_hook *hooks;
static grub_command_t command;
static volatile int modifier_pressed;
static int countdown;
static grub_err_t (*show_menu)(int,grub_menu_t,int);

#ifdef GRUB_MACHINE_EFI
/* UEFI's standard notification reports modifier-only keys without changing locks. */
static grub_efi_status_t __grub_efi_api modifier_key(grub_efi_key_data_t *key)
{
   (void)key;
   modifier_pressed=1;
   return GRUB_EFI_SUCCESS;
}
#endif

/* Restore input before editors, command lines or the kernel take ownership. */
static void release_inputs(void)
{
   while (hooks) {
      struct input_hook *hook=hooks;
      hooks=hook->next;
      hook->term->getkey=hook->getkey;
#ifdef GRUB_MACHINE_EFI
      if (hook->notify) {
         grub_efi_simple_text_input_ex_interface_t *input=hook->term->data;
         input->unregister_key_notify(input,hook->notify);
      }
#endif
      grub_free(hook);
   }
   modifier_pressed=0;
}

/* Enter boots the selected default; other countdown keys reveal the parent menu. */
static int first_key(grub_term_input_t term)
{
   struct input_hook *hook;
   for (hook=hooks;hook;hook=hook->next) if (hook->term==term) {
      /* EFI buffers modifier state only until getkey consumes the keystroke. */
      int status=term->getkeystatus?term->getkeystatus(term):0;
      int key=hook->getkey(term);
      if (!countdown) return key=='\t'?'e':key;
      if (key!=GRUB_TERM_NO_KEY || status!=hook->status || modifier_pressed) {
         release_inputs();
         return key=='\r' || key=='\n'?'\r':GRUB_TERM_ESC;
      }
      return key;
   }
   return GRUB_TERM_NO_KEY;
}

/* Arm all currently active terminals, including firmware and serial keyboards. */
static grub_err_t arm_inputs(grub_command_t cmd, int argc, char **argv)
{
   grub_term_input_t term;
   (void)cmd;
   release_inputs();
   if (argc && !grub_strcmp(argv[0],"stop")) return GRUB_ERR_NONE;
   countdown=grub_menu_get_timeout()>0;
   /* Menu shortcuts belong to the viewer, which releases them before editing. */
   if (cmd && !countdown) return GRUB_ERR_NONE;
   FOR_ACTIVE_TERM_INPUTS(term) {
      struct input_hook *hook=grub_malloc(sizeof(*hook));
      if (!hook) { release_inputs(); return grub_errno; }
      hook->term=term; hook->getkey=term->getkey;
      hook->status=term->getkeystatus?term->getkeystatus(term):0;
#ifdef GRUB_MACHINE_EFI
      hook->notify=0;
      if (countdown && !grub_strcmp(term->name,"console") && term->data) {
         grub_efi_simple_text_input_ex_interface_t *input=term->data;
         grub_efi_key_data_t key={0};
         input->register_key_notify(input,&key,modifier_key,&hook->notify);
      }
#endif
      hook->next=hooks; hooks=hook;
      term->getkey=first_key;
   }
   return GRUB_ERR_NONE;
}

/* These viewer callbacks leave rendering to GRUB's existing theme. */
static void keep_item(int value,void *data) { (void)value; (void)data; }
static void keep_timeout(void *data) { (void)data; }
static void close_menu(void *data) { (void)data; release_inputs(); }

/* Re-arm shortcuts on menu refresh, and remove them before entering its editor. */
static grub_err_t open_menu(int entry,grub_menu_t menu,int nested)
{
   grub_err_t error=show_menu(entry,menu,nested);
   if (error) return error;
   struct grub_menu_viewer *viewer=grub_zalloc(sizeof(*viewer));
   if (!viewer) return grub_errno;
   error=arm_inputs(0,0,0);
   if (error) { grub_free(viewer); return error; }
   viewer->set_chosen_entry=keep_item; viewer->print_timeout=keep_item;
   viewer->clear_timeout=keep_timeout; viewer->fini=close_menu;
   grub_menu_register_viewer(viewer);
   return GRUB_ERR_NONE;
}

/* Register the menu observer; keyboard hooks follow the displayed menu's lifetime. */
GRUB_MOD_INIT(anykey)
{
   (void)mod;
   show_menu=grub_gfxmenu_try_hook;
   grub_gfxmenu_try_hook=open_menu;
   command=grub_register_command("anykey",arm_inputs,"[stop]","Handle countdown keys and menu Tab editing.");
}

/* Never leave callback pointers referring to an unloaded module. */
GRUB_MOD_FINI(anykey)
{
   release_inputs();
   grub_gfxmenu_try_hook=show_menu;
   grub_unregister_command(command);
}
