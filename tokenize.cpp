#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static inline int is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static inline unsigned char to_lower_ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    return c;
}

static double now_sec_monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct Stats {
    uint64_t total_bytes = 0;
    uint64_t token_count = 0;
    uint64_t token_total_len = 0;

    uint64_t next_report_bytes = 0;
    uint64_t report_step_bytes = 0;

    double t0 = 0.0;
};

static void print_report(const Stats& st, double t_now, const char* label) {
    double elapsed = t_now - st.t0;
    double kb = (double)st.total_bytes / 1024.0;
    double kbps = (elapsed > 0.0) ? (kb / elapsed) : 0.0;
    double avg_len = (st.token_count > 0) ? ((double)st.token_total_len / (double)st.token_count) : 0.0;

    std::printf(
        "%s bytes=%llu (%.1f KB) tokens=%llu avg_token_len=%.3f time=%.3f sec speed=%.1f KB/s\n",
        label,
        (unsigned long long)st.total_bytes, kb,
        (unsigned long long)st.token_count,
        avg_len,
        elapsed,
        kbps
    );
}

static int tokenize_file(const char* path, Stats* st) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "Cannot open %s: %s\n", path, std::strerror(errno));
        return -1;
    }

    const size_t BUF_SZ = 1 << 20; // 1MB
    unsigned char* buf = (unsigned char*)std::malloc(BUF_SZ);
    if (!buf) {
        std::fprintf(stderr, "malloc failed\n");
        std::fclose(f);
        return -1;
    }

    uint64_t cur_tok_len = 0;
    size_t nread = 0;

    while ((nread = std::fread(buf, 1, BUF_SZ, f)) > 0) {
        st->total_bytes += (uint64_t)nread;

        for (size_t i = 0; i < nread; i++) {
            unsigned char c = buf[i];

            if (is_ascii_alnum(c)) {
                c = to_lower_ascii(c);
                (void)c;
                cur_tok_len++;
            } else {
                if (cur_tok_len > 0) {
                    st->token_count++;
                    st->token_total_len += cur_tok_len;
                    cur_tok_len = 0;
                }
            }
        }

        if (st->report_step_bytes > 0 && st->total_bytes >= st->next_report_bytes) {
            double t_now = now_sec_monotonic();
            print_report(*st, t_now, "[PROGRESS]");
            st->next_report_bytes += st->report_step_bytes;
        }
    }

    if (cur_tok_len > 0) {
        st->token_count++;
        st->token_total_len += cur_tok_len;
        cur_tok_len = 0;
    }

    std::free(buf);
    std::fclose(f);
    return 0;
}

static int has_txt_ext(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (!dot) return 0;
    return (std::strcmp(dot, ".txt") == 0);
}

static int walk_dir_recursive(const char* dir_path, Stats* st) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        std::fprintf(stderr, "Cannot open dir %s: %s\n", dir_path, std::strerror(errno));
        return -1;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;


        size_t dlen = std::strlen(dir_path);
        size_t nlen = std::strlen(name);
 
        char* full = (char*)std::malloc(dlen + 1 + nlen + 1);
        if (!full) {
            std::fprintf(stderr, "malloc failed\n");
            closedir(dir);
            return -1;
        }
        std::memcpy(full, dir_path, dlen);
        full[dlen] = '/';
        std::memcpy(full + dlen + 1, name, nlen);
        full[dlen + 1 + nlen] = '\0';

        struct stat stbuf;
        if (stat(full, &stbuf) == 0) {
            if (S_ISDIR(stbuf.st_mode)) {
                walk_dir_recursive(full, st);
            } else if (S_ISREG(stbuf.st_mode)) {

                if (has_txt_ext(name)) {
                    tokenize_file(full, st);
                }
            }
        }

        std::free(full);
    }

    closedir(dir);
    return 0;
}

int main(int argc, char** argv) {
    const char* dir = NULL;

    uint64_t report_mb = 50;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (std::strcmp(argv[i], "--report-mb") == 0 && i + 1 < argc) {
            report_mb = (uint64_t)std::strtoull(argv[++i], NULL, 10);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s --dir <folder> [--report-mb N]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            std::printf("Usage: %s --dir <folder> [--report-mb N]\n", argv[0]);
            return 2;
        }
    }

    if (!dir) {
        std::fprintf(stderr, "Missing --dir\n");
        std::printf("Usage: %s --dir <folder> [--report-mb N]\n", argv[0]);
        return 2;
    }

    Stats st;
    st.report_step_bytes = report_mb * 1024ULL * 1024ULL;
    st.next_report_bytes = st.report_step_bytes;
    st.t0 = now_sec_monotonic();

    int rc = walk_dir_recursive(dir, &st);

    double t1 = now_sec_monotonic();
    print_report(st, t1, "[FINAL]");

    if (rc != 0) return 1;
    return 0;
}
