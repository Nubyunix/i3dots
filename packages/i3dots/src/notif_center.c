#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <dbus/dbus.h>

#define MAX_CARDS 1024
#define LOCK_FILE "/tmp/notif_center.lock"
#define CLOSE_STAMP_FILE "/tmp/notif_center.closed"
#define DEBOUNCE_MS 60
#define CARD_SPACING 8

static void send_dbus_invoke_action(int id, const char *action_key) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) return;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "InvokeAction"
    );
    if (msg) {
        dbus_uint32_t uid = (dbus_uint32_t)id;
        const char *act = action_key ? action_key : "default";
        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &uid);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &act);

        dbus_connection_send(conn, msg, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
}

static void send_dbus_close_notification(int id) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) return;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "DeleteNotification"
    );
    if (msg) {
        dbus_uint32_t uid = (dbus_uint32_t)id;
        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &uid);

        dbus_connection_send(conn, msg, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
}

static void send_dbus_close_all(void) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) return;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "CloseAll"
    );
    if (msg) {
        dbus_connection_send(conn, msg, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
}

static bool send_dbus_toggle_dnd(void) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) return false;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "ToggleDND"
    );
    bool new_dnd = false;
    if (msg) {
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
        if (reply) {
            dbus_bool_t val = 0;
            if (dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID)) {
                new_dnd = val ? true : false;
            }
            dbus_message_unref(reply);
        }
        dbus_message_unref(msg);
    }
    dbus_connection_unref(conn);
    return new_dnd;
}

static bool check_dnd_status(void) {
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/i3dots/core/state/i3dots/notifications/dnd.state", home ? home : "/home/dereck");
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[16] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            return (strncmp(buf, "true", 4) == 0 || buf[0] == '1');
        }
        fclose(f);
    }
    return false;
}

typedef struct {
    double r, g, b, a;
} Color;

typedef struct {
    Color bg_panel;
    Color bg_card;
    Color bg_card_hover;
    Color bg_card_urgent;
    Color fg_text;
    Color fg_muted;
    Color accent;
    Color accent_dim;
    Color alert;
    Color border;
    Color surface;
} Theme;

typedef struct {
    int panel_w;
    int margin_right;
    int margin_gap;
    int margin_top;
    int margin_bottom;
    bool auto_adjust_bar;
    double panel_radius;
    double card_radius;
    double button_radius;
    int border_width;
    char icon_position[16];
    bool show_quick_actions;
    bool show_images;
    int image_size;
    int max_entries;
    char font_title[128];
    char font_bold[128];
    char font_normal[128];
    char font_small[128];
} AppConfig;

typedef struct {
    int id;
    char appname[64];
    char summary[128];
    char body[512];
    char clean_body[512];
    char icon_path[256];
    char urls[512];
    char avatar[64];
    int64_t timestamp_us;
    int64_t elapsed_sec;
    int urgency; /* 0: LOW, 1: NORMAL, 2: CRITICAL */
    int is_today;
    int is_yesterday;
    int is_older;

    /* Metrics for layout and interaction */
    int card_y;
    int card_h;
    int close_x, close_y, close_w, close_h;
    int icon_x, icon_y, icon_w, icon_h;
    bool is_hovered;
    bool close_hovered;
    cairo_surface_t *icon_surface;
    cairo_surface_t *badge_surface;
    bool show_app_badge;
    char badge_letter;
} NotifCard;

typedef enum {
    FILTER_ALL = 0,
    FILTER_TODAY = 1,
    FILTER_YESTERDAY = 2,
    FILTER_OLDER = 3,
    FILTER_COUNT = 4
} FilterTab;

typedef struct {
    PangoFontDescription *f_title;
    PangoFontDescription *f_bold10;
    PangoFontDescription *f_norm9;
    PangoFontDescription *f_bold9;
    PangoFontDescription *f_norm8;
    PangoLayout *layout_title;
    PangoLayout *layout_sum;
    PangoLayout *layout_body;
    PangoLayout *layout_meta;
    PangoLayout *layout_tab;
    PangoLayout *layout_btn;
    PangoLayout *layout_tile_t;
    PangoLayout *layout_tile_s;
    PangoLayout *layout_foot;
    PangoLayout *layout_empty;
} PangoCache;

typedef struct {
    Display *dpy;
    int screen;
    Window root;
    Window win;
    cairo_surface_t *cairo_surface;
    cairo_surface_t *buffer_surface;
    cairo_t *cr;

    int screen_w;
    int screen_h;
    int panel_x;
    int panel_y;
    int panel_w;
    int panel_h;

    AppConfig cfg;
    Theme theme;
    PangoCache pango;

    NotifCard cards[MAX_CARDS];
    int card_count;
    int filtered_count;

    FilterTab active_filter;
    int scroll_offset;
    int max_scroll;

    bool is_dnd;
    int dnd_btn_x, dnd_btn_y, dnd_btn_w, dnd_btn_h;
    bool dnd_btn_hovered;

    int clear_btn_x, clear_btn_y, clear_btn_w, clear_btn_h;
    bool clear_btn_hovered;

    int filter_rects[FILTER_COUNT][4]; /* x, y, w, h */
    int filter_hovered;

    /* Quick tiles at bottom */
    int tiles_area_y;
    int tiles_area_h;
    int tile_rects[3][4]; /* x, y, w, h */
    int tile_hovered;

    /* Vertical scrollbar */
    bool scrollbar_visible;
    int scrollbar_x, scrollbar_y, scrollbar_w, scrollbar_h;
    int thumb_y, thumb_h;
    bool scrollbar_hovered;
    bool scrollbar_dragging;
    int drag_start_y;
    int drag_start_scroll;

    bool running;
} AppState;

static AppState g_app;
static int g_sig_pipe[2] = {-1, -1};
static int64_t g_last_notif_time = 0;
static int g_self_updating_history = 0;

static Color hex_to_color(const char *hex, double alpha) {
    Color c = {0.0, 0.0, 0.0, alpha};
    if (!hex || hex[0] != '#') return c;
    unsigned int r = 0, g = 0, b = 0;
    if (strlen(hex) == 7) {
        if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            c.r = r / 255.0;
            c.g = g / 255.0;
            c.b = b / 255.0;
        }
    } else if (strlen(hex) == 9) {
        unsigned int a = 255;
        if (sscanf(hex + 1, "%02x%02x%02x%02x", &a, &r, &g, &b) == 4) {
            c.r = r / 255.0;
            c.g = g / 255.0;
            c.b = b / 255.0;
            c.a = a / 255.0;
        }
    }
    return c;
}

static void load_config(AppConfig *cfg) {
    cfg->panel_w = 400;
    cfg->margin_right = 12;
    cfg->margin_gap = 12;
    cfg->margin_top = 42;
    cfg->margin_bottom = 14;
    cfg->auto_adjust_bar = true;
    cfg->panel_radius = 0.0;
    cfg->card_radius = 0.0;
    cfg->button_radius = 0.0;
    cfg->border_width = 1;
    strcpy(cfg->icon_position, "left");
    cfg->show_quick_actions = false;
    cfg->show_images = true;
    cfg->image_size = 48;
    cfg->max_entries = 500;
    snprintf(cfg->font_title, sizeof(cfg->font_title), "JetBrainsMono Nerd Font Bold 12");
    snprintf(cfg->font_bold, sizeof(cfg->font_bold), "JetBrainsMono Nerd Font Bold 10");
    snprintf(cfg->font_normal, sizeof(cfg->font_normal), "JetBrainsMono Nerd Font 9");
    snprintf(cfg->font_small, sizeof(cfg->font_small), "JetBrainsMono Nerd Font 8");

    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/notif_center/config.ini", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '[' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;

        while (isspace((unsigned char)*k)) k++;
        char *kend = k + strlen(k) - 1;
        while (kend > k && isspace((unsigned char)*kend)) *kend-- = '\0';

        while (isspace((unsigned char)*v)) v++;
        char *vend = v + strlen(v) - 1;
        while (vend >= v && (*vend == '\n' || *vend == '\r' || isspace((unsigned char)*vend))) *vend-- = '\0';

        if (strcmp(k, "width") == 0) cfg->panel_w = atoi(v);
        else if (strcmp(k, "margin_right") == 0) cfg->margin_right = atoi(v);
        else if (strcmp(k, "margin_gap") == 0) cfg->margin_gap = atoi(v);
        else if (strcmp(k, "margin_top") == 0) cfg->margin_top = atoi(v);
        else if (strcmp(k, "margin_bottom") == 0) cfg->margin_bottom = atoi(v);
        else if (strcmp(k, "auto_adjust_bar") == 0) {
            cfg->auto_adjust_bar = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
        }
        else if (strcmp(k, "panel_radius") == 0) cfg->panel_radius = atof(v);
        else if (strcmp(k, "card_radius") == 0) cfg->card_radius = atof(v);
        else if (strcmp(k, "button_radius") == 0) cfg->button_radius = atof(v);
        else if (strcmp(k, "border_width") == 0) cfg->border_width = atoi(v);
        else if (strcmp(k, "icon_position") == 0) snprintf(cfg->icon_position, sizeof(cfg->icon_position), "%s", v);
        else if (strcmp(k, "show_quick_actions") == 0) {
            cfg->show_quick_actions = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
        }
        else if (strcmp(k, "show_images") == 0) {
            cfg->show_images = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
        }
        else if (strcmp(k, "image_size") == 0) cfg->image_size = atoi(v);
        else if (strcmp(k, "max_entries") == 0 || strcmp(k, "max_history") == 0) {
            int val = atoi(v);
            if (val >= 50 && val <= 1024) cfg->max_entries = val;
        }
        else if (strcmp(k, "font_title") == 0) snprintf(cfg->font_title, sizeof(cfg->font_title), "%s", v);
        else if (strcmp(k, "font_bold") == 0) snprintf(cfg->font_bold, sizeof(cfg->font_bold), "%s", v);
        else if (strcmp(k, "font_normal") == 0) snprintf(cfg->font_normal, sizeof(cfg->font_normal), "%s", v);
        else if (strcmp(k, "font_small") == 0) snprintf(cfg->font_small, sizeof(cfg->font_small), "%s", v);
    }
    fclose(f);
}

static void load_theme(Theme *th) {
    th->bg_panel = hex_to_color("#181d18", 1.0);
    th->bg_card = hex_to_color("#262b26", 1.0);
    th->bg_card_hover = hex_to_color("#353a35", 1.0);
    th->bg_card_urgent = hex_to_color("#4a282c", 1.0);
    th->fg_text = hex_to_color("#dfe4dc", 1.0);
    th->fg_muted = hex_to_color("#8b938a", 1.0);
    th->accent = hex_to_color("#97d5a5", 1.0);
    th->accent_dim = hex_to_color("#384b3c", 1.0);
    th->alert = hex_to_color("#ffb4ab", 1.0);
    th->border = hex_to_color("#414941", 0.9);
    th->surface = hex_to_color("#101510", 1.0);

    const char *home = getenv("HOME");
    if (!home) return;

    /* 1. Inherit dynamic colors from current palette */
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/polybar/colors.ini", home);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[64];
            if (sscanf(line, "%63s = %63s", key, val) == 2) {
                if (strcmp(key, "background-solid") == 0) th->bg_panel = hex_to_color(val, 1.0);
                else if (strcmp(key, "surface") == 0) th->surface = hex_to_color(val, 1.0);
                else if (strcmp(key, "background-alt") == 0) {
                    th->bg_card = hex_to_color(val, 1.0);
                    th->bg_card_hover = hex_to_color(val, 1.0);
                    th->bg_card_hover.r = fmin(1.0, th->bg_card_hover.r + 0.08);
                    th->bg_card_hover.g = fmin(1.0, th->bg_card_hover.g + 0.08);
                    th->bg_card_hover.b = fmin(1.0, th->bg_card_hover.b + 0.08);
                }
                else if (strcmp(key, "gray0") == 0) th->bg_card_hover = hex_to_color(val, 1.0);
                else if (strcmp(key, "gray1") == 0) th->border = hex_to_color(val, 0.9);
                else if (strcmp(key, "foreground") == 0) th->fg_text = hex_to_color(val, 1.0);
                else if (strcmp(key, "primary") == 0) th->accent = hex_to_color(val, 1.0);
                else if (strcmp(key, "alert") == 0 || strcmp(key, "red") == 0) th->alert = hex_to_color(val, 1.0);
                else if (strcmp(key, "disabled") == 0) th->fg_muted = hex_to_color(val, 1.0);
                else if (strcmp(key, "secondary-container") == 0) th->accent_dim = hex_to_color(val, 1.0);
            }
        }
        fclose(f);
    }

    /* 2. Optional manual color overrides from config */
    snprintf(path, sizeof(path), "%s/.config/notif_center/config.ini", home);
    f = fopen(path, "r");
    if (f) {
        char line[512];
        bool in_colors = false;
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '#' || *p == ';') continue;
            if (*p == '[') {
                in_colors = (strncasecmp(p, "[colors]", 8) == 0);
                continue;
            }
            if (!in_colors) continue;

            char key[64], val[64];
            if (sscanf(p, "%63s = %63s", key, val) == 2 && val[0] == '#') {
                if (strcmp(key, "bg_panel") == 0) th->bg_panel = hex_to_color(val, 1.0);
                else if (strcmp(key, "bg_card") == 0) th->bg_card = hex_to_color(val, 1.0);
                else if (strcmp(key, "bg_card_hover") == 0) th->bg_card_hover = hex_to_color(val, 1.0);
                else if (strcmp(key, "border") == 0) th->border = hex_to_color(val, 0.9);
                else if (strcmp(key, "accent") == 0) th->accent = hex_to_color(val, 1.0);
                else if (strcmp(key, "accent_dim") == 0) th->accent_dim = hex_to_color(val, 1.0);
                else if (strcmp(key, "fg_text") == 0) th->fg_text = hex_to_color(val, 1.0);
                else if (strcmp(key, "fg_muted") == 0) th->fg_muted = hex_to_color(val, 1.0);
            }
        }
        fclose(f);
    }

    /* 3. Ensure visible contrast even in pure pitch black OLED themes */
    double border_lum = 0.299 * th->border.r + 0.587 * th->border.g + 0.114 * th->border.b;
    if (border_lum < 0.08) {
        th->border = hex_to_color("#2d2d2d", 0.9);
    }
    double card_lum = 0.299 * th->bg_card.r + 0.587 * th->bg_card.g + 0.114 * th->bg_card.b;
    if (card_lum < 0.04) {
        th->bg_card = hex_to_color("#141414", 1.0);
        th->bg_card_hover = hex_to_color("#222222", 1.0);
    }
}

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pixbuf) {
    if (!pixbuf) return NULL;
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    unsigned char *cairo_data = cairo_image_surface_get_data(surface);
    int cairo_stride = cairo_image_surface_get_stride(surface);

    cairo_surface_flush(surface);
    for (int y = 0; y < height; y++) {
        const guchar *src = pixels + y * rowstride;
        uint32_t *dst = (uint32_t *)(cairo_data + y * cairo_stride);
        for (int x = 0; x < width; x++) {
            uint8_t r = src[0], g = src[1], b = src[2];
            uint8_t a = (n_channels == 4) ? src[3] : 255;
            uint32_t pr = (r * a + 127) / 255;
            uint32_t pg = (g * a + 127) / 255;
            uint32_t pb = (b * a + 127) / 255;
            dst[x] = ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb;
            src += n_channels;
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

static cairo_surface_t *load_image_surface(const char *path, int max_w, int max_h) {
    if (!path || path[0] == '\0' || access(path, R_OK) != 0) return NULL;
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, max_w, max_h, TRUE, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return NULL;
    }
    cairo_surface_t *surf = pixbuf_to_cairo_surface(pb);
    g_object_unref(pb);
    return surf;
}

#define SURF_CACHE_MAX 128
typedef struct {
    char path[256];
    int max_w;
    int max_h;
    cairo_surface_t *surface;
} SurfaceCacheEntry;

static SurfaceCacheEntry g_surf_cache[SURF_CACHE_MAX];
static int g_surf_cache_count = 0;

static cairo_surface_t *get_cached_image_surface(const char *path, int max_w, int max_h) {
    if (!path || path[0] == '\0' || access(path, R_OK) != 0) return NULL;

    for (int i = 0; i < g_surf_cache_count; i++) {
        if (g_surf_cache[i].max_w == max_w &&
            g_surf_cache[i].max_h == max_h &&
            strcmp(g_surf_cache[i].path, path) == 0) {
            return cairo_surface_reference(g_surf_cache[i].surface);
        }
    }

    cairo_surface_t *s = load_image_surface(path, max_w, max_h);
    if (!s) return NULL;

    if (g_surf_cache_count < SURF_CACHE_MAX) {
        strncpy(g_surf_cache[g_surf_cache_count].path, path, sizeof(g_surf_cache[g_surf_cache_count].path) - 1);
        g_surf_cache[g_surf_cache_count].path[sizeof(g_surf_cache[g_surf_cache_count].path) - 1] = '\0';
        g_surf_cache[g_surf_cache_count].max_w = max_w;
        g_surf_cache[g_surf_cache_count].max_h = max_h;
        g_surf_cache[g_surf_cache_count].surface = s;
        g_surf_cache_count++;
        return cairo_surface_reference(s);
    }

    return s;
}

static void destroy_surface_cache(void) {
    for (int i = 0; i < g_surf_cache_count; i++) {
        if (g_surf_cache[i].surface) {
            cairo_surface_destroy(g_surf_cache[i].surface);
            g_surf_cache[i].surface = NULL;
        }
    }
    g_surf_cache_count = 0;
}

#define ICON_CACHE_MAX 128
typedef struct {
    char name[64];
    char path[256];
} IconCacheEntry;

static IconCacheEntry g_icon_cache[ICON_CACHE_MAX];
static int g_icon_cache_count = 0;

static const char *find_in_icon_cache(const char *name) {
    if (!name || name[0] == '\0') return NULL;
    for (int i = 0; i < g_icon_cache_count; i++) {
        if (strcasecmp(g_icon_cache[i].name, name) == 0) {
            return g_icon_cache[i].path;
        }
    }
    return NULL;
}

static void add_to_icon_cache(const char *name, const char *path) {
    if (!name || name[0] == '\0' || g_icon_cache_count >= ICON_CACHE_MAX) return;
    for (int i = 0; i < g_icon_cache_count; i++) {
        if (strcasecmp(g_icon_cache[i].name, name) == 0) return;
    }
    strncpy(g_icon_cache[g_icon_cache_count].name, name, sizeof(g_icon_cache[g_icon_cache_count].name) - 1);
    g_icon_cache[g_icon_cache_count].name[sizeof(g_icon_cache[g_icon_cache_count].name) - 1] = '\0';
    if (path) {
        strncpy(g_icon_cache[g_icon_cache_count].path, path, sizeof(g_icon_cache[g_icon_cache_count].path) - 1);
        g_icon_cache[g_icon_cache_count].path[sizeof(g_icon_cache[g_icon_cache_count].path) - 1] = '\0';
    } else {
        g_icon_cache[g_icon_cache_count].path[0] = '\0';
    }
    g_icon_cache_count++;
}

static unsigned int simple_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static void extract_image_url(const char *urls, const char *body, char *out_url, size_t out_len) {
    out_url[0] = '\0';
    const char *candidates[2] = { urls, body };

    for (int i = 0; i < 2; i++) {
        const char *src = candidates[i];
        if (!src || src[0] == '\0') continue;

        const char *p = src;
        while ((p = strstr(p, "http")) != NULL) {
            const char *end = p;
            while (*end && !isspace((unsigned char)*end) && *end != ')' && *end != ']' && *end != '"' && *end != '\'') {
                end++;
            }
            size_t ulen = end - p;
            if (ulen > 10 && ulen < out_len) {
                char temp[512];
                strncpy(temp, p, ulen);
                temp[ulen] = '\0';

                if (strstr(temp, ".png") || strstr(temp, ".jpg") || strstr(temp, ".jpeg") ||
                    strstr(temp, ".gif") || strstr(temp, ".webp") || strstr(temp, ".svg") ||
                    strstr(temp, "format=png") || strstr(temp, "format=webp") || strstr(temp, "format=jpg")) {
                    snprintf(out_url, out_len, "%s", temp);
                    return;
                }
            }
            p = end;
        }
    }
}

static bool resolve_cached_url_image(const char *url, char *out_path, size_t out_len) {
    if (!url || url[0] == '\0' || !out_path || out_len < 32) return false;
    const char *home = getenv("HOME");
    if (!home) return false;

    char cache_dir[256];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/notif_center", home);
    mkdir(cache_dir, 0755);

    unsigned int h = simple_hash(url);
    snprintf(out_path, out_len, "%s/%08x.img", cache_dir, h);

    if (access(out_path, R_OK) == 0) {
        return true;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -s -f -m 2 -o '%s' '%s' >/dev/null 2>&1 &", out_path, url);
    system(cmd);
    out_path[0] = '\0';
    return false;
}

static void extract_clean_author(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return;
    const char *paren = strchr(in, '(');
    size_t len = paren ? (size_t)(paren - in) : strlen(in);
    if (len >= out_len) len = out_len - 1;

    /* Copy skipping Unicode direction marks (0xE2 0x81 0xA8 / 0xA9) and trimming */
    size_t j = 0;
    for (size_t i = 0; i < len && j < out_len - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xE2 && i + 2 < len && (unsigned char)in[i+1] == 0x81) {
            i += 2;
            continue;
        }
        out[j++] = in[i];
    }
    out[j] = '\0';

    /* Trim trailing whitespace */
    while (j > 0 && isspace((unsigned char)out[j - 1])) {
        out[--j] = '\0';
    }
    /* Trim leading whitespace */
    char *start = out;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != out) {
        memmove(out, start, strlen(start) + 1);
    }
}

static void get_notif_paths(char *state_dir, size_t dir_len, char *hist_file, size_t hist_len, char *assets_dir, size_t assets_len) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";
    if (state_dir) snprintf(state_dir, dir_len, "%s/.config/i3dots/core/state/i3dots/notifications", home);
    if (hist_file) snprintf(hist_file, hist_len, "%s/.config/i3dots/core/state/i3dots/notifications/history.json", home);
    if (assets_dir) snprintf(assets_dir, assets_len, "%s/.config/i3dots/core/state/i3dots/notifications/assets", home);
}

static bool resolve_cached_avatar(const char *app_name, const char *summary, char *out_path, size_t out_len) {
    if (!out_path || out_len < 32) return false;
    const char *home = getenv("HOME");
    if (!home) return false;

    char dir_assets[256], dir_cache[256];
    snprintf(dir_assets, sizeof(dir_assets), "%s/.config/i3dots/core/state/i3dots/notifications/assets", home);
    snprintf(dir_cache, sizeof(dir_cache), "%s/.cache/notif_center/avatars", home);

    /* 1. Exact match by app_name:summary */
    char exact_key[256];
    snprintf(exact_key, sizeof(exact_key), "%s:%s", app_name ? app_name : "", summary ? summary : "");
    unsigned int h1 = simple_hash(exact_key);

    snprintf(out_path, out_len, "%s/%08x.png", dir_assets, h1);
    if (access(out_path, R_OK) == 0) return true;
    snprintf(out_path, out_len, "%s/%08x.png", dir_cache, h1);
    if (access(out_path, R_OK) == 0) return true;

    /* 2. Clean author match (e.g. Discord sender name extracted from summary) */
    char clean_author[128] = {0};
    extract_clean_author(summary, clean_author, sizeof(clean_author));
    if (clean_author[0] != '\0') {
        char author_key[256];
        snprintf(author_key, sizeof(author_key), "%s:%s", app_name ? app_name : "", clean_author);
        unsigned int h2 = simple_hash(author_key);
        snprintf(out_path, out_len, "%s/%08x.png", dir_assets, h2);
        if (access(out_path, R_OK) == 0) return true;
        snprintf(out_path, out_len, "%s/%08x.png", dir_cache, h2);
        if (access(out_path, R_OK) == 0) return true;
    }

    out_path[0] = '\0';
    return false;
}

static bool is_daemon_running(void) {
    FILE *fp = fopen("/tmp/notif_daemon.pid", "r");
    if (!fp) return false;
    pid_t pid = 0;
    if (fscanf(fp, "%d", &pid) == 1 && pid > 1) {
        fclose(fp);
        if (kill(pid, 0) == 0) return true;
    } else {
        fclose(fp);
    }
    return false;
}

static void ensure_daemon_running(void) {
    if (is_daemon_running()) return;

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp("notif_daemon", "notif_daemon", NULL);
        const char *home = getenv("HOME");
        if (home) {
            char path[512];
            snprintf(path, sizeof(path), "%s/.local/bin/notif_daemon", home);
            execl(path, "notif_daemon", NULL);
        }
        _exit(0);
    }
}

static void clean_notification_body(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return;
    if (in[0] == '\0') {
        out[0] = '\0';
        return;
    }

    const char *open_bracket = strchr(in, '[');
    const char *close_bracket = open_bracket ? strchr(open_bracket, ']') : NULL;
    const char *open_paren = close_bracket ? strchr(close_bracket, '(') : NULL;
    const char *close_paren = open_paren ? strchr(open_paren, ')') : NULL;

    if (open_bracket && close_bracket && open_paren == close_bracket + 1 && close_paren) {
        size_t label_len = close_bracket - (open_bracket + 1);
        if (label_len > 0 && label_len < out_len - 16) {
            if (strstr(open_paren, "stickers") != NULL) {
                snprintf(out, out_len, "[Sticker] %.*s", (int)label_len, open_bracket + 1);
            } else {
                snprintf(out, out_len, "%.*s", (int)label_len, open_bracket + 1);
            }
            return;
        }
    }

    if (in != out) {
        strncpy(out, in, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static void resolve_icon(const char *name, char *out, size_t out_len) {
    if (!name || name[0] == '\0') {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    if (access(name, R_OK) == 0) {
        snprintf(out, out_len, "%s", name);
        return;
    }

    const char *cached = find_in_icon_cache(name);
    if (cached) {
        if (cached[0] != '\0') {
            snprintf(out, out_len, "%s", cached);
        } else if (out && out_len > 0) {
            out[0] = '\0';
        }
        return;
    }

    char low_name[128];
    size_t len = strlen(name);
    if (len >= sizeof(low_name)) len = sizeof(low_name) - 1;
    for (size_t i = 0; i < len; i++) {
        low_name[i] = (char)tolower((unsigned char)name[i]);
    }
    low_name[len] = '\0';

    const char *candidates[] = {
        "/usr/share/pixmaps/%s.png",
        "/usr/share/pixmaps/%s.svg",
        "/usr/share/icons/Papirus/48x48/apps/%s.svg",
        "/usr/share/icons/Papirus/32x32/apps/%s.svg",
        "/usr/share/icons/Papirus/24x24/apps/%s.svg",
        "/usr/share/icons/Papirus/16x16/apps/%s.svg",
        "/usr/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/icons/hicolor/48x48/apps/%s.png",
        "/usr/share/icons/hicolor/32x32/apps/%s.png",
        "/usr/share/icons/hicolor/128x128/apps/%s.png",
        "/usr/share/icons/hicolor/256x256/apps/%s.png",
        NULL
    };

    char buf[512];
    for (int i = 0; candidates[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), candidates[i], low_name);
        if (access(buf, R_OK) == 0) {
            snprintf(out, out_len, "%s", buf);
            add_to_icon_cache(name, buf);
            return;
        }
    }

    add_to_icon_cache(name, "");
    if (out && out_len > 0) out[0] = '\0';
}

static void init_pango_cache(AppState *app) {
    PangoCache *p = &app->pango;
    p->f_title = pango_font_description_from_string(app->cfg.font_title);
    p->f_bold10 = pango_font_description_from_string(app->cfg.font_bold);
    p->f_norm9 = pango_font_description_from_string(app->cfg.font_normal);
    p->f_bold9 = pango_font_description_from_string(app->cfg.font_bold);
    p->f_norm8 = pango_font_description_from_string(app->cfg.font_small);

    p->layout_title = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_title, p->f_title);

    p->layout_sum = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_sum, p->f_bold10);

    p->layout_body = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_body, p->f_norm9);

    p->layout_meta = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_meta, p->f_norm9);

    p->layout_tab = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_tab, p->f_norm9);

    p->layout_btn = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_btn, p->f_bold9);

    p->layout_tile_t = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_tile_t, p->f_bold9);

    p->layout_tile_s = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_tile_s, p->f_norm8);

    p->layout_foot = pango_cairo_create_layout(app->cr);
    pango_layout_set_font_description(p->layout_foot, p->f_norm8);

    p->layout_empty = pango_cairo_create_layout(app->cr);
}

static void destroy_pango_cache(AppState *app) {
    PangoCache *p = &app->pango;
    g_object_unref(p->layout_title);
    g_object_unref(p->layout_sum);
    g_object_unref(p->layout_body);
    g_object_unref(p->layout_meta);
    g_object_unref(p->layout_tab);
    g_object_unref(p->layout_btn);
    g_object_unref(p->layout_tile_t);
    g_object_unref(p->layout_tile_s);
    g_object_unref(p->layout_foot);
    g_object_unref(p->layout_empty);

    pango_font_description_free(p->f_title);
    pango_font_description_free(p->f_bold10);
    pango_font_description_free(p->f_norm9);
    pango_font_description_free(p->f_bold9);
    pango_font_description_free(p->f_norm8);
}

static void cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static int64_t get_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000LL + (ts.tv_nsec / 1000000LL);
    }
    return 0;
}

static void get_current_boot_id(char *out_id, size_t out_len) {
    if (!out_id || out_len == 0) return;
    out_id[0] = '\0';
    FILE *fp = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (fp) {
        if (fgets(out_id, out_len, fp)) {
            char *nl = strchr(out_id, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);
    }
}

static void free_card_surfaces(AppState *app) {
    for (int i = 0; i < app->card_count; i++) {
        if (app->cards[i].icon_surface) {
            cairo_surface_destroy(app->cards[i].icon_surface);
            app->cards[i].icon_surface = NULL;
        }
        if (app->cards[i].badge_surface) {
            cairo_surface_destroy(app->cards[i].badge_surface);
            app->cards[i].badge_surface = NULL;
        }
    }
}

static void load_card_assets(NotifCard *c, const char *avatar_filename);

static void load_card_assets(NotifCard *c, const char *avatar_filename) {
    if (!g_app.cfg.show_images) return;
    char notif_img[768] = {0};
    char app_icon[512] = {0};

    /* 1. Direct avatar from state assets directory if known */
    if (avatar_filename && avatar_filename[0] != '\0') {
        char assets_dir[512];
        get_notif_paths(NULL, 0, NULL, 0, assets_dir, sizeof(assets_dir));
        char full_p[768];
        snprintf(full_p, sizeof(full_p), "%s/%s", assets_dir, avatar_filename);
        if (access(full_p, R_OK) == 0) {
            snprintf(notif_img, sizeof(notif_img), "%s", full_p);
        }
    }

    /* 2. Resolve notification image / avatar / sticker if not loaded yet */
    if (notif_img[0] == '\0') {
        bool has_avatar = resolve_cached_avatar(c->appname, c->summary, notif_img, sizeof(notif_img));
        if (!has_avatar) {
            if (c->icon_path[0] != '\0') {
                if (access(c->icon_path, R_OK) == 0) {
                    snprintf(notif_img, sizeof(notif_img), "%s", c->icon_path);
                } else {
                    resolve_icon(c->icon_path, notif_img, sizeof(notif_img));
                }
            } else {
                char url[512] = {0};
                extract_image_url(c->urls, c->body, url, sizeof(url));
                if (url[0] != '\0') {
                    resolve_cached_url_image(url, notif_img, sizeof(notif_img));
                }
            }
        }
    }

    /* 3. Resolve application icon based on appname */
    if (c->appname[0] != '\0' && strcasecmp(c->appname, "notify-send") != 0) {
        resolve_icon(c->appname, app_icon, sizeof(app_icon));
    }

    /* 4. Determine if notif_img is an avatar/picture distinct from the application's icon */
    bool has_notif_img = (notif_img[0] != '\0');
    bool is_app_icon = (has_notif_img && app_icon[0] != '\0' && strcmp(notif_img, app_icon) == 0);

    if (has_notif_img && !is_app_icon) {
        c->icon_surface = get_cached_image_surface(notif_img, g_app.cfg.image_size, g_app.cfg.image_size);
        c->show_app_badge = true;
        if (app_icon[0] != '\0') {
            c->badge_surface = get_cached_image_surface(app_icon, 20, 20);
        }
        if (c->appname[0] != '\0') {
            c->badge_letter = (char)toupper((unsigned char)c->appname[0]);
        } else {
            c->badge_letter = 'N';
        }
    } else if (has_notif_img || app_icon[0] != '\0') {
        const char *main_path = has_notif_img ? notif_img : app_icon;
        c->icon_surface = get_cached_image_surface(main_path, g_app.cfg.image_size, g_app.cfg.image_size);
        c->show_app_badge = false;
    } else {
        c->icon_surface = NULL;
        c->show_app_badge = false;
    }
}

static int load_persistent_history(NotifCard *cards, int max_cards) {
    char hist_file[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), NULL, 0);

    if (access(hist_file, R_OK) != 0) {
        return 0;
    }

    char lock_path[768];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", hist_file);
    int lock_fd = open(lock_path, O_RDONLY);
    if (lock_fd >= 0) flock(lock_fd, LOCK_SH);

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    if (!root) {
        return 0;
    }

    json_t *entries = json_object_get(root, "entries");
    if (!entries || !json_is_array(entries)) {
        json_decref(root);
        return 0;
    }

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    struct tm mid_tm = now_tm;
    mid_tm.tm_hour = 0;
    mid_tm.tm_min = 0;
    mid_tm.tm_sec = 0;
    mid_tm.tm_isdst = -1;
    time_t midnight_today = mktime(&mid_tm);
    time_t midnight_yesterday = midnight_today - 86400;

    int count = 0;
    size_t total = json_array_size(entries);

    for (size_t i = 0; i < total && count < max_cards; i++) {
        json_t *entry = json_array_get(entries, i);
        if (!json_is_object(entry)) continue;

        NotifCard *c = &cards[count];
        memset(c, 0, sizeof(NotifCard));

        json_t *j_id = json_object_get(entry, "id");
        if (j_id && json_is_integer(j_id)) c->id = (int)json_integer_value(j_id);

        json_t *j_app = json_object_get(entry, "appname");
        if (j_app && json_is_string(j_app)) snprintf(c->appname, sizeof(c->appname), "%s", json_string_value(j_app));
        if (c->appname[0] == '\0') strcpy(c->appname, "Notificación");

        json_t *j_sum = json_object_get(entry, "summary");
        if (j_sum && json_is_string(j_sum)) snprintf(c->summary, sizeof(c->summary), "%s", json_string_value(j_sum));

        json_t *j_bod = json_object_get(entry, "body");
        if (j_bod && json_is_string(j_bod)) snprintf(c->body, sizeof(c->body), "%s", json_string_value(j_bod));

        json_t *j_urg = json_object_get(entry, "urgency");
        if (j_urg && json_is_integer(j_urg)) c->urgency = (int)json_integer_value(j_urg);
        else c->urgency = 1;

        json_t *j_ts = json_object_get(entry, "timestamp");
        time_t entry_time = (j_ts && json_is_integer(j_ts)) ? (time_t)json_integer_value(j_ts) : now;
        c->elapsed_sec = (now >= entry_time) ? (now - entry_time) : 0;

        if (entry_time >= midnight_today) {
            c->is_today = 1;
        } else if (entry_time >= midnight_yesterday) {
            c->is_yesterday = 1;
        } else {
            c->is_older = 1;
        }

        json_t *j_icon = json_object_get(entry, "icon_path");
        if (j_icon && json_is_string(j_icon)) snprintf(c->icon_path, sizeof(c->icon_path), "%s", json_string_value(j_icon));

        const char *avatar_fn = NULL;
        json_t *j_av = json_object_get(entry, "avatar");
        if (j_av && json_is_string(j_av)) {
            avatar_fn = json_string_value(j_av);
            snprintf(c->avatar, sizeof(c->avatar), "%s", avatar_fn);
        }

        clean_notification_body(c->body, c->clean_body, sizeof(c->clean_body));

        load_card_assets(c, avatar_fn);
        count++;
    }

    json_decref(root);
    return count;
}

static void delete_card_from_history(int id) {
    char hist_file[512], assets_dir[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), assets_dir, sizeof(assets_dir));

    char lock_path[768];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", hist_file);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    char deleted_avatar[128] = {0};

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (root && json_is_object(root)) {
        json_t *entries = json_object_get(root, "entries");
        if (entries && json_is_array(entries)) {
            for (size_t i = 0; i < json_array_size(entries); i++) {
                json_t *item = json_array_get(entries, i);
                json_t *j_id = json_object_get(item, "id");
                if (j_id && json_is_integer(j_id) && json_integer_value(j_id) == id) {
                    json_t *j_av = json_object_get(item, "avatar");
                    if (j_av && json_is_string(j_av)) {
                        snprintf(deleted_avatar, sizeof(deleted_avatar), "%s", json_string_value(j_av));
                    }
                    json_array_remove(entries, i);
                    break;
                }
            }

            /* Prune orphaned avatar if no other card uses it */
            if (deleted_avatar[0] != '\0') {
                bool still_used = false;
                for (size_t i = 0; i < json_array_size(entries); i++) {
                    json_t *item = json_array_get(entries, i);
                    json_t *j_av = json_object_get(item, "avatar");
                    if (j_av && json_is_string(j_av) && strcmp(json_string_value(j_av), deleted_avatar) == 0) {
                        still_used = true;
                        break;
                    }
                }
                if (!still_used) {
                    char av_path[768];
                    snprintf(av_path, sizeof(av_path), "%s/%s", assets_dir, deleted_avatar);
                    unlink(av_path);
                }
            }

            char tmp_file[768];
            snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", hist_file);
            if (json_dump_file(root, tmp_file, JSON_INDENT(2)) == 0) {
                rename(tmp_file, hist_file);
            }
        }
        json_decref(root);
    }

    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    if (id > 0) {
        send_dbus_close_notification(id);
    }
}

static void clear_all_history_file(void) {
    char hist_file[512], assets_dir[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), assets_dir, sizeof(assets_dir));

    char lock_path[768];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", hist_file);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    int next_id = 1;
    if (root && json_is_object(root)) {
        json_t *j_nid = json_object_get(root, "next_id");
        if (j_nid && json_is_integer(j_nid)) next_id = (int)json_integer_value(j_nid);
        json_decref(root);
    }

    json_t *new_root = json_object();
    json_object_set_new(new_root, "next_id", json_integer(next_id));
    json_object_set_new(new_root, "entries", json_array());

    char tmp_file[768];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", hist_file);
    if (json_dump_file(new_root, tmp_file, JSON_INDENT(2)) == 0) {
        rename(tmp_file, hist_file);
    }
    json_decref(new_root);

    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    /* Clean all files in assets_dir */
    DIR *d = opendir(assets_dir);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_name[0] != '.') {
                char p[768];
                snprintf(p, sizeof(p), "%s/%s", assets_dir, dir->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }

    send_dbus_close_all();
}

static void format_time_ago(int64_t elapsed_sec, char *out, size_t out_len) {
    if (elapsed_sec < 60) {
        snprintf(out, out_len, "ahora");
    } else if (elapsed_sec < 3600) {
        snprintf(out, out_len, "hace %ldm", (long)(elapsed_sec / 60));
    } else if (elapsed_sec < 86400) {
        snprintf(out, out_len, "hace %ldh", (long)(elapsed_sec / 3600));
    } else {
        snprintf(out, out_len, "hace %ldd", (long)(elapsed_sec / 86400));
    }
}

static bool card_matches_filter(const NotifCard *c, FilterTab tab) {
    if (tab == FILTER_ALL) return true;
    if (tab == FILTER_TODAY) return c->is_today;
    if (tab == FILTER_YESTERDAY) return c->is_yesterday;
    if (tab == FILTER_OLDER) return c->is_older;
    return true;
}

static void layout_cards(AppState *app);

static void reload_notifications(AppState *app) {
    free_card_surfaces(app);
    int limit = (app->cfg.max_entries > 0 && app->cfg.max_entries <= MAX_CARDS) ? app->cfg.max_entries : 500;
    app->card_count = load_persistent_history(app->cards, limit);

    app->filtered_count = 0;
    for (int i = 0; i < app->card_count; i++) {
        if (card_matches_filter(&app->cards[i], app->active_filter)) {
            app->filtered_count++;
        }
    }
    layout_cards(app);
}

static void layout_cards(AppState *app) {
    PangoLayout *layout_sum = app->pango.layout_sum;
    PangoLayout *layout_body = app->pango.layout_body;

    int card_w = app->panel_w - 32;
    int icon_size = app->cfg.image_size > 0 ? app->cfg.image_size : 48;
    bool is_left = (strcasecmp(app->cfg.icon_position, "right") != 0);

    int start_y = 104;
    int current_y = start_y - app->scroll_offset;

    for (int i = 0; i < app->card_count; i++) {
        NotifCard *c = &app->cards[i];
        if (!card_matches_filter(c, app->active_filter)) {
            c->card_h = 0;
            continue;
        }

        int text_w;
        if (is_left) {
            c->icon_x = 16 + 10;
            c->icon_y = current_y + 10;
            c->icon_w = icon_size;
            c->icon_h = icon_size;
            text_w = card_w - (10 + icon_size + 10) - 28;
            c->close_w = 24;
            c->close_h = 24;
            c->close_x = 16 + card_w - c->close_w - 6;
            c->close_y = current_y + 6;
        } else {
            c->close_w = 24;
            c->close_h = 24;
            c->close_x = 16 + card_w - c->close_w - 6;
            c->close_y = current_y + 6;
            c->icon_x = c->close_x - 8 - icon_size;
            c->icon_y = current_y + 10;
            c->icon_w = icon_size;
            c->icon_h = icon_size;
            text_w = c->icon_x - 16 - 16;
        }
        if (text_w < 100) text_w = 100;

        pango_layout_set_width(layout_sum, text_w * PANGO_SCALE);
        pango_layout_set_ellipsize(layout_sum, PANGO_ELLIPSIZE_END);
        int sum_h = 0;
        if (c->summary[0] != '\0') {
            pango_layout_set_text(layout_sum, c->summary, -1);
            pango_layout_get_pixel_size(layout_sum, NULL, &sum_h);
            if (sum_h > 38) sum_h = 38;
        }

        const char *body_text = (c->clean_body[0] != '\0') ? c->clean_body : c->body;
        int body_h = 0;
        if (body_text[0] != '\0') {
            pango_layout_set_width(layout_body, text_w * PANGO_SCALE);
            pango_layout_set_wrap(layout_body, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_height(layout_body, 72 * PANGO_SCALE);
            pango_layout_set_ellipsize(layout_body, PANGO_ELLIPSIZE_END);
            pango_layout_set_text(layout_body, body_text, -1);
            pango_layout_get_pixel_size(layout_body, NULL, &body_h);
            if (body_h > 72) body_h = 72;
        }

        int header_h = 18;
        int text_block_h = header_h + (sum_h > 0 ? (sum_h + 3) : 0) + (body_h > 0 ? (body_h + 4) : 0);
        int content_h = text_block_h > icon_size ? text_block_h : icon_size;

        int total_h = 10 + content_h + 10;
        if (total_h < 56) total_h = 56;

        c->card_y = current_y;
        c->card_h = total_h;

        current_y += total_h + CARD_SPACING;
    }

    int list_view_h;
    if (app->cfg.show_quick_actions) {
        app->tiles_area_h = 52;
        app->tiles_area_y = app->panel_h - app->tiles_area_h - 16;
        list_view_h = app->tiles_area_y - start_y - 10;
    } else {
        app->tiles_area_h = 0;
        app->tiles_area_y = app->panel_h;
        list_view_h = app->panel_h - start_y - 14;
    }

    int content_total_h = (current_y + app->scroll_offset) - start_y;
    app->max_scroll = content_total_h > list_view_h ? (content_total_h - list_view_h) : 0;
    if (app->scroll_offset > app->max_scroll) app->scroll_offset = app->max_scroll;
    if (app->scroll_offset < 0) app->scroll_offset = 0;
}

static void draw_centered_icon(cairo_t *cr, PangoLayout *layout, const char *glyph, double rx, double ry, double rw, double rh) {
    pango_layout_set_text(layout, glyph, -1);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(layout, &ink, &log);
    double w = (ink.width > 0) ? ink.width : log.width;
    double h = (ink.height > 0) ? ink.height : log.height;
    double ox = (ink.width > 0) ? ink.x : log.x;
    double oy = (ink.height > 0) ? ink.y : log.y;
    double x = rx + (rw - w) / 2.0 - ox;
    double y = ry + (rh - h) / 2.0 - oy;
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

static void draw_app_badge(cairo_t *cr, double x, double y, double size, const NotifCard *c, AppState *app) {
    const Theme *th = &app->theme;
    cairo_save(cr);
    cairo_rounded_rect(cr, x, y, size, size, app->cfg.card_radius);
    cairo_set_source_rgba(cr, th->accent_dim.r, th->accent_dim.g, th->accent_dim.b, 0.7);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    char initial = 'N';
    if (c->appname[0] != '\0') {
        initial = (c->appname[0] >= 'a' && c->appname[0] <= 'z') ? (c->appname[0] - 32) : c->appname[0];
    }
    char text[2] = {initial, '\0'};

    cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
    draw_centered_icon(cr, app->pango.layout_btn, text, x, y, size, size);
    cairo_restore(cr);
}

static void render_panel(AppState *app) {
    cairo_t *cr = app->cr;
    Theme *th = &app->theme;
    PangoCache *p = &app->pango;

    /* Clear surface */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Main panel container */
    cairo_rounded_rect(cr, 1.0, 1.0, app->panel_w - 2.0, app->panel_h - 2.0, app->cfg.panel_radius);
    cairo_set_source_rgba(cr, th->bg_panel.r, th->bg_panel.g, th->bg_panel.b, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, th->border.a);
    cairo_set_line_width(cr, app->cfg.border_width > 0 ? app->cfg.border_width : 1.0);
    cairo_stroke(cr);

    /* --- 1. Header Bar --- */
    /* Bell icon on the left (centered in 24x28 area at x=16, y=16) */
    cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
    draw_centered_icon(cr, p->layout_title, "\xef\x83\xb3", 16, 16, 24, 28);
    int bell_end_x = 16 + 24;

    /* Notification counter badge next to the bell (square aesthetic, pixel-centered number) */
    if (app->filtered_count > 0) {
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", app->filtered_count);
        pango_layout_set_text(p->layout_btn, count_str, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(p->layout_btn, &ink, &log);
        int cw = (ink.width > 0) ? ink.width : log.width;

        int badge_pad_x = 6;
        int badge_w = cw + badge_pad_x * 2;
        if (badge_w < 20) badge_w = 20;
        int badge_h = 20;
        int badge_x = bell_end_x + 8;
        int badge_y = 16 + (28 - badge_h) / 2;

        cairo_rounded_rect(cr, badge_x, badge_y, badge_w, badge_h, app->cfg.button_radius);
        cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.2);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.6);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
        draw_centered_icon(cr, p->layout_btn, count_str, badge_x, badge_y, badge_w, badge_h);
    }

    /* Clear all button (Trash icon) - Square 28x28 button */
    app->clear_btn_w = 28;
    app->clear_btn_h = 28;
    app->clear_btn_x = app->panel_w - app->clear_btn_w - 16;
    app->clear_btn_y = 16;

    cairo_rounded_rect(cr, app->clear_btn_x, app->clear_btn_y, app->clear_btn_w, app->clear_btn_h, app->cfg.button_radius);
    if (app->clear_btn_hovered) {
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 0.25);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 0.8);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 1.0);
    } else {
        cairo_set_source_rgba(cr, th->surface.r, th->surface.g, th->surface.b, 0.6);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.6);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.9);
    }
    draw_centered_icon(cr, p->layout_btn, "\xef\x87\xb8", app->clear_btn_x, app->clear_btn_y, app->clear_btn_w, app->clear_btn_h);

    /* DND Toggle Button (Bell-slash icon) - Square 28x28 button */
    app->dnd_btn_w = 28;
    app->dnd_btn_h = 28;
    app->dnd_btn_x = app->clear_btn_x - app->dnd_btn_w - 8;
    app->dnd_btn_y = 16;

    cairo_rounded_rect(cr, app->dnd_btn_x, app->dnd_btn_y, app->dnd_btn_w, app->dnd_btn_h, app->cfg.button_radius);
    if (app->is_dnd) {
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, app->dnd_btn_hovered ? 0.35 : 0.2);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 0.8);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 1.0);
    } else {
        cairo_set_source_rgba(cr, th->surface.r, th->surface.g, th->surface.b, app->dnd_btn_hovered ? 0.9 : 0.6);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.6);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.9);
    }
    draw_centered_icon(cr, p->layout_btn, "\xef\x87\xb6", app->dnd_btn_x, app->dnd_btn_y, app->dnd_btn_w, app->dnd_btn_h);

    /* --- 2. Filter Bar --- */
    const char *filter_labels[FILTER_COUNT] = {"Todas", "Hoy", "Ayer", "Antiguas"};
    int tab_w = (app->panel_w - 32 - 9) / 4;
    int tab_h = 28;
    int tab_y = 56;

    for (int t = 0; t < FILTER_COUNT; t++) {
        int tx = 16 + t * (tab_w + 3);
        app->filter_rects[t][0] = tx;
        app->filter_rects[t][1] = tab_y;
        app->filter_rects[t][2] = tab_w;
        app->filter_rects[t][3] = tab_h;

        cairo_rounded_rect(cr, tx, tab_y, tab_w, tab_h, app->cfg.button_radius);
        if ((int)app->active_filter == t) {
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.25);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.9);
            cairo_set_line_width(cr, 1.2);
            cairo_stroke(cr);
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
        } else if (app->filter_hovered == t) {
            cairo_set_source_rgba(cr, th->bg_card_hover.r, th->bg_card_hover.g, th->bg_card_hover.b, 0.9);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.6);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_set_source_rgba(cr, th->fg_text.r, th->fg_text.g, th->fg_text.b, 0.95);
        } else {
            cairo_set_source_rgba(cr, th->surface.r, th->surface.g, th->surface.b, 0.5);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.4);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.7);
        }

        pango_layout_set_text(p->layout_tab, filter_labels[t], -1);
        int tw, th_px;
        pango_layout_get_pixel_size(p->layout_tab, &tw, &th_px);
        cairo_move_to(cr, tx + (tab_w - tw) / 2.0, tab_y + (tab_h - th_px) / 2.0);
        pango_cairo_show_layout(cr, p->layout_tab);
    }

    /* Separator line below filters */
    cairo_move_to(cr, 16, tab_y + tab_h + 10);
    cairo_line_to(cr, app->panel_w - 16, tab_y + tab_h + 10);
    cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* --- 3. Notification Cards List --- */
    int list_clip_y = tab_y + tab_h + 12;
    int list_clip_h = (app->cfg.show_quick_actions ? app->tiles_area_y : app->panel_h) - list_clip_y - 12;

    cairo_save(cr);
    cairo_rectangle(cr, 0, list_clip_y, app->panel_w, list_clip_h);
    cairo_clip(cr);

    if (app->filtered_count == 0) {
        int cy = list_clip_y + list_clip_h / 2 - 30;
        pango_layout_set_font_description(p->layout_empty, p->f_bold10);
        pango_layout_set_text(p->layout_empty, "No hay notificaciones", -1);
        int tw, th_px;
        pango_layout_get_pixel_size(p->layout_empty, &tw, &th_px);
        cairo_move_to(cr, (app->panel_w - tw) / 2.0, cy);
        cairo_set_source_rgba(cr, th->fg_text.r, th->fg_text.g, th->fg_text.b, 0.85);
        pango_cairo_show_layout(cr, p->layout_empty);

        pango_layout_set_font_description(p->layout_empty, p->f_norm9);
        pango_layout_set_text(p->layout_empty, "Todo esta al dia", -1);
        pango_layout_get_pixel_size(p->layout_empty, &tw, &th_px);
        cairo_move_to(cr, (app->panel_w - tw) / 2.0, cy + 28);
        cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.6);
        pango_cairo_show_layout(cr, p->layout_empty);
    } else {
        int card_x = 16;
        int card_w = app->panel_w - 32;
        bool is_left = (strcasecmp(app->cfg.icon_position, "right") != 0);

        for (int i = 0; i < app->card_count; i++) {
            NotifCard *c = &app->cards[i];
            if (!card_matches_filter(c, app->active_filter)) continue;
            if (c->card_y + c->card_h < list_clip_y - 20 || c->card_y > list_clip_y + list_clip_h + 20) {
                continue;
            }

            cairo_new_path(cr);
            cairo_rounded_rect(cr, card_x, c->card_y, card_w, c->card_h, app->cfg.card_radius);
            if (c->urgency == 2) {
                cairo_set_source_rgba(cr, th->bg_card_urgent.r, th->bg_card_urgent.g, th->bg_card_urgent.b, 0.95);
            } else if (c->is_hovered) {
                cairo_set_source_rgba(cr, th->bg_card_hover.r, th->bg_card_hover.g, th->bg_card_hover.b, 0.95);
            } else {
                cairo_set_source_rgba(cr, th->bg_card.r, th->bg_card.g, th->bg_card.b, 0.85);
            }
            cairo_fill_preserve(cr);

            if (c->urgency == 2) {
                cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 0.8);
            } else if (c->is_hovered) {
                cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.5);
            } else {
                cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.5);
            }
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            /* Draw icon / image on left or right */
            if (c->icon_surface && app->cfg.show_images) {
                cairo_save(cr);
                cairo_rounded_rect(cr, c->icon_x, c->icon_y, c->icon_w, c->icon_h, app->cfg.card_radius);
                cairo_clip(cr);
                int iw = cairo_image_surface_get_width(c->icon_surface);
                int ih = cairo_image_surface_get_height(c->icon_surface);
                if (iw > 0 && ih > 0) {
                    double scale = fmin((double)c->icon_w / iw, (double)c->icon_h / ih);
                    double ox = c->icon_x + (c->icon_w - iw * scale) / 2.0;
                    double oy = c->icon_y + (c->icon_h - ih * scale) / 2.0;
                    cairo_translate(cr, ox, oy);
                    cairo_scale(cr, scale, scale);
                    cairo_set_source_surface(cr, c->icon_surface, 0, 0);
                    cairo_paint(cr);
                }
                cairo_restore(cr);

                cairo_rounded_rect(cr, c->icon_x, c->icon_y, c->icon_w, c->icon_h, app->cfg.card_radius);
                cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.4);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);

                /* Draw overlay application emblem if this is a messenger/sender avatar */
                if (c->show_app_badge) {
                    int bw = 18;
                    int bh = 18;
                    int bx = c->icon_x + c->icon_w - bw + 2;
                    int by = c->icon_y + c->icon_h - bh + 2;

                    /* Badge background cutout */
                    cairo_rounded_rect(cr, bx - 1, by - 1, bw + 2, bh + 2, app->cfg.button_radius);
                    cairo_set_source_rgba(cr, th->bg_card.r, th->bg_card.g, th->bg_card.b, 1.0);
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.9);
                    cairo_set_line_width(cr, 1.0);
                    cairo_stroke(cr);

                    if (c->badge_surface) {
                        cairo_save(cr);
                        cairo_rounded_rect(cr, bx, by, bw, bh, app->cfg.button_radius);
                        cairo_clip(cr);
                        int biw = cairo_image_surface_get_width(c->badge_surface);
                        int bih = cairo_image_surface_get_height(c->badge_surface);
                        if (biw > 0 && bih > 0) {
                            double scale = fmin((double)bw / biw, (double)bh / bih);
                            double ox = bx + (bw - biw * scale) / 2.0;
                            double oy = by + (bh - bih * scale) / 2.0;
                            cairo_translate(cr, ox, oy);
                            cairo_scale(cr, scale, scale);
                            cairo_set_source_surface(cr, c->badge_surface, 0, 0);
                            cairo_paint(cr);
                        }
                        cairo_restore(cr);
                    } else if (c->badge_letter != '\0') {
                        char str[2] = { c->badge_letter, '\0' };
                        pango_layout_set_font_description(p->layout_btn, p->f_norm8);
                        pango_layout_set_text(p->layout_btn, str, -1);
                        int tw, th_px;
                        pango_layout_get_pixel_size(p->layout_btn, &tw, &th_px);
                        cairo_move_to(cr, bx + (bw - tw) / 2.0, by + (bh - th_px) / 2.0);
                        cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
                        pango_cairo_show_layout(cr, p->layout_btn);
                        pango_layout_set_font_description(p->layout_btn, p->f_bold9);
                    }
                }
            } else {
                draw_app_badge(cr, c->icon_x, c->icon_y, c->icon_w, c, app);
            }

            /* Draw text block */
            int tx = is_left ? (card_x + 10 + c->icon_w + 10) : (card_x + 12);
            int ty = c->card_y + 8;

            /* Header: appname • time */
            char time_str[32];
            format_time_ago(c->elapsed_sec, time_str, sizeof(time_str));

            pango_layout_set_text(p->layout_meta, c->appname, -1);
            cairo_move_to(cr, tx, ty);
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.95);
            pango_cairo_show_layout(cr, p->layout_meta);

            int app_w;
            pango_layout_get_pixel_size(p->layout_meta, &app_w, NULL);

            pango_layout_set_text(p->layout_meta, "•", -1);
            cairo_move_to(cr, tx + app_w + 5, ty);
            cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.5);
            pango_cairo_show_layout(cr, p->layout_meta);

            pango_layout_set_text(p->layout_meta, time_str, -1);
            cairo_move_to(cr, tx + app_w + 16, ty);
            cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.7);
            pango_cairo_show_layout(cr, p->layout_meta);

            ty += 20;

            cairo_save(cr);
            cairo_rounded_rect(cr, card_x, c->card_y, card_w, c->card_h, app->cfg.card_radius);
            cairo_clip(cr);

            /* Summary */
            if (c->summary[0] != '\0') {
                pango_layout_set_text(p->layout_sum, c->summary, -1);
                cairo_move_to(cr, tx, ty);
                cairo_set_source_rgba(cr, th->fg_text.r, th->fg_text.g, th->fg_text.b, 1.0);
                pango_cairo_show_layout(cr, p->layout_sum);
                int sh;
                pango_layout_get_pixel_size(p->layout_sum, NULL, &sh);
                ty += sh + 3;
            }

            /* Body */
            const char *body_text = (c->clean_body[0] != '\0') ? c->clean_body : c->body;
            if (body_text[0] != '\0') {
                int body_w = is_left ? (card_w - (10 + c->icon_w + 10) - 28) : (c->icon_x - 16 - 16);
                if (body_w < 100) body_w = 100;
                pango_layout_set_width(p->layout_body, body_w * PANGO_SCALE);
                pango_layout_set_wrap(p->layout_body, PANGO_WRAP_WORD_CHAR);
                pango_layout_set_height(p->layout_body, 72 * PANGO_SCALE);
                pango_layout_set_ellipsize(p->layout_body, PANGO_ELLIPSIZE_END);
                pango_layout_set_text(p->layout_body, body_text, -1);
                cairo_move_to(cr, tx, ty);
                cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.9);
                pango_cairo_show_layout(cr, p->layout_body);
            }

            /* Close [x] button */
            if (c->close_hovered) {
                cairo_rounded_rect(cr, c->close_x, c->close_y, c->close_w, c->close_h, app->cfg.button_radius);
                cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 0.35);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, th->alert.r, th->alert.g, th->alert.b, 1.0);
            } else {
                cairo_set_source_rgba(cr, th->fg_muted.r, th->fg_muted.g, th->fg_muted.b, 0.6);
            }
            draw_centered_icon(cr, p->layout_btn, "×", c->close_x, c->close_y, c->close_w, c->close_h);

            cairo_restore(cr);
            cairo_new_path(cr);
        }
    }
    cairo_restore(cr);

    /* Vertical Scrollbar with Matugen theme */
    if (app->max_scroll > 0) {
        int sb_w = (app->scrollbar_hovered || app->scrollbar_dragging) ? 6 : 4;
        int sb_x = app->panel_w - 6 - sb_w;
        int track_y = list_clip_y + 4;
        int track_h = list_clip_h - 8;

        app->scrollbar_visible = true;
        app->scrollbar_x = sb_x;
        app->scrollbar_y = track_y;
        app->scrollbar_w = sb_w;
        app->scrollbar_h = track_h;

        int content_h = app->max_scroll + list_clip_h;
        int thumb_h = (int)((double)track_h * list_clip_h / content_h);
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > track_h) thumb_h = track_h;

        int thumb_y = track_y + (int)((double)(track_h - thumb_h) * app->scroll_offset / app->max_scroll);
        app->thumb_h = thumb_h;
        app->thumb_y = thumb_y;

        /* Track: Matugen th->surface */
        cairo_rounded_rect(cr, sb_x, track_y, sb_w, track_h, sb_w / 2.0);
        cairo_set_source_rgba(cr, th->surface.r, th->surface.g, th->surface.b, 0.45);
        cairo_fill(cr);

        /* Thumb: Matugen th->accent */
        cairo_rounded_rect(cr, sb_x, thumb_y, sb_w, thumb_h, sb_w / 2.0);
        if (app->scrollbar_dragging || app->scrollbar_hovered) {
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 1.0);
        } else {
            cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.85);
        }
        cairo_fill(cr);
    } else {
        app->scrollbar_visible = false;
    }

    /* --- 4. Quick Actions (Only if enabled in config) --- */
    if (app->cfg.show_quick_actions) {
        cairo_move_to(cr, 16, app->tiles_area_y - 6);
        cairo_line_to(cr, app->panel_w - 16, app->tiles_area_y - 6);
        cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.4);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        const char *tile_titles[3] = {"WiFi", "No Molestar", "Silenciar"};
        int tile_count = 3;
        int gap = 8;
        int tile_w = (app->panel_w - 32 - (tile_count - 1) * gap) / tile_count;
        int tile_h = 38;
        int tile_y = app->tiles_area_y + 4;

        for (int k = 0; k < tile_count; k++) {
            int tx = 16 + k * (tile_w + gap);
            app->tile_rects[k][0] = tx;
            app->tile_rects[k][1] = tile_y;
            app->tile_rects[k][2] = tile_w;
            app->tile_rects[k][3] = tile_h;

            cairo_rounded_rect(cr, tx, tile_y, tile_w, tile_h, app->cfg.button_radius);
            if (k == 1 && app->is_dnd) {
                cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, app->tile_hovered == k ? 0.35 : 0.22);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, th->accent.r, th->accent.g, th->accent.b, 0.9);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            } else if (app->tile_hovered == k) {
                cairo_set_source_rgba(cr, th->bg_card_hover.r, th->bg_card_hover.g, th->bg_card_hover.b, 0.9);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.8);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgba(cr, th->surface.r, th->surface.g, th->surface.b, 0.6);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, th->border.r, th->border.g, th->border.b, 0.4);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            pango_layout_set_text(p->layout_tile_t, tile_titles[k], -1);
            int tw, th_px;
            pango_layout_get_pixel_size(p->layout_tile_t, &tw, &th_px);
            cairo_move_to(cr, tx + (tile_w - tw) / 2.0, tile_y + (tile_h - th_px) / 2.0);
            cairo_set_source_rgba(cr, th->fg_text.r, th->fg_text.g, th->fg_text.b, 0.95);
            pango_cairo_show_layout(cr, p->layout_tile_t);
        }
    }

    /* Blit double buffer to X11 drawable */
    cairo_t *xcr = cairo_create(app->cairo_surface);
    cairo_set_source_surface(xcr, app->buffer_surface, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    XFlush(app->dpy);
}

static void trigger_redraw(AppState *app) {
    layout_cards(app);
    render_panel(app);
}

static void clear_hover_states(AppState *app) {
    app->dnd_btn_hovered = false;
    app->clear_btn_hovered = false;
    app->filter_hovered = -1;
    app->tile_hovered = -1;
    app->scrollbar_hovered = false;
    for (int i = 0; i < app->card_count; i++) {
        app->cards[i].is_hovered = false;
        app->cards[i].close_hovered = false;
    }
}

static void on_motion(AppState *app, int mx, int my) {
    bool needs_redraw = false;

    if (app->scrollbar_dragging) {
        int dy = my - app->drag_start_y;
        int travel = app->scrollbar_h - app->thumb_h;
        if (travel > 0) {
            int new_scroll = app->drag_start_scroll + (int)((double)dy * app->max_scroll / travel);
            if (new_scroll < 0) new_scroll = 0;
            if (new_scroll > app->max_scroll) new_scroll = app->max_scroll;
            if (new_scroll != app->scroll_offset) {
                app->scroll_offset = new_scroll;
                trigger_redraw(app);
                return;
            }
        }
        return;
    } else {
        bool sb_hov = (app->scrollbar_visible && mx >= app->panel_w - 18 && mx <= app->panel_w &&
                       my >= app->scrollbar_y && my <= app->scrollbar_y + app->scrollbar_h);
        if (sb_hov != app->scrollbar_hovered) {
            app->scrollbar_hovered = sb_hov;
            needs_redraw = true;
        }
    }

    bool dnd_hov = (mx >= app->dnd_btn_x && mx <= app->dnd_btn_x + app->dnd_btn_w &&
                    my >= app->dnd_btn_y && my <= app->dnd_btn_y + app->dnd_btn_h);
    if (dnd_hov != app->dnd_btn_hovered) {
        app->dnd_btn_hovered = dnd_hov;
        needs_redraw = true;
    }

    bool clr_hov = (mx >= app->clear_btn_x && mx <= app->clear_btn_x + app->clear_btn_w &&
                    my >= app->clear_btn_y && my <= app->clear_btn_y + app->clear_btn_h);
    if (clr_hov != app->clear_btn_hovered) {
        app->clear_btn_hovered = clr_hov;
        needs_redraw = true;
    }

    int f_hov = -1;
    for (int t = 0; t < FILTER_COUNT; t++) {
        if (mx >= app->filter_rects[t][0] && mx <= app->filter_rects[t][0] + app->filter_rects[t][2] &&
            my >= app->filter_rects[t][1] && my <= app->filter_rects[t][1] + app->filter_rects[t][3]) {
            f_hov = t;
            break;
        }
    }
    if (f_hov != app->filter_hovered) {
        app->filter_hovered = f_hov;
        needs_redraw = true;
    }

    if (app->cfg.show_quick_actions) {
        int t_hov = -1;
        for (int k = 0; k < 3; k++) {
            if (mx >= app->tile_rects[k][0] && mx <= app->tile_rects[k][0] + app->tile_rects[k][2] &&
                my >= app->tile_rects[k][1] && my <= app->tile_rects[k][1] + app->tile_rects[k][3]) {
                t_hov = k;
                break;
            }
        }
        if (t_hov != app->tile_hovered) {
            app->tile_hovered = t_hov;
            needs_redraw = true;
        }
    }

    int card_w = app->panel_w - 32;
    for (int i = 0; i < app->card_count; i++) {
        NotifCard *c = &app->cards[i];
        if (!card_matches_filter(c, app->active_filter)) continue;

        bool in_card = (mx >= 16 && mx <= 16 + card_w && my >= c->card_y && my <= c->card_y + c->card_h);
        if (in_card != c->is_hovered) {
            c->is_hovered = in_card;
            needs_redraw = true;
        }

        bool in_close = (mx >= c->close_x && mx <= c->close_x + c->close_w &&
                         my >= c->close_y && my <= c->close_y + c->close_h);
        if (in_close != c->close_hovered) {
            c->close_hovered = in_close;
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        render_panel(app);
    }
}

static void relaunch_card(const NotifCard *c) {
    if (!c) return;

    const char *app = (c->appname[0] != '\0') ? c->appname : "notif_center";
    const char *sum = (c->summary[0] != '\0') ? c->summary : "Notificación";
    const char *body = (c->clean_body[0] != '\0') ? c->clean_body : c->body;
    const char *urg = (c->urgency == 0) ? "low" : (c->urgency == 2) ? "critical" : "normal";

    char icon_path[768] = {0};
    if (c->avatar[0] != '\0') {
        char assets_dir[512];
        get_notif_paths(NULL, 0, NULL, 0, assets_dir, sizeof(assets_dir));
        char full_av[768];
        snprintf(full_av, sizeof(full_av), "%s/%s", assets_dir, c->avatar);
        if (access(full_av, R_OK) == 0) {
            snprintf(icon_path, sizeof(icon_path), "%s", full_av);
        }
    }

    if (icon_path[0] == '\0' && c->icon_path[0] != '\0' && access(c->icon_path, R_OK) == 0) {
        snprintf(icon_path, sizeof(icon_path), "%s", c->icon_path);
    }

    if (icon_path[0] == '\0') {
        resolve_cached_avatar(c->appname, c->summary, icon_path, sizeof(icon_path));
        if (icon_path[0] == '\0' || access(icon_path, R_OK) != 0) {
            if (c->appname[0] != '\0') {
                resolve_icon(c->appname, icon_path, sizeof(icon_path));
            }
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        if (icon_path[0] != '\0' && access(icon_path, R_OK) == 0) {
            execlp("notify-send", "notify-send", "-a", app, "-u", urg, "-i", icon_path,
                   "-h", "boolean:x-i3dots-relaunch:true", sum, body, NULL);
        } else {
            execlp("notify-send", "notify-send", "-a", app, "-u", urg,
                   "-h", "boolean:x-i3dots-relaunch:true", sum, body, NULL);
        }
        _exit(0);
    }
}

static void activate_card(const NotifCard *c) {
    if (!c) return;

    /* Invoke action via notif_daemon (central daemon handles IPC navigation, window focus, and fallbacks generically) */
    send_dbus_invoke_action(c->id, "default");
}

static void on_button_press(AppState *app, int button, int mx, int my) {
    if (button == 4) { /* Scroll up */
        app->scroll_offset -= 45;
        if (app->scroll_offset < 0) app->scroll_offset = 0;
        trigger_redraw(app);
        return;
    }
    if (button == 5) { /* Scroll down */
        app->scroll_offset += 45;
        if (app->scroll_offset > app->max_scroll) app->scroll_offset = app->max_scroll;
        trigger_redraw(app);
        return;
    }

    if (button != 1 && button != 3) return;

    /* Click outside window: close panel */
    if (mx < 0 || mx > app->panel_w || my < 0 || my > app->panel_h) {
        app->running = false;
        return;
    }

    if (button == 1) {
        /* Scrollbar click & drag */
    if (app->scrollbar_visible && mx >= app->panel_w - 18 && mx <= app->panel_w &&
        my >= app->scrollbar_y && my <= app->scrollbar_y + app->scrollbar_h) {
        if (my >= app->thumb_y && my <= app->thumb_y + app->thumb_h) {
            app->scrollbar_dragging = true;
            app->drag_start_y = my;
            app->drag_start_scroll = app->scroll_offset;
            return;
        } else {
            /* Page jump */
            if (my < app->thumb_y) app->scroll_offset -= 120;
            else app->scroll_offset += 120;
            if (app->scroll_offset < 0) app->scroll_offset = 0;
            if (app->scroll_offset > app->max_scroll) app->scroll_offset = app->max_scroll;
            trigger_redraw(app);
            return;
        }
    }

    /* DND Button */
    if (mx >= app->dnd_btn_x && mx <= app->dnd_btn_x + app->dnd_btn_w &&
        my >= app->dnd_btn_y && my <= app->dnd_btn_y + app->dnd_btn_h) {
        app->is_dnd = send_dbus_toggle_dnd();
        trigger_redraw(app);
        return;
    }

    /* Clear All Button */
    if (mx >= app->clear_btn_x && mx <= app->clear_btn_x + app->clear_btn_w &&
        my >= app->clear_btn_y && my <= app->clear_btn_y + app->clear_btn_h) {
        free_card_surfaces(app);
        destroy_surface_cache();
        app->card_count = 0;
        app->filtered_count = 0;
        app->scroll_offset = 0;
        trigger_redraw(app);

        g_self_updating_history++;
        clear_all_history_file();
        return;
    }

    /* Filter Tabs */
    for (int t = 0; t < FILTER_COUNT; t++) {
        if (mx >= app->filter_rects[t][0] && mx <= app->filter_rects[t][0] + app->filter_rects[t][2] &&
            my >= app->filter_rects[t][1] && my <= app->filter_rects[t][1] + app->filter_rects[t][3]) {
            if (app->active_filter != (FilterTab)t) {
                app->active_filter = (FilterTab)t;
                app->scroll_offset = 0;
                int fc = 0;
                for (int j = 0; j < app->card_count; j++) {
                    if (card_matches_filter(&app->cards[j], app->active_filter)) {
                        fc++;
                    }
                }
                app->filtered_count = fc;
                layout_cards(app);
                trigger_redraw(app);
            }
            return;
        }
    }

    /* Quick Action Tiles */
    if (app->cfg.show_quick_actions) {
        for (int k = 0; k < 3; k++) {
            if (mx >= app->tile_rects[k][0] && mx <= app->tile_rects[k][0] + app->tile_rects[k][2] &&
                my >= app->tile_rects[k][1] && my <= app->tile_rects[k][1] + app->tile_rects[k][3]) {
                if (k == 0) {
                    app->running = false;
                    system("$HOME/.local/bin/network_dmenu >/dev/null 2>&1 &");
                } else if (k == 1) {
                    app->is_dnd = send_dbus_toggle_dnd();
                    trigger_redraw(app);
                } else if (k == 2) {
                    system("$HOME/.local/bin/sys_control.sh mute >/dev/null 2>&1 &");
                }
                return;
            }
        }
    }

    } /* end if (button == 1) */

    /* Notification Cards */
    int card_w = app->panel_w - 32;
    for (int i = 0; i < app->card_count; i++) {
        NotifCard *c = &app->cards[i];
        if (!card_matches_filter(c, app->active_filter)) continue;

        /* Close [x] button (Left click only) */
        if (button == 1 && mx >= c->close_x && mx <= c->close_x + c->close_w &&
            my >= c->close_y && my <= c->close_y + c->close_h) {
            int card_id = c->id;
            if (c->icon_surface) {
                cairo_surface_destroy(c->icon_surface);
                c->icon_surface = NULL;
            }
            if (c->badge_surface) {
                cairo_surface_destroy(c->badge_surface);
                c->badge_surface = NULL;
            }

            for (int j = i; j < app->card_count - 1; j++) {
                app->cards[j] = app->cards[j + 1];
            }
            app->card_count--;

            int fc = 0;
            for (int j = 0; j < app->card_count; j++) {
                if (card_matches_filter(&app->cards[j], app->active_filter)) {
                    fc++;
                }
            }
            app->filtered_count = fc;
            layout_cards(app);
            trigger_redraw(app);

            g_self_updating_history++;
            delete_card_from_history(card_id);
            return;
        }

        /* Card click: Left click activates app, Right click relaunches popup */
        if (mx >= 16 && mx <= 16 + card_w && my >= c->card_y && my <= c->card_y + c->card_h) {
            unlink(LOCK_FILE);
            if (button == 3) {
                relaunch_card(c);
            } else {
                activate_card(c);
            }
            app->running = false;
            return;
        }
    }
}

static void on_button_release(AppState *app, int button, int mx, int my) {
    (void)mx; (void)my;
    if (button == 1 && app->scrollbar_dragging) {
        app->scrollbar_dragging = false;
        trigger_redraw(app);
    }
}

typedef enum {
    BAR_POS_UNKNOWN,
    BAR_POS_TOP,
    BAR_POS_BOTTOM
} BarPosition;

typedef struct {
    bool found;
    BarPosition pos;
    int bar_y;
    int bar_h;
} DetectedBar;

static void find_polybar_window_rec(Display *dpy, Window root, Window w, int screen_h, DetectedBar *bar, int depth) {
    if (bar->found || depth > 8) return;
    XClassHint ch;
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_class && strcasecmp(ch.res_class, "Polybar") == 0) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, w, &wa) && wa.map_state == IsViewable && wa.width > 200) {
                int root_x = 0, root_y = 0;
                Window child;
                XTranslateCoordinates(dpy, w, root, 0, 0, &root_x, &root_y, &child);
                bar->found = true;
                bar->bar_y = root_y;
                bar->bar_h = wa.height;
                if (root_y < screen_h / 2) {
                    bar->pos = BAR_POS_TOP;
                } else {
                    bar->pos = BAR_POS_BOTTOM;
                }
            }
        }
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (bar->found) return;

    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, w, &root_ret, &parent_ret, &children, &nchildren) && children) {
        for (unsigned int i = 0; i < nchildren && !bar->found; i++) {
            find_polybar_window_rec(dpy, root, children[i], screen_h, bar, depth + 1);
        }
        XFree(children);
    }
}

static DetectedBar detect_polybar_geometry(Display *dpy, Window root, int screen_h) {
    DetectedBar bar = {false, BAR_POS_UNKNOWN, 0, 0};
    find_polybar_window_rec(dpy, root, root, screen_h, &bar, 0);

    /* If not found via active X11 windows (e.g. autohide is hiding it), fallback to config files */
    if (!bar.found) {
        const char *home = getenv("HOME");
        char path[512];
        bool is_bottom = true;
        bool have_pos = false;
        int bar_h = 33;

        snprintf(path, sizeof(path), "%s/.config/polybar/variables.ini", home ? home : "");
        FILE *f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "bottom", 6) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        char *v = eq + 1;
                        while (isspace((unsigned char)*v)) v++;
                        if (strncmp(v, "true", 4) == 0) { is_bottom = true; have_pos = true; }
                        else if (strncmp(v, "false", 5) == 0) { is_bottom = false; have_pos = true; }
                    }
                } else if (strncmp(line, "height", 6) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        int h_val = atoi(eq + 1);
                        if (h_val > 10) bar_h = (int)(h_val * 1.8);
                    }
                }
            }
            fclose(f);
        }

        if (!have_pos) {
            snprintf(path, sizeof(path), "%s/.config/i3dots/core/state/i3dots/bar/state.env", home ? home : "");
            f = fopen(path, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "position=", 9) == 0) {
                        if (strstr(line, "top")) { is_bottom = false; have_pos = true; }
                        else if (strstr(line, "bottom")) { is_bottom = true; have_pos = true; }
                    }
                }
                fclose(f);
            }
        }

        if (have_pos) {
            bar.found = true;
            bar.bar_h = bar_h;
            if (is_bottom) {
                bar.pos = BAR_POS_BOTTOM;
                bar.bar_y = screen_h - bar_h;
            } else {
                bar.pos = BAR_POS_TOP;
                bar.bar_y = 0;
            }
        }
    }
    return bar;
}

static void calculate_panel_geometry(AppState *app) {
    app->panel_w = app->cfg.panel_w;
    app->panel_x = app->screen_w - app->panel_w - app->cfg.margin_right;

    if (!app->cfg.auto_adjust_bar) {
        int top_margin = app->cfg.margin_top;
        app->panel_y = top_margin;
        app->panel_h = app->screen_h - top_margin - app->cfg.margin_bottom;
        if (app->panel_h < 200) app->panel_h = 200;
        return;
    }

    DetectedBar bar = detect_polybar_geometry(app->dpy, app->root, app->screen_h);
    int gap = (app->cfg.margin_gap > 0) ? app->cfg.margin_gap : app->cfg.margin_right;
    if (gap <= 0) gap = 12;

    if (bar.found && bar.pos == BAR_POS_BOTTOM) {
        /* Polybar is at the BOTTOM: panel starts near top, ends above Polybar */
        app->panel_y = gap;
        int bottom_limit = bar.bar_y - gap;
        app->panel_h = bottom_limit - app->panel_y;
    } else if (bar.found && bar.pos == BAR_POS_TOP) {
        /* Polybar is at the TOP: panel starts below Polybar, ends near bottom */
        int bar_bottom = bar.bar_y + bar.bar_h;
        app->panel_y = bar_bottom + gap;
        int bottom_limit = app->screen_h - gap;
        app->panel_h = bottom_limit - app->panel_y;
    } else {
        /* Fallback: no bar detected, uniform gap on top and bottom */
        app->panel_y = gap;
        app->panel_h = app->screen_h - (gap * 2);
    }

    if (app->panel_h < 200) app->panel_h = 200;
}

static void reload_theme_and_config(AppState *app) {
    destroy_surface_cache();
    destroy_pango_cache(app);
    load_config(&app->cfg);
    load_theme(&app->theme);

    /* Recalculate dimensions dynamically according to Polybar position and config */
    calculate_panel_geometry(app);

    XMoveResizeWindow(app->dpy, app->win, app->panel_x, app->panel_y, app->panel_w, app->panel_h);

    /* Resize Cairo surfaces if dimensions changed */
    int cur_w = cairo_image_surface_get_width(app->buffer_surface);
    int cur_h = cairo_image_surface_get_height(app->buffer_surface);
    if (cur_w != app->panel_w || cur_h != app->panel_h) {
        cairo_destroy(app->cr);
        cairo_surface_destroy(app->buffer_surface);
        cairo_surface_destroy(app->cairo_surface);

        app->cairo_surface = cairo_xlib_surface_create(
            app->dpy, app->win, DefaultVisual(app->dpy, app->screen),
            app->panel_w, app->panel_h
        );
        app->buffer_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, app->panel_w, app->panel_h);
        app->cr = cairo_create(app->buffer_surface);
    }

    init_pango_cache(app);
    app->is_dnd = check_dnd_status();
    trigger_redraw(app);
}

static void handle_signal(int sig) {
    if (g_sig_pipe[1] >= 0) {
        char b = (sig == SIGUSR1) ? 'R' : (sig == SIGUSR2) ? 'N' : 'Q';
        (void)write(g_sig_pipe[1], &b, 1);
    }
}

static void get_polybar_color(const char *key, char *out, size_t out_len, const char *def) {
    char path[512];
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.config/polybar/colors.ini", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, out_len, "%s", def);
        return;
    }
    char line[256];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, klen) == 0 && (p[klen] == ' ' || p[klen] == '\t' || p[klen] == '=')) {
            char *eq = strchr(p, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;
                char *end = eq;
                while (*end && *end != '\r' && *end != '\n') end++;
                *end = '\0';
                if (*eq) {
                    snprintf(out, out_len, "%s", eq);
                    fclose(f);
                    return;
                }
            }
        }
    }
    fclose(f);
    snprintf(out, out_len, "%s", def);
}

static const char *detect_bar_style(void) {
    char target[512] = {0};
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/polybar/config.ini", home ? home : "");
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len > 0) {
        target[len] = '\0';
        if (strstr(target, "polybar_antigua")) return "antigua";
        if (strstr(target, "polybar_floating")) return "floating";
        if (strstr(target, "polybar_compact")) return "compact";
        if (strstr(target, "polybar_underline")) {
            char mod_target[512] = {0};
            char mod_path[512];
            snprintf(mod_path, sizeof(mod_path), "%s/.config/polybar/modules.ini", home ? home : "");
            ssize_t mlen = readlink(mod_path, mod_target, sizeof(mod_target) - 1);
            if (mlen > 0) {
                mod_target[mlen] = '\0';
                if (strstr(mod_target, "modules_solid")) return "bloques";
                if (strstr(mod_target, "modules_underline")) return "minimalista";
            }
            char env_path[512];
            snprintf(env_path, sizeof(env_path), "%s/.config/i3dots/core/state/i3dots/bar/state.env", home ? home : "");
            FILE *f_env = fopen(env_path, "r");
            if (f_env) {
                char line[256];
                while (fgets(line, sizeof(line), f_env)) {
                    if (strncmp(line, "mode=", 5) == 0) {
                        if (strstr(line, "solid")) { fclose(f_env); return "bloques"; }
                        if (strstr(line, "underline")) { fclose(f_env); return "minimalista"; }
                    }
                }
                fclose(f_env);
            }
            return "minimalista";
        }
    }
    snprintf(path, sizeof(path), "%s/.config/i3dots/core/state/i3dots/bar/state.env", home ? home : "");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        char bar_type[256] = {0};
        char bar_mode[256] = {0};
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "type=", 5) == 0) {
                char *p = line + 5;
                while (*p == '"' || *p == '\'') p++;
                snprintf(bar_type, sizeof(bar_type), "%s", p);
                char *end = strpbrk(bar_type, "\"\';\r\n ");
                if (end) *end = '\0';
            } else if (strncmp(line, "mode=", 5) == 0) {
                char *p = line + 5;
                while (*p == '"' || *p == '\'') p++;
                snprintf(bar_mode, sizeof(bar_mode), "%s", p);
                char *end = strpbrk(bar_mode, "\"\';\r\n ");
                if (end) *end = '\0';
            }
        }
        fclose(f);
        if (strstr(bar_type, "polybar_antigua")) return "antigua";
        if (strstr(bar_type, "polybar_floating")) return "floating";
        if (strstr(bar_type, "polybar_compact")) return "compact";
        if (strstr(bar_type, "polybar_underline")) {
            if (strstr(bar_mode, "solid")) return "bloques";
            return "minimalista";
        }
    }
    return "default";
}

static int print_status_cli(const char *forced_style) {
    bool is_dnd = check_dnd_status();

    int count = 0;
    char hist_file[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), NULL, 0);
    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (root) {
        json_t *entries = json_object_get(root, "entries");
        count = (entries && json_is_array(entries)) ? (int)json_array_size(entries) : 0;
        json_decref(root);
    }

    const char *icon = is_dnd ? "\xef\x87\xb6" : "\xef\x83\xb3"; /*  (bell-slash) or  (bell) */
    const char *style = (forced_style && *forced_style) ? forced_style : detect_bar_style();

    char col_primary[32], col_green[32], col_bg_solid[32], col_fg[32], col_red[32], col_orange[32];
    get_polybar_color("primary", col_primary, sizeof(col_primary), "#e6d5ff");
    get_polybar_color("green", col_green, sizeof(col_green), col_primary);
    get_polybar_color("background-solid", col_bg_solid, sizeof(col_bg_solid), "#1f1c24");
    get_polybar_color("foreground", col_fg, sizeof(col_fg), "#ffffff");
    get_polybar_color("red", col_red, sizeof(col_red), "#ffd2cc");
    get_polybar_color("orange", col_orange, sizeof(col_orange), "#ffd5ac");

    if (strcmp(style, "floating") == 0) {
        /* Barra flotante: icono con recuadro coloreado y valor con recuadro solido */
        const char *box_bg = is_dnd ? col_orange : col_green;
        if (count > 0) {
            printf("%%{B%s}%%{F%s}%%{T5} %s %%{T-}%%{B%s}%%{F%s}%%{T1} %d %%{B-}%%{F-}\n",
                   box_bg, col_bg_solid, icon,
                   col_bg_solid, col_fg, count);
        } else {
            printf("%%{B%s}%%{F%s}%%{T5} %s %%{T-}%%{B-}%%{F-}\n",
                   box_bg, col_bg_solid, icon);
        }
    } else if (strcmp(style, "antigua") == 0) {
        /* Barra de pastillas: fondo continuo background-alt, icono coloreado primary/red, valor en foreground */
        const char *icon_color = is_dnd ? col_red : col_primary;
        if (count > 0) {
            printf("%%{F%s}%s%%{F-} %d\n", icon_color, icon, count);
        } else {
            printf("%%{F%s}%s%%{F-}\n", icon_color, icon);
        }
    } else if (strcmp(style, "bloques") == 0) {
        /* Barra Bloques: icono con recuadro coloreado y valor sobre fondo solido de la barra */
        const char *box_bg = is_dnd ? col_orange : col_primary;
        if (count > 0) {
            printf("%%{B%s}%%{F%s}%%{T5} %s %%{T-}%%{B-}%%{F%s}%%{T1} %d %%{F-}\n",
                   box_bg, col_bg_solid, icon,
                   col_fg, count);
        } else {
            printf("%%{B%s}%%{F%s}%%{T5} %s %%{T-}%%{B-}%%{F-}\n",
                   box_bg, col_bg_solid, icon);
        }
    } else if (strcmp(style, "minimalista") == 0 || strcmp(style, "underline") == 0) {
        /* Barra Minimalista: icono sin recuadro coloreado, con subrayado nativo de polybar */
        const char *icon_color = is_dnd ? col_red : col_primary;
        const char *und_color = is_dnd ? col_red : col_primary;
        if (count > 0) {
            printf("%%{U%s}%%{F%s}%%{T5}%s%%{T-}%%{F-} %d \n",
                   und_color, icon_color, icon, count);
        } else {
            printf("%%{U%s}%%{F%s}%%{T5} %s %%{T-}%%{F-}\n",
                   und_color, icon_color, icon);
        }
    } else {
        /* Default / Compacto */
        if (count > 0) {
            printf("%s %d\n", icon, count);
        } else {
            printf("%s\n", icon);
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "--toggle-dnd") == 0) {
            bool dnd = send_dbus_toggle_dnd();
            printf("[OK] DND alternado: %s\n", dnd ? "activo" : "inactivo");
            return 0;
        } else if (strcmp(argv[1], "--get-dnd") == 0) {
            printf("%s\n", check_dnd_status() ? "true" : "false");
            return 0;
        } else if (strcmp(argv[1], "--clear-all") == 0) {
            clear_all_history_file();
            return 0;
        } else if (strcmp(argv[1], "--close-all") == 0) {
            send_dbus_close_all();
            return 0;
        } else if (strcmp(argv[1], "--status") == 0 || strcmp(argv[1], "-s") == 0) {
            const char *style = NULL;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--style") == 0 && i + 1 < argc) {
                    style = argv[++i];
                } else if (strncmp(argv[i], "--style=", 8) == 0) {
                    style = argv[i] + 8;
                }
            }
            ensure_daemon_running();
            return print_status_cli(style);
        }
    }

    ensure_daemon_running();

    int64_t now_ms = get_monotonic_ms();

    int lock_fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        fprintf(stderr, "[ERROR] Cannot open lock file\n");
        return 1;
    }

    /* Try non-blocking exclusive lock */
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        char buf[64] = {0};
        lseek(lock_fd, 0, SEEK_SET);
        ssize_t n = read(lock_fd, buf, sizeof(buf) - 1);
        close(lock_fd);

        if (n > 0) {
            buf[n] = '\0';
            pid_t old_pid = 0;
            long start_ms = 0;
            if (sscanf(buf, "%d %ld", &old_pid, &start_ms) >= 1 && old_pid > 1) {
                if (start_ms > 0 && (now_ms - (int64_t)start_ms) >= 0 && (now_ms - (int64_t)start_ms) < DEBOUNCE_MS) {
                    return 0;
                }

                if (kill(old_pid, 0) == 0) {
                    kill(old_pid, SIGTERM);
                    for (int i = 0; i < 30; i++) {
                        if (kill(old_pid, 0) != 0) break;
                        usleep(10000);
                    }
                }

                char boot_id[64] = {0};
                get_current_boot_id(boot_id, sizeof(boot_id));
                FILE *cf = fopen(CLOSE_STAMP_FILE, "w");
                if (cf) {
                    fprintf(cf, "%s %ld\n", boot_id, (long)get_monotonic_ms());
                    fclose(cf);
                }
            }
        }
        return 0;
    }

    /* Check if recent close occurred within debounce window (verify boot_id to prevent reboot false positives) */
    FILE *cf = fopen(CLOSE_STAMP_FILE, "r");
    if (cf) {
        char f_boot[64] = {0};
        long closed_ms = 0;
        char cur_boot[64] = {0};
        get_current_boot_id(cur_boot, sizeof(cur_boot));
        if (fscanf(cf, "%63s %ld", f_boot, &closed_ms) == 2) {
            if (strcmp(f_boot, cur_boot) == 0 &&
                (now_ms - (int64_t)closed_ms) >= 0 &&
                (now_ms - (int64_t)closed_ms) < DEBOUNCE_MS) {
                fclose(cf);
                close(lock_fd);
                return 0;
            }
        }
        fclose(cf);
    }

    ftruncate(lock_fd, 0);
    lseek(lock_fd, 0, SEEK_SET);
    dprintf(lock_fd, "%d %ld\n", getpid(), (long)now_ms);

    if (pipe(g_sig_pipe) != 0) {
        fprintf(stderr, "[ERROR] Cannot create signal pipe\n");
        close(lock_fd);
        return 1;
    }
    fcntl(g_sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sig_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(g_sig_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_sig_pipe[1], F_SETFD, FD_CLOEXEC);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        const char *home = getenv("HOME");
        if (home) {
            char dir_path[512];
            snprintf(dir_path, sizeof(dir_path), "%s/.config/polybar", home);
            inotify_add_watch(inotify_fd, dir_path, IN_CLOSE_WRITE | IN_MOVED_TO);
            snprintf(dir_path, sizeof(dir_path), "%s/.config/notif_center", home);
            inotify_add_watch(inotify_fd, dir_path, IN_CLOSE_WRITE | IN_MOVED_TO);
            snprintf(dir_path, sizeof(dir_path), "%s/.config/i3dots/core/state/i3dots/notifications", home);
            inotify_add_watch(inotify_fd, dir_path, IN_CLOSE_WRITE | IN_MOVED_TO);
        }
    }

    memset(&g_app, 0, sizeof(AppState));
    g_app.running = true;

    load_config(&g_app.cfg);

    g_app.dpy = XOpenDisplay(NULL);
    if (!g_app.dpy) {
        fprintf(stderr, "[ERROR] Cannot open X display\n");
        if (inotify_fd >= 0) close(inotify_fd);
        close(lock_fd);
        return 1;
    }

    g_app.screen = DefaultScreen(g_app.dpy);
    g_app.root = RootWindow(g_app.dpy, g_app.screen);
    g_app.screen_w = DisplayWidth(g_app.dpy, g_app.screen);
    g_app.screen_h = DisplayHeight(g_app.dpy, g_app.screen);

    load_theme(&g_app.theme);

    calculate_panel_geometry(&g_app);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | Button1MotionMask | StructureNotifyMask | LeaveWindowMask | FocusChangeMask;

    g_app.win = XCreateWindow(
        g_app.dpy, g_app.root,
        g_app.panel_x, g_app.panel_y, g_app.panel_w, g_app.panel_h, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask, &swa
    );

    XClassHint class_hint;
    class_hint.res_name = "notif_center";
    class_hint.res_class = "notif_center";
    XSetClassHint(g_app.dpy, g_app.win, &class_hint);

    XStoreName(g_app.dpy, g_app.win, "notif_center");

    Atom net_wm_type = XInternAtom(g_app.dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom net_wm_type_dock = XInternAtom(g_app.dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(g_app.dpy, g_app.win, net_wm_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&net_wm_type_dock, 1);

    Atom wm_delete = XInternAtom(g_app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_app.dpy, g_app.win, &wm_delete, 1);

    g_app.cairo_surface = cairo_xlib_surface_create(
        g_app.dpy, g_app.win, DefaultVisual(g_app.dpy, g_app.screen),
        g_app.panel_w, g_app.panel_h
    );
    g_app.buffer_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g_app.panel_w, g_app.panel_h);
    g_app.cr = cairo_create(g_app.buffer_surface);

    init_pango_cache(&g_app);

    /* Pre-load and render all notification cards to buffer before mapping */
    g_app.is_dnd = check_dnd_status();
    reload_notifications(&g_app);
    render_panel(&g_app);

    /* Copy rendered buffer directly onto the X11 surface so frame 0 is ready */
    cairo_t *cr_win = cairo_create(g_app.cairo_surface);
    cairo_set_source_surface(cr_win, g_app.buffer_surface, 0, 0);
    cairo_paint(cr_win);
    cairo_destroy(cr_win);
    cairo_surface_flush(g_app.cairo_surface);
    XFlush(g_app.dpy);

    /* Map the fully rendered window: compositor starts slide-in immediately */
    XMapRaised(g_app.dpy, g_app.win);
    XSetInputFocus(g_app.dpy, g_app.win, RevertToParent, CurrentTime);

    send_dbus_close_all();

    struct pollfd pfds[3];
    pfds[0].fd = ConnectionNumber(g_app.dpy);
    pfds[0].events = POLLIN;
    pfds[1].fd = g_sig_pipe[0];
    pfds[1].events = POLLIN;
    pfds[2].fd = inotify_fd;
    pfds[2].events = (inotify_fd >= 0) ? POLLIN : 0;

    XEvent ev;
    while (g_app.running) {
        while (XPending(g_app.dpy)) {
            XNextEvent(g_app.dpy, &ev);

            /* Compress motion events */
            if (ev.type == MotionNotify) {
                while (XCheckTypedEvent(g_app.dpy, MotionNotify, &ev));
            }

            if (ev.type == Expose) {
                if (ev.xexpose.count == 0) {
                    render_panel(&g_app);
                }
            } else if (ev.type == KeyPress) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape || sym == XK_q) {
                    g_app.running = false;
                } else if (sym == XK_Down || sym == XK_j) {
                    g_app.scroll_offset += 45;
                    if (g_app.scroll_offset > g_app.max_scroll) g_app.scroll_offset = g_app.max_scroll;
                    trigger_redraw(&g_app);
                } else if (sym == XK_Up || sym == XK_k) {
                    g_app.scroll_offset -= 45;
                    if (g_app.scroll_offset < 0) g_app.scroll_offset = 0;
                    trigger_redraw(&g_app);
                }
            } else if (ev.type == ButtonPress) {
                int rx = ev.xbutton.x_root;
                int ry = ev.xbutton.y_root;
                int lx = rx - g_app.panel_x;
                int ly = ry - g_app.panel_y;
                on_button_press(&g_app, ev.xbutton.button, lx, ly);
            } else if (ev.type == ButtonRelease) {
                int rx = ev.xbutton.x_root;
                int ry = ev.xbutton.y_root;
                int lx = rx - g_app.panel_x;
                int ly = ry - g_app.panel_y;
                on_button_release(&g_app, ev.xbutton.button, lx, ly);
            } else if (ev.type == MotionNotify) {
                int rx = ev.xmotion.x_root;
                int ry = ev.xmotion.y_root;
                int lx = rx - g_app.panel_x;
                int ly = ry - g_app.panel_y;
                on_motion(&g_app, lx, ly);
            } else if (ev.type == LeaveNotify) {
                clear_hover_states(&g_app);
                render_panel(&g_app);
            } else if (ev.type == FocusOut) {
                if (ev.xfocus.mode == NotifyNormal) {
                    if ((get_monotonic_ms() - now_ms) < 600) {
                        /* Ignore initial startup focus fluctuations */
                        continue;
                    }
                    if ((get_monotonic_ms() - g_last_notif_time) < 600) {
                        /* Ignore transient focus shifts caused by background notification popups */
                        continue;
                    }
                    Window root_r, child_r;
                    int rx, ry, wx, wy;
                    unsigned int mask = 0;
                    if (XQueryPointer(g_app.dpy, g_app.root, &root_r, &child_r, &rx, &ry, &wx, &wy, &mask)) {
                        bool pointer_inside = (rx >= g_app.panel_x && rx <= g_app.panel_x + g_app.panel_w &&
                                               ry >= g_app.panel_y && ry <= g_app.panel_y + g_app.panel_h);
                        if (pointer_inside) {
                            continue;
                        }
                        /* Only close when pointer is outside AND user clicked a mouse button */
                        if (mask & (Button1Mask | Button2Mask | Button3Mask)) {
                            g_app.running = false;
                        }
                    }
                }
            } else if (ev.type == ClientMessage) {
                Atom wm_del = XInternAtom(g_app.dpy, "WM_DELETE_WINDOW", False);
                if ((Atom)ev.xclient.data.l[0] == wm_del) {
                    g_app.running = false;
                }
            }

            if (!g_app.running) break;
        }

        if (!g_app.running) break;

        XFlush(g_app.dpy);

        int pret = poll(pfds, (inotify_fd >= 0) ? 3 : 2, 5000);
        if (pret > 0) {
            if (pfds[1].revents & POLLIN) {
                char b[32];
                ssize_t n = read(g_sig_pipe[0], b, sizeof(b));
                bool should_quit = false;
                bool should_reload = false;
                bool should_reload_notifs = false;
                for (ssize_t i = 0; i < n; i++) {
                    if (b[i] == 'Q') should_quit = true;
                    else if (b[i] == 'R') should_reload = true;
                    else if (b[i] == 'N') should_reload_notifs = true;
                }
                if (should_quit) {
                    g_app.running = false;
                    break;
                }
                if (should_reload) {
                    reload_theme_and_config(&g_app);
                }
                if (should_reload_notifs) {
                    g_last_notif_time = get_monotonic_ms();
                    reload_notifications(&g_app);
                    trigger_redraw(&g_app);
                    XSetInputFocus(g_app.dpy, g_app.win, RevertToParent, CurrentTime);
                }
            }
            if (inotify_fd >= 0 && (pfds[2].revents & POLLIN)) {
                char ibuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                bool reload_needed = false;
                bool reload_notifs_needed = false;
                while ((len = read(inotify_fd, ibuf, sizeof(ibuf))) > 0) {
                    for (char *ptr = ibuf; ptr < ibuf + len; ) {
                        const struct inotify_event *event = (const struct inotify_event *)ptr;
                        if (event->len > 0) {
                            if (strcmp(event->name, "colors.ini") == 0 ||
                                strcmp(event->name, "config.ini") == 0) {
                                reload_needed = true;
                            } else if (strcmp(event->name, "history.json") == 0) {
                                reload_notifs_needed = true;
                            }
                        }
                        ptr += sizeof(struct inotify_event) + event->len;
                    }
                }
                if (reload_needed) {
                    reload_theme_and_config(&g_app);
                }
                if (reload_notifs_needed) {
                    if (g_self_updating_history > 0) {
                        g_self_updating_history--;
                    } else {
                        reload_notifications(&g_app);
                        trigger_redraw(&g_app);
                    }
                }
            }
        } else if (pret == 0) {
            /* Periodic check: verify if DND status changed */
            bool dnd = check_dnd_status();
            if (dnd != g_app.is_dnd) {
                g_app.is_dnd = dnd;
                trigger_redraw(&g_app);
            }
        }
    }

    char boot_id[64] = {0};
    get_current_boot_id(boot_id, sizeof(boot_id));
    FILE *cf_exit = fopen(CLOSE_STAMP_FILE, "w");
    if (cf_exit) {
        fprintf(cf_exit, "%s %ld\n", boot_id, (long)get_monotonic_ms());
        fclose(cf_exit);
    }

    if (lock_fd >= 0) {
        close(lock_fd);
        lock_fd = -1;
    }
    unlink(LOCK_FILE);

    XUnmapWindow(g_app.dpy, g_app.win);
    XFlush(g_app.dpy);
    usleep(90000);

    free_card_surfaces(&g_app);
    destroy_surface_cache();
    destroy_pango_cache(&g_app);

    cairo_destroy(g_app.cr);
    cairo_surface_destroy(g_app.buffer_surface);
    cairo_surface_destroy(g_app.cairo_surface);
    XDestroyWindow(g_app.dpy, g_app.win);
    XCloseDisplay(g_app.dpy);

    if (inotify_fd >= 0) close(inotify_fd);
    if (g_sig_pipe[0] >= 0) close(g_sig_pipe[0]);
    if (g_sig_pipe[1] >= 0) close(g_sig_pipe[1]);
    if (lock_fd >= 0) close(lock_fd);

    return 0;
}
