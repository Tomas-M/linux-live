/* Share escaped device details between the file manager and native GTK choosers. */
static char *slax_places_detail(GVariant *location,const char *operation)
{
    const char *id,*name,*group,*icon,*type,*source,*path,*target,*reason;
    g_variant_get(location,"(&s&s&s&s&s&s&s&s&s)",&id,&name,&group,&icon,&type,&source,&path,&target,&reason);
    const char *state=*path?"Mounted":"Not mounted";
    if (*reason && strcmp(reason,"In use by Slax; cannot unmount")) state=reason;
    if (operation) state=!strcmp(operation,"mount")?"Mounting…":"Unmounting…";
    char *origin=g_strdup(source);
    if (g_str_has_prefix(id,"block:")) { g_free(origin); origin=g_path_get_basename(source); }
    if (!strcmp(type,"vboxsf")) { g_free(origin); origin=g_strdup("VirtualBox"); }
    if (!strcmp(type,"fuse.vmhgfs-fuse")) { g_free(origin); origin=g_strdup("VMware"); }
    char *detail=g_markup_printf_escaped("%s · %s",state,origin);
    g_free(origin);
    return detail;
}
