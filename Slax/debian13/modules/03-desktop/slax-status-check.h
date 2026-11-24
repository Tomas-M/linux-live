static Window repair_client;
static XErrorHandler previous_command_error;

/* A requester can close during a check; other GTK/X11 errors retain their normal handler. */
static int command_error(Display *connection,XErrorEvent *error)
{
    if (connection==display && error->error_code==BadWindow) return 0;
    return previous_command_error(connection,error);
}

/* Reply to one request window rather than broadcasting completion to unrelated browsers. */
static void check_reply(Window client,gboolean ready)
{
    if (!client) return;
    XEvent event={0};event.xclient.type=ClientMessage;event.xclient.window=client;
    event.xclient.message_type=check_done_atom;event.xclient.format=32;event.xclient.data.l[0]=ready;
    XSendEvent(display,client,False,NoEventMask,&event);XSync(display,False);
}

/* Clear ownership before notifying a caller that may immediately request another operation. */
static void repair_reply(gboolean ready)
{
    Window client=repair_client;repair_client=None;check_reply(client,ready);
}

/* The short-lived CLI waits outside the browser, then reuses its normal privileged mount helper. */
static int check_request(const char *id,Atom selection)
{
    XGrabServer(display);
    Window owner=XGetSelectionOwner(display,selection);
    if (!owner) {XUngrabServer(display);g_printerr("Slax Status is not running.\n");return 1;}
    Window client=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1,1,0,0,0);
    XSelectInput(display,owner,StructureNotifyMask);
    XEvent event={0};event.xclient.type=ClientMessage;event.xclient.window=owner;
    event.xclient.message_type=check_atom;event.xclient.format=32;
    event.xclient.data.l[0]=XInternAtom(display,id,False);event.xclient.data.l[1]=client;
    XSendEvent(display,owner,False,NoEventMask,&event);XUngrabServer(display);XSync(display,False);
    gboolean ready=FALSE,closed=XGetSelectionOwner(display,selection)!=owner;
    while (!closed)
    {
        XNextEvent(display,&event);
        if (event.type==ClientMessage && event.xclient.message_type==check_done_atom) {ready=event.xclient.data.l[0];break;}
        closed=event.type==DestroyNotify && event.xdestroywindow.window==owner;
    }
    XDestroyWindow(display,client);XCloseDisplay(display);
    if (closed) {g_printerr("Slax Status closed before the filesystem check completed.\n");return 1;}
    if (!ready) return 0;
    char *argv[]={"/usr/bin/pkexec","/usr/libexec/slax-places","mount",(char *)id,NULL};
    char **command=argv+(geteuid()==0);execv(command[0],command);
    g_printerr("Cannot mount the checked filesystem: %s\n",g_strerror(errno));return 1;
}
