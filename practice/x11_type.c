#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#define MAX_TEXT 256

int main() {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }

    int screen = DefaultScreen(display);
    Window win = XCreateSimpleWindow(display, RootWindow(display, screen),
                                     100, 100, 450, 150, 2,
                                     BlackPixel(display, screen),
                                     WhitePixel(display, screen));

    XSelectInput(display, win, ExposureMask | KeyPressMask);
    XMapWindow(display, win);

    GC gc = DefaultGC(display, screen);
    
    XFontStruct *font_info = XLoadQueryFont(display, "fixed");
    if (font_info) {
        XSetFont(display, gc, font_info->fid);
    }

    char text_buffer[MAX_TEXT] = "";
    int text_len = 0;
    
    int cursor_visible = 1;
    int x_start = 30;
    int y_pos = 75;

    int x11_fd = ConnectionNumber(display);

    while (1) {
        fd_set in_fds;
        FD_ZERO(&in_fds);
        FD_SET(x11_fd, &in_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int num_ready = select(x11_fd + 1, &in_fds, NULL, NULL, &tv);

        if (num_ready == 0) {
            cursor_visible = !cursor_visible;
            XEvent exp_event;
            exp_event.type = Expose;
            XSendEvent(display, win, False, ExposureMask, &exp_event);
            XFlush(display);
        }

        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose) {
                XClearWindow(display, win);
                
                XDrawString(display, win, gc, x_start, y_pos, text_buffer, text_len);
                
                int text_width = 0;
                if (font_info) {
                    text_width = XTextWidth(font_info, text_buffer, text_len);
                } else {
                    text_width = text_len * 6;
                }

                if (cursor_visible) {
                    int cursor_x = x_start + text_width + 2;
                    XDrawLine(display, win, gc, cursor_x, y_pos - 12, cursor_x, y_pos + 2);
                }
            }
            else if (event.type == KeyPress) {
                char buf[8]; // Fixed: Declared as an array instead of a single char
                KeySym keysym;
                int len = XLookupString(&event.xkey, buf, sizeof(buf) - 1, &keysym, NULL);
                
                if (len > 0) {
                    buf[len] = '\0';
                    
                    // Check the first character in the string buffer
                    if ((buf[0] == 8 || buf[0] == 127) && text_len > 0) {
                        text_len--;
                        text_buffer[text_len] = '\0';
                    } 
                    else if (buf[0] >= 32 && text_len < MAX_TEXT - 1) {
                        text_buffer[text_len++] = buf[0];
                        text_buffer[text_len] = '\0';
                    }
                    
                    cursor_visible = 1;
                    
                    XEvent exp_event;
                    exp_event.type = Expose;
                    XSendEvent(display, win, False, ExposureMask, &exp_event);
                }
            }
        }
    }

    if (font_info) XFreeFont(display, font_info);
    XCloseDisplay(display);
    return 0;
}
