/* Checking and ordinary mounting return through the same asynchronous browser job. */
static GSubprocess *slax_places_command(GVariant *location,const char *action,GError **error)
{
    const char *id,*reason;
    g_variant_get_child(location,0,"&s",&id);
    g_variant_get_child(location,8,"&s",&reason);
    GSubprocessFlags flags=G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (!strcmp(action,"mount") && !strcmp(reason,"Needs filesystem check"))
    {
        const char *argv[]={"/usr/bin/slax-status","--check",id,NULL};
        return g_subprocess_newv(argv,flags,error);
    }
    if (*reason) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_BUSY,reason);return NULL;}
    const char *argv[]={"/usr/bin/pkexec","/usr/libexec/slax-places",action,id,NULL};
    return g_subprocess_newv(argv+(geteuid()==0),flags,error);
}
