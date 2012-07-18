#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "config.h"

// Here be dragons

static xcb_connection_t *c;
static int ft_height, ft_width;
static xcb_window_t root, win;
static xcb_gcontext_t gc;
static int bw, bh;
static const uint32_t pal[] = {COLOR0,COLOR1,COLOR2,COLOR3,COLOR4,COLOR5,COLOR6,COLOR7,COLOR8,COLOR9};
static xcb_drawable_t canvas;

#define MIN(a,b) ((a > b ? b : a))

void
fillrect (int color, int x, int y, int width, int height)
{
    xcb_change_gc (c, gc, XCB_GC_FOREGROUND, (const uint32_t []){ pal[color] });
    xcb_poly_fill_rectangle (c, canvas, gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

/* libc doesn't give a shit to -fshort-wchar, so here's our wcslen */
size_t
wcslen_ (wchar_t *s) { 
    size_t len; 
    for (len = 0; *s; s++, len++);
    return len;
}

int
draw (int x, int align, int fgcol, int bgcol, wchar_t *text)
{
    int done = 0;
    int pos_x = x;
    int len = MIN(bw / ft_width, wcslen_ (text));
    int strw = len * ft_width;

    if (!strw) return 0;

    switch (align) {
        case 1:
            xcb_copy_area (c, canvas, canvas, gc, bw / 2 - pos_x / 2, 0, bw / 2 - (pos_x + strw) / 2, 0, pos_x, BAR_HEIGHT);
            pos_x = bw / 2 - (pos_x + strw) / 2 + pos_x;
            break;
        case 2:
            xcb_copy_area (c, canvas, canvas, gc, bw - pos_x, 0, bw - pos_x - strw, 0, pos_x, BAR_HEIGHT);
            pos_x = bw - strw; 
            break;
    }
    /* Draw the background first */
    fillrect (bgcol, pos_x, 0, strw, BAR_HEIGHT);
    /* Setup the colors */
    xcb_change_gc (c, gc, XCB_GC_FOREGROUND, (const uint32_t []){ pal[fgcol] });
    xcb_change_gc (c, gc, XCB_GC_BACKGROUND, (const uint32_t []){ pal[bgcol] });
    do {
        xcb_image_text_16 (c, MIN(len - done, 255), canvas, gc, pos_x, bh - ft_height / 2, /* Bottom left coords */
                (xcb_char2b_t *)text + done);
        done += MIN(len - done, 255);
        pos_x = done * ft_width;
    } while (done < len);

    return pos_x;
}

void
parse (char *text)
{
    wchar_t parsed_text[2048] = {0, };

    wchar_t *q = parsed_text;
    char    *p = text;

    int pos_x = 0;
    int align = 0;

    int fgcol = 1;
    int bgcol = 0;

    fillrect (0, 0, 0, bw, BAR_HEIGHT);
    for (;;) {
        if (*p == 0x0 || *p == 0xA || (*p == '\\' && p++ && *p != '\\' && strchr ("fblcr", *p))) {
            pos_x += draw (pos_x, align, fgcol, bgcol, parsed_text);
            switch (*p++) {
                case 0x0: return; /* EOL */
                case 0xA: return; /* NL */
                case 'f': if (*p == 'r') *p = '1'; if (isdigit (*p)) { fgcol = *p-'0'; } p++; break;
                case 'b': if (*p == 'r') *p = '0'; if (isdigit (*p)) { bgcol = *p-'0'; } p++; break;
                case 'l': align = 0; pos_x = 0; break;
                case 'c': align = 1; pos_x = 0; break;
                case 'r': align = 2; pos_x = 0; break;
            }
            q = parsed_text;
        } else { /* utf-8 -> ucs-2 */
            if (!(p[0] & 0x80)) {
                *q++ = p[0] << 8; 
                p   += 1;
            }
            else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
                wchar_t t = (p[0] & 0x1f) << 6 | p[1] & 0x3f;
                *q++ = (t >> 8) | (t << 8);
                p   += 2;
            }
            else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
                wchar_t t = (p[0] & 0xf) << 12 | (p[1] & 0x3f) << 6 | p[2] & 0x3f;
                *q++ = (t >> 8) | (t << 8);
                p   += 3;
            }
        }
        *q = 0;
    }
}

void
cleanup (void)
{
    xcb_free_pixmap (c, canvas);
    xcb_destroy_window (c, win);
    xcb_free_gc (c, gc);
    xcb_disconnect (c);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM) exit (0);
}

void
init (void)
{
    xcb_font_t xf;
    xcb_screen_t *scr;
    xcb_query_font_reply_t *ft_info;

    /* Connect to X */
    c = xcb_connect (NULL, NULL);
    if (xcb_connection_has_error (c)) {
        fprintf (stderr, "Couldn't connect to X\n");
        exit (1);
    }
    /* Grab infos from the first screen */
    scr = xcb_setup_roots_iterator (xcb_get_setup (c)).data;
    bw = scr->width_in_pixels;
    bh = BAR_HEIGHT;
    root = scr->root;
    /* Load the font */
    xf = xcb_generate_id (c);
    if (xcb_request_check (c, xcb_open_font_checked (c, xf, strlen(BAR_FONT), BAR_FONT))) {
        fprintf (stderr, "Couldn't load the font\n");
        exit (1);
    }
    /* Grab infos from the font */
    ft_info = xcb_query_font_reply (c, xcb_query_font (c, xf), NULL);
    ft_height = ft_info->font_ascent + ft_info->font_descent;
    ft_width = ft_info->max_bounds.character_width;
    /* Create the main window */
    win = xcb_generate_id (c);
    xcb_create_window (c, XCB_COPY_FROM_PARENT, win, root, 0, 0, bw, bh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, 
            scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, 
            (const uint32_t []){ pal[0], 1, XCB_EVENT_MASK_EXPOSURE });
    /* Create a temporary canvas */
    canvas = xcb_generate_id (c);
    xcb_create_pixmap (c, scr->root_depth, canvas, root, bw, BAR_HEIGHT);
    /* Create the gc for drawing */
    gc = xcb_generate_id (c);
    xcb_create_gc (c, gc, root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
            (const uint32_t []){ pal[1], pal[0], xf });
    /* Get rid of the font */
    xcb_close_font (c, xf);
    /* Make the bar visible */
    xcb_map_window (c, win);
    xcb_flush (c);
}

int 
main (int argc, char **argv)
{
    struct pollfd pollin[2] = { { .fd = STDIN_FILENO, .events = POLLIN }, { .fd = -1, .events = POLLIN } };
    static char input[1024] = {0, };

    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;

    int permanent = 0;

    char ch;
    while ((ch = getopt (argc, argv, "ph")) != -1) {
        switch (ch) {
            case 'h': 
                printf ("usage: %s [-p | -h]\n"
                        "\t-h Shows this help\n"
                        "\t-p Don't close after the data ends\n", argv[0]); 
                exit (0);
            case 'p': permanent = 1; break;
        }
    }

    atexit (cleanup);
    signal (SIGINT, sighandle);
    signal (SIGTERM, sighandle);
    init ();

    /* Get the fd to Xserver */
    pollin[1].fd = xcb_get_file_descriptor (c);

    fillrect (0, 0, 0, bw, BAR_HEIGHT);

    for (;;) {
        int redraw = 0;

        if (poll ((struct pollfd *)&pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      /* No more data... */
                if (permanent) pollin[0].fd = -1;   /* ...null the fd and continue polling :D */
                else           break;               /* ...bail out */
            }
            if (pollin[0].revents & POLLIN) { /* New input, process it */
                fgets (input, sizeof(input), stdin);
                parse (input);
                redraw = 1;
            }
            if (pollin[1].revents & POLLIN) { /* Xserver broadcasted an event */
                while ((ev = xcb_poll_for_event (c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                        case XCB_EXPOSE: 
                            if (expose_ev->count == 0) redraw = 1; 
                        break;
                    }

                    free (ev);
                }
            }
        }

        if (redraw) /* Copy our temporary pixmap onto the window */
            xcb_copy_area (c, canvas, win, gc, 0, 0, 0, 0, bw, BAR_HEIGHT);
        xcb_flush (c);
    }

    return 0;
}
