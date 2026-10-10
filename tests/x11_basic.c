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

    Window window = XCreateSimpleWindow(
        display,
        DefaultRootWindow(display),
        100,
        100,
        500,
        300,
        1,
        BlackPixel(display, 0),
        WhitePixel(display, 0)
    );
    Atom delete_window = XInternAtom(
        display,
        "WM_DELETE_WINDOW",
        False
    );

    XSetWMProtocols(
        display,
        window,
        &delete_window,
        1
    );
    XSelectInput(display, window, ExposureMask| StructureNotifyMask );
    
    XMapWindow(display, window);

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