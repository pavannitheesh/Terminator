#include <X11/Xlib.h>
#include <stdio.h>

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL)
    {
        printf("Unable to connect to X11\n");
        return 1;
    }
    int scr = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        100,
        100,
        500,
        300,
        1,
        BlackPixel(display, scr),
        WhitePixel(display, scr)
    );
    Atom delete_window = XInternAtom(
        display,
        "WM_DELETE_WINDOW",
        False
    );
    XStoreName(display, window, "Terminator");

    XSetWMProtocols(
        display,
        window,
        &delete_window,
        1
    );
    XSelectInput(display, window, ExposureMask| StructureNotifyMask);
    
    XMapWindow(display, window);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, BlackPixel(display, scr));
    XSetBackground(display, gc, rgb(21, 58, 204));

    while (1)
    {
        XEvent event;

        XNextEvent(display, &event);

        if (event.type == Expose)
        {
            XDrawString(
                display,
                window,
                DefaultGC(display, 0),
                180,
                150,
                "Hello World",
                11
            );
        }
        if (event.type == ClientMessage &&
            event.xclient.data.l[0] == delete_window)
        {
            break;
        }
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}