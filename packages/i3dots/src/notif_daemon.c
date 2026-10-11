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
#include <dirent.h>
#include <stdbool.h>

#define DAEMON_PID_FILE "/tmp/notif_daemon.pid"
#define DEFAULT_MAX_HISTORY 500

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void prune_all_orphaned_assets(void);

static void get_notif_paths(char *state_dir, size_t dir_len, char *hist_file, size_t hist_len, char *assets_dir, size_t assets_len) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";
    if (state_dir) snprintf(state_dir, dir_len, "%s/.config/i3dots/core/state/i3dots/notifications", home);
    if (hist_file) snprintf(hist_file, hist_len, "%s/.config/i3dots/core/state/i3dots/notifications/history.json", home);
    if (assets_dir) snprintf(assets_dir, assets_len, "%s/.config/i3dots/core/state/i3dots/notifications/assets", home);
}

static int get_max_history_entries(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/dereck";
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/.config/notif_center/config.ini", home);
    FILE *f = fopen(cfg_path, "r");
    if (!f) return DEFAULT_MAX_HISTORY;
    char line[256];
    int max_val = DEFAULT_MAX_HISTORY;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '[') continue;
        char key[64], val[64];
        if (sscanf(p, "%63[^= \t] = %63s", key, val) == 2) {
            if (strcmp(key, "max_entries") == 0 || strcmp(key, "max_history") == 0) {
                int v = atoi(val);
                if (v >= 50 && v <= 5000) max_val = v;
            }
        }
    }
    fclose(f);
    return max_val;
}

static unsigned int simple_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static void extract_clean_author(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return;
    const char *paren = strchr(in, '(');
    size_t len = paren ? (size_t)(paren - in) : strlen(in);
    if (len >= out_len) len = out_len - 1;

    size_t j = 0;
    for (size_t i = 0; i < len && j < out_len - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xE2 && i + 2 < len && (unsigned char)in[i+1] == 0x81) {
            /* Skip Unicode direction isolates U+2068, U+2069 */
            i += 2;
            continue;
        }
        out[j++] = in[i];
    }
    out[j] = '\0';

    while (j > 0 && isspace((unsigned char)out[j - 1])) {
        out[--j] = '\0';
    }
    char *start = out;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != out) {
        memmove(out, start, strlen(start) + 1);
    }
}

static int write_png_file(const char *filename, int width, int height, int channels, const unsigned char *data, int stride) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return 0;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return 0;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return 0;
    }

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

static void save_avatar(const char *app, const char *summary, int w, int h, int channels, const unsigned char *data, int stride, char *out_name, size_t out_len) {
    char assets_dir[512];
    char cache_dir[512];
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

    /* Content-based deduplication: only write if file does not exist yet */
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

static void append_to_history(const char *app, const char *summary, const char *body, int urgency, const char *icon, const char *avatar) {
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
        if (!json_is_array(entries)) entries = NULL;
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
                /* Duplicate notification within 2 seconds, ignore */
                json_decref(root);
                if (lock_fd >= 0) {
                    flock(lock_fd, LOCK_UN);
                    close(lock_fd);
                }
                return;
            }
        }
    }

    int current_id = next_id++;
    json_object_set_new(root, "next_id", json_integer(next_id));

    json_t *item = json_object();
    json_object_set_new(item, "id", json_integer(current_id));
    json_object_set_new(item, "appname", json_string(app ? app : "Notificación"));
    json_object_set_new(item, "summary", json_string(summary ? summary : ""));
    json_object_set_new(item, "body", json_string(body ? body : ""));
    json_object_set_new(item, "urgency", json_integer(urgency));
    json_object_set_new(item, "timestamp", json_integer((json_int_t)now));
    if (icon && icon[0]) json_object_set_new(item, "icon_path", json_string(icon));
    if (avatar && avatar[0]) json_object_set_new(item, "avatar", json_string(avatar));

    json_array_insert_new(entries, 0, item);

    int max_entries = get_max_history_entries();
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
                /* Center is actively visible: signal GUI and dismiss popup into Dunst history */
                kill(pid, SIGUSR2);
                system("dunstctl close >/dev/null 2>&1 &");
            }
        }
    }
}

static DBusHandlerResult filter_cb(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)conn;
    (void)user_data;

    if (!dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) return DBUS_HANDLER_RESULT_HANDLED;

    const char *app = NULL;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &app);
    }
    dbus_message_iter_next(&iter); /* 1: replaces_id */
    dbus_message_iter_next(&iter); /* 2: app_icon */
    const char *app_icon = NULL;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &app_icon);
    }
    dbus_message_iter_next(&iter); /* 3: summary */
    const char *summary = NULL;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &summary);
    }
    dbus_message_iter_next(&iter); /* 4: body */
    const char *body = NULL;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &body);
    }
    dbus_message_iter_next(&iter); /* 5: actions */
    dbus_message_iter_next(&iter); /* 6: hints */

    int urgency = 1; /* normal */
    bool is_relaunch = false;
    bool no_history = false;
    char saved_avatar[64] = {0};
    char image_path[256] = {0};

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

            if (key) {
                if (strcmp(key, "x-i3dots-relaunch") == 0) {
                    is_relaunch = true;
                } else if (strcmp(key, "x-i3dots-no-history") == 0 || strcmp(key, "transient") == 0) {
                    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var_iter;
                        dbus_message_iter_recurse(&entry_iter, &var_iter);
                        int vt = dbus_message_iter_get_arg_type(&var_iter);
                        if (vt == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b = 0;
                            dbus_message_iter_get_basic(&var_iter, &b);
                            if (b) no_history = true;
                        } else {
                            no_history = true;
                        }
                    } else {
                        no_history = true;
                    }
                } else if (strcmp(key, "urgency") == 0) {
                    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var_iter;
                        dbus_message_iter_recurse(&entry_iter, &var_iter);
                        int vt = dbus_message_iter_get_arg_type(&var_iter);
                        if (vt == DBUS_TYPE_BYTE) {
                            unsigned char u = 1;
                            dbus_message_iter_get_basic(&var_iter, &u);
                            urgency = (int)u;
                        } else if (vt == DBUS_TYPE_UINT32 || vt == DBUS_TYPE_INT32) {
                            int u = 1;
                            dbus_message_iter_get_basic(&var_iter, &u);
                            urgency = u;
                        }
                    }
                } else if (strcmp(key, "image-path") == 0 || strcmp(key, "image_path") == 0) {
                    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var_iter;
                        dbus_message_iter_recurse(&entry_iter, &var_iter);
                        const char *s = NULL;
                        if (dbus_message_iter_get_arg_type(&var_iter) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&var_iter, &s);
                            if (s) snprintf(image_path, sizeof(image_path), "%s", s);
                        }
                    }
                } else if (strcmp(key, "image-data") == 0 || strcmp(key, "image_data") == 0 || strcmp(key, "icon_data") == 0) {
                    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var_iter;
                        dbus_message_iter_recurse(&entry_iter, &var_iter);
                        if (dbus_message_iter_get_arg_type(&var_iter) == DBUS_TYPE_STRUCT) {
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
                                    save_avatar(app, summary, w, h, channels, data, stride, saved_avatar, sizeof(saved_avatar));
                                }
                            }
                        }
                    }
                }
            }
            dbus_message_iter_next(&dict_iter);
        }
    }

    if (app && (strcasecmp(app, "sys_control") == 0 || strcasecmp(app, "sys_control.sh") == 0)) {
        no_history = true;
    }
    if (summary && (strcmp(summary, "Volumen") == 0 ||
                    strcmp(summary, "Volumen (Silenciado)") == 0 ||
                    strcmp(summary, "Brillo") == 0 ||
                    strcmp(summary, "Micrófono") == 0)) {
        no_history = true;
    }

    if (!is_relaunch && !no_history) {
        const char *icon_to_save = (image_path[0] != '\0') ? image_path : app_icon;
        append_to_history(app, summary, body, urgency, icon_to_save, saved_avatar);
        notify_center_if_running();
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

int main(void) {
    int pid_fd = open(DAEMON_PID_FILE, O_RDWR | O_CREAT, 0644);
    if (pid_fd < 0 || flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        /* Another instance is already running */
        return 0;
    }
    ftruncate(pid_fd, 0);
    dprintf(pid_fd, "%d\n", getpid());

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, SIG_IGN);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        close(pid_fd);
        unlink(DAEMON_PID_FILE);
        return 1;
    }

    dbus_connection_add_filter(conn, filter_cb, NULL, NULL);

    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                    "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
    if (!msg) {
        close(pid_fd);
        unlink(DAEMON_PID_FILE);
        return 1;
    }

    const char *rule = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";
    const char *rules[] = { rule };
    dbus_uint32_t flags = 0;
    DBusMessageIter iter, sub;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &rules[0]);
    dbus_message_iter_close_container(&iter, &sub);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
    dbus_message_unref(msg);
    if (reply) dbus_message_unref(reply);

    prune_all_orphaned_assets();

    while (g_running && dbus_connection_read_write_dispatch(conn, 1000)) {
    }

    dbus_connection_unref(conn);
    close(pid_fd);
    unlink(DAEMON_PID_FILE);
    return 0;
}
