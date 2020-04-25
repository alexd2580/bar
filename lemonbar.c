// vim:sw=4:ts=4:et:
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>

#include <X11/Xft/Xft.h>
#include <X11/Xlib-xcb.h>

// Here be dragons!

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define indexof(c,s) (strchr((s),(c))-(s))

typedef struct font_t {
    XftFont *xft_ft;

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
    unsigned int begin:16;
    unsigned int end:16;
    bool active:1;
    int align:3;
    unsigned int button:3;
    xcb_window_t window;
    char *cmd;
} area_t;

typedef struct draw_context_t {
    monitor_t* monitor;
    XftDraw* xft_draw;
    int preferred_font_index;
    font_t *font;
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

typedef struct area_stack_t {
    int at, max;
    area_t *area;
} area_stack_t;

enum {
    ATTR_OVERL = (1 << 0),
    ATTR_UNDERL = (1 << 1),
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

enum {
    GC_DRAW = 0,
    GC_CLEAR,
    GC_ATTR,
    GC_MAX
};

#define MAX_FONT_COUNT 5

static Display *dpy;
static xcb_connection_t *c;

static xcb_screen_t *scr;
static int scr_nbr = 0;

static xcb_gcontext_t gc[GC_MAX];
static xcb_visualid_t visual;
static Visual *visual_ptr;
static xcb_colormap_t colormap;

static monitor_t *monhead, *montail;
static font_t *font_list[MAX_FONT_COUNT];
static int font_count = 0;
static int offsets_y[MAX_FONT_COUNT];
static int offset_y_count = 0;
static int offset_y_index = 0;

static uint32_t attrs = 0;
static bool dock = false;
static bool topbar = true;
static int bw = -1, bh = -1, bx = 0, by = 0;
static int bu = 1; // Underline height
static rgba_t fgc, bgc, ugc;
static rgba_t dfgc, dbgc, dugc;
static area_stack_t area_stack;

static XftColor sel_fg;

void update_gc(void) {
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });
    xcb_change_gc(c, gc[GC_CLEAR], XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });
    xcb_change_gc(c, gc[GC_ATTR], XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });
    XftColorFree(dpy, visual_ptr, colormap , &sel_fg);
    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);
    if (!XftColorAllocName (dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }
}

void
fill_gradient (xcb_drawable_t d, int x, int y, int width, int height, rgba_t start, rgba_t stop)
{
    float i;
    const int K = 25; // The number of steps

    for (i = 0.; i < 1.; i += (1. / K)) {
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

        xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ step.v });
        xcb_poly_fill_rectangle(c, d, gc[GC_DRAW], 1,
                               (const xcb_rectangle_t []){ { x, i * bh, width, bh / K + 1 } });
    }

    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });
}

void fill_rect(xcb_drawable_t d, xcb_gcontext_t _gc, int x, int y, int width, int height) {
    xcb_poly_fill_rectangle(c, d, _gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

int xft_string_width(uint16_t* string, int num_chars, font_t *cur_font) {
    XGlyphInfo extents;
    XftTextExtents16(dpy, cur_font->xft_ft, (FcChar16*)string, num_chars, &extents);

    char* buffer = (char*)malloc(num_chars * sizeof(char));
    for (int i=0; i<num_chars; i++) {
        buffer[i] = string[i];
    }

    printf("%.*s, %d %d %d\n", num_chars, buffer, extents.width, extents.xOff, extents.width + extents.xOff);
    return extents.width > extents.xOff ? 2 * extents.xOff : extents.xOff;
}

void area_copy_inplace(
    monitor_t *mon, int16_t src_x, int16_t src_y, int16_t dst_x, int16_t dst_y, uint16_t width, uint16_t height
) {
    xcb_copy_area(c, mon->pixmap, mon->pixmap, gc[GC_DRAW], src_x, src_y, dst_x, dst_y, width, height);
}

/**
 * Shift the rendered content of `content_width` size at alignment `align`
 * to the left by `offset_width`. Clear the source rect.
 * Returns the position of the cursor.
 */
int shift(monitor_t *mon, int content_width, int align, int offset_width) {
    switch (align) {
        case ALIGN_C:
            area_copy_inplace(mon,
                (mon->width - content_width) / 2, 0,
                (mon->width - content_width - offset_width) / 2, 0,
                content_width, bh
            );
            content_width = (mon->width - content_width - offset_width) / 2 + content_width;
            break;
        case ALIGN_R:
            area_copy_inplace(mon,
                mon->width - content_width, 0,
                mon->width - content_width - offset_width, 0,
                content_width, bh
            );
            content_width = mon->width - offset_width;
            break;
    }

    // Fill the gap where the content has been moved away from.
    fill_rect(mon->pixmap, gc[GC_CLEAR], content_width, 0, offset_width, bh);
    return content_width;
}

void draw_lines(monitor_t *mon, int x, int w) {
    if (attrs & ATTR_OVERL) {
        fill_rect(mon->pixmap, gc[GC_ATTR], x, 0, w, bu);
    }
    if (attrs & ATTR_UNDERL) {
        fill_rect(mon->pixmap, gc[GC_ATTR], x, bh - bu, w, bu);
    }
}

/**
 * Draw the `string` to monitor `monitor` with font `font` at `x_offset`.
 */
void draw_string(
    monitor_t* monitor, font_t* font, XftDraw* xft_draw, int x_offset, uint16_t* string, int num_chars
) {
    int y = bh / 2 + font->height / 2 - font->descent + offsets_y[offset_y_index];
    XftDrawString16(xft_draw, &sel_fg, font->xft_ft, x_offset, y, string, num_chars);
}

rgba_t expand_rgba_t(rgba_t color, int source_length, rgba_t const default_color) {
    switch (source_length) {
    case 3:
        // Expand the #rgb format into #rrggbb (aa is set to 0xff).
        color.v = (color.v & 0xf00) * 0x1100
                | (color.v & 0x0f0) * 0x0110
                | (color.v & 0x00f) * 0x0011;
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
    if (color.a == 0) {
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

rgba_t parse_color(char** cursor_ptr, rgba_t const default_color) {
    char* cursor = *cursor_ptr;
    if (cursor == NULL) {
        return default_color;
    }

    // A `-` means: "reset to default".
    char const next_char = cursor[0];
    if (next_char == '-') {
        *cursor_ptr = ++cursor;
        return default_color;
    }

    // Require hex representation.
    if (next_char != '#') {
        fprintf(stderr, "Invalid color specified\n");
        return default_color;
    }

    // Skip the first char.
    cursor++;

    char *end;
    errno = 0;
    rgba_t tmp = (rgba_t)(uint32_t)strtoul(cursor, &end, 16);

    if (errno) {
        fprintf(stderr, "Invalid color specified\n");
        return default_color;
    }

    int string_len = end - cursor;
    *cursor_ptr = cursor;

    return expand_rgba_t(tmp, string_len, default_color);
}

void set_attribute(const char modifier, const char attribute) {
    int pos = indexof(attribute, "ou");

    if (pos < 0) {
        fprintf(stderr, "Invalid attribute \"%c\" found\n", attribute);
        return;
    }

    switch (modifier) {
    case '+':
        attrs |= (1u << pos);
        break;
    case '-':
        attrs &=~(1u << pos);
        break;
    case '!':
        attrs ^= (1u << pos);
        break;
    }
}

void swap_colors() {
    rgba_t tmp = fgc;
    fgc = bgc;
    bgc = tmp;
    update_gc();
}


area_t* area_get(xcb_window_t win, const int btn, const int x) {
    // Looping backwards ensures that we get the innermost area first
    for (int i = area_stack.at - 1; i >= 0; i--) {
        area_t *a = &area_stack.area[i];
        if (a->window == win && a->button == btn && x >= a->begin && x < a->end)
            return a;
    }
    return NULL;
}

void area_shift(xcb_window_t win, const int align, int delta) {
    if (align == ALIGN_L)
        return;
    if (align == ALIGN_C)
        delta /= 2;

    for (int i = 0; i < area_stack.at; i++) {
        area_t *a = &area_stack.area[i];
        if (a->window == win && a->align == align && !a->active) {
            a->begin -= delta;
            a->end -= delta;
        }
    }
}

bool
area_add (char *str, const char *optend, char **end, monitor_t *mon, const int x, const int align, const int button)
{
    int i;
    char *trail;
    area_t *a;

    // A wild close area tag appeared!
    if (*str != ':') {
        *end = str;

        // Find most recent unclosed area.
        for (i = area_stack.at - 1; i >= 0 && !area_stack.area[i].active; i--)
            ;
        a = &area_stack.area[i];

        // Basic safety checks
        if (!a->cmd || a->align != align || a->window != mon->window) {
            fprintf(stderr, "Invalid geometry for the clickable area\n");
            return false;
        }

        const int size = x - a->begin;

        switch (align) {
        case ALIGN_L:
            a->end = x;
            break;
        case ALIGN_C:
            a->begin = mon->width / 2 - size / 2 + a->begin / 2;
            a->end = a->begin + size;
            break;
        case ALIGN_R:
            // The newest is the rightmost one
            a->begin = mon->width - size;
            a->end = mon->width;
            break;
        }

        a->active = false;
        return true;
    }

    if (area_stack.at + 1 > area_stack.max) {
        fprintf(stderr, "Cannot add any more clickable areas (used %d/%d)\n",
                area_stack.at, area_stack.max);
        return false;
    }
    a = &area_stack.area[area_stack.at++];

    // Found the closing : and check if it's just an escaped one
    for (trail = strchr(++str, ':'); trail && trail[-1] == '\\'; trail = strchr(trail + 1, ':'))
        ;

    // Find the trailing : and make sure it's within the formatting block, also reject empty commands
    if (!trail || str == trail || trail > optend) {
        *end = str;
        return false;
    }

    *trail = '\0';

    // Sanitize the user command by unescaping all the :
    for (char *needle = str; *needle; needle++) {
        int delta = trail - &needle[1];
        if (needle[0] == '\\' && needle[1] == ':') {
            memmove(&needle[0], &needle[1], delta);
            needle[delta] = 0;
        }
    }

    // This is a pointer to the string buffer allocated in the main
    a->cmd = str;
    a->active = true;
    a->align = align;
    a->begin = x;
    a->window = mon->window;
    a->button = button;

    *end = trail + 1;

    return true;
}

bool font_has_glyph(font_t *font, const uint16_t c) {
    return XftCharExists(dpy, font->xft_ft, (FcChar32) c) != 0;
}

font_t* select_drawable_font(draw_context_t* context, const uint16_t c) {
    // If the user has specified a font to use, try that first.
    int font_index = context->preferred_font_index;
    if (font_index != -1 && font_has_glyph(font_list[font_index], c)) {
        offset_y_index = font_index;
        return font_list[font_index];
    }

    // If the end is reached without finding an appropriate font, return NULL.
    // If the font can draw the character, return it.
    for (int i = 0; i < font_count; i++) {
        if (font_has_glyph(font_list[i], c)) {
            offset_y_index = i;
            return font_list[i];
        }
    }

    return NULL;
}

bool is_end_of_input(char* cursor_ptr) {
    return *cursor_ptr == '\0' || *cursor_ptr == '\n';
}

/**
 * Convert the next (presumably) UTF-8 symbol to UCS-2.
 */
uint16_t parse_ucs2_char(char** cursor_ptr) {
    uint8_t* utf = (uint8_t*)*cursor_ptr;

    // ASCII.
    if (utf[0] < 0x80) {
        *(cursor_ptr) += 1;
        return utf[0];
    }
    // Two byte utf8 sequence.
    if ((utf[0] & 0xe0) == 0xc0) {
        *(cursor_ptr) += 2;
        return (utf[0] & 0x1f) << 6 | (utf[1] & 0x3f);
    }
    // Three byte utf8 sequence.
    if ((utf[0] & 0xf0) == 0xe0) {
        *(cursor_ptr) += 3;
        return (utf[0] & 0xf) << 12 | (utf[1] & 0x3f) << 6 | (utf[2] & 0x3f);
    }
    // Four byte utf8 sequence.
    if ((utf[0] & 0xf8) == 0xf0) {
        *(cursor_ptr) += 4;
        return 0xfffd;
    }
    // Five byte utf8 sequence.
    if ((utf[0] & 0xfc) == 0xf8) {
        *(cursor_ptr) += 5;
        return 0xfffd;
    }
    // Six byte utf8 sequence.
    if ((utf[0] & 0xfe) == 0xfc) {
        *(cursor_ptr) += 6;
        return 0xfffd;
    }

    // Not a valid utf-8 sequence.
    *(cursor_ptr) += 1;
    return utf[0];
}

void clear_on_all_monitors() {
    for (monitor_t* m = monhead; m != NULL; m = m->next) {
        fill_rect(m->pixmap, gc[GC_CLEAR], 0, 0, m->width, bh);
    }
}

void write_text(draw_context_t* context, uint16_t* string, int num_chars) {
    int* section_width = context->section_widths + context->align;
    int string_width = xft_string_width(string, num_chars, context->font);

    // Shift visuals and clickable areas.
    int cursor_x = shift(context->monitor, *section_width, context->align, string_width);
    area_shift(context->monitor->window, context->align, string_width);

    // Draw text and lines.
    draw_string(context->monitor, context->font, context->xft_draw, cursor_x, string, num_chars);
    draw_lines(context->monitor, cursor_x, string_width);

    *section_width += string_width;
}

void add_button(draw_context_t* context, char** cursor_ptr, char* block_end) {
    char c = *((*cursor_ptr)++);
    int button = XCB_BUTTON_INDEX_1;

    // The range is 1-5
    if (isdigit(c) && (c > '0' && c < '6')) {
        button = c - '0';
    } else {
        return;
    }

    char* asd = NULL;
    if (!area_add(*cursor_ptr, block_end, &asd, context->monitor, context->section_widths[context->align], context->align, button)) {
        fprintf(stderr, "Failed to add area\n");
    }

    *cursor_ptr = block_end;
}

void select_next_monitor(draw_context_t* context) {
    context->monitor = context->monitor->next;
}

void select_prev_monitor(draw_context_t* context) {
    context->monitor = context->monitor->prev;
}

void reset_section_widths(draw_context_t* context) {
    context->section_widths[0] = context->section_widths[1] = context->section_widths[2] = 0;
}

void reinit_xft_draw(draw_context_t* context) {
    if (context->xft_draw != NULL) {
        XftDrawDestroy(context->xft_draw);
    }
    context->xft_draw = XftDrawCreate(dpy, context->monitor->pixmap, visual_ptr, colormap);
    if (context->xft_draw == NULL) {
        fprintf(stderr, "Couldn't create xft drawable\n");
    }
}

void select_monitor(draw_context_t* context, char** cursor) {
    char c = *((*cursor)++);
    if (c == '+' && context->monitor->next) {
        select_next_monitor(context);
    } else if (c == '-' && context->monitor->prev) {
        select_prev_monitor(context);
    } else if (c == 'f') {
        context->monitor = monhead;
    } else if (c == 'l') {
        context->monitor = montail ? montail : monhead;
    } else if (isdigit(c)) {
        context->monitor = monhead;
        for (int i = 0; i != c - '0' && context->monitor->next; i++) {
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
    printf("Flushing buffer %d\n", *string_buffer_offset);
    if (*string_buffer_offset == 0) {
        printf("Buffer empty\n");
        return;
    }
    write_text(context, string_buffer, *string_buffer_offset);
    *string_buffer_offset = 0;
    printf("Done\n");
}

void offset_by_pixels(draw_context_t* context, char** cursor_ptr) {
    errno = 0;
    int w = (int) strtoul(*cursor_ptr, cursor_ptr, 10);
    if (errno) {
      fprintf(stderr, "Invalid amount of pixels `%.5s`\n", *cursor_ptr);
      return;
    }

    int* section_width = context->section_widths + context->align;

    // Shift visuals and clickable areas.
    int cursor_x = shift(context->monitor, *section_width, context->align, w);
    area_shift(context->monitor->window, context->align, w);

    // Draw no text but lines.
    draw_lines(context->monitor, cursor_x, w);

    *section_width += w;
}

void set_preferred_font_index(draw_context_t* context, char** cursor_ptr) {
    char c = **cursor_ptr;
    if (c == '-') { //Reset to automatic font selection
        context->preferred_font_index = -1;
    } else if (isdigit(c)) {
        // User-specified `font_index` ∊ [0,font_count)
        context->preferred_font_index = c - '0' - 1;

        // Otherwise just fallback to the automatic font selection.
        if (!context->preferred_font_index || context->preferred_font_index >= font_count) {
            context->preferred_font_index = -1;
        }
    } else {
        fprintf(stderr, "Invalid font slot `%c`\n", c);
    }
    (*cursor_ptr)++;
}

void parse_command(draw_context_t* context, char* cursor, char* command_end) {
    switch (*cursor++) {
    // Set/unset/toggle a SINGLE attribute.
    case '+': set_attribute('+', *cursor); break;
    case '-': set_attribute('-', *cursor); break;
    case '!': set_attribute('!', *cursor); break;

    // Swap fg/bg colors.
    case 'R': swap_colors(); break;

    // Select alignment.
    case 'l': context->align = ALIGN_L; break;
    case 'c': context->align = ALIGN_C; break;
    case 'r': context->align = ALIGN_R; break;

    case 'A': add_button(&context, cursor, command_end); break;

    // Set a color.
    case 'B': bgc = parse_color(cursor, dbgc); update_gc(); break;
    case 'F': fgc = parse_color(cursor, dfgc); update_gc(); break;
    case 'U': ugc = parse_color(cursor, dugc); update_gc(); break;

    case 'S': select_monitor(&context, cursor); break;
    case 'O': offset_by_pixels(&context, cursor); break;
    case 'T': set_preferred_font_index(&context, cursor); break;

    default:
        break;
}

void parse(char* cursor) {
    char *block_end;

    // Reset the stack position
    area_stack.at = 0;

    uint16_t string_buffer[2048];
    int string_buffer_offset = 0;

    draw_context_t context;
    context.monitor = monhead;
    context.xft_draw = NULL;
    context.font = NULL;
    context.align = ALIGN_L;
    reset_section_widths(&context);

    clear_on_all_monitors();
    reinit_xft_draw(&context);

    for (;;) {
        printf("Next char %.1s\n", cursor);
        // If input ends then stop.
        if (is_end_of_input(cursor)) {
            printf("it's over\n");
            break;
        }

        // In case it's a command (Button commands MAY NOT include closing curly braces!).
        if (cursor[0] == '%' && cursor[1] == '{' && (block_end = strchr(cursor, '}'))) {
            flush_buffer(&context, string_buffer, &string_buffer_offset);

            // Parse a command starting at the first letter in the curly braces.
            parse_command(&context, cursor + 2, block_end) {

            // Skip to the end of the command and eat the trailing `}`.
            cursor = block_end + 1;
            continue;
        }

        uint16_t ucs = parse_ucs2_char(&cursor);
        font_t* available_font = select_drawable_font(&context, ucs);
        if (available_font == NULL) {
            // Skip this character if it cannot be rendered.
            continue;
        }

        // If the font doesn't match, then flush the buffer first.
        if (available_font != context.font) {
            flush_buffer(&context, string_buffer, &string_buffer_offset);
            context.font = available_font;
        }

        string_buffer[string_buffer_offset] = ucs;
        string_buffer_offset++;
    }

    printf("Final flush\n");
    flush_buffer(&context, string_buffer, &string_buffer_offset);
    XftDrawDestroy(context.xft_draw);
}

void font_load(const char *pattern) {
    if (font_count >= MAX_FONT_COUNT) {
        fprintf(stderr, "Max font count reached. Could not load font \"%s\"\n", pattern);
        return;
    }

    font_t *new_font = calloc(1, sizeof(font_t));
    if (!new_font) {
        fprintf(stderr, "Failed to allocate memory\n");
        return;
    }

    new_font->xft_ft = XftFontOpenName(dpy, scr_nbr, pattern);
    if (!new_font->xft_ft) {
        fprintf(stderr, "Could not load font %s\n", pattern);
        free(new_font);
        return;
    }

    new_font->ascent = new_font->xft_ft->ascent;
    new_font->descent = new_font->xft_ft->descent;
    new_font->height = new_font->ascent + new_font->descent;

    font_list[font_count++] = new_font;
}

void add_y_offset(int offset) {
    if (offset_y_count >= MAX_FONT_COUNT) {
        fprintf(stderr, "Max offset count reached. Could not set offset \"%d\"\n", offset);
        return;
    }

    offsets_y[offset_y_count] = strtol(optarg, NULL, 10);
    if (offset_y_count == 0) {
        for (int i = 1; i < MAX_FONT_COUNT; ++i) {
            offsets_y[i] = offsets_y[0];
        }
    }
    ++offset_y_count;
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
    const char *atom_names[] = {
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
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];
    xcb_intern_atom_reply_t *atom_reply;

    // As suggested fetch all the cookies first (yum!) and then retrieve the
    // atoms to exploit the async'ness
    for (int i = 0; i < atoms; i++)
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);

    for (int i = 0; i < atoms; i++) {
        atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if (!atom_reply)
            return;
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    // Prepare the strut array
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        int strut[12] = {0};
        if (topbar) {
            strut[2] = bh;
            strut[8] = mon->x;
            strut[9] = mon->x + mon->width;
        } else {
            strut[3]  = bh;
            strut[10] = mon->x;
            strut[11] = mon->x + mon->width;
        }

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
        xcb_change_property(c, XCB_PROP_MODE_APPEND,  mon->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []) {
            0u - 1u
        } );
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 3, "bar");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 12, "lemonbar\0Bar");
    }
}

monitor_t* monitor_new(int x, int y, int width, int height) {
    monitor_t* monitor = calloc(1, sizeof(monitor_t));
    if (!monitor) {
        fprintf(stderr, "Failed to allocate new monitor\n");
        exit(EXIT_FAILURE);
    }

    monitor->x = x;
    monitor->y = (topbar ? by : height - bh - by) + y;
    monitor->width = width;
    monitor->next = monitor->prev = NULL;
    monitor->window = xcb_generate_id(c);
    int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t value_list[] = { bgc.v, bgc.v, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap };
    xcb_create_window(c, depth, monitor->window, scr->root, monitor->x, monitor->y, width, bh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, (void*)value_list);

    monitor->pixmap = xcb_generate_id(c);
    xcb_create_pixmap(c, depth, monitor->pixmap, monitor->window, width, bh);

    return monitor;
}

void monitor_add(monitor_t *mon) {
    if (!monhead) {
        monhead = mon;
    } else if (!montail) {
        montail = mon;
        monhead->next = mon;
        mon->prev = monhead;
    } else {
        mon->prev = montail;
        montail->next = mon;
        montail = montail->next;
    }
}

int rect_sort_cb(const void *p1, const void *p2) {
    const xcb_rectangle_t *r1 = (xcb_rectangle_t *)p1;
    const xcb_rectangle_t *r2 = (xcb_rectangle_t *)p2;

    if (r1->x < r2->x || r1->y + r1->height <= r2->y) {
        return -1;
    }

    if (r1->x > r2->x || r1->y + r1->height > r2->y) {
        return 1;
    }

    return 0;
}

void
monitor_create_chain (xcb_rectangle_t *rects, const int num)
{
    int i;
    int width = 0, height = 0;
    int left = bx;

    // Sort before use
    qsort(rects, num, sizeof(xcb_rectangle_t), rect_sort_cb);

    for (i = 0; i < num; i++) {
        int h = rects[i].y + rects[i].height;
        // Accumulated width of all monitors
        width += rects[i].width;
        // Get height of screen from y_offset + height of lowest monitor
        if (h >= height)
            height = h;
    }

    if (bw < 0)
        bw = width - bx;

    // Use the first font height as all the font heights have been set to the biggest of the set
    if (bh < 0 || bh > height)
        bh = font_list[0]->height + bu + 2;

    // Check the geometry
    if (bx + bw > width || by + bh > height) {
        fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
        exit(EXIT_FAILURE);
    }

    // Left is a positive number or zero therefore monitors with zero width are excluded
    width = bw;
    for (i = 0; i < num; i++) {
        if (rects[i].y + rects[i].height < by)
            continue;
        if (rects[i].width > left) {
            monitor_t *mon = monitor_new(
                                 rects[i].x + left,
                                 rects[i].y,
                                 min(width, rects[i].width - left),
                                 rects[i].height);

            if (!mon)
                break;

            monitor_add(mon);

            width -= rects[i].width - left;
            // No need to check for other monitors
            if (width <= 0)
                break;
        }

        left -= rects[i].width;

        if (left < 0)
            left = 0;
    }
}

void
get_randr_monitors (void)
{
    xcb_randr_get_screen_resources_current_reply_t *rres_reply;
    xcb_randr_output_t *outputs;
    int i, j, num, valid = 0;

    rres_reply = xcb_randr_get_screen_resources_current_reply(c,
                 xcb_randr_get_screen_resources_current(c, scr->root), NULL);

    if (!rres_reply) {
        fprintf(stderr, "Failed to get current randr screen resources\n");
        return;
    }

    num = xcb_randr_get_screen_resources_current_outputs_length(rres_reply);
    outputs = xcb_randr_get_screen_resources_current_outputs(rres_reply);


    // There should be at least one output
    if (num < 1) {
        free(rres_reply);
        return;
    }

    xcb_rectangle_t rects[num];

    // Get all outputs
    for (i = 0; i < num; i++) {
        xcb_randr_get_output_info_reply_t *oi_reply;
        xcb_randr_get_crtc_info_reply_t *ci_reply;

        oi_reply = xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME), NULL);

        // Output disconnected or not attached to any CRTC ?
        if (!oi_reply || oi_reply->crtc == XCB_NONE || oi_reply->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oi_reply);
            rects[i].width = 0;
            continue;
        }

        ci_reply = xcb_randr_get_crtc_info_reply(c,
                   xcb_randr_get_crtc_info(c, oi_reply->crtc, XCB_CURRENT_TIME), NULL);

        free(oi_reply);

        if (!ci_reply) {
            fprintf(stderr, "Failed to get RandR ctrc info\n");
            free(rres_reply);
            return;
        }

        // There's no need to handle rotated screens here (see #69)
        rects[i] = (xcb_rectangle_t){ ci_reply->x, ci_reply->y, ci_reply->width, ci_reply->height };

        free(ci_reply);

        valid++;
    }

    free(rres_reply);

    // Check for clones and inactive outputs
    for (i = 0; i < num; i++) {
        if (rects[i].width == 0)
            continue;

        for (j = 0; j < num; j++) {
            // Does I contain J ?

            if (i != j && rects[j].width) {
                if (rects[j].x >= rects[i].x && rects[j].x + rects[j].width <= rects[i].x + rects[i].width &&
                        rects[j].y >= rects[i].y && rects[j].y + rects[j].height <= rects[i].y + rects[i].height) {
                    rects[j].width = 0;
                    valid--;
                }
            }
        }
    }

    if (valid < 1) {
        fprintf(stderr, "No usable RandR output found\n");
        return;
    }

    xcb_rectangle_t r[valid];

    for (i = j = 0; i < num && j < valid; i++)
        if (rects[i].width != 0)
            r[j++] = rects[i];

    monitor_create_chain(r, valid);
}

xcb_visualid_t
get_visual (void)
{

    XVisualInfo xv;
    xv.depth = 32;
    int result = 0;
    XVisualInfo* result_ptr = NULL;
    result_ptr = XGetVisualInfo(dpy, VisualDepthMask, &xv, &result);

    if (result > 0) {
        visual_ptr = result_ptr->visual;
        return result_ptr->visualid;
    }

    //Fallback
    visual_ptr = DefaultVisual(dpy, scr_nbr);
    return scr->root_visual;
}

// Parse an X-styled geometry string, we don't support signed offsets though.
bool
parse_geometry_string (char *str, int *tmp)
{
    char *p = str;
    int i = 0, j;

    if (!str || !str[0])
        return false;

    // The leading = is optional
    if (*p == '=')
        p++;

    while (*p) {
        // A geometry string has only 4 fields
        if (i >= 4) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Move on if we encounter a 'x' or '+'
        if (*p == 'x') {
            if (i > 0) // The 'x' must precede '+'
                break;
            i++; p++; continue;
        }
        if (*p == '+') {
            if (i < 1) // Stray '+', skip the first two fields
                i = 2;
            else
                i++;
            p++; continue;
        }
        // A digit must follow
        if (!isdigit(*p)) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Try to parse the number
        errno = 0;
        j = strtoul(p, &p, 10);
        if (errno) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        tmp[i] = j;
    }

    return true;
}

void
xconn (void)
{
    if ((dpy = XOpenDisplay(0)) == NULL) {
        fprintf (stderr, "Couldnt open display\n");
    }

    if ((c = XGetXCBConnection(dpy)) == NULL) {
        fprintf (stderr, "Couldnt connect to X\n");
        exit (EXIT_FAILURE);
    }

    XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    /* Grab infos from the first screen */
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    /* Try to get a RGBA visual and build the colormap for that */
    visual = get_visual();
    colormap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, scr->root, visual);
}

void init (char *wm_name, char *wm_instance) {
    if (!font_count) {
        fprintf(stderr, "No font specified. Not using default\n");
        exit(EXIT_FAILURE);
    }

    // To make the alignment uniform, find maximum height.
    int maxh = font_list[0]->height;
    for (int i = 1; i < font_count; i++) {
        maxh = max(maxh, font_list[i]->height);
    }

    // Set maximum height to all fonts.
    for (int i = 0; i < font_count; i++)
        font_list[i]->height = maxh;

    // Generate a list of screens
    const xcb_query_extension_reply_t *qe_reply;

    // Initialize monitor list head and tail
    monhead = montail = NULL;

    // Check if RandR is present
    qe_reply = xcb_get_extension_data(c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        get_randr_monitors();
    }

    if (!monhead) {
        // If I fits I sits
        if (bw < 0)
            bw = scr->width_in_pixels - bx;

        // Adjust the height
        if (bh < 0 || bh > scr->height_in_pixels)
            bh = maxh + bu + 2;

        // Check the geometry
        if (bx + bw > scr->width_in_pixels || by + bh > scr->height_in_pixels) {
            fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
            exit(EXIT_FAILURE);
        }

        // If no RandR outputs or Xinerama screens, fall back to using whole screen
        monhead = monitor_new(0, 0, bw, scr->height_in_pixels);
    }

    if (!monhead)
        exit(EXIT_FAILURE);

    // For WM that support EWMH atoms
    set_ewmh_atoms();

    // Create the gc for drawing
    gc[GC_DRAW] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_DRAW], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });

    gc[GC_CLEAR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_CLEAR], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });

    gc[GC_ATTR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_ATTR], monhead->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });

    // Make the bar visible and clear the pixmap
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        fill_rect(mon->pixmap, gc[GC_CLEAR], 0, 0, mon->width, bh);
        xcb_map_window(c, mon->window);

        // Make sure that the window really gets in the place it's supposed to be
        // Some WM such as Openbox need this
        xcb_configure_window(c, mon->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, (const uint32_t []){ mon->x, mon->y });

        // Set the WM_NAME atom to the user specified value
        if (wm_name)
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8 ,strlen(wm_name), wm_name);

        // set the WM_CLASS atom instance to the executable name
        if (wm_instance) {
            char *wm_class;
            int wm_class_offset, wm_class_len;

            // WM_CLASS is nullbyte seperated: wm_instance + "\0Bar\0"
            wm_class_offset = strlen(wm_instance) + 1;
            wm_class_len = wm_class_offset + 4;

            wm_class = calloc(1, wm_class_len + 1);
            strcpy(wm_class, wm_instance);
            strcpy(wm_class+wm_class_offset, "Bar");

            xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, wm_class_len, wm_class);

            free(wm_class);
        }
    }

    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);

    if (!XftColorAllocName (dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }
    xcb_flush(c);
}

void
cleanup (void)
{
    free(area_stack.area);
    for (int i = 0; font_list[i]; i++) {
        XftFontClose (dpy, font_list[i]->xft_ft);
        free(font_list[i]);
    }

    while (monhead) {
        monitor_t *next = monhead->next;
        xcb_destroy_window(c, monhead->window);
        xcb_free_pixmap(c, monhead->pixmap);
        free(monhead);
        monhead = next;
    }

    XftColorFree(dpy, visual_ptr, colormap, &sel_fg);

    if (gc[GC_DRAW])
        xcb_free_gc(c, gc[GC_DRAW]);
    if (gc[GC_CLEAR])
        xcb_free_gc(c, gc[GC_CLEAR]);
    if (gc[GC_ATTR])
        xcb_free_gc(c, gc[GC_ATTR]);
    if (c)
        xcb_disconnect(c);
}

char* strip_path(char *path) {
    char *slash;

    if (path == NULL || *path == '\0') {
        return strdup("lemonbar");
    }

    slash = strrchr(path, '/');
    if (slash != NULL) {
        return strndup(slash + 1, 31);
    }

    return strndup(path, 31);
}

void sighandle(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        exit(EXIT_SUCCESS);
    }
}


int main(int argc, char **argv) {
    struct pollfd pollin[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = -1          , .events = POLLIN },
    };
    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;
    xcb_button_press_event_t *press_ev;
    char input[4096] = {0, };
    bool permanent = false;
    int geom_v[4] = { -1, -1, 0, 0 };
    int ch, areas;
    char *wm_name;
    char *instance_name;

    // Install the parachute!
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    // B/W combo
    dbgc = bgc = (rgba_t)0x00000000U;
    dfgc = fgc = (rgba_t)0xffffffffU;

    dugc = ugc = fgc;

    // A safe default
    areas = 10;
    wm_name = NULL;

    instance_name = strip_path(argv[0]);

    // Connect to the Xserver and initialize scr
    xconn();

    while ((ch = getopt(argc, argv, "hg:bdf:a:pu:B:F:U:n:o:")) != -1) {
        char* optarg_cpy = optarg;
        switch (ch) {
        case 'h':
            printf ("lemonbar version %s patched with XFT support\n", VERSION);
            printf ("usage: %s [-h | -g | -b | -d | -f | -a | -p | -n | -u | -B | -F]\n"
                    "\t-h Show this help\n"
                    "\t-g Set the bar geometry {width}x{height}+{xoffset}+{yoffset}\n"
                    "\t-b Put the bar at the bottom of the screen\n"
                    "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                    "\t-f Set the font name to use\n"
                    "\t-a Number of clickable areas available (default is 10)\n"
                    "\t-p Don't close after the data ends\n"
                    "\t-n Set the WM_NAME atom to the specified value for this bar\n"
                    "\t-u Set the underline/overline height in pixels\n"
                    "\t-B Set background color in #AARRGGBB\n"
                    "\t-F Set foreground color in #AARRGGBB\n"
                    "\t-o Add a vertical offset to the text, it can be negative\n", argv[0]);
            exit (EXIT_SUCCESS);
        case 'g': (void)parse_geometry_string(optarg, geom_v); break;
        case 'p': permanent = true; break;
        case 'n': wm_name = strdup(optarg); break;
        case 'b': topbar = false; break;
        case 'd': dock = true; break;
        case 'f': font_load(optarg); break;
        case 'u': bu = strtoul(optarg, NULL, 10); break;
        case 'o': add_y_offset(strtol(optarg, NULL, 10)); break;
        case 'B': dbgc = bgc = parse_color(&optarg_cpy, (rgba_t)0x00000000U); break;
        case 'F': dfgc = fgc = parse_color(&optarg_cpy, (rgba_t)0xffffffffU); break;
        case 'U': dugc = ugc = parse_color(&optarg_cpy, fgc); break;
        case 'a': areas = strtoul(optarg, NULL, 10); break;
        }
    }

    // Initialize the stack holding the clickable areas
    area_stack.at = 0;
    area_stack.max = areas;
    if (areas) {
        area_stack.area = calloc(areas, sizeof(area_t));

        if (!area_stack.area) {
            fprintf(stderr, "Could not allocate enough memory for %d clickable areas, try lowering the number\n", areas);
            return EXIT_FAILURE;
        }
    }
    else
        area_stack.area = NULL;


    // Copy the geometry values in place
    bw = geom_v[0];
    bh = geom_v[1];
    bx = geom_v[2];
    by = geom_v[3];

    // Do the heavy lifting
    init(wm_name, instance_name);
    // The string is strdup'd when the command line arguments are parsed
    free(wm_name);
    // The string is strdup'd when stripping argv[0]
    free(instance_name);
    // Get the fd to Xserver
    pollin[1].fd = xcb_get_file_descriptor(c);

    // Prevent fgets to block
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    for (;;) {
        bool redraw = false;

        // If connection is in error state, then it has been shut down.
        if (xcb_connection_has_error(c))
            break;

        if (poll(pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      // No more data...
                if (permanent) pollin[0].fd = -1;   // ...null the fd and continue polling :D
                else break;                         // ...bail out
            }
            if (pollin[0].revents & POLLIN) { // New input, process it
                input[0] = '\0';
                while (fgets(input, sizeof(input), stdin) != NULL)
                    ; // Drain the buffer, the last line is actually used
                parse(input);
                redraw = true;
            }
            if (pollin[1].revents & POLLIN) { // The event comes from the Xorg server
                while ((ev = xcb_poll_for_event(c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                    case XCB_EXPOSE:
                        if (expose_ev->count == 0)
                            redraw = true;
                        break;
                    case XCB_BUTTON_PRESS:
                        press_ev = (xcb_button_press_event_t *)ev;
                        {
                            area_t *area = area_get(press_ev->event, press_ev->detail, press_ev->event_x);
                            // Respond to the click
                            if (area) {
                                (void)write(STDOUT_FILENO, area->cmd, strlen(area->cmd));
                                (void)write(STDOUT_FILENO, "\n", 1);
                            }
                        }
                    break;
                    }

                    free(ev);
                }
            }
        }

        if (redraw) { // Copy our temporary pixmap onto the window
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                xcb_copy_area(c, mon->pixmap, mon->window, gc[GC_DRAW], 0, 0, 0, 0, mon->width, bh);
            }
        }

        xcb_flush(c);
    }

    return EXIT_SUCCESS;
}
