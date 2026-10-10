#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#define SVG_STACK_LIMIT 32768

typedef struct {
    char *pat;
    size_t pat_len;
    char *rep;
    size_t rep_len;
} Replacement;

static Replacement *replacements = NULL;
static int num_replacements = 0;

static void mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp)) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Replaces patterns in src buffer into out. Returns number of bytes written to out. */
static size_t replace_buffer(const char *src, size_t fsize, char *out, size_t out_cap) {
    size_t out_len = 0;
    const char *p = src;
    const char *end = src + fsize;

    while (p < end) {
        int matched = 0;
        for (int i = 0; i < num_replacements; i++) {
            if (replacements[i].pat_len > 0 &&
                *p == replacements[i].pat[0] &&
                (size_t)(end - p) >= replacements[i].pat_len &&
                memcmp(p, replacements[i].pat, replacements[i].pat_len) == 0)
            {
                if (out_len + replacements[i].rep_len < out_cap) {
                    memcpy(out + out_len, replacements[i].rep, replacements[i].rep_len);
                    out_len += replacements[i].rep_len;
                }
                p += replacements[i].pat_len;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            if (out_len + 1 < out_cap) {
                out[out_len++] = *p;
            }
            p++;
        }
    }
    if (out_len < out_cap) {
        out[out_len] = '\0';
    } else if (out_cap > 0) {
        out[out_cap - 1] = '\0';
    }
    return out_len;
}

static void process_file(const char *src_path, const char *dst_path) {
    FILE *f = fopen(src_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return; }

    if (fsize < SVG_STACK_LIMIT) {
        char in_buf[SVG_STACK_LIMIT];
        size_t nread = fread(in_buf, 1, fsize, f);
        fclose(f);
        if (nread != (size_t)fsize) return;
        in_buf[fsize] = '\0';

        char out_buf[SVG_STACK_LIMIT * 2];
        size_t out_len = replace_buffer(in_buf, fsize, out_buf, sizeof(out_buf));

        FILE *out_f = fopen(dst_path, "wb");
        if (out_f) {
            fwrite(out_buf, 1, out_len, out_f);
            fclose(out_f);
        }
    } else {
        char *src = malloc(fsize + 1);
        if (!src) { fclose(f); return; }
        if (fread(src, 1, fsize, f) != (size_t)fsize) { free(src); fclose(f); return; }
        src[fsize] = '\0';
        fclose(f);

        size_t out_cap = fsize * 2 + 4096;
        char *out = malloc(out_cap);
        if (!out) { free(src); return; }

        size_t out_len = replace_buffer(src, fsize, out, out_cap);
        free(src);

        FILE *out_f = fopen(dst_path, "wb");
        if (out_f) {
            fwrite(out, 1, out_len, out_f);
            fclose(out_f);
        }
        free(out);
    }
}

typedef struct {
    char s_path[512];
    char d_path[512];
} Job;

static Job *jobs_list = NULL;
static int jobs_count = 0;
static int jobs_cap = 0;

static void add_job(const char *src, const char *dst) {
    if (jobs_count >= jobs_cap) {
        jobs_cap = (jobs_cap == 0) ? 1024 : jobs_cap * 2;
        jobs_list = realloc(jobs_list, jobs_cap * sizeof(Job));
    }
    if (!jobs_list) return;
    strncpy(jobs_list[jobs_count].s_path, src, sizeof(jobs_list[jobs_count].s_path) - 1);
    jobs_list[jobs_count].s_path[sizeof(jobs_list[jobs_count].s_path) - 1] = '\0';
    strncpy(jobs_list[jobs_count].d_path, dst, sizeof(jobs_list[jobs_count].d_path) - 1);
    jobs_list[jobs_count].d_path[sizeof(jobs_list[jobs_count].d_path) - 1] = '\0';
    jobs_count++;
}

static void walk_dir_copy_collect(const char *src_dir, const char *dst_dir) {
    mkdir_p(dst_dir);
    DIR *d = opendir(src_dir);
    if (!d) return;

    struct dirent *ent;
    char s_path[512], d_path[512];

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(s_path, sizeof(s_path), "%s/%s", src_dir, ent->d_name);
        snprintf(d_path, sizeof(d_path), "%s/%s", dst_dir, ent->d_name);

        struct stat st;
        if (lstat(s_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            walk_dir_copy_collect(s_path, d_path);
        } else if (S_ISLNK(st.st_mode)) {
            char target[512];
            ssize_t len = readlink(s_path, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                unlink(d_path);
                symlink(target, d_path);
            }
        } else if (S_ISREG(st.st_mode)) {
            size_t s_len = strlen(s_path);
            if (s_len >= 4 && strcmp(s_path + s_len - 4, ".svg") == 0) {
                add_job(s_path, d_path);
            } else {
                /* Non-SVG file: copy directly */
                FILE *f = fopen(s_path, "rb");
                if (f) {
                    FILE *out_f = fopen(d_path, "wb");
                    if (out_f) {
                        char buf[8192];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, out_f);
                        fclose(out_f);
                    }
                    fclose(f);
                }
            }
        }
    }
    closedir(d);
}

static void walk_dir_inplace(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    char sub_path[512];

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (lstat(sub_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            walk_dir_inplace(sub_path);
        } else if (S_ISREG(st.st_mode)) {
            size_t l = strlen(sub_path);
            if (l >= 4 && strcmp(sub_path + l - 4, ".svg") == 0) {
                add_job(sub_path, sub_path);
            }
        }
    }
    closedir(d);
}

static void collect_symbolic_pair(const char *src_dir, const char *dst_dir) {
    mkdir_p(dst_dir);
    DIR *d = opendir(src_dir);
    if (!d) return;

    struct dirent *ent;
    char s_path[512], d_path[512];

    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 13 || memcmp(ent->d_name + len - 13, "-symbolic.svg", 13) != 0) continue;

        size_t base_len = len - 13;
        char base_name[128];
        if (base_len >= sizeof(base_name)) continue;
        memcpy(base_name, ent->d_name, base_len);
        base_name[base_len] = '\0';

        snprintf(s_path, sizeof(s_path), "%s/%s", src_dir, ent->d_name);
        snprintf(d_path, sizeof(d_path), "%s/%s.svg", dst_dir, base_name);
        add_job(s_path, d_path);
    }
    closedir(d);
}

static atomic_int next_job_idx = 0;

static void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        int idx = atomic_fetch_add(&next_job_idx, 1);
        if (idx >= jobs_count) break;
        process_file(jobs_list[idx].s_path, jobs_list[idx].d_path);
    }
    return NULL;
}

static void run_jobs_parallel(void) {
    if (jobs_count <= 0) return;

    long num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 16) num_threads = 16;
    if (num_threads > jobs_count) num_threads = jobs_count;

    if (num_threads <= 1) {
        for (int i = 0; i < jobs_count; i++) {
            process_file(jobs_list[i].s_path, jobs_list[i].d_path);
        }
        return;
    }

    atomic_store(&next_job_idx, 0);

    pthread_t threads[16];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);

    int created = 0;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], &attr, worker_thread, NULL) == 0) {
            created++;
        }
    }
    pthread_attr_destroy(&attr);

    if (created == 0) {
        for (int i = 0; i < jobs_count; i++) {
            process_file(jobs_list[i].s_path, jobs_list[i].d_path);
        }
        return;
    }

    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s [opciones]\n", argv[0]);
        return 1;
    }

    /* Modo 1: Symbolic batch: --symbolic --patterns pat1 rep1 ... --dirs src1 dst1 ... */
    if (strcmp(argv[1], "--symbolic") == 0) {
        int pat_start = -1, dirs_start = -1;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--patterns") == 0) pat_start = i + 1;
            else if (strcmp(argv[i], "--dirs") == 0) dirs_start = i + 1;
        }

        if (pat_start == -1 || dirs_start == -1) {
            fprintf(stderr, "Uso: %s --symbolic --patterns <p1> <r1> ... --dirs <src1> <dst1> ...\n", argv[0]);
            return 1;
        }

        int pat_end = (dirs_start > pat_start) ? dirs_start - 1 : argc;
        int num_pairs = (pat_end - pat_start) / 2;
        if (num_pairs < 1) return 1;

        num_replacements = num_pairs;
        replacements = malloc(num_replacements * sizeof(Replacement));
        if (!replacements) return 1;
        for (int i = 0; i < num_replacements; i++) {
            replacements[i].pat = argv[pat_start + i * 2];
            replacements[i].pat_len = strlen(replacements[i].pat);
            replacements[i].rep = argv[pat_start + i * 2 + 1];
            replacements[i].rep_len = strlen(replacements[i].rep);
        }

        int dirs_end = (pat_start > dirs_start) ? pat_start - 1 : argc;
        int num_dir_pairs = (dirs_end - dirs_start) / 2;

        for (int i = 0; i < num_dir_pairs; i++) {
            const char *src = argv[dirs_start + i * 2];
            const char *dst = argv[dirs_start + i * 2 + 1];
            collect_symbolic_pair(src, dst);
        }

        run_jobs_parallel();
        free(replacements);
        free(jobs_list);
        return 0;
    }

    /* Modo 2: Copy and recolor: --src <src> --dst <dst> <pat1> <rep1> ... */
    if (argc >= 6 && strcmp(argv[1], "--src") == 0 && strcmp(argv[3], "--dst") == 0) {
        const char *src_dir = argv[2];
        const char *dst_dir = argv[4];
        int num_pairs = (argc - 5) / 2;
        if (num_pairs < 1) return 1;

        num_replacements = num_pairs;
        replacements = malloc(num_replacements * sizeof(Replacement));
        if (!replacements) return 1;
        for (int i = 0; i < num_replacements; i++) {
            replacements[i].pat = argv[5 + i * 2];
            replacements[i].pat_len = strlen(replacements[i].pat);
            replacements[i].rep = argv[6 + i * 2];
            replacements[i].rep_len = strlen(replacements[i].rep);
        }

        walk_dir_copy_collect(src_dir, dst_dir);
        run_jobs_parallel();

        free(replacements);
        free(jobs_list);
        return 0;
    }

    /* Modo 3: In-place: <dir> <pat1> <rep1> [pat2 rep2 ...] */
    if (argc < 4 || (argc - 2) % 2 != 0) {
        fprintf(stderr, "Uso in-place: %s <dir> <pat1> <rep1> [pat2 rep2 ...]\n", argv[0]);
        return 1;
    }

    const char *target_dir = argv[1];
    num_replacements = (argc - 2) / 2;
    replacements = malloc(num_replacements * sizeof(Replacement));
    if (!replacements) return 1;
    for (int i = 0; i < num_replacements; i++) {
        replacements[i].pat = argv[2 + i * 2];
        replacements[i].pat_len = strlen(replacements[i].pat);
        replacements[i].rep = argv[3 + i * 2];
        replacements[i].rep_len = strlen(replacements[i].rep);
    }

    walk_dir_inplace(target_dir);
    run_jobs_parallel();

    free(replacements);
    free(jobs_list);
    return 0;
}
