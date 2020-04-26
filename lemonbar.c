// vim:sw=4:ts=4:et:
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <X11/Xft/Xft.h>
#include <X11/Xlib-xcb.h>

// Here be dragons!

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define indexof(c, s) (strchr((s), (c)) - (s))

typedef struct font_t {
    XftFont* xft_ft;

    int ascent;

    int descent, height, width;
    uint16_t char_max;
    uint16_t char_min;
} font_t;

typedef struct monitor_t {
    int x, y, width;
    xcb_window_t window;
    xcb_pixmap_t pixmap;
    struct monitor_t *prev, *next;
} monitor_t;

typedef struct area_t {
    xcb_window_t window;
    unsigned int button;
    int align;
    unsigned int begin;
    unsigned int end;
    char* cmd;
    bool closed;
} area_t;

typedef struct draw_context_t {
    monitor_t* monitor;
    XftDraw* xft_draw;
    int preferred_font_index;
    int align;
    int section_widths[3];
} draw_context_t;

typedef union rgba_t {
    struct {
        uint8_t b;
        uint8_t g;
        uint8_t r;
        uint8_t a;
    };
    uint32_t v;
} rgba_t;

enum { ALIGN_L = 0, ALIGN_C, ALIGN_R };

enum { GC_DRAW = 0, GC_CLEAR, GC_MAX };

static Display* dpy;
static xcb_connection_t* c;

static xcb_screen_t* scr;
static int scr_nbr = 0;

static xcb_gcontext_t gc[GC_MAX];
static xcb_visualid_t visual;
static Visual* visual_ptr;
static xcb_colormap_t colormap;

static monitor_t *monhead, *montail;
static font_t font;

static char* wm_name = NULL;
static bool permanent = false;
static int bar_height;
static int offset_y = 0;

static bool dock = false;
static bool topbar = true;
static rgba_t fgc, bgc;
static rgba_t dfgc, dbgc;

static area_t* areas;
static int num_areas;
static int max_areas;

static XftColor sel_fg;

void update_gc(void) {
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t[]){fgc.v});
    xcb_change_gc(c, gc[GC_CLEAR], XCB_GC_FOREGROUND, (const uint32_t[]){bgc.v});
    XftColorFree(dpy, visual_ptr, colormap, &sel_fg);
    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);
    if(!XftColorAllocName(dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }
}

void fill_gradient(xcb_drawable_t d, int x, int y, int width, int height, rgba_t start, rgba_t stop) {
    float i;
    const int K = 25; // The number of steps

    for(i = 0.; i < 1.; i += (1. / K)) {
        // Perform the linear interpolation magic
        unsigned int rr = i * stop.r + (1. - i) * start.r;
        unsigned int gg = i * stop.g + (1. - i) * start.g;
        unsigned int bb = i * stop.b + (1. - i) * start.b;

        // The alpha is ignored here
        rgba_t step = {
            .r = rr,
            .g = gg,
            .b = bb,
            .a = 255,
        };

        xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t[]){step.v});
        xcb_poly_fill_rectangle(c, d, gc[GC_DRAW], 1,
                                (const xcb_rectangle_t[]){{x, i * bar_height, width, bar_height / K + 1}});
    }

    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t[]){fgc.v});
}

void fill_rect(xcb_drawable_t d, xcb_gcontext_t _gc, int x, int y, int width, int height) {
    xcb_poly_fill_rectangle(c, d, _gc, 1, (const xcb_rectangle_t[]){{x, y, width, height}});
}

void area_copy_inplace(monitor_t* mon, int16_t src_x, int16_t src_y, int16_t dst_x, int16_t dst_y, uint16_t width,
                       uint16_t height) {
    xcb_copy_area(c, mon->pixmap, mon->pixmap, gc[GC_DRAW], src_x, src_y, dst_x, dst_y, width, height);
}

/**
 * Shift the rendered content of `content_width` size at alignment `align`
 * to the left by `offset_width`. Clear the source rect.
 * Returns the position of the cursor.
 */
int shift(monitor_t* mon, int content_width, int align, int offset_width) {
    switch(align) {
    case ALIGN_C:
        area_copy_inplace(mon, (mon->width - content_width) / 2, 0, (mon->width - content_width - offset_width) / 2, 0,
                          content_width, bar_height);
        content_width = (mon->width - content_width - offset_width) / 2 + content_width;
        break;
    case ALIGN_R:
        area_copy_inplace(mon, mon->width - content_width, 0, mon->width - content_width - offset_width, 0,
                          content_width, bar_height);
        content_width = mon->width - offset_width;
        break;
    }

    // Fill the gap where the content has been moved away from.
    fill_rect(mon->pixmap, gc[GC_CLEAR], content_width, 0, offset_width, bar_height);
    return content_width;
}

int xft_string_width(uint16_t* string, int num_chars) {
    XGlyphInfo extents;
    XftTextExtents16(dpy, font.xft_ft, (FcChar16*)string, num_chars, &extents);

    char* buffer = (char*)malloc(num_chars * sizeof(char));
    for(int i = 0; i < num_chars; i++) {
        buffer[i] = string[i];
    }

    printf("%.*s\t%d %d\n", num_chars, buffer, extents.width, extents.xOff);
    // This is the continuation of the mega dirty ugly hack for making lemonbar work again.
    // Whenever we render something which is wider than what we should offset the cursor by,
    // we offset the cursor by the width instead. Thus, all icons which are wider than monospace
    // are rendered tightly, but at least full-width.
    return extents.width > extents.xOff ? extents.width : extents.xOff;
}

/**
 * Draw the `string` to monitor `monitor` with font `font` at `x_offset`.
 */
void draw_string(monitor_t* monitor, XftDraw* xft_draw, int x_offset, uint16_t* string, int num_chars) {
    int y = bar_height / 2 + font.height / 2 - font.descent + offset_y;
    XftDrawString16(xft_draw, &sel_fg, font.xft_ft, x_offset, y, string, num_chars);
}

rgba_t expand_rgba_t(rgba_t color, int source_length, rgba_t const default_color) {
    switch(source_length) {
    case 3:
        // Expand the #rgb format into #rrggbb (aa is set to 0xff).
        color.v = (color.v & 0xf00) * 0x1100 | (color.v & 0x0f0) * 0x0110 | (color.v & 0x00f) * 0x0011;
    case 6:
        // If the code is in #rrggbb form then assume it's opaque.
        color.a = 255;
        break;
    case 7:
    case 8:
        // Colors in #aarrggbb format, those need no adjustments.
        break;
    default:
        fprintf(stderr, "Invalid color specified\n");
        return default_color;
    }

    // Premultiply the alpha in.
    if(color.a == 0) {
        return (rgba_t)0U;
    }

    // The components are clamped automagically as the rgba_t is made of uint8_t.
    return (rgba_t){
        .r = (color.r * color.a) / 255,
        .g = (color.g * color.a) / 255,
        .b = (color.b * color.a) / 255,
        .a = color.a,
    };
}

rgba_t parse_color(char* cursor, rgba_t const default_color) {
    if(cursor == NULL) {
        return default_color;
    }

    // A `-` means: "reset to default".
    char const next_char = cursor[0];
    if(next_char == '-') {
        return default_color;
    }

    // Require hex representation.
    if(next_char != '#') {
        fprintf(stderr, "Invalid color specified\n");
        return default_color;
    }

    // Skip the first char.
    cursor++;

    char* end;
    errno = 0;
    rgba_t tmp = (rgba_t)(uint32_t)strtoul(cursor, &end, 16);

    if(errno) {
        fprintf(stderr, "Invalid color specified\n");
        return default_color;
    }

    int string_len = end - cursor;
    return expand_rgba_t(tmp, string_len, default_color);
}

void swap_colors() {
    rgba_t tmp = fgc;
    fgc = bgc;
    bgc = tmp;
    update_gc();
}

area_t* area_get(xcb_window_t win, const int btn, const int x) {
    // Looping backwards ensures that we get the innermost area first.
    for(int i = num_areas - 1; i >= 0; i--) {
        area_t* a = areas + i;
        if(a->window == win && a->button == btn && x >= a->begin && x < a->end) {
            return a;
        }
    }
    return NULL;
}

void area_shift(xcb_window_t win, const int align, int delta) {
    if(align == ALIGN_L) {
        return;
    }
    if(align == ALIGN_C) {
        delta /= 2;
    }

    for(int i = 0; i < num_areas; i++) {
        area_t* a = areas + i;
        if(a->window == win && a->align == align) {
            a->begin -= delta;
            a->end -= delta;
        }
    }
}

void area_open(char* cursor, const char* cursor_end, monitor_t* mon, const int x, const int align, const int button) {
    if(num_areas == max_areas) {
        fprintf(stderr, "Cannot add more clickable areas\n");
        return;
    }

    area_t* a = areas + num_areas;

    // Skip the starting `:`.
    cursor++;

    // Search for the next `:` which is not escaped.
    char* end = strchr(cursor, ':');
    while(end != NULL && end[-1] == '\\') {
        end = strchr(end + 1, ':');
    }

    // Sanity check.
    if(end == NULL || end == cursor || end > cursor_end) {
        fprintf(stderr, "Invalid command: %.10s\n", cursor);
        return;
    }

    // Terminate the command string.
    // We can do this, because the string is allocated in the main and is only valid during one cycle.
    *end = '\0';

    // Unescape all the `:`.
    for(char* c_ptr = cursor; *c_ptr != '\0'; c_ptr++) {
        if(c_ptr[0] == '\\' && c_ptr[1] == ':') {
            memmove(c_ptr, c_ptr + 1, end - c_ptr + 1);
        }
    }

    a->window = mon->window;
    a->button = button;
    a->align = align;
    a->begin = x;
    a->cmd = cursor;
    a->closed = false;

    num_areas++;
}

void area_close(monitor_t* mon, const int section_width, const int align, const int button) {
    // Find most recent unclosed area.
    int i = num_areas - 1;
    while(i >= 0 && (areas[i].closed || areas[i].window != mon->window || areas[i].align != align)) {
        i--;
    };
    if(i < 0) {
        fprintf(stderr, "No open clickable areas to close\n");
        return;
    }

    area_t* a = areas + i;

    switch(align) {
    case ALIGN_L:
        a->end = section_width;
        break;
    case ALIGN_C:
        a->end = (mon->width + section_width) / 2;
        break;
    case ALIGN_R:
        a->end = mon->width;
        break;
    }

    a->closed = true;
}

bool is_end_of_input(char* cursor_ptr) { return *cursor_ptr == '\0' || *cursor_ptr == '\n'; }

/**
 * Convert the next (presumably) UTF-8 symbol to UCS-2.
 */
uint16_t parse_ucs2_char(char** cursor_ptr) {
    uint8_t* utf = (uint8_t*)*cursor_ptr;

    // ASCII.
    if(utf[0] < 0x80) {
        *(cursor_ptr) += 1;
        return utf[0];
    }
    // Two byte utf8 sequence.
    if((utf[0] & 0xe0) == 0xc0) {
        *(cursor_ptr) += 2;
        return (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
    }
    // Three byte utf8 sequence.
    if((utf[0] & 0xf0) == 0xe0) {
        *(cursor_ptr) += 3;
        return (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
    }
    // Four byte utf8 sequence.
    if((utf[0] & 0xf8) == 0xf0) {
        *(cursor_ptr) += 4;
        return 0xfffd;
    }
    // Five byte utf8 sequence.
    if((utf[0] & 0xfc) == 0xf8) {
        *(cursor_ptr) += 5;
        return 0xfffd;
    }
    // Six byte utf8 sequence.
    if((utf[0] & 0xfe) == 0xfc) {
        *(cursor_ptr) += 6;
        return 0xfffd;
    }

    // Not a valid utf-8 sequence.
    *(cursor_ptr) += 1;
    return utf[0];
}

void clear_on_all_monitors() {
    for(monitor_t* m = monhead; m != NULL; m = m->next) {
        fill_rect(m->pixmap, gc[GC_CLEAR], 0, 0, m->width, bar_height);
    }
}

void write_text(draw_context_t* context, uint16_t* string, int num_chars) {
    int* section_width = context->section_widths + context->align;
    int string_width = xft_string_width(string, num_chars);

    // Shift visuals and clickable areas.
    int cursor_x = shift(context->monitor, *section_width, context->align, string_width);
    area_shift(context->monitor->window, context->align, string_width);

    // Draw text.
    draw_string(context->monitor, context->xft_draw, cursor_x, string, num_chars);

    *section_width += string_width;
}

void button_command(draw_context_t* context, char* cursor, char* block_end) {
    char c = *cursor++;
    int button = XCB_BUTTON_INDEX_1;

    // The range is 1-5
    if(!isdigit(c) || c <= '0' || c >= '6') {
        return;
    }
    button = c - '0';

    int section_width = context->section_widths[context->align];
    if(cursor == block_end) {
        area_open(cursor, block_end, context->monitor, section_width, context->align, button);
    } else {
        area_close(context->monitor, section_width, context->align, button);
    }
}

void select_next_monitor(draw_context_t* context) { context->monitor = context->monitor->next; }
void select_prev_monitor(draw_context_t* context) { context->monitor = context->monitor->prev; }

void reset_section_widths(draw_context_t* context) {
    context->section_widths[0] = context->section_widths[1] = context->section_widths[2] = 0;
}

void reinit_xft_draw(draw_context_t* context) {
    if(context->xft_draw != NULL) {
        XftDrawDestroy(context->xft_draw);
    }
    context->xft_draw = XftDrawCreate(dpy, context->monitor->pixmap, visual_ptr, colormap);
    if(context->xft_draw == NULL) {
        fprintf(stderr, "Couldn't create xft drawable\n");
    }
}

void select_monitor(draw_context_t* context, char* cursor) {
    char c = *cursor;
    if(c == '+' && context->monitor->next) {
        select_next_monitor(context);
    } else if(c == '-' && context->monitor->prev) {
        select_prev_monitor(context);
    } else if(c == 'f') {
        context->monitor = monhead;
    } else if(c == 'l') {
        context->monitor = montail ? montail : monhead;
    } else if(isdigit(c)) {
        context->monitor = monhead;
        for(int i = 0; i != c - '0' && context->monitor->next; i++) {
            select_next_monitor(context);
        }
    } else {
        fprintf(stderr, "Invalid monitor selection character `%c`\n", c);
        return;
    }

    reinit_xft_draw(context);
    reset_section_widths(context);
}

void flush_buffer(draw_context_t* context, uint16_t* string_buffer, int* string_buffer_offset) {
    if(*string_buffer_offset == 0) {
        return;
    }
    write_text(context, string_buffer, *string_buffer_offset);
    *string_buffer_offset = 0;
}

void offset_by_pixels(draw_context_t* context, char* cursor) {
    errno = 0;
    int w = (int)strtoul(cursor, NULL, 10);
    if(errno) {
        fprintf(stderr, "Invalid amount of pixels `%.5s`\n", cursor);
        return;
    }

    int* section_width = context->section_widths + context->align;

    // Shift visuals and clickable areas.
    shift(context->monitor, *section_width, context->align, w);
    area_shift(context->monitor->window, context->align, w);

    *section_width += w;
}

int parse_align(char c) {
    switch(c) {
    case 'c':
        return ALIGN_C;
    case 'r':
        return ALIGN_R;
    case 'l':
    default:
        return ALIGN_L;
    }
}

void parse_command(draw_context_t* context, char* cursor, char* command_end) {
    char c = cursor[0];

    if(c == 'l' || c == 'c' || c == 'r') {
        context->align = parse_align(c);
    } else if(c == 'B' || c == 'F') {
        bool is_foreground = c == 'F';
        *(is_foreground ? &fgc : &bgc) = parse_color(cursor + 1, is_foreground ? dfgc : dbgc);
        update_gc();
    } else {
        switch(c) {
        // Swap fg/bg colors.
        case 'R':
            swap_colors();
            break;

        case 'A':
            button_command(context, cursor + 1, command_end);
            break;

        case 'S':
            select_monitor(context, cursor + 1);
            break;
        case 'O':
            offset_by_pixels(context, cursor + 1);
            break;

        default:
            break;
        }
    }
}

void parse(char* cursor) {
    char* block_end;

    uint16_t string_buffer[2048];
    int string_buffer_offset = 0;

    draw_context_t context;
    context.monitor = monhead;
    context.xft_draw = NULL;
    context.align = ALIGN_L;
    reset_section_widths(&context);

    clear_on_all_monitors();
    reinit_xft_draw(&context);

    num_areas = 0;

    for(;;) {
        // If input ends then stop.
        if(is_end_of_input(cursor)) {
            break;
        }

        // In case it's a command (Button commands MAY NOT include closing curly braces!).
        if(cursor[0] == '%' && cursor[1] == '{' && (block_end = strchr(cursor, '}'))) {
            flush_buffer(&context, string_buffer, &string_buffer_offset);

            // Parse a command starting at the first letter in the curly braces.
            parse_command(&context, cursor + 2, block_end);

            // Skip to the end of the command and eat the trailing `}`.
            cursor = block_end + 1;
            continue;
        }

        uint16_t next = parse_ucs2_char(&cursor);

        // This here is a mega dirty ugly hack, which makes the bar look nice again.
        // The icons in AUR's `nerd-fonts-fira-mono` package are hopelessly broken.
        // Their offsets are smaller than the icons themselves, which leads to icons being
        // partially overlapped with whatever comes next. Therefore, while regular text uses
        // the buffered rendering, any non-ascii (>0xFF) characters are rendered separately.
        // See `xft_string_width` for reference.
        if(string_buffer_offset > 0 && (string_buffer[string_buffer_offset - 1] > 0xFF || next > 0xFF)) {
            flush_buffer(&context, string_buffer, &string_buffer_offset);
        }
        string_buffer[string_buffer_offset++] = next;
    }

    flush_buffer(&context, string_buffer, &string_buffer_offset);
    XftDrawDestroy(context.xft_draw);
}

void font_load(const char* pattern) {
    font.xft_ft = XftFontOpenName(dpy, scr_nbr, pattern);
    if(font.xft_ft == NULL) {
        fprintf(stderr, "Could not load font %s\n", pattern);
        return;
    }

    font.ascent = font.xft_ft->ascent;
    font.descent = font.xft_ft->descent;
    font.height = font.ascent + font.descent;

    bar_height = font.height;
}

enum {
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_DESKTOP,
    NET_WM_STRUT_PARTIAL,
    NET_WM_STRUT,
    NET_WM_STATE,
    NET_WM_STATE_STICKY,
    NET_WM_STATE_ABOVE,
};

void set_ewmh_atoms(void) {
    const char* atom_names[] = {
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_DESKTOP",
        "_NET_WM_STRUT_PARTIAL",
        "_NET_WM_STRUT",
        "_NET_WM_STATE",
        // Leave those at the end since are batch-set
        "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_ABOVE",
    };
    const int atoms = sizeof(atom_names) / sizeof(char*);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];

    // As suggested fetch all the cookies first (yum!) and then retrieve the
    // atoms to exploit the async'ness.
    for(int i = 0; i < atoms; i++) {
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);
    }

    for(int i = 0; i < atoms; i++) {
        xcb_intern_atom_reply_t* atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if(!atom_reply) {
            return;
        }
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    // Prepare the strut array
    for(monitor_t* mon = monhead; mon; mon = mon->next) {
        int strut[12] = {0};
        if(topbar) {
            strut[2] = bar_height;
            strut[8] = mon->x;
            strut[9] = mon->x + mon->width;
        } else {
            strut[3] = bar_height;
            strut[10] = mon->x;
            strut[11] = mon->x + mon->width;
        }

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1,
                            &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
        xcb_change_property(c, XCB_PROP_MODE_APPEND, mon->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2,
                            &atom_list[NET_WM_STATE_STICKY]);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1,
                            (const uint32_t[]){0u - 1u});
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL,
                            32, 12, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4,
                            strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 3, "bar");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 12,
                            "lemonbar\0Bar");
    }
}

monitor_t* monitor_new(int x, int y, int width, int height) {
    monitor_t* monitor = calloc(1, sizeof(monitor_t));
    if(!monitor) {
        fprintf(stderr, "Failed to allocate new monitor\n");
        exit(EXIT_FAILURE);
    }

    monitor->x = x;
    monitor->y = y;
    monitor->width = width;

    monitor->next = monitor->prev = NULL;
    monitor->window = xcb_generate_id(c);
    int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    uint32_t mask =
        XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t value_list[] = {bgc.v, bgc.v, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap};
    xcb_create_window(c, depth, monitor->window, scr->root, monitor->x, monitor->y, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, (void*)value_list);

    monitor->pixmap = xcb_generate_id(c);
    xcb_create_pixmap(c, depth, monitor->pixmap, monitor->window, width, height);

    return monitor;
}

void monitor_add(monitor_t* mon) {
    if(!monhead) {
        monhead = mon;
    } else if(!montail) {
        montail = mon;
        monhead->next = mon;
        mon->prev = monhead;
    } else {
        mon->prev = montail;
        montail->next = mon;
        montail = montail->next;
    }
}

int rect_sort_cb(const void* p1, const void* p2) {
    const xcb_rectangle_t* r1 = (xcb_rectangle_t*)p1;
    const xcb_rectangle_t* r2 = (xcb_rectangle_t*)p2;

    if(r1->x < r2->x || r1->y + r1->height <= r2->y) {
        return -1;
    }

    if(r1->x > r2->x || r1->y + r1->height > r2->y) {
        return 1;
    }

    return 0;
}

void monitor_create_chain(xcb_rectangle_t* rects, const int num) {
    int width = 0, height = 0;
    int left = 0;

    // Sort before use
    qsort(rects, num, sizeof(xcb_rectangle_t), rect_sort_cb);

    for(int i = 0; i < num; i++) {
        int h = rects[i].y + rects[i].height;
        // Accumulated width of all monitors
        width += rects[i].width;
        // Get height of screen from y_offset + height of lowest monitor
        if(h >= height)
            height = h;
    }

    // Left is a positive number or zero therefore monitors with zero width are excluded
    /* width = bw; */
    for(int i = 0; i < num; i++) {
        if(rects[i].width > left) {
            monitor_t* mon =
                monitor_new(rects[i].x + left, rects[i].y, min(width, rects[i].width - left), rects[i].height);

            if(!mon)
                break;

            monitor_add(mon);

            width -= rects[i].width - left;
            // No need to check for other monitors
            if(width <= 0)
                break;
        }

        left -= rects[i].width;

        if(left < 0)
            left = 0;
    }
}

bool xcb_rect_contains_xcb_rect(xcb_rect* a, xcb_rect* b) {
    bool a_contains_b_horizontal = a.x <= b.x && b.x + b.width <= a.x + a.width;
    bool a_contains_b_vertical = a.y <= b.y && b.y + b.height <= a.y + a.height;
    return a_contains_b_horizontal && a_contains_b_vertical;
}

#define _RANDR_REQUEST(operation, variable, ...)                                                                       \
    operation##_reply_t* variable = operation##_reply(c, operation(__VA_ARGS__), NULL);
#define RANDR_REQUEST(operation, variable, ...) _RANDR_REQUEST(xcb_randr_##operation, variable, __VA_ARGS__)

int populate_rects_from_outputs(xcb_randr_output_t* outputs, xcb_rectangle_t* rects, int max_rects) {
    // Get all outputs.
    int num_rects = 0;
    for(int i = 0; i < num; i++) {
        RANDR_REQUEST(get_output_info, output_info, c, outputs[i], XCB_CURRENT_TIME)
        if(output_info == NULL) {
            fprintf(stderr, "No output info available\n");
            continue;
        }

        uint8_t connection = output_info->connection;
        xcb_randr_crtc_t crtc = output_info->crtc;
        free(output_info);

        // Output disconnected or not attached to any CRTC ?
        if(crtc == XCB_NONE || connection != XCB_RANDR_CONNECTION_CONNECTED) {
            continue;
        }

        RANDR_REQUEST(get_crtc_info, crtc_info, c, crtc, XCB_CURRENT_TIME);
        if(crtc_info == NULL) {
            fprintf(stderr, "Failed to get RandR crtc info\n");
            continue;
        }

        // There's no need to handle rotated screens here (see #69)
        xcb_rectangle_t new_rect{crtc_info->x, crtc_info->y, crtc_info->width, crtc_info->height};
        free(crtc_info);

        // Check for clones and inactive outputs.
        // When one screen contains another, use the outermost screen.
        for(int j = 0; j < num_rects; j++) {
            if(rects[j].x >= rects[i].x && rects[j].x + rects[j].width <= rects[i].x + rects[i].width &&
               rects[j].y >= rects[i].y && rects[j].y + rects[j].height <= rects[i].y + rects[i].height) {
                rects[j].width = 0;
                num_rects--;
            }
        }

        rects[i] = (xcb_rectangle_t)num_rects++;

    }
    return num_rects;
}

void handle_randr_monitors(xcb_randr_get_screen_resources_current_reply_t* screen_resources) {
    // There should be at least one output.
    int num = xcb_randr_get_screen_resources_current_outputs_length(screen_resources);
    if(num < 1) {
        return;
    }

    xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_current_outputs(screen_resources);

    xcb_rectangle_t rects[num];
    int num_rects = populate_rects_from_outputs(outputs, rects, num);


    if(num_rects < 1) {
        fprintf(stderr, "No usable RandR output found\n");
        return;
    }

    xcb_rectangle_t r[num_rects];

    for(int i = 0, j = 0; i < num && j < num_rects; i++)
        if(rects[i].width != 0)
            r[j++] = rects[i];

    monitor_create_chain(r, num_rects);
}

// Query the screen resources to get an overview of the current monitor setup.
void get_randr_monitors(void) {
    RANDR_REQUEST(get_screen_resources_current, screen_resources, c, scr->root)
    if(screen_resources == NULL) {
        fprintf(stderr, "Failed to get current randr screen resources\n");
        return;
    }
    handle_randr_monitors(screen_resources);
    free(screen_resources);
}

xcb_visualid_t get_visual(void) {
    XVisualInfo xv;
    xv.depth = 32;
    int result = 0;
    XVisualInfo* result_ptr = XGetVisualInfo(dpy, VisualDepthMask, &xv, &result);

    if(result > 0) {
        visual_ptr = result_ptr->visual;
        return result_ptr->visualid;
    }

    // Fallback
    visual_ptr = DefaultVisual(dpy, scr_nbr);
    return scr->root_visual;
}

void xconn(void) {
    if((dpy = XOpenDisplay(0)) == NULL) {
        fprintf(stderr, "Couldn't open display\n");
        exit(EXIT_FAILURE);
    }

    if((c = XGetXCBConnection(dpy)) == NULL) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

    if(xcb_connection_has_error(c)) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    // Grab infos from the first screen.
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    /* Try to get a RGBA visual and build the colormap for that */
    visual = get_visual();
    colormap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, scr->root, visual);
}

void init(char* wm_instance) {
    // Initialize monitor list head and tail.
    monhead = montail = NULL;

    // Generate a list of screens and check if RandR is present.
    const xcb_query_extension_reply_t* qe_reply = xcb_get_extension_data(c, &xcb_randr_id);
    if(qe_reply && qe_reply->present) {
        get_randr_monitors();
    }

    if(!monhead) {
        // If no RandR outputs or Xinerama screens, fall back to using whole screen
        monhead = monitor_new(0, 0, 0, scr->height_in_pixels);
    }

    if(!monhead) {
        fprintf(stderr, "Failed to initialize new monitor\n");
        exit(EXIT_FAILURE);
    }

    // For WM that support EWMH atoms
    set_ewmh_atoms();

    // Create the gc for drawing
    gc[GC_DRAW] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_DRAW], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t[]){fgc.v});

    gc[GC_CLEAR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_CLEAR], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t[]){bgc.v});

    // Make the bar visible and clear the pixmap
    for(monitor_t* mon = monhead; mon; mon = mon->next) {
        fill_rect(mon->pixmap, gc[GC_CLEAR], 0, 0, mon->width, bar_height);
        xcb_map_window(c, mon->window);

        // Make sure that the window really gets in the place it's supposed to be
        // Some WM such as Openbox need this
        xcb_configure_window(c, mon->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             (const uint32_t[]){mon->x, mon->y});

        // Set the WM_NAME atom to the user specified value
        if(wm_name)
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                                strlen(wm_name), wm_name);

        // set the WM_CLASS atom instance to the executable name
        if(wm_instance) {
            char* wm_class;
            int wm_class_offset, wm_class_len;

            // WM_CLASS is nullbyte seperated: wm_instance + "\0Bar\0"
            wm_class_offset = strlen(wm_instance) + 1;
            wm_class_len = wm_class_offset + 4;

            wm_class = calloc(1, wm_class_len + 1);
            strcpy(wm_class, wm_instance);
            strcpy(wm_class + wm_class_offset, "Bar");

            xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                                wm_class_len, wm_class);

            free(wm_class);
        }
    }

    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);

    if(!XftColorAllocName(dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }
    xcb_flush(c);
}

void cleanup(void) {
    free(areas);
    XftFontClose(dpy, font.xft_ft);

    while(monhead) {
        monitor_t* next = monhead->next;
        xcb_destroy_window(c, monhead->window);
        xcb_free_pixmap(c, monhead->pixmap);
        free(monhead);
        monhead = next;
    }

    XftColorFree(dpy, visual_ptr, colormap, &sel_fg);

    if(gc[GC_DRAW])
        xcb_free_gc(c, gc[GC_DRAW]);
    if(gc[GC_CLEAR])
        xcb_free_gc(c, gc[GC_CLEAR]);
    if(c)
        xcb_disconnect(c);
}

char* strip_path(char* path) {
    if(path == NULL || *path == '\0') {
        return strdup("lemonbar");
    }

    char* slash = strrchr(path, '/');
    if(slash != NULL) {
        return strndup(slash + 1, 31);
    }

    return strndup(path, 31);
}

void sighandle(int signal) {
    if(signal == SIGINT || signal == SIGTERM) {
        exit(EXIT_SUCCESS);
    }
}

void print_help(char* executable_name) {
    printf("lemonbar version %s patched with XFT support\n", VERSION);
    printf("usage: %s [-h | -b | -d | -f | -a | -p | -n | -B | -F]\n"
           "\t-h Show this help\n"
           "\t-b Put the bar at the bottom of the screen\n"
           "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
           "\t-f Set the font name to use\n"
           "\t-a Number of clickable areas available (default is 10)\n"
           "\t-p Don't close after the data ends\n"
           "\t-n Set the WM_NAME atom to the specified value for this bar\n"
           "\t-B Set background color in #AARRGGBB\n"
           "\t-F Set foreground color in #AARRGGBB\n",
           executable_name);
}

void parse_options(int argc, char* argv[]) {
    int ch;
    while((ch = getopt(argc, argv, "hbdf:a:pB:F:n:")) != -1) {
        switch(ch) {
        case 'h':
            print_help(argv[0]);
            exit(EXIT_SUCCESS);
        case 'p':
            permanent = true;
            break;
        case 'n':
            wm_name = strdup(optarg);
            break;
        case 'b':
            topbar = false;
            break;
        case 'd':
            dock = true;
            break;
        case 'f':
            font_load(optarg);
            break;
        case 'B':
            dbgc = bgc = parse_color(optarg, (rgba_t)0x00000000U);
            break;
        case 'F':
            dfgc = fgc = parse_color(optarg, (rgba_t)0xffffffffU);
            break;
        case 'a':
            max_areas = strtoul(optarg, NULL, 10);
            break;
        }
    }
}

void handle_xcb_events(bool* redraw) {
    xcb_generic_event_t* ev;
    while((ev = xcb_poll_for_event(c))) {
        switch(ev->response_type & 0x7F) {
        case XCB_EXPOSE: {
            xcb_expose_event_t* expose_ev = (xcb_expose_event_t*)ev;
            if(expose_ev->count == 0) {
                *redraw = true;
            }
            break;
        }
        case XCB_BUTTON_PRESS: {
            xcb_button_press_event_t* press_ev = (xcb_button_press_event_t*)ev;
            area_t* area = area_get(press_ev->event, press_ev->detail, press_ev->event_x);
            if(area != NULL) {
                (void)write(STDOUT_FILENO, area->cmd, strlen(area->cmd));
                (void)write(STDOUT_FILENO, "\n", 1);
            }
            break;
        }
        }

        free(ev);
    }
}

int main(int argc, char* argv[]) {
    struct pollfd pollin[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = -1, .events = POLLIN},
    };
    char input[4096] = {0};
    char* instance_name = strip_path(argv[0]);

    // Install the parachute!
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    // B/W combo
    dbgc = bgc = (rgba_t)0x00000000U;
    dfgc = fgc = (rgba_t)0xffffffffU;

    max_areas = 10;

    // Connect to the Xserver and initialize scr
    xconn();

    parse_options(argc, argv);

    areas = (area_t*)malloc(max_areas * sizeof(area_t));
    if(areas == NULL) {
        fprintf(stderr, "Could not allocate memory for %d clickable areas\n", max_areas);
        return EXIT_FAILURE;
    }

    // Do the heavy lifting
    init(instance_name);
    // The string is strdup'd when the command line arguments are parsed
    free(wm_name);
    // The string is strdup'd when stripping argv[0]
    free(instance_name);
    // Get the fd to Xserver
    pollin[1].fd = xcb_get_file_descriptor(c);

    // Prevent fgets to block
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    for(;;) {
        bool redraw = false;

        // If connection is in error state, then it has been shut down.
        if(xcb_connection_has_error(c))
            break;

        if(poll(pollin, 2, -1) > 0) {
            // No more data...
            if(pollin[0].revents & POLLHUP) {
                if(!permanent) {
                    break; // ...bail out
                }
                pollin[0].fd = -1; // ...null the fd and continue polling :D
            }

            // New input, process it
            if(pollin[0].revents & POLLIN) {
                input[0] = '\0';
                while(fgets(input, sizeof(input), stdin) != NULL)
                    ; // Drain the buffer, the last line is actually used
                parse(input);
                redraw = true;
            }

            // The event comes from the Xorg server.
            if(pollin[1].revents & POLLIN) {
                handle_xcb_events(&redraw);
            }
        }

        // Copy our temporary pixmap onto the window.
        if(redraw) {
            for(monitor_t* mon = monhead; mon; mon = mon->next) {
                xcb_copy_area(c, mon->pixmap, mon->window, gc[GC_DRAW], 0, 0, 0, 0, mon->width, bar_height);
            }
        }

        xcb_flush(c);
    }

    return EXIT_SUCCESS;
}
