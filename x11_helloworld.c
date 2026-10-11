#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 256
#define MAX_TABS 10

typedef struct {
    char text_buffer[BUFFER_SIZE];
    int text_len;
} Tab;

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
        600,
        400,
        1,
        BlackPixel(display, scr),
        BlackPixel(display, scr)
    );
    
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XStoreName(display, window, "Multi-Tab Terminator");
    XSetWMProtocols(display, window, &delete_window, 1);
    
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, WhitePixel(display, scr));
    XSetBackground(display, gc, BlackPixel(display, scr));
    XMapWindow(display, window);

    // Initialize tabs
    Tab tabs[MAX_TABS];
    int tab_count = 1;
    int current_tab = 0;

    // Initialize the first tab
    snprintf(tabs[0].text_buffer, BUFFER_SIZE, "Tab 1 > ");
    tabs[0].text_len = strlen(tabs[0].text_buffer);

    XEvent event;
    KeySym keysym;
    char lookup_buffer[8];

    while (1)
    {
        XNextEvent(display, &event);
        
        if (event.type == Expose) {
            XClearWindow(display, window);
            
            // Draw tab header bar
            char header_buf[128];
            snprintf(header_buf, sizeof(header_buf), "Tabs: %d/%d (Active: %d) | Ctrl+T: New, Ctrl+W: Close, Ctrl+Tab: Switch", tab_count, MAX_TABS, current_tab + 1);
            XDrawString(display, window, gc, 20, 30, header_buf, strlen(header_buf));

            // Draw divider line
            XDrawLine(display, window, gc, 20, 45, 580, 45);

            // Draw current tab's text buffer
            XDrawString(display, window, gc, 20, 80, tabs[current_tab].text_buffer, tabs[current_tab].text_len);
        }
        else if (event.type == KeyPress) {
           int count = XLookupString(&event.xkey, lookup_buffer, sizeof(lookup_buffer), &keysym, NULL);

            // Check for modifier keys (Control)
            unsigned int state = event.xkey.state;

            if (keysym == XK_Escape) {
                break; 
            }
            else if ((state & ControlMask) && (keysym == XK_t || keysym == XK_T)) {
                // Create a new tab if under limit
                if (tab_count < MAX_TABS) {
                    current_tab = tab_count;
                    snprintf(tabs[current_tab].text_buffer, BUFFER_SIZE, "Tab %d > ", current_tab + 1);
                    tabs[current_tab].text_len = strlen(tabs[current_tab].text_buffer);
                    tab_count++;
                }
            }
            else if ((state & ControlMask) && (keysym == XK_w || keysym == XK_W)) {
                // Close current tab
                if (tab_count > 1) {
                    // Shift remaining tabs down
                    for (int i = current_tab; i < tab_count - 1; i++) {
                        tabs[i] = tabs[i + 1];
                    }
                    tab_count--;
                    if (current_tab >= tab_count) {
                        current_tab = tab_count - 1;
                    }
                } else {
                    // If it's the last tab, exit the application
                    break;
                }
            }
            else if ((state & ControlMask) && (keysym == XK_Tab || keysym == XK_ISO_Left_Tab)) {
                // Cycle through tabs
                current_tab = (current_tab + 1) % tab_count;
            }
            else if (keysym == XK_BackSpace) {
                // Prevent deleting beyond the prompt prefix
                char prefix[16];
                int prefix_len = snprintf(prefix, sizeof(prefix), "Tab %d > ", current_tab + 1);
                
                if (tabs[current_tab].text_len > prefix_len) {
                    tabs[current_tab].text_len--;
                    tabs[current_tab].text_buffer[tabs[current_tab].text_len] = '\0';
                }
            } 
            else if (count > 0 && tabs[current_tab].text_len < BUFFER_SIZE - 1) {
                // Ignore control characters being printed directly
                if (lookup_buffer[0] >= 32 && lookup_buffer[0] < 127) {
                    tabs[current_tab].text_buffer[tabs[current_tab].text_len] = lookup_buffer[0];
                    tabs[current_tab].text_len++;
                    tabs[current_tab].text_buffer[tabs[current_tab].text_len] = '\0';
                }
            }

            // Trigger redraw
            XClearWindow(display, window);
            
            char header_buf[128];
            snprintf(header_buf, sizeof(header_buf), "Tabs: %d/%d (Active: %d) | Ctrl+T: New, Ctrl+W: Close, Ctrl+Tab: Switch", tab_count, MAX_TABS, current_tab + 1);
            XDrawString(display, window, gc, 20, 30, header_buf, strlen(header_buf));
            XDrawLine(display, window, gc, 20, 45, 580, 45);
            XDrawString(display, window, gc, 20, 80, tabs[current_tab].text_buffer, tabs[current_tab].text_len);
        }
    }

    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}