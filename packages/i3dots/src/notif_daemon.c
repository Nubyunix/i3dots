#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <png.h>
#include <jansson.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <stdbool.h>
#include <poll.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define DAEMON_PID_FILE "/tmp/notif_daemon.pid"
#define DEFAULT_MAX_HISTORY 500
#define MAX_ACTIVE_TOASTS 4
#define TOAST_W 380
#define TOAST_GAP 10
#define DEFAULT_TIMEOUT_MS 5000

/* Global running flag */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_requested = 0;

static void handle_signal(int sig) {
    if (sig == SIGUSR1) {
        g_reload_requested = 1;
    } else {
        g_running = 0;
    }
}

/* Color structure */
typedef struct {
    double r, g, b, a;
} ColorRGBA;

static ColorRGBA hex_to_color(const char *hex, double alpha) {
    ColorRGBA c = {0.0, 0.0, 0.0, alpha};
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return c;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        c.r = r / 255.0;
        c.g = g / 255.0;
        c.b = b / 255.0;
    }
    return c;
}

typedef struct {
    ColorRGBA bg_card;
    ColorRGBA bg_card_hover;
    ColorRGBA bg_card_urgent;
    ColorRGBA fg_text;
    ColorRGBA fg_muted;
    ColorRGBA accent;
    ColorRGBA alert;
    ColorRGBA border;
    int card_radius;
    int border_width;
    char font_small[128];
    char font_bold[128];
    char font_normal[128];
    int max_entries;
} ThemeConfig;

static ThemeConfig g_theme;
static bool g_dnd = false;

/* Path helpers */
static void get_notif_paths(char *state_dir, size_t dir_len, char *hist_file, size_t hist_len, char *assets_dir, size_t assets_len) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";
    if (state_dir) snprintf(state_dir, dir_len, "%s/.config/i3dots/core/state/i3dots/notifications", home);
    if (hist_file) snprintf(hist_file, hist_len, "%s/.config/i3dots/core/state/i3dots/notifications/history.json", home);
    if (assets_dir) snprintf(assets_dir, assets_len, "%s/.config/i3dots/core/state/i3dots/notifications/assets", home);
}

static void get_dnd_state_path(char *out_path, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";
    snprintf(out_path, out_len, "%s/.config/i3dots/core/state/i3dots/notifications/dnd.state", home);
}

static void save_dnd_state(bool dnd) {
    char path[512];
    get_dnd_state_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", dnd ? "true" : "false");
        fclose(f);
    }
}

static bool load_dnd_state(void) {
    char path[512];
    get_dnd_state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            return (strncmp(buf, "true", 4) == 0 || buf[0] == '1');
        }
        fclose(f);
    }
    return false;
}

static void load_theme_and_config(ThemeConfig *th) {
    th->bg_card = hex_to_color("#262b26", 1.0);
    th->bg_card_hover = hex_to_color("#353a35", 1.0);
    th->bg_card_urgent = hex_to_color("#4a282c", 1.0);
    th->fg_text = hex_to_color("#dfe4dc", 1.0);
    th->fg_muted = hex_to_color("#8b938a", 1.0);
    th->accent = hex_to_color("#97d5a5", 1.0);
    th->alert = hex_to_color("#ffb4ab", 1.0);
    th->border = hex_to_color("#414941", 0.9);
    th->card_radius = 0;
    th->border_width = 1;
    strcpy(th->font_small, "JetBrainsMono Nerd Font 8");
    strcpy(th->font_bold, "JetBrainsMono Nerd Font Bold 10");
    strcpy(th->font_normal, "JetBrainsMono Nerd Font 9");
    th->max_entries = DEFAULT_MAX_HISTORY;

    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";

    /* 1. Inherit from Polybar colors */
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/polybar/colors.ini", home);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[64];
            if (sscanf(line, "%63s = %63s", key, val) == 2) {
                if (strcmp(key, "background-alt") == 0) {
                    th->bg_card = hex_to_color(val, 1.0);
                    th->bg_card_hover = hex_to_color(val, 1.0);
                    th->bg_card_hover.r = fmin(1.0, th->bg_card_hover.r + 0.08);
                    th->bg_card_hover.g = fmin(1.0, th->bg_card_hover.g + 0.08);
                    th->bg_card_hover.b = fmin(1.0, th->bg_card_hover.b + 0.08);
                } else if (strcmp(key, "gray0") == 0) th->bg_card_hover = hex_to_color(val, 1.0);
                else if (strcmp(key, "gray1") == 0) th->border = hex_to_color(val, 0.9);
                else if (strcmp(key, "foreground") == 0) th->fg_text = hex_to_color(val, 1.0);
                else if (strcmp(key, "primary") == 0) th->accent = hex_to_color(val, 1.0);
                else if (strcmp(key, "alert") == 0 || strcmp(key, "red") == 0) th->alert = hex_to_color(val, 1.0);
                else if (strcmp(key, "disabled") == 0) th->fg_muted = hex_to_color(val, 1.0);
            }
        }
        fclose(f);
    }

    /* 2. Read notif_center config.ini overrides */
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
            char key[64], val[128];
            if (sscanf(p, "%63[^= \t] = %127[^\r\n]", key, val) == 2) {
                if (in_colors && val[0] == '#') {
                    if (strcmp(key, "bg_card") == 0) th->bg_card = hex_to_color(val, 1.0);
                    else if (strcmp(key, "bg_card_hover") == 0) th->bg_card_hover = hex_to_color(val, 1.0);
                    else if (strcmp(key, "border") == 0) th->border = hex_to_color(val, 0.9);
                    else if (strcmp(key, "accent") == 0) th->accent = hex_to_color(val, 1.0);
                    else if (strcmp(key, "fg_text") == 0) th->fg_text = hex_to_color(val, 1.0);
                    else if (strcmp(key, "fg_muted") == 0) th->fg_muted = hex_to_color(val, 1.0);
                } else {
                    if (strcmp(key, "card_radius") == 0) th->card_radius = atoi(val);
                    else if (strcmp(key, "border_width") == 0) th->border_width = atoi(val);
                    else if (strcmp(key, "max_entries") == 0 || strcmp(key, "max_history") == 0) {
                        int v = atoi(val);
                        if (v >= 50 && v <= 5000) th->max_entries = v;
                    } else if (strcmp(key, "font_bold") == 0) {
                        snprintf(th->font_bold, sizeof(th->font_bold), "%s", val);
                    } else if (strcmp(key, "font_normal") == 0) {
                        snprintf(th->font_normal, sizeof(th->font_normal), "%s", val);
                    } else if (strcmp(key, "font_small") == 0) {
                        snprintf(th->font_small, sizeof(th->font_small), "%s", val);
                    }
                }
            }
        }
        fclose(f);
    }
}

/* Polybar Geometry Detection */
typedef enum {
    BAR_POS_UNKNOWN = 0,
    BAR_POS_TOP = 1,
    BAR_POS_BOTTOM = 2
} BarPosition;

typedef struct {
    bool found;
    BarPosition pos;
    int bar_y;
    int bar_h;
} DetectedBar;

static void find_polybar_window_rec(Display *dpy, Window root, Window w, int screen_h, DetectedBar *bar, int depth) {
    if (depth > 6 || bar->found) return;

    XClassHint ch;
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_name && strcasecmp(ch.res_name, "polybar") == 0) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, w, &wa) && wa.map_state == IsViewable) {
                int wx = 0, wy = 0;
                Window child;
                XTranslateCoordinates(dpy, w, root, 0, 0, &wx, &wy, &child);
                bar->found = true;
                bar->bar_y = wy;
                bar->bar_h = wa.height;
                bar->pos = (wy + wa.height / 2 > screen_h / 2) ? BAR_POS_BOTTOM : BAR_POS_TOP;
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

/* PNG avatar saving and deduplication */
static int write_png_file(const char *filename, int width, int height, int channels, const unsigned char *data, int stride) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return 0;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return 0; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }

    png_init_io(png, fp);
    int color_type = (channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
    png_set_IHDR(png, info, width, height, 8, color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);

    png_bytep *rows = (png_bytep *)malloc(height * sizeof(png_bytep));
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }

    for (int y = 0; y < height; y++) {
        rows[y] = (png_bytep)(data + y * stride);
    }

    png_write_image(png, rows);
    png_write_end(png, NULL);

    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

static unsigned long long hash_image_bytes(const unsigned char *data, size_t len) {
    unsigned long long h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned long long)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void save_avatar(int w, int h, int channels, const unsigned char *data, int stride, char *out_name, size_t out_len) {
    char assets_dir[512], cache_dir[512];
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";

    snprintf(assets_dir, sizeof(assets_dir), "%s/.config/i3dots/core/state/i3dots/notifications/assets", home);
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/notif_center/avatars", home);
    mkdir(assets_dir, 0755);
    mkdir(cache_dir, 0755);

    size_t total_bytes = (size_t)h * stride;
    unsigned long long img_hash = hash_image_bytes(data, total_bytes);

    char fname[64];
    snprintf(fname, sizeof(fname), "%016llx.png", img_hash);

    char p1[768], p1_cache[768];
    snprintf(p1, sizeof(p1), "%s/%s", assets_dir, fname);
    snprintf(p1_cache, sizeof(p1_cache), "%s/%s", cache_dir, fname);

    if (access(p1, R_OK) != 0) {
        write_png_file(p1, w, h, channels, data, stride);
    }
    if (access(p1_cache, R_OK) != 0) {
        write_png_file(p1_cache, w, h, channels, data, stride);
    }

    if (out_name && out_len > 0) {
        snprintf(out_name, out_len, "%s", fname);
    }
}

static void prune_all_orphaned_assets(void) {
    char sdir[512], hist_file[512], adir[512];
    get_notif_paths(sdir, sizeof(sdir), hist_file, sizeof(hist_file), adir, sizeof(adir));

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        return;
    }

    json_t *entries = json_object_get(root, "entries");
    if (!entries || !json_is_array(entries)) {
        json_decref(root);
        return;
    }

    DIR *d = opendir(adir);
    if (!d) {
        json_decref(root);
        return;
    }

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        const char *ext = strrchr(dir->d_name, '.');
        if (!ext || strcmp(ext, ".png") != 0) continue;

        bool found = false;
        for (size_t i = 0; i < json_array_size(entries); i++) {
            json_t *item = json_array_get(entries, i);
            json_t *j_av = json_object_get(item, "avatar");
            if (j_av && json_is_string(j_av) && strcmp(json_string_value(j_av), dir->d_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            char path[768];
            snprintf(path, sizeof(path), "%s/%s", adir, dir->d_name);
            unlink(path);
        }
    }

    closedir(d);
    json_decref(root);
}

/* Icon resolution helper */
static void resolve_icon_path(const char *name, char *out, size_t out_len) {
    if (!name || name[0] == '\0') {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    if (access(name, R_OK) == 0) {
        snprintf(out, out_len, "%s", name);
        return;
    }

    char low_name[128];
    size_t len = strlen(name);
    if (len >= sizeof(low_name)) len = sizeof(low_name) - 1;
    for (size_t i = 0; i < len; i++) low_name[i] = (char)tolower((unsigned char)name[i]);
    low_name[len] = '\0';

    const char *candidates[] = {
        "/usr/share/pixmaps/%s.png",
        "/usr/share/pixmaps/%s.svg",
        "/usr/share/icons/Papirus/48x48/apps/%s.svg",
        "/usr/share/icons/Papirus/32x32/apps/%s.svg",
        "/usr/share/icons/Papirus/24x24/apps/%s.svg",
        "/usr/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/icons/hicolor/48x48/apps/%s.png",
        "/usr/share/icons/hicolor/128x128/apps/%s.png",
        NULL
    };

    char buf[512];
    for (int i = 0; candidates[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), candidates[i], low_name);
        if (access(buf, R_OK) == 0) {
            snprintf(out, out_len, "%s", buf);
            return;
        }
    }
    if (out && out_len > 0) out[0] = '\0';
}

/* Helper to detect generic URI scheme start (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) "://") */
static bool is_generic_uri_start(const char *p) {
    if (!p || !isalpha((unsigned char)*p)) return false;
    const char *s = p + 1;
    while (*s && (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.')) {
        s++;
    }
    size_t scheme_len = (size_t)(s - p);
    if (scheme_len < 2 || scheme_len > 32) return false;

    /* Generic schemes followed by "://" (e.g. http://, https://, spotify://, steam://, tg://, discord://) */
    if (s[0] == ':' && s[1] == '/' && s[2] == '/') return true;

    /* Well-known opaque schemes followed by ":" (mailto:, magnet:) */
    if (s[0] == ':' && (strncasecmp(p, "mailto:", 7) == 0 || strncasecmp(p, "magnet:", 7) == 0)) return true;

    return false;
}

/* Extract URLs from notification body generically */
static void extract_urls_from_body(const char *body, json_t *j_urls) {
    if (!body || !j_urls) return;
    const char *p = body;
    while (*p) {
        const char *u_start = NULL;
        char quote_char = '\0';

        if (strncasecmp(p, "href=\"", 6) == 0) {
            u_start = p + 6;
            quote_char = '"';
        } else if (strncasecmp(p, "href='", 6) == 0) {
            u_start = p + 6;
            quote_char = '\'';
        } else if (is_generic_uri_start(p)) {
            u_start = p;
            quote_char = '\0';
        }

        if (u_start) {
            const char *u_end = u_start;
            if (quote_char != '\0') {
                while (*u_end && *u_end != quote_char) u_end++;
            } else {
                while (*u_end && !isspace((unsigned char)*u_end) &&
                       *u_end != '"' && *u_end != '\'' && *u_end != '<' && *u_end != '>' &&
                       *u_end != ')' && *u_end != ']') {
                    u_end++;
                }
            }

            size_t ulen = (size_t)(u_end - u_start);

            /* Strip trailing sentence punctuation for raw URLs */
            if (quote_char == '\0') {
                while (ulen > 0) {
                    char last_c = u_start[ulen - 1];
                    if (last_c == '.' || last_c == ',' || last_c == ';' || last_c == '!' || last_c == '?') {
                        ulen--;
                    } else {
                        break;
                    }
                }
            }

            if (ulen > 4 && ulen < 512) {
                char url_buf[512];
                memcpy(url_buf, u_start, ulen);
                url_buf[ulen] = '\0';

                /* Deduplicate URLs */
                bool exists = false;
                for (size_t i = 0; i < json_array_size(j_urls); i++) {
                    json_t *item = json_array_get(j_urls, i);
                    if (json_is_string(item) && strcmp(json_string_value(item), url_buf) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    json_array_append_new(j_urls, json_string(url_buf));
                }
            }
            p = u_end;
        } else {
            p++;
        }
    }
}

static void notify_center_if_running(void) {
    int fd = open("/tmp/notif_center.lock", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        pid_t pid = 0;
        if (sscanf(buf, "%d", &pid) == 1 && pid > 1) {
            if (kill(pid, 0) == 0) {
                kill(pid, SIGUSR2);
            }
        }
    }
}

static bool is_center_currently_visible(void) {
    int fd = open("/tmp/notif_center.lock", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        pid_t pid = 0;
        if (sscanf(buf, "%d", &pid) == 1 && pid > 1) {
            return (kill(pid, 0) == 0);
        }
    }
    return false;
}

/* History Persistence */
static void append_to_history(uint32_t notif_id, const char *sender, const char *app,
                              const char *summary, const char *body, int urgency,
                              const char *icon, const char *avatar,
                              json_t *actions_arr, json_t *hints_obj, json_t *urls_arr) {
    char sdir[512], hist_file[512], adir[512];
    get_notif_paths(sdir, sizeof(sdir), hist_file, sizeof(hist_file), adir, sizeof(adir));
    mkdir(sdir, 0755);
    mkdir(adir, 0755);

    char lock_path[768];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", hist_file);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    json_t *entries = NULL;
    int next_id = 1;

    if (root && json_is_object(root)) {
        json_t *j_nid = json_object_get(root, "next_id");
        if (j_nid && json_is_integer(j_nid)) {
            next_id = (int)json_integer_value(j_nid);
        }
        entries = json_object_get(root, "entries");
        if (!json_is_array(entries)) {
            entries = NULL;
        } else {
            for (size_t i = 0; i < json_array_size(entries); i++) {
                json_t *item = json_array_get(entries, i);
                json_t *jid = json_object_get(item, "id");
                if (jid && json_is_integer(jid)) {
                    int val = (int)json_integer_value(jid);
                    if (val >= next_id) next_id = val + 1;
                }
            }
        }
    } else {
        if (root) json_decref(root);
        root = json_object();
    }

    if (!entries) {
        entries = json_array();
        json_object_set_new(root, "entries", entries);
    }

    time_t now = time(NULL);

    /* Duplicate check: if the very first entry has same app, summary, body within 2s, skip */
    if (json_array_size(entries) > 0) {
        json_t *first = json_array_get(entries, 0);
        if (first && json_is_object(first)) {
            json_t *j_app = json_object_get(first, "appname");
            json_t *j_sum = json_object_get(first, "summary");
            json_t *j_bod = json_object_get(first, "body");
            json_t *j_ts  = json_object_get(first, "timestamp");

            const char *f_app = (j_app && json_is_string(j_app)) ? json_string_value(j_app) : "";
            const char *f_sum = (j_sum && json_is_string(j_sum)) ? json_string_value(j_sum) : "";
            const char *f_bod = (j_bod && json_is_string(j_bod)) ? json_string_value(j_bod) : "";
            time_t f_ts = (j_ts && json_is_integer(j_ts)) ? (time_t)json_integer_value(j_ts) : 0;

            if (strcmp(f_app, app ? app : "") == 0 &&
                strcmp(f_sum, summary ? summary : "") == 0 &&
                strcmp(f_bod, body ? body : "") == 0 &&
                (now - f_ts) < 2) {
                json_decref(root);
                if (lock_fd >= 0) {
                    flock(lock_fd, LOCK_UN);
                    close(lock_fd);
                }
                return;
            }
        }
    }

    int current_id = (notif_id > 0) ? (int)notif_id : next_id++;
    if (current_id >= next_id) {
        next_id = current_id + 1;
    }
    json_object_set_new(root, "next_id", json_integer(next_id));

    json_t *item = json_object();
    json_object_set_new(item, "id", json_integer(current_id));
    if (sender && sender[0]) json_object_set_new(item, "sender", json_string(sender));
    json_object_set_new(item, "appname", json_string(app ? app : "Notificación"));
    json_object_set_new(item, "summary", json_string(summary ? summary : ""));
    json_object_set_new(item, "body", json_string(body ? body : ""));
    json_object_set_new(item, "urgency", json_integer(urgency));
    json_object_set_new(item, "timestamp", json_integer((json_int_t)now));
    if (icon && icon[0]) json_object_set_new(item, "icon_path", json_string(icon));
    if (avatar && avatar[0]) json_object_set_new(item, "avatar", json_string(avatar));
    if (actions_arr) json_object_set(item, "actions", actions_arr);
    if (hints_obj) json_object_set(item, "hints", hints_obj);
    if (urls_arr) json_object_set(item, "urls", urls_arr);

    json_array_insert_new(entries, 0, item);

    int max_entries = g_theme.max_entries;
    bool evicted_any = false;
    while ((int)json_array_size(entries) > max_entries) {
        json_array_remove(entries, json_array_size(entries) - 1);
        evicted_any = true;
    }

    char tmp_file[768];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", hist_file);
    if (json_dump_file(root, tmp_file, JSON_INDENT(2)) == 0) {
        rename(tmp_file, hist_file);
    }

    json_decref(root);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    if (evicted_any) {
        prune_all_orphaned_assets();
    }

    notify_center_if_running();
}

static void delete_notification_entry(uint32_t id) {
    char hist_file[512], assets_dir[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), assets_dir, sizeof(assets_dir));

    char lock_path[768];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", hist_file);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (root && json_is_object(root)) {
        json_t *entries = json_object_get(root, "entries");
        if (entries && json_is_array(entries)) {
            char deleted_avatar[64] = {0};
            for (size_t i = 0; i < json_array_size(entries); i++) {
                json_t *item = json_array_get(entries, i);
                json_t *jid = json_object_get(item, "id");
                if (jid && json_is_integer(jid) && (uint32_t)json_integer_value(jid) == id) {
                    json_t *j_av = json_object_get(item, "avatar");
                    if (j_av && json_is_string(j_av)) {
                        snprintf(deleted_avatar, sizeof(deleted_avatar), "%s", json_string_value(j_av));
                    }
                    json_array_remove(entries, i);
                    break;
                }
            }

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

    notify_center_if_running();
}

static void clear_all_history_and_assets(void) {
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

    DIR *d = opendir(assets_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] != '.') {
                char p[768];
                snprintf(p, sizeof(p), "%s/%s", assets_dir, ent->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }

    notify_center_if_running();
}

/* Active Toasts Graphical System */
typedef struct {
    uint32_t id;
    char sender[64];
    char appname[64];
    char summary[128];
    char body[512];
    char icon_path[256];
    char avatar[64];
    int urgency;
    int timeout_ms;
    int64_t show_time_ms;
    bool hovered;
    bool active;

    Window win;
    Pixmap pixmap;
    int x, y, w, h;
    int close_x, close_y, close_w, close_h;
} ActiveToast;

static ActiveToast g_toasts[MAX_ACTIVE_TOASTS];
static int g_toast_count = 0;
static Display *g_dpy = NULL;
static Window g_root = 0;
static int g_screen = 0;
static int g_screen_w = 0;
static int g_screen_h = 0;

static int64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void draw_rounded_rectangle(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90 * deg, 0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0 * deg, 90 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90 * deg, 180 * deg);
    cairo_arc(cr, x + r, y + r, r, 180 * deg, 270 * deg);
    cairo_close_path(cr);
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

static void set_pango_text_or_markup(PangoLayout *layout, const char *text) {
    if (!text || !*text) {
        pango_layout_set_text(layout, "", 0);
        return;
    }
    PangoAttrList *attrs = NULL;
    char *plain = NULL;
    GError *gerr = NULL;
    if (pango_parse_markup(text, -1, 0, &attrs, &plain, NULL, &gerr)) {
        pango_layout_set_text(layout, plain ? plain : "", -1);
        pango_layout_set_attributes(layout, attrs);
        if (attrs) pango_attr_list_unref(attrs);
        if (plain) g_free(plain);
    } else {
        if (gerr) g_error_free(gerr);
        pango_layout_set_text(layout, text, -1);
    }
}

static void render_toast(ActiveToast *t) {
    if (!t->active || !t->win || !t->pixmap) return;

    cairo_surface_t *surf = cairo_xlib_surface_create(g_dpy, t->pixmap,
                                                     DefaultVisual(g_dpy, g_screen),
                                                     t->w, t->h);
    cairo_t *cr = cairo_create(surf);

    /* Background */
    ColorRGBA bg = (t->urgency == 2) ? g_theme.bg_card_urgent :
                   (t->hovered ? g_theme.bg_card_hover : g_theme.bg_card);

    draw_rounded_rectangle(cr, 0.5, 0.5, t->w - 1.0, t->h - 1.0, g_theme.card_radius);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_fill_preserve(cr);

    /* Border */
    ColorRGBA bcol = (t->urgency == 2) ? g_theme.alert : g_theme.border;
    cairo_set_source_rgba(cr, bcol.r, bcol.g, bcol.b, bcol.a);
    cairo_set_line_width(cr, (double)g_theme.border_width);
    cairo_stroke(cr);

    /* Avatar or Icon */
    char full_icon[768] = {0};
    if (t->avatar[0] != '\0') {
        char adir[512];
        get_notif_paths(NULL, 0, NULL, 0, adir, sizeof(adir));
        snprintf(full_icon, sizeof(full_icon), "%s/%s", adir, t->avatar);
    } else if (t->icon_path[0] != '\0') {
        resolve_icon_path(t->icon_path, full_icon, sizeof(full_icon));
    }
    if (full_icon[0] == '\0' && t->appname[0] != '\0') {
        resolve_icon_path(t->appname, full_icon, sizeof(full_icon));
    }

    int img_size = 46;
    int img_x = 12;
    int img_y = (t->h - img_size) / 2;

    cairo_surface_t *icon_surf = (full_icon[0] != '\0') ? load_image_surface(full_icon, img_size, img_size) : NULL;
    if (icon_surf) {
        cairo_save(cr);
        draw_rounded_rectangle(cr, img_x, img_y, img_size, img_size, 6.0);
        cairo_clip(cr);
        cairo_set_source_surface(cr, icon_surf, img_x, img_y);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(icon_surf);
    } else {
        /* Fallback icon letter badge */
        cairo_save(cr);
        draw_rounded_rectangle(cr, img_x, img_y, img_size, img_size, 6.0);
        cairo_set_source_rgba(cr, g_theme.border.r, g_theme.border.g, g_theme.border.b, 0.4);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, g_theme.accent.r, g_theme.accent.g, g_theme.accent.b, 1.0);
        char badge_str[2] = { (char)toupper((unsigned char)(t->appname[0] ? t->appname[0] : 'N')), '\0' };
        PangoLayout *blayout = pango_cairo_create_layout(cr);
        PangoFontDescription *bfd = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 16");
        pango_layout_set_font_description(blayout, bfd);
        pango_layout_set_text(blayout, badge_str, -1);
        int bw, bh;
        pango_layout_get_pixel_size(blayout, &bw, &bh);
        cairo_move_to(cr, img_x + (img_size - bw) / 2, img_y + (img_size - bh) / 2);
        pango_cairo_show_layout(cr, blayout);
        pango_font_description_free(bfd);
        g_object_unref(blayout);
        cairo_restore(cr);
    }

    /* Text Area */
    int text_x = img_x + img_size + 12;
    int text_max_w = t->w - text_x - 30;

    /* 1. App Name Badge */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *f_small = pango_font_description_from_string(g_theme.font_small);
    pango_layout_set_font_description(layout, f_small);
    pango_layout_set_width(layout, text_max_w * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, t->appname[0] ? t->appname : "Notificación", -1);
    cairo_set_source_rgba(cr, g_theme.fg_muted.r, g_theme.fg_muted.g, g_theme.fg_muted.b, 0.9);
    cairo_move_to(cr, text_x, 10);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(f_small);

    /* 2. Summary */
    PangoFontDescription *f_bold = pango_font_description_from_string(g_theme.font_bold);
    pango_layout_set_font_description(layout, f_bold);
    pango_layout_set_width(layout, text_max_w * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, t->summary[0] ? t->summary : "", -1);
    cairo_set_source_rgba(cr, g_theme.fg_text.r, g_theme.fg_text.g, g_theme.fg_text.b, 1.0);
    cairo_move_to(cr, text_x, 26);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(f_bold);

    /* 3. Body */
    if (t->body[0] != '\0') {
        PangoFontDescription *f_norm = pango_font_description_from_string(g_theme.font_normal);
        pango_layout_set_font_description(layout, f_norm);
        pango_layout_set_width(layout, text_max_w * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_height(layout, 36 * PANGO_SCALE); /* up to 2 lines */

        set_pango_text_or_markup(layout, t->body);

        cairo_set_source_rgba(cr, g_theme.fg_text.r, g_theme.fg_text.g, g_theme.fg_text.b, 0.85);
        cairo_move_to(cr, text_x, 44);
        pango_cairo_show_layout(cr, layout);
        pango_font_description_free(f_norm);
    }
    g_object_unref(layout);

    /* Close Button [x] */
    t->close_w = 20;
    t->close_h = 20;
    t->close_x = t->w - t->close_w - 8;
    t->close_y = 8;

    cairo_set_source_rgba(cr, g_theme.fg_muted.r, g_theme.fg_muted.g, g_theme.fg_muted.b, 0.7);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, t->close_x + 5, t->close_y + 5);
    cairo_line_to(cr, t->close_x + t->close_w - 5, t->close_y + t->close_h - 5);
    cairo_move_to(cr, t->close_x + t->close_w - 5, t->close_y + 5);
    cairo_line_to(cr, t->close_x + 5, t->close_y + t->close_h - 5);
    cairo_stroke(cr);

    /* Flush Cairo to Pixmap and CopyArea to Window */
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    GC gc = XCreateGC(g_dpy, t->win, 0, NULL);
    XCopyArea(g_dpy, t->pixmap, t->win, gc, 0, 0, t->w, t->h, 0, 0);
    XFreeGC(g_dpy, gc);
    XFlush(g_dpy);
}

static void update_toasts_layout(void) {
    if (g_toast_count == 0) return;

    DetectedBar bar = detect_polybar_geometry(g_dpy, g_root, g_screen_h);
    int gap = 12;
    int start_y = (bar.found && bar.pos == BAR_POS_TOP) ? (bar.bar_y + bar.bar_h + gap) : gap;

    for (int i = 0; i < g_toast_count; i++) {
        ActiveToast *t = &g_toasts[i];
        if (!t->active) continue;

        t->x = g_screen_w - t->w - gap;
        if (bar.found && bar.pos == BAR_POS_BOTTOM) {
            /* Stacking upwards from bottom bar */
            int bottom_anchor = bar.bar_y - gap;
            int total_height = 0;
            for (int k = 0; k <= i; k++) {
                total_height += g_toasts[k].h + TOAST_GAP;
            }
            t->y = bottom_anchor - total_height + TOAST_GAP;
        } else {
            /* Stacking downwards from top */
            int offset = 0;
            for (int k = 0; k < i; k++) {
                offset += g_toasts[k].h + TOAST_GAP;
            }
            t->y = start_y + offset;
        }

        XMoveWindow(g_dpy, t->win, t->x, t->y);
        XMapRaised(g_dpy, t->win);
        render_toast(t);
    }
}

static void dismiss_toast_by_index(int index) {
    if (index < 0 || index >= g_toast_count) return;
    ActiveToast *t = &g_toasts[index];
    if (t->active) {
        if (t->win) {
            XUnmapWindow(g_dpy, t->win);
            XDestroyWindow(g_dpy, t->win);
            t->win = 0;
        }
        if (t->pixmap) {
            XFreePixmap(g_dpy, t->pixmap);
            t->pixmap = 0;
        }
        t->active = false;
    }

    for (int i = index; i < g_toast_count - 1; i++) {
        g_toasts[i] = g_toasts[i + 1];
    }
    memset(&g_toasts[g_toast_count - 1], 0, sizeof(ActiveToast));
    g_toast_count--;

    update_toasts_layout();
}

static void dismiss_all_toasts(void) {
    while (g_toast_count > 0) {
        dismiss_toast_by_index(0);
    }
}

static void show_or_update_toast(uint32_t id, const char *sender, const char *app,
                                 const char *summary, const char *body, int urgency,
                                 const char *icon, const char *avatar, int timeout_ms) {
    if (g_dnd || is_center_currently_visible()) return;

    /* Check if toast with this id already exists */
    for (int i = 0; i < g_toast_count; i++) {
        if (g_toasts[i].active && g_toasts[i].id == id) {
            ActiveToast *t = &g_toasts[i];
            if (sender) snprintf(t->sender, sizeof(t->sender), "%s", sender);
            if (app) snprintf(t->appname, sizeof(t->appname), "%s", app);
            if (summary) snprintf(t->summary, sizeof(t->summary), "%s", summary);
            if (body) snprintf(t->body, sizeof(t->body), "%s", body);
            t->urgency = urgency;
            if (icon) snprintf(t->icon_path, sizeof(t->icon_path), "%s", icon);
            if (avatar) snprintf(t->avatar, sizeof(t->avatar), "%s", avatar);
            t->timeout_ms = (timeout_ms >= 0) ? timeout_ms : DEFAULT_TIMEOUT_MS;
            t->show_time_ms = get_monotonic_ms();
            render_toast(t);
            return;
        }
    }

    /* If queue is full, dismiss oldest toast */
    if (g_toast_count >= MAX_ACTIVE_TOASTS) {
        dismiss_toast_by_index(0);
    }

    ActiveToast *t = &g_toasts[g_toast_count];
    memset(t, 0, sizeof(ActiveToast));
    t->id = id;
    if (sender) snprintf(t->sender, sizeof(t->sender), "%s", sender);
    if (app) snprintf(t->appname, sizeof(t->appname), "%s", app);
    if (summary) snprintf(t->summary, sizeof(t->summary), "%s", summary);
    if (body) snprintf(t->body, sizeof(t->body), "%s", body);
    t->urgency = urgency;
    if (icon) snprintf(t->icon_path, sizeof(t->icon_path), "%s", icon);
    if (avatar) snprintf(t->avatar, sizeof(t->avatar), "%s", avatar);
    t->timeout_ms = (timeout_ms > 0) ? timeout_ms : (timeout_ms == 0 ? 0 : DEFAULT_TIMEOUT_MS);
    t->show_time_ms = get_monotonic_ms();
    t->active = true;

    t->w = TOAST_W;
    t->h = (t->body[0] != '\0') ? 88 : 66;

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = 0;
    swa.event_mask = ExposureMask | ButtonPressMask | EnterWindowMask | LeaveWindowMask;

    t->win = XCreateWindow(g_dpy, g_root, 0, 0, t->w, t->h, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWEventMask, &swa);

    XClassHint ch;
    ch.res_name = (char *)"i3dots-notif-toast";
    ch.res_class = (char *)"i3dots-notif-toast";
    XSetClassHint(g_dpy, t->win, &ch);

    t->pixmap = XCreatePixmap(g_dpy, t->win, t->w, t->h,
                              DefaultDepth(g_dpy, g_screen));

    g_toast_count++;
    update_toasts_layout();
}

/* Forward declaration of invoke_action */
static void invoke_action(DBusConnection *conn, uint32_t id, const char *action_key);

/* D-Bus Server Implementation */
static const char *g_introspection_xml =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\">\n"
"      <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
"    </method>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.Notifications\">\n"
"    <method name=\"Notify\">\n"
"      <arg name=\"app_name\" direction=\"in\" type=\"s\"/>\n"
"      <arg name=\"replaces_id\" direction=\"in\" type=\"u\"/>\n"
"      <arg name=\"app_icon\" direction=\"in\" type=\"s\"/>\n"
"      <arg name=\"summary\" direction=\"in\" type=\"s\"/>\n"
"      <arg name=\"body\" direction=\"in\" type=\"s\"/>\n"
"      <arg name=\"actions\" direction=\"in\" type=\"as\"/>\n"
"      <arg name=\"hints\" direction=\"in\" type=\"a{sv}\"/>\n"
"      <arg name=\"expire_timeout\" direction=\"in\" type=\"i\"/>\n"
"      <arg name=\"id\" direction=\"out\" type=\"u\"/>\n"
"    </method>\n"
"    <method name=\"CloseNotification\">\n"
"      <arg name=\"id\" direction=\"in\" type=\"u\"/>\n"
"    </method>\n"
"    <method name=\"GetCapabilities\">\n"
"      <arg name=\"capabilities\" direction=\"out\" type=\"as\"/>\n"
"    </method>\n"
"    <method name=\"GetServerInformation\">\n"
"      <arg name=\"name\" direction=\"out\" type=\"s\"/>\n"
"      <arg name=\"vendor\" direction=\"out\" type=\"s\"/>\n"
"      <arg name=\"version\" direction=\"out\" type=\"s\"/>\n"
"      <arg name=\"spec_version\" direction=\"out\" type=\"s\"/>\n"
"    </method>\n"
"    <method name=\"InvokeAction\">\n"
"      <arg name=\"id\" direction=\"in\" type=\"u\"/>\n"
"      <arg name=\"action_key\" direction=\"in\" type=\"s\"/>\n"
"    </method>\n"
"    <method name=\"CloseAll\"/>\n"
"    <method name=\"GetDND\">\n"
"      <arg name=\"enabled\" direction=\"out\" type=\"b\"/>\n"
"    </method>\n"
"    <method name=\"SetDND\">\n"
"      <arg name=\"enabled\" direction=\"in\" type=\"b\"/>\n"
"    </method>\n"
"    <method name=\"ToggleDND\">\n"
"      <arg name=\"enabled\" direction=\"out\" type=\"b\"/>\n"
"    </method>\n"
"    <method name=\"ReloadTheme\"/>\n"
"    <method name=\"DeleteNotification\">\n"
"      <arg name=\"id\" direction=\"in\" type=\"u\"/>\n"
"    </method>\n"
"    <method name=\"ClearAll\"/>\n"
"    <signal name=\"ActionInvoked\">\n"
"      <arg name=\"id\" type=\"u\"/>\n"
"      <arg name=\"action_key\" type=\"s\"/>\n"
"    </signal>\n"
"    <signal name=\"NotificationClosed\">\n"
"      <arg name=\"id\" type=\"u\"/>\n"
"      <arg name=\"reason\" type=\"u\"/>\n"
"    </signal>\n"
"    <signal name=\"NotificationReplied\">\n"
"      <arg name=\"id\" type=\"u\"/>\n"
"      <arg name=\"text\" type=\"s\"/>\n"
"    </signal>\n"
"  </interface>\n"
"</node>\n";

static DBusHandlerResult handle_dbus_message(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)user_data;

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &g_introspection_xml, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!dbus_message_has_interface(msg, "org.freedesktop.Notifications")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *member = dbus_message_get_member(msg);

    /* Method: Notify */
    if (strcmp(member, "Notify") == 0) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(msg, &iter)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        const char *app = NULL;
        dbus_uint32_t replaces_id = 0;
        const char *app_icon = NULL;
        const char *summary = NULL;
        const char *body = NULL;
        dbus_int32_t expire_timeout = -1;

        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&iter, &app);
        }
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&iter, &replaces_id);
        }
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&iter, &app_icon);
        }
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&iter, &summary);
        }
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&iter, &body);
        }
        dbus_message_iter_next(&iter);

        /* Parse actions */
        json_t *actions_arr = json_array();
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter act_iter;
            dbus_message_iter_recurse(&iter, &act_iter);
            while (dbus_message_iter_get_arg_type(&act_iter) == DBUS_TYPE_STRING) {
                const char *act_str = NULL;
                dbus_message_iter_get_basic(&act_iter, &act_str);
                if (act_str) json_array_append_new(actions_arr, json_string(act_str));
                dbus_message_iter_next(&act_iter);
            }
        }
        dbus_message_iter_next(&iter);

        /* Parse hints */
        int urgency = 1;
        bool is_relaunch = false;
        bool no_history = false;
        char saved_avatar[64] = {0};
        char image_path[256] = {0};
        json_t *hints_obj = json_object();

        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict_iter;
            dbus_message_iter_recurse(&iter, &dict_iter);
            while (dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_INVALID) {
                DBusMessageIter entry_iter;
                dbus_message_iter_recurse(&dict_iter, &entry_iter);
                const char *key = NULL;
                if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&entry_iter, &key);
                }
                dbus_message_iter_next(&entry_iter);

                if (key && dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var_iter;
                    dbus_message_iter_recurse(&entry_iter, &var_iter);
                    int vt = dbus_message_iter_get_arg_type(&var_iter);

                    if (strcmp(key, "urgency") == 0) {
                        if (vt == DBUS_TYPE_BYTE) {
                            unsigned char u = 1;
                            dbus_message_iter_get_basic(&var_iter, &u);
                            urgency = (int)u;
                        } else if (vt == DBUS_TYPE_UINT32 || vt == DBUS_TYPE_INT32) {
                            int u = 1;
                            dbus_message_iter_get_basic(&var_iter, &u);
                            urgency = u;
                        }
                        json_object_set_new(hints_obj, "urgency", json_integer(urgency));
                    } else if (strcmp(key, "x-i3dots-relaunch") == 0) {
                        is_relaunch = true;
                    } else if (strcmp(key, "x-i3dots-no-history") == 0 || strcmp(key, "transient") == 0) {
                        no_history = true;
                        json_object_set_new(hints_obj, key, json_true());
                    } else if (strcmp(key, "image-path") == 0 || strcmp(key, "image_path") == 0) {
                        if (vt == DBUS_TYPE_STRING) {
                            const char *s = NULL;
                            dbus_message_iter_get_basic(&var_iter, &s);
                            if (s) {
                                snprintf(image_path, sizeof(image_path), "%s", s);
                                json_object_set_new(hints_obj, "image-path", json_string(s));
                            }
                        }
                    } else if (strcmp(key, "image-data") == 0 || strcmp(key, "image_data") == 0 || strcmp(key, "icon_data") == 0) {
                        if (vt == DBUS_TYPE_STRUCT) {
                            DBusMessageIter st_iter;
                            dbus_message_iter_recurse(&var_iter, &st_iter);
                            dbus_int32_t w = 0, h = 0, stride = 0, bits = 0, channels = 0;
                            dbus_bool_t has_alpha = 0;
                            dbus_message_iter_get_basic(&st_iter, &w); dbus_message_iter_next(&st_iter);
                            dbus_message_iter_get_basic(&st_iter, &h); dbus_message_iter_next(&st_iter);
                            dbus_message_iter_get_basic(&st_iter, &stride); dbus_message_iter_next(&st_iter);
                            dbus_message_iter_get_basic(&st_iter, &has_alpha); dbus_message_iter_next(&st_iter);
                            dbus_message_iter_get_basic(&st_iter, &bits); dbus_message_iter_next(&st_iter);
                            dbus_message_iter_get_basic(&st_iter, &channels); dbus_message_iter_next(&st_iter);

                            if (dbus_message_iter_get_arg_type(&st_iter) == DBUS_TYPE_ARRAY) {
                                DBusMessageIter arr_iter;
                                dbus_message_iter_recurse(&st_iter, &arr_iter);
                                const unsigned char *data = NULL;
                                int n_bytes = 0;
                                dbus_message_iter_get_fixed_array(&arr_iter, &data, &n_bytes);
                                if (data && w > 0 && h > 0 && n_bytes > 0) {
                                    save_avatar(w, h, channels, data, stride, saved_avatar, sizeof(saved_avatar));
                                    json_object_set_new(hints_obj, "avatar", json_string(saved_avatar));
                                }
                            }
                        }
                    } else if (vt == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var_iter, &s);
                        if (s) json_object_set_new(hints_obj, key, json_string(s));
                    } else if (vt == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t b = 0;
                        dbus_message_iter_get_basic(&var_iter, &b);
                        json_object_set_new(hints_obj, key, b ? json_true() : json_false());
                    } else if (vt == DBUS_TYPE_INT32 || vt == DBUS_TYPE_UINT32) {
                        int v = 0;
                        dbus_message_iter_get_basic(&var_iter, &v);
                        json_object_set_new(hints_obj, key, json_integer(v));
                    }
                }
                dbus_message_iter_next(&dict_iter);
            }
        }
        dbus_message_iter_next(&iter);

        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32) {
            dbus_message_iter_get_basic(&iter, &expire_timeout);
        }

        /* Filter transient system notifications */
        if (app && (strcasecmp(app, "sys_control") == 0 || strcasecmp(app, "sys_control.sh") == 0)) {
            no_history = true;
        }
        if (summary && (strcmp(summary, "Volumen") == 0 ||
                        strcmp(summary, "Volumen (Silenciado)") == 0 ||
                        strcmp(summary, "Brillo") == 0 ||
                        strcmp(summary, "Micrófono") == 0)) {
            no_history = true;
        }

        /* Extract sender name */
        const char *sender = dbus_message_get_sender(msg);

        /* Generate ID */
        static uint32_t s_next_id = 1;
        if (s_next_id == 1) {
            char hist_file[512];
            get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), NULL, 0);
            json_error_t jerr;
            json_t *r = json_load_file(hist_file, 0, &jerr);
            if (r) {
                json_t *jn = json_object_get(r, "next_id");
                if (jn && json_is_integer(jn)) s_next_id = (uint32_t)json_integer_value(jn);
                json_t *entries = json_object_get(r, "entries");
                if (entries && json_is_array(entries)) {
                    for (size_t i = 0; i < json_array_size(entries); i++) {
                        json_t *item = json_array_get(entries, i);
                        json_t *jid = json_object_get(item, "id");
                        if (jid && json_is_integer(jid)) {
                            uint32_t val = (uint32_t)json_integer_value(jid);
                            if (val >= s_next_id) s_next_id = val + 1;
                        }
                    }
                }
                json_decref(r);
            }
        }

        uint32_t assigned_id = (replaces_id > 0) ? replaces_id : s_next_id++;
        if (assigned_id >= s_next_id) s_next_id = assigned_id + 1;

        /* Extract URLs */
        json_t *urls_arr = json_array();
        extract_urls_from_body(body, urls_arr);

        /* Save to history if not transient */
        const char *icon_to_save = (image_path[0] != '\0') ? image_path : app_icon;
        if (!is_relaunch && !no_history) {
            append_to_history(assigned_id, sender, app, summary, body, urgency,
                              icon_to_save, saved_avatar, actions_arr, hints_obj, urls_arr);
        }

        /* Display on screen */
        show_or_update_toast(assigned_id, sender, app, summary, body, urgency,
                             icon_to_save, saved_avatar, (int)expire_timeout);

        json_decref(actions_arr);
        json_decref(hints_obj);
        json_decref(urls_arr);

        /* Reply with ID */
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter r_iter;
        dbus_message_iter_init_append(reply, &r_iter);
        dbus_message_iter_append_basic(&r_iter, DBUS_TYPE_UINT32, &assigned_id);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: CloseNotification */
    if (strcmp(member, "CloseNotification") == 0) {
        dbus_uint32_t id = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
            for (int i = 0; i < g_toast_count; i++) {
                if (g_toasts[i].active && g_toasts[i].id == id) {
                    dismiss_toast_by_index(i);
                    break;
                }
            }
            /* Intentionally do NOT emit NotificationClosed here.
             * Apps (like Chromium/Discord) call CloseNotification when an action is invoked or when timed out.
             * Emitting NotificationClosed causes Chromium to delete the action callback from memory,
             * breaking repeat clicks from the notification center.
             * NotificationClosed is ONLY emitted when explicitly removed from history via DeleteNotification.
             */
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: DeleteNotification (called when permanently deleted from history) */
    if (strcmp(member, "DeleteNotification") == 0) {
        dbus_uint32_t id = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
            for (int i = 0; i < g_toast_count; i++) {
                if (g_toasts[i].active && g_toasts[i].id == id) {
                    dismiss_toast_by_index(i);
                    break;
                }
            }
            delete_notification_entry(id);

            /* Emit signal NotificationClosed (reason 2: dismissed by user) */
            DBusMessage *sig = dbus_message_new_signal("/org/freedesktop/Notifications",
                                                       "org.freedesktop.Notifications",
                                                       "NotificationClosed");
            if (sig) {
                dbus_uint32_t reason = 2;
                DBusMessageIter siter;
                dbus_message_iter_init_append(sig, &siter);
                dbus_message_iter_append_basic(&siter, DBUS_TYPE_UINT32, &id);
                dbus_message_iter_append_basic(&siter, DBUS_TYPE_UINT32, &reason);
                dbus_connection_send(conn, sig, NULL);
                dbus_message_unref(sig);
            }
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: ClearAll */
    if (strcmp(member, "ClearAll") == 0) {
        dismiss_all_toasts();
        clear_all_history_and_assets();
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: GetCapabilities */
    if (strcmp(member, "GetCapabilities") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter r_iter, arr_iter;
        dbus_message_iter_init_append(reply, &r_iter);
        dbus_message_iter_open_container(&r_iter, DBUS_TYPE_ARRAY, "s", &arr_iter);

        const char *caps[] = {
            "actions", "action-icons", "body", "body-hyperlinks",
            "body-images", "body-markup", "icon-multi", "icon-static",
            "inline-reply", "persistence", "sound"
        };
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
            dbus_message_iter_append_basic(&arr_iter, DBUS_TYPE_STRING, &caps[i]);
        }
        dbus_message_iter_close_container(&r_iter, &arr_iter);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: GetServerInformation */
    if (strcmp(member, "GetServerInformation") == 0) {
        const char *name = "i3dots-notifd";
        const char *vendor = "Neko-Void";
        const char *version = "2.0.0";
        const char *spec_version = "1.2";

        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING, &name,
                                 DBUS_TYPE_STRING, &vendor,
                                 DBUS_TYPE_STRING, &version,
                                 DBUS_TYPE_STRING, &spec_version,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: InvokeAction */
    if (strcmp(member, "InvokeAction") == 0) {
        dbus_uint32_t id = 0;
        const char *action_key = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &action_key, DBUS_TYPE_INVALID)) {
            invoke_action(conn, id, action_key ? action_key : "default");
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: CloseAll */
    if (strcmp(member, "CloseAll") == 0) {
        dismiss_all_toasts();
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: GetDND */
    if (strcmp(member, "GetDND") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_bool_t val = g_dnd ? 1 : 0;
        dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: SetDND */
    if (strcmp(member, "SetDND") == 0) {
        dbus_bool_t val = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID)) {
            g_dnd = val ? true : false;
            save_dnd_state(g_dnd);
            if (g_dnd) dismiss_all_toasts();
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: ToggleDND */
    if (strcmp(member, "ToggleDND") == 0) {
        g_dnd = !g_dnd;
        save_dnd_state(g_dnd);
        if (g_dnd) dismiss_all_toasts();

        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_bool_t val = g_dnd ? 1 : 0;
        dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Method: ReloadTheme */
    if (strcmp(member, "ReloadTheme") == 0) {
        load_theme_and_config(&g_theme);
        for (int i = 0; i < g_toast_count; i++) {
            if (g_toasts[i].active) render_toast(&g_toasts[i]);
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void send_unicast_action_to_app(DBusConnection *conn, const char *appname, const char *orig_sender, uint32_t id, const char *act) {
    /* 1. Direct Unicast to original sender if still present */
    if (orig_sender && orig_sender[0] != '\0') {
        DBusMessage *unicast_sig = dbus_message_new_signal("/org/freedesktop/Notifications",
                                                           "org.freedesktop.Notifications",
                                                           "ActionInvoked");
        if (unicast_sig) {
            dbus_message_set_destination(unicast_sig, orig_sender);
            dbus_message_append_args(unicast_sig,
                                     DBUS_TYPE_UINT32, &id,
                                     DBUS_TYPE_STRING, &act,
                                     DBUS_TYPE_INVALID);
            dbus_connection_send(conn, unicast_sig, NULL);
            dbus_message_unref(unicast_sig);
        }
    }

    if (!appname || appname[0] == '\0') return;

    /* 2. Scan active D-Bus connections to discover currently running instances of app (e.g. after restart) */
    DBusMessage *list_msg = dbus_message_new_method_call("org.freedesktop.DBus",
                                                         "/org/freedesktop/DBus",
                                                         "org.freedesktop.DBus",
                                                         "ListNames");
    if (!list_msg) return;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *list_reply = dbus_connection_send_with_reply_and_block(conn, list_msg, 200, &err);
    dbus_message_unref(list_msg);
    if (!list_reply) return;

    char **names = NULL;
    int n_names = 0;
    if (dbus_message_get_args(list_reply, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &names, &n_names, DBUS_TYPE_INVALID)) {
        for (int i = 0; i < n_names; i++) {
            const char *name = names[i];
            if (!name || name[0] != ':' || (orig_sender && strcmp(name, orig_sender) == 0)) {
                continue;
            }

            DBusMessage *pid_msg = dbus_message_new_method_call("org.freedesktop.DBus",
                                                                "/org/freedesktop/DBus",
                                                                "org.freedesktop.DBus",
                                                                "GetConnectionUnixProcessID");
            if (!pid_msg) continue;
            dbus_message_append_args(pid_msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);

            DBusMessage *pid_reply = dbus_connection_send_with_reply_and_block(conn, pid_msg, 50, NULL);
            dbus_message_unref(pid_msg);
            if (!pid_reply) continue;

            dbus_uint32_t pid = 0;
            if (dbus_message_get_args(pid_reply, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID) && pid > 0) {
                char comm_path[64];
                snprintf(comm_path, sizeof(comm_path), "/proc/%u/comm", pid);
                FILE *f = fopen(comm_path, "r");
                if (f) {
                    char comm[64] = {0};
                    if (fgets(comm, sizeof(comm), f)) {
                        char *nl = strchr(comm, '\n');
                        if (nl) *nl = '\0';
                        if (contains_case_insensitive(comm, appname) || contains_case_insensitive(appname, comm)) {
                            DBusMessage *sig = dbus_message_new_signal("/org/freedesktop/Notifications",
                                                                       "org.freedesktop.Notifications",
                                                                       "ActionInvoked");
                            if (sig) {
                                dbus_message_set_destination(sig, name);
                                dbus_message_append_args(sig,
                                                         DBUS_TYPE_UINT32, &id,
                                                         DBUS_TYPE_STRING, &act,
                                                         DBUS_TYPE_INVALID);
                                dbus_connection_send(conn, sig, NULL);
                                dbus_message_unref(sig);
                            }
                        }
                    }
                    fclose(f);
                }
            }
            dbus_message_unref(pid_reply);
        }
        dbus_free_string_array(names);
    }
    dbus_message_unref(list_reply);
}

/* Generic application navigation IPC dispatcher:
 * Probes for an IPC socket exposed by the application:
 *   1. $XDG_RUNTIME_DIR/notif_nav_<app>.sock
 *   2. $XDG_RUNTIME_DIR/<app>_nav.sock
 *   3. /tmp/notif_nav_<app>.sock
 *   4. /tmp/<app>_nav.sock
 * If an IPC socket exists, writes the notification context.
 * Also invokes user hook script if present and executable:
 *   ~/.config/i3dots/hooks/notifications/on_action.sh <app> <summary> <body> <id> <action>
 */
static void dispatch_app_navigation(const char *appname, const char *summary, const char *body,
                                    uint32_t id, const char *action_key) {
    if (!appname || appname[0] == '\0') return;

    char low_app[32];
    size_t len = strlen(appname);
    if (len >= sizeof(low_app)) len = sizeof(low_app) - 1;
    for (size_t i = 0; i < len; i++) low_app[i] = (char)tolower((unsigned char)appname[i]);
    low_app[len] = '\0';

    char sock_paths[4][108];
    int sock_count = 0;

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        snprintf(sock_paths[sock_count++], sizeof(sock_paths[0]), "%.60s/notif_nav_%.25s.sock", runtime_dir, low_app);
        snprintf(sock_paths[sock_count++], sizeof(sock_paths[0]), "%.60s/%.25s_nav.sock", runtime_dir, low_app);
    }
    snprintf(sock_paths[sock_count++], sizeof(sock_paths[0]), "/tmp/notif_nav_%.25s.sock", low_app);
    snprintf(sock_paths[sock_count++], sizeof(sock_paths[0]), "/tmp/%.25s_nav.sock", low_app);

    for (int i = 0; i < sock_count; i++) {
        struct stat st;
        if (stat(sock_paths[i], &st) == 0 && S_ISSOCK(st.st_mode)) {
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                size_t path_len = strlen(sock_paths[i]);
                if (path_len < sizeof(addr.sun_path)) {
                    memcpy(addr.sun_path, sock_paths[i], path_len + 1);
                    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        if (summary && summary[0] != '\0') {
                            write(sock, summary, strlen(summary));
                            write(sock, "\n", 1);
                        }
                        close(sock);
                        break;
                    }
                }
                close(sock);
            }
        }
    }

    /* Optional user hook execution */
    const char *home = getenv("HOME");
    if (home) {
        char hook_path[512];
        snprintf(hook_path, sizeof(hook_path), "%s/.config/i3dots/hooks/notifications/on_action.sh", home);
        if (access(hook_path, X_OK) == 0) {
            char hook_cmd[2048];
            snprintf(hook_cmd, sizeof(hook_cmd),
                     "'%s' '%s' '%s' '%s' %u '%s' >/dev/null 2>&1 &",
                     hook_path, low_app,
                     summary ? summary : "",
                     body ? body : "",
                     id,
                     action_key ? action_key : "default");
            system(hook_cmd);
        }
    }
}

static void sanitize_window_token(const char *src, char *dst, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < max_len; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* Generic window focus via i3wm IPC and EWMH */
static int focus_app_window(const char *appname, const char *desktop_entry) {
    if ((!appname || appname[0] == '\0') && (!desktop_entry || desktop_entry[0] == '\0')) return -1;
    if (appname && (strcasecmp(appname, "notificación") == 0 || strcasecmp(appname, "notify-send") == 0)) return -1;

    char pattern_buf[512] = {0};
    char *tokens[8];
    int token_count = 0;

    char t_app[64] = {0};
    char t_app_dash[64] = {0};
    char t_entry[64] = {0};
    char t_entry_base[64] = {0};

    if (appname && appname[0] != '\0') {
        sanitize_window_token(appname, t_app, sizeof(t_app));
        if (t_app[0] != '\0') tokens[token_count++] = t_app;

        /* If appname has spaces (e.g. "Google Chrome"), build dashed token ("Google-Chrome") */
        if (strchr(appname, ' ') != NULL) {
            size_t j = 0;
            for (size_t i = 0; appname[i] != '\0' && j + 1 < sizeof(t_app_dash); i++) {
                char c = appname[i];
                if (c == ' ') {
                    if (j > 0 && t_app_dash[j-1] != '-') t_app_dash[j++] = '-';
                } else if (isalnum((unsigned char)c) || c == '-' || c == '_') {
                    t_app_dash[j++] = c;
                }
            }
            t_app_dash[j] = '\0';
            if (t_app_dash[0] != '\0') tokens[token_count++] = t_app_dash;
        }
    }

    if (desktop_entry && desktop_entry[0] != '\0') {
        char clean_de[64];
        sanitize_window_token(desktop_entry, clean_de, sizeof(clean_de));

        /* Strip trailing .desktop if present */
        size_t de_len = strlen(clean_de);
        if (de_len > 8 && strcasecmp(clean_de + de_len - 8, ".desktop") == 0) {
            clean_de[de_len - 8] = '\0';
        }
        if (clean_de[0] != '\0') {
            snprintf(t_entry, sizeof(t_entry), "%s", clean_de);
            tokens[token_count++] = t_entry;

            /* If reverse-DNS (e.g. org.telegram or com.spotify.Client), extract last component */
            const char *last_dot = strrchr(clean_de, '.');
            if (last_dot && *(last_dot + 1) != '\0') {
                snprintf(t_entry_base, sizeof(t_entry_base), "%s", last_dot + 1);
                tokens[token_count++] = t_entry_base;
            }
        }
    }

    /* Build deduplicated regex pattern: (tok1|tok2|...) */
    size_t pat_len = 0;
    for (int i = 0; i < token_count; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) {
            if (strcasecmp(tokens[i], tokens[k]) == 0) { dup = 1; break; }
        }
        if (dup || strlen(tokens[i]) < 2) continue;

        if (pat_len > 0 && pat_len + 1 < sizeof(pattern_buf)) {
            pattern_buf[pat_len++] = '|';
        }
        size_t tlen = strlen(tokens[i]);
        if (pat_len + tlen < sizeof(pattern_buf)) {
            memcpy(pattern_buf + pat_len, tokens[i], tlen);
            pat_len += tlen;
            pattern_buf[pat_len] = '\0';
        }
    }

    if (pat_len == 0) return -1;

    /* 1. Try i3wm criteria: class, instance, title (supports native windows & browser webapps) */
    char cmd[1536];
    snprintf(cmd, sizeof(cmd),
             "i3-msg '[class=\"(?i)(%s)\"] focus' >/dev/null 2>&1 || "
             "i3-msg '[instance=\"(?i)(%s)\"] focus' >/dev/null 2>&1 || "
             "i3-msg '[title=\"(?i)(%s)\"] focus' >/dev/null 2>&1",
             pattern_buf, pattern_buf, pattern_buf);
    int res = system(cmd);
    if (res == 0) return 0;

    /* 2. Fallback for non-i3wm or EWMH via wmctrl / xdotool */
    if (token_count > 0) {
        snprintf(cmd, sizeof(cmd),
                 "wmctrl -x -a '%s' >/dev/null 2>&1 || "
                 "xdotool search --onlyvisible --class '(?i)%s' windowactivate >/dev/null 2>&1",
                 tokens[0], tokens[0]);
        return system(cmd);
    }
    return -1;
}

/* Action invocation: Unicast + Broadcast ActionInvoked, generic IPC dispatch, i3 focus, and URL fallback */
static void invoke_action(DBusConnection *conn, uint32_t id, const char *action_key) {
    /* 1. Dismiss toast from screen if visible */
    for (int i = 0; i < g_toast_count; i++) {
        if (g_toasts[i].active && g_toasts[i].id == id) {
            dismiss_toast_by_index(i);
            break;
        }
    }

    /* 2. Locate notification in history.json to get sender, appname, summary, body, desktop-entry, urls */
    char sender[64] = {0};
    char appname[64] = {0};
    char summary[128] = {0};
    char body[512] = {0};
    char desktop_entry[128] = {0};
    char url[512] = {0};

    char hist_file[512];
    get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), NULL, 0);

    json_error_t err;
    json_t *root = json_load_file(hist_file, 0, &err);
    if (root && json_is_object(root)) {
        json_t *entries = json_object_get(root, "entries");
        if (entries && json_is_array(entries)) {
            for (size_t i = 0; i < json_array_size(entries); i++) {
                json_t *item = json_array_get(entries, i);
                json_t *jid = json_object_get(item, "id");
                if (jid && json_is_integer(jid) && (uint32_t)json_integer_value(jid) == id) {
                    json_t *js = json_object_get(item, "sender");
                    if (js && json_is_string(js)) snprintf(sender, sizeof(sender), "%s", json_string_value(js));
                    json_t *ja = json_object_get(item, "appname");
                    if (ja && json_is_string(ja)) snprintf(appname, sizeof(appname), "%s", json_string_value(ja));
                    json_t *jsum = json_object_get(item, "summary");
                    if (jsum && json_is_string(jsum)) snprintf(summary, sizeof(summary), "%s", json_string_value(jsum));
                    json_t *jb = json_object_get(item, "body");
                    if (jb && json_is_string(jb)) snprintf(body, sizeof(body), "%s", json_string_value(jb));
                    json_t *jh = json_object_get(item, "hints");
                    if (jh && json_is_object(jh)) {
                        json_t *jde = json_object_get(jh, "desktop-entry");
                        if (jde && json_is_string(jde)) snprintf(desktop_entry, sizeof(desktop_entry), "%s", json_string_value(jde));
                    }
                    json_t *ju = json_object_get(item, "urls");
                    if (ju && json_is_array(ju) && json_array_size(ju) > 0) {
                        json_t *u0 = json_array_get(ju, 0);
                        if (u0 && json_is_string(u0)) snprintf(url, sizeof(url), "%s", json_string_value(u0));
                    }
                    break;
                }
            }
        }
        json_decref(root);
    }

    /* 3. Emit ActionInvoked signal */
    const char *act = action_key ? action_key : "default";

    /* A. Unicast directly to original sender AND all active instances of the app */
    send_unicast_action_to_app(conn, appname, sender, id, act);

    /* B. Broadcast signal */
    DBusMessage *bcast_sig = dbus_message_new_signal("/org/freedesktop/Notifications",
                                                     "org.freedesktop.Notifications",
                                                     "ActionInvoked");
    if (bcast_sig) {
        dbus_message_append_args(bcast_sig,
                                 DBUS_TYPE_UINT32, &id,
                                 DBUS_TYPE_STRING, &act,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, bcast_sig, NULL);
        dbus_message_unref(bcast_sig);
    }
    dbus_connection_flush(conn);

    /* 4. Generic application navigation dispatch (IPC sockets & user hook) */
    dispatch_app_navigation(appname, summary, body, id, act);

    /* 5. Generic window focus via i3wm IPC */
    int focused = focus_app_window(appname, desktop_entry);

    /* 6. Fallback if app was not focused (e.g. app was restarted or closed) */
    if (focused != 0) {
        if (url[0] != '\0') {
            char open_cmd[1024];
            snprintf(open_cmd, sizeof(open_cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
            system(open_cmd);
        } else if (appname[0] != '\0' && strcasecmp(appname, "notificación") != 0 && strcasecmp(appname, "notify-send") != 0) {
            char launch_cmd[1024];
            const char *target_launch = desktop_entry[0] != '\0' ? desktop_entry : appname;
            snprintf(launch_cmd, sizeof(launch_cmd), "gtk-launch '%s' >/dev/null 2>&1 || %s >/dev/null 2>&1 &", target_launch, appname);
            system(launch_cmd);
        }
    }
}

/* CLI client dispatch helper */
static int handle_cli_request(const char *cmd_arg) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "[ERROR] No se pudo conectar al bus D-Bus de sesion\n");
        return 1;
    }

    if (strcmp(cmd_arg, "--toggle-dnd") == 0) {
        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
                                                        "/org/freedesktop/Notifications",
                                                        "org.freedesktop.Notifications",
                                                        "ToggleDND");
        if (msg) {
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
            if (reply) {
                dbus_bool_t val = 0;
                if (dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID)) {
                    printf("[OK] DND alternado: %s\n", val ? "activo" : "inactivo");
                }
                dbus_message_unref(reply);
            }
            dbus_message_unref(msg);
        }
    } else if (strcmp(cmd_arg, "--get-dnd") == 0) {
        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
                                                        "/org/freedesktop/Notifications",
                                                        "org.freedesktop.Notifications",
                                                        "GetDND");
        if (msg) {
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
            if (reply) {
                dbus_bool_t val = 0;
                if (dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID)) {
                    printf("%s\n", val ? "true" : "false");
                }
                dbus_message_unref(reply);
            }
            dbus_message_unref(msg);
        }
    } else if (strcmp(cmd_arg, "--close-all") == 0) {
        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
                                                        "/org/freedesktop/Notifications",
                                                        "org.freedesktop.Notifications",
                                                        "CloseAll");
        if (msg) {
            dbus_connection_send(conn, msg, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(msg);
        }
    } else if (strcmp(cmd_arg, "--clear-all") == 0) {
        /* Clear history file and close all toasts */
        char hist_file[512], assets_dir[512];
        get_notif_paths(NULL, 0, hist_file, sizeof(hist_file), assets_dir, sizeof(assets_dir));
        json_t *new_root = json_object();
        json_object_set_new(new_root, "next_id", json_integer(1));
        json_object_set_new(new_root, "entries", json_array());
        char tmp_file[768];
        snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", hist_file);
        if (json_dump_file(new_root, tmp_file, JSON_INDENT(2)) == 0) {
            rename(tmp_file, hist_file);
        }
        json_decref(new_root);
        prune_all_orphaned_assets();

        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
                                                        "/org/freedesktop/Notifications",
                                                        "org.freedesktop.Notifications",
                                                        "CloseAll");
        if (msg) {
            dbus_connection_send(conn, msg, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(msg);
        }
        notify_center_if_running();
    } else if (strcmp(cmd_arg, "--reload") == 0) {
        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
                                                        "/org/freedesktop/Notifications",
                                                        "org.freedesktop.Notifications",
                                                        "ReloadTheme");
        if (msg) {
            dbus_connection_send(conn, msg, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(msg);
        }
    }

    dbus_connection_unref(conn);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        return handle_cli_request(argv[1]);
    }

    /* Kill any rogue Dunst process to claim the bus cleanly */
    system("pkill -x dunst >/dev/null 2>&1");

    int pid_fd = open(DAEMON_PID_FILE, O_RDWR | O_CREAT, 0644);
    if (pid_fd < 0 || flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        /* Another daemon is already running */
        return 0;
    }
    ftruncate(pid_fd, 0);
    dprintf(pid_fd, "%d\n", getpid());

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGUSR1, handle_signal);
    signal(SIGHUP, SIG_IGN);

    /* Initialize D-Bus */
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "[ERROR] Fallo al conectar con D-Bus: %s\n", err.message);
        close(pid_fd);
        unlink(DAEMON_PID_FILE);
        return 1;
    }

    int ret = dbus_bus_request_name(conn, "org.freedesktop.Notifications",
                                    DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                    &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER && ret != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "[ERROR] No se pudo obtener el nombre org.freedesktop.Notifications: %s\n", err.message);
        dbus_connection_unref(conn);
        close(pid_fd);
        unlink(DAEMON_PID_FILE);
        return 1;
    }

    dbus_connection_add_filter(conn, handle_dbus_message, NULL, NULL);

    /* Initialize X11 */
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "[ERROR] No se pudo abrir el display X11\n");
        dbus_connection_unref(conn);
        close(pid_fd);
        unlink(DAEMON_PID_FILE);
        return 1;
    }

    g_screen = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, g_screen);
    g_screen_w = DisplayWidth(g_dpy, g_screen);
    g_screen_h = DisplayHeight(g_dpy, g_screen);

    load_theme_and_config(&g_theme);
    g_dnd = load_dnd_state();
    prune_all_orphaned_assets();

    int x11_fd = ConnectionNumber(g_dpy);
    int dbus_fd = -1;
    dbus_connection_get_socket(conn, &dbus_fd);

    struct pollfd pfds[2];
    pfds[0].fd = x11_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = dbus_fd;
    pfds[1].events = POLLIN;

    while (g_running) {
        if (g_reload_requested) {
            g_reload_requested = 0;
            load_theme_and_config(&g_theme);
            for (int i = 0; i < g_toast_count; i++) {
                if (g_toasts[i].active) render_toast(&g_toasts[i]);
            }
        }

        /* 1. Process all pending X11 events */
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);

            if (ev.type == Expose) {
                for (int i = 0; i < g_toast_count; i++) {
                    if (g_toasts[i].active && g_toasts[i].win == ev.xexpose.window) {
                        GC gc = XCreateGC(g_dpy, g_toasts[i].win, 0, NULL);
                        XCopyArea(g_dpy, g_toasts[i].pixmap, g_toasts[i].win, gc, 0, 0, g_toasts[i].w, g_toasts[i].h, 0, 0);
                        XFreeGC(g_dpy, gc);
                        break;
                    }
                }
            } else if (ev.type == EnterNotify) {
                for (int i = 0; i < g_toast_count; i++) {
                    if (g_toasts[i].active && g_toasts[i].win == ev.xcrossing.window) {
                        g_toasts[i].hovered = true;
                        render_toast(&g_toasts[i]);
                        break;
                    }
                }
            } else if (ev.type == LeaveNotify) {
                for (int i = 0; i < g_toast_count; i++) {
                    if (g_toasts[i].active && g_toasts[i].win == ev.xcrossing.window) {
                        g_toasts[i].hovered = false;
                        /* Reset timer after hover ends */
                        g_toasts[i].show_time_ms = get_monotonic_ms();
                        render_toast(&g_toasts[i]);
                        break;
                    }
                }
            } else if (ev.type == ButtonPress) {
                for (int i = 0; i < g_toast_count; i++) {
                    if (g_toasts[i].active && g_toasts[i].win == ev.xbutton.window) {
                        int bx = ev.xbutton.x;
                        int by = ev.xbutton.y;

                        if (ev.xbutton.button == 1) {
                            /* Left click */
                            if (bx >= g_toasts[i].close_x && bx <= g_toasts[i].close_x + g_toasts[i].close_w &&
                                by >= g_toasts[i].close_y && by <= g_toasts[i].close_y + g_toasts[i].close_h) {
                                /* Close button clicked: dismiss toast from screen, keep in history */
                                dismiss_toast_by_index(i);
                            } else {
                                /* Card body clicked: invoke default action and dismiss toast */
                                uint32_t cid = g_toasts[i].id;
                                dismiss_toast_by_index(i);
                                invoke_action(conn, cid, "default");
                            }
                        } else if (ev.xbutton.button == 2 || ev.xbutton.button == 3) {
                            /* Middle or right click: dismiss toast without action */
                            dismiss_toast_by_index(i);
                        }
                        break;
                    }
                }
            }
        }

        /* 2. Process all pending D-Bus messages */
        while (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(conn);
        }

        /* 3. Check expiration timeouts of active toasts */
        int64_t now_ms = get_monotonic_ms();
        for (int i = 0; i < g_toast_count; i++) {
            if (g_toasts[i].active && !g_toasts[i].hovered && g_toasts[i].timeout_ms > 0) {
                if (now_ms - g_toasts[i].show_time_ms >= g_toasts[i].timeout_ms) {
                    /* Timed out: dismiss toast from screen WITHOUT emitting NotificationClosed */
                    dismiss_toast_by_index(i);
                    i--; /* adjust index after removal */
                }
            }
        }

        /* 4. Poll on descriptors */
        int poll_timeout = 50; /* 50ms */
        int poll_res = poll(pfds, 2, poll_timeout);
        if (poll_res > 0) {
            if (pfds[1].revents & (POLLIN | POLLPRI)) {
                dbus_connection_read_write(conn, 0);
                while (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS) {
                    dbus_connection_dispatch(conn);
                }
            }
        }
    }

    dismiss_all_toasts();
    if (g_dpy) XCloseDisplay(g_dpy);
    if (conn) dbus_connection_unref(conn);
    close(pid_fd);
    unlink(DAEMON_PID_FILE);
    return 0;
}
