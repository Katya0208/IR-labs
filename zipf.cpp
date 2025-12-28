#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <sys/stat.h>
#include <dirent.h>

#include "stemmer_api.h"


static inline int is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}
static inline unsigned char to_lower_ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    return c;
}

static uint64_t fnv1a64(const char* s, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int ends_with_txt(const char* name) {
    size_t n = std::strlen(name);
    return (n >= 4 && std::strcmp(name + (n - 4), ".txt") == 0);
}


struct StrPool {
    char* buf = nullptr;
    uint32_t n = 0;
    uint32_t cap = 0;

    void ensure(uint32_t add) {
        uint32_t need = n + add;
        if (need <= cap) return;
        uint32_t nc = cap ? cap : 1024 * 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)std::realloc(buf, nc);
        if (!nb) { std::fprintf(stderr, "StrPool realloc failed\n"); std::exit(1); }
        buf = nb; cap = nc;
    }

    uint32_t add_str(const char* s, uint16_t len) {

        ensure((uint32_t)len + 1);
        uint32_t off = n;
        std::memcpy(buf + n, s, len);
        n += len;
        buf[n++] = '\0';
        return off;
    }

    const char* at(uint32_t off) const { return buf + off; }

    void free_mem() { std::free(buf); buf = nullptr; n = cap = 0; }
};

struct TermEntry {
    uint64_t hash = 0;
    uint32_t off = 0;  
    uint16_t len = 0;
    uint32_t cnt = 0;
    uint8_t  used = 0;
};

struct TermHash {
    TermEntry* tab = nullptr;
    uint32_t cap = 0;
    uint32_t size = 0;
    StrPool pool;

    void init(uint32_t initial_cap_pow2) {
        cap = 1;
        while (cap < initial_cap_pow2) cap <<= 1;
        tab = (TermEntry*)std::calloc(cap, sizeof(TermEntry));
        if (!tab) { std::fprintf(stderr, "calloc hash failed\n"); std::exit(1); }
        size = 0;
    }

    void destroy() {
        std::free(tab); tab = nullptr; cap = size = 0;
        pool.free_mem();
    }

    void rehash(uint32_t new_cap) {
        TermEntry* old = tab;
        uint32_t old_cap = cap;

        tab = (TermEntry*)std::calloc(new_cap, sizeof(TermEntry));
        if (!tab) { std::fprintf(stderr, "calloc rehash failed\n"); std::exit(1); }
        cap = new_cap;
        size = 0;

        for (uint32_t i = 0; i < old_cap; i++) {
            if (!old[i].used) continue;
            uint32_t mask = cap - 1;
            uint32_t pos = (uint32_t)old[i].hash & mask;
            while (tab[pos].used) pos = (pos + 1) & mask;
            tab[pos] = old[i];
            tab[pos].used = 1;
            size++;
        }
        std::free(old);
    }

    void maybe_grow() {
        if (size * 10 < cap * 7) return;
        rehash(cap * 2);
    }

    void add_term(const char* s, uint16_t len) {
        if (len == 0) return;

        maybe_grow();
        uint64_t h = fnv1a64(s, (int)len);

        uint32_t mask = cap - 1;
        uint32_t pos = (uint32_t)h & mask;

        while (tab[pos].used) {
            if (tab[pos].hash == h && tab[pos].len == len) {
                const char* t = pool.at(tab[pos].off);
                if (std::memcmp(t, s, len) == 0) {
                    tab[pos].cnt++;
                    return;
                }
            }
            pos = (pos + 1) & mask;
        }

        uint32_t off = pool.add_str(s, len);
        tab[pos].used = 1;
        tab[pos].hash = h;
        tab[pos].off  = off;
        tab[pos].len  = len;
        tab[pos].cnt  = 1;
        size++;
    }
};

struct OutItem {
    uint32_t off;
    uint16_t len;
    uint32_t cnt;
};

static int cmp_desc_cnt(const void* pa, const void* pb) {
    const OutItem* a = (const OutItem*)pa;
    const OutItem* b = (const OutItem*)pb;
    if (a->cnt > b->cnt) return -1;
    if (a->cnt < b->cnt) return 1;
    return 0;
}

static uint64_t file_size(FILE* f) {
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    return (sz < 0) ? 0 : (uint64_t)sz;
}

static void ensure_dir(const char* path) {
    struct stat st{};
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        std::fprintf(stderr, "Path exists but not a directory: %s\n", path);
        std::exit(1);
    }
    if (mkdir(path, 0755) != 0) {
        std::fprintf(stderr, "mkdir failed: %s (%s)\n", path, std::strerror(errno));
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    const char* dir = nullptr;
    const char* outdir = "./zipf_out";
    uint32_t report_mb = 200;
    uint32_t topN = 20;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outdir = argv[++i];
        else if (std::strcmp(argv[i], "--report-mb") == 0 && i + 1 < argc) report_mb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) topN = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s --dir <corpus_dir> [--out out_dir] [--report-mb 200] [--top 20]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (!dir) {
        std::fprintf(stderr, "ERROR: --dir is required\n");
        return 2;
    }

    ensure_dir(outdir);

    TermHash h;
    h.init(1u << 21);

    uint64_t bytes_total = 0;
    uint64_t next_report = (uint64_t)report_mb * 1024ULL * 1024ULL;
    uint64_t tokens_total = 0;


    DIR* d = opendir(dir);
    if (!d) {
        std::fprintf(stderr, "opendir failed: %s (%s)\n", dir, std::strerror(errno));
        return 1;
    }


    const size_t BUF_SZ = 1 << 20;
    char* buf = (char*)std::malloc(BUF_SZ);
    if (!buf) { std::fprintf(stderr, "malloc buf failed\n"); return 1; }

    char tok[256];

    struct dirent* ent;
    uint32_t files = 0;

    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (!ends_with_txt(ent->d_name)) continue;

        char path[2048];
        std::snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        FILE* f = std::fopen(path, "rb");
        if (!f) continue;

        files++;
        uint64_t fsz = file_size(f);
        (void)fsz;

        size_t rd;
        int in_tok = 0;
        int tlen = 0;

        while ((rd = std::fread(buf, 1, BUF_SZ, f)) > 0) {
            bytes_total += (uint64_t)rd;

            for (size_t i = 0; i < rd; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (is_ascii_alnum(c)) {
                    if (tlen < 255) {
                        tok[tlen++] = (char)to_lower_ascii(c);
                    } else {
                       
                    }
                    in_tok = 1;
                } else {
                    if (in_tok && tlen > 0) {
                        tok[tlen] = '\0';

                        int newlen = stem_word_en(tok, tlen);
                        if (newlen > 0) {
                            h.add_term(tok, (uint16_t)newlen);
                            tokens_total++;
                        }
                    }
                    in_tok = 0;
                    tlen = 0;
                }
            }

            if (bytes_total >= next_report) {
                double mb = (double)bytes_total / (1024.0 * 1024.0);
                std::fprintf(stderr, "[PROGRESS] files=%u bytes=%.1f MB tokens=%llu uniq_terms=%u\n",
                             files, mb, (unsigned long long)tokens_total, h.size);
                next_report += (uint64_t)report_mb * 1024ULL * 1024ULL;
            }
        }

        if (in_tok && tlen > 0) {
            tok[tlen] = '\0';
            int newlen = stem_word_en(tok, tlen);
            if (newlen > 0) {
                h.add_term(tok, (uint16_t)newlen);
                tokens_total++;
            }
        }

        std::fclose(f);
    }

    closedir(d);
    std::free(buf);

    std::fprintf(stderr, "[DONE] files=%u bytes=%llu tokens=%llu uniq_terms=%u\n",
                 files, (unsigned long long)bytes_total,
                 (unsigned long long)tokens_total, h.size);

    OutItem* items = (OutItem*)std::malloc((size_t)h.size * sizeof(OutItem));
    if (!items) { std::fprintf(stderr, "malloc items failed\n"); return 1; }

    uint32_t k = 0;
    for (uint32_t i = 0; i < h.cap; i++) {
        if (!h.tab[i].used) continue;
        items[k++] = OutItem{h.tab[i].off, h.tab[i].len, h.tab[i].cnt};
    }
    std::qsort(items, k, sizeof(OutItem), cmp_desc_cnt);


    char p1[2048], p2[2048], p3[2048];
    std::snprintf(p1, sizeof(p1), "%s/zipf_rank_freq.csv", outdir);
    std::snprintf(p2, sizeof(p2), "%s/zipf_top_terms.csv", outdir);
    std::snprintf(p3, sizeof(p3), "%s/zipf_summary.txt", outdir);

    FILE* f_rank = std::fopen(p1, "w");
    if (!f_rank) { std::fprintf(stderr, "open %s failed\n", p1); return 1; }
    std::fprintf(f_rank, "rank,freq\n");
    for (uint32_t i = 0; i < k; i++) {
        std::fprintf(f_rank, "%u,%u\n", i + 1, items[i].cnt);
    }
    std::fclose(f_rank);

    FILE* f_top = std::fopen(p2, "w");
    if (!f_top) { std::fprintf(stderr, "open %s failed\n", p2); return 1; }
    std::fprintf(f_top, "rank,term,freq\n");
    uint32_t top = topN;
    if (top > k) top = k;
    for (uint32_t i = 0; i < top; i++) {
        const char* term = h.pool.at(items[i].off);
        std::fprintf(f_top, "%u,%s,%u\n", i + 1, term, items[i].cnt);
    }
    std::fclose(f_top);

    FILE* f_sum = std::fopen(p3, "w");
    if (f_sum) {
        std::fprintf(f_sum, "files=%u\n", files);
        std::fprintf(f_sum, "bytes_total=%llu\n", (unsigned long long)bytes_total);
        std::fprintf(f_sum, "tokens_total=%llu\n", (unsigned long long)tokens_total);
        std::fprintf(f_sum, "unique_terms=%u\n", h.size);
        std::fprintf(f_sum, "topN=%u\n", topN);
        std::fclose(f_sum);
    }

    std::free(items);
    h.destroy();

    std::fprintf(stderr, "[OK] written:\n  %s\n  %s\n  %s\n", p1, p2, p3);
    return 0;
}
