#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <ctype.h>

#define MAX_TEXT 256

// Helper function to find the next word boundary moving right
int find_next_word_boundary(const char *text, int len, int current) {
    if (current >= len) return len;
    int idx = current;
    // Skip spaces first if we are on them
    while (idx < len && isspace((unsigned char)text[idx])) idx++;
    // Skip the actual word characters
    while (idx < len && !isspace((unsigned char)text[idx])) idx++;
    return idx;
}

// Helper function to find the previous word boundary moving left
int find_prev_word_boundary(const char *text, int current) {
    if (current <= 0) return 0;
    int idx = current;
    // Move past any trailing spaces just before the cursor
    while (idx > 0 && isspace((unsigned char)text[idx - 1])) idx--;
    // Move past the word characters
    while (idx > 0 && !isspace((unsigned char)text[idx - 1])) idx--;
    return idx;
}

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

    // Determine font height metrics for highlighting bounds
    int font_ascent = 12;
    int font_descent = 2;
    if (font_info) {
        font_ascent = font_info->ascent;
        font_descent = font_info->descent;
    }

    char text_buffer[MAX_TEXT] = "";
    int text_len = 0;
    int cursor_idx = 0; // Tracks the actual insertion index inside the text
    
    // Selection indexes (-1 means no active selection)
    int sel_start = -1;
    int sel_end = -1;

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
                
                // --- 1. Draw Highlight Background ---
                if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
                    int start_idx = (sel_start < sel_end) ? sel_start : sel_end;
                    int end_idx = (sel_start < sel_end) ? sel_end : sel_start;
                    
                    int h_x1 = 0;
                    int h_x2 = 0;
                    if (font_info) {
                        h_x1 = XTextWidth(font_info, text_buffer, start_idx);
                        h_x2 = XTextWidth(font_info, text_buffer, end_idx);
                    } else {
                        h_x1 = start_idx * 6;
                        h_x2 = end_idx * 6;
                    }
                    
                    // Temporarily change color to Black to draw the block
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XFillRectangle(display, win, gc, 
                                   x_start + h_x1, 
                                   y_pos - font_ascent, 
                                   h_x2 - h_x1, 
                                   font_ascent + font_descent);
                }

                // --- 2. Draw Text Layers ---
                // If there's a selection, we split rendering so highlighted text shows white
                if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
                    int start_idx = (sel_start < sel_end) ? sel_start : sel_end;
                    int end_idx = (sel_start < sel_end) ? sel_end : sel_start;

                    // Text before selection (Black text)
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, win, gc, x_start, y_pos, text_buffer, start_idx);

                    // Selected text (White text over Black background block)
                    int offset_x1 = font_info ? XTextWidth(font_info, text_buffer, start_idx) : start_idx * 6;
                    XSetForeground(display, gc, WhitePixel(display, screen));
                    XDrawString(display, win, gc, x_start + offset_x1, y_pos, text_buffer + start_idx, end_idx - start_idx);

                    // Text after selection (Black text)
                    int offset_x2 = font_info ? XTextWidth(font_info, text_buffer, end_idx) : end_idx * 6;
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, win, gc, x_start + offset_x2, y_pos, text_buffer + end_idx, text_len - end_idx);
                } else {
                    // Regular clean fallback rendering (No selection active)
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, win, gc, x_start, y_pos, text_buffer, text_len);
                }
                
                // --- 3. Draw Blinking Caret ---
                int text_width = 0;
                if (font_info) {
                    text_width = XTextWidth(font_info, text_buffer, cursor_idx);
                } else {
                    text_width = cursor_idx * 6;
                }

                if (cursor_visible) {
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    int cursor_x = x_start + text_width;
                    XDrawLine(display, win, gc, cursor_x, y_pos - font_ascent, cursor_x, y_pos + font_descent);
                }
            }
            else if (event.type == KeyPress) {
                char buf[8];
                KeySym keysym;
                int len = XLookupString(&event.xkey, buf, sizeof(buf) - 1, &keysym, NULL);
                
                int shift_pressed = (event.xkey.state & ShiftMask);
                int ctrl_pressed = (event.xkey.state & ControlMask);

                // Handle Cursor Left Movement
                if (keysym == XK_Left) {
                    int old_cursor = cursor_idx;
                    if (ctrl_pressed) {
                        cursor_idx = find_prev_word_boundary(text_buffer, cursor_idx);
                    } else if (cursor_idx > 0) {
                        cursor_idx--;
                    }

                    if (shift_pressed) {
                        if (sel_start == -1) sel_start = old_cursor;
                        sel_end = cursor_idx;
                    } else {
                        sel_start = -1; sel_end = -1; // Reset selection
                    }
                    cursor_visible = 1;
                }
                // Handle Cursor Right Movement
                else if (keysym == XK_Right) {
                    int old_cursor = cursor_idx;
                    if (ctrl_pressed) {
                        cursor_idx = find_next_word_boundary(text_buffer, text_len, cursor_idx);
                    } else if (cursor_idx < text_len) {
                        cursor_idx++;
                    }

                    if (shift_pressed) {
                        if (sel_start == -1) sel_start = old_cursor;
                        sel_end = cursor_idx;
                    } else {
                        sel_start = -1; sel_end = -1; // Reset selection
                    }
                    cursor_visible = 1;
                }
                // Handle standard text entry / modification keys
                else if (keysym == XK_BackSpace || keysym == XK_Delete || len > 0) {
                    
                    // If a text block is actively selected, delete the block first
                    if (sel_start != -1 && sel_end != -1 && sel_start != sel_end) {
                        int start_idx = (sel_start < sel_end) ? sel_start : sel_end;
                        int end_idx = (sel_start < sel_end) ? sel_end : sel_start;
                        
                        memmove(text_buffer + start_idx, text_buffer + end_idx, text_len - end_idx + 1);
                        text_len -= (end_idx - start_idx);
                        cursor_idx = start_idx;
                        sel_start = -1; sel_end = -1;
                    } 
                    // Otherwise execute a classic standard Backspace operation
                    else if ((buf[0] == 8 || buf[0] == 127) && cursor_idx > 0) {
                        memmove(text_buffer + cursor_idx - 1, text_buffer + cursor_idx, text_len - cursor_idx + 1);
                        text_len--;
                        cursor_idx--;
                    } 
                    // Regular Typing characters input
                    else if (len > 0 && buf[0] >= 32 && text_len < MAX_TEXT - 1) {
                        memmove(text_buffer + cursor_idx + 1, text_buffer + cursor_idx, text_len - cursor_idx + 1);
                        text_buffer[cursor_idx] = buf[0];
                        text_len++;
                        cursor_idx++;
                    }
cursor_visible = 1;
}
// Force layout redraw
XEvent exp_event;
exp_event.type = Expose;
XSendEvent(display, win, False, ExposureMask, &exp_event);
}
}
}
if (font_info) XFreeFont(display, font_info);
XCloseDisplay(display);
return 0;
}
