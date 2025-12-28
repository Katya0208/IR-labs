#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static double now_sec_monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline int is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}
static inline unsigned char to_lower_ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    return c;
}
static uint64_t fnv1a_64(const char* s, int len) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static void ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        std::fprintf(stderr, "Path exists but not dir: %s\n", path);
        std::exit(1);
    }
    if (mkdir(path, 0755) != 0) {
        std::fprintf(stderr, "mkdir(%s) failed: %s\n", path, std::strerror(errno));
        std::exit(1);
    }
}

struct Arena {
    char*  buf = nullptr;
    size_t cap = 0;
    size_t used = 0;

    void init(size_t cap_bytes) {
        buf = (char*)std::malloc(cap_bytes);
        if (!buf) { std::fprintf(stderr, "Arena malloc failed\n"); std::exit(1); }
        cap = cap_bytes;
        used = 0;
    }
    void reset() { used = 0; }
    void destroy() { std::free(buf); buf=nullptr; cap=used=0; }

    const char* add(const char* s, int len) {
        size_t need = (size_t)len + 1;
        if (used + need > cap) {
            size_t new_cap = cap ? cap : (1 << 20);
            while (used + need > new_cap) new_cap *= 2;
            char* nb = (char*)std::realloc(buf, new_cap);
            if (!nb) { std::fprintf(stderr, "Arena realloc failed\n"); std::exit(1); }
            buf = nb; cap = new_cap;
        }
        char* p = buf + used;
        std::memcpy(p, s, (size_t)len);
        p[len] = '\0';
        used += need;
        return p;
    }
};

struct U32List {
    uint32_t* a = nullptr;
    uint32_t  n = 0;
    uint32_t  cap = 0;

    void free_mem() { std::free(a); a=nullptr; n=cap=0; }

    void push_unique_sorted(uint32_t v) {
        if (n > 0 && a[n-1] == v) return;
        if (n == cap) {
            uint32_t new_cap = cap ? cap*2 : 8;
            uint32_t* nb = (uint32_t*)std::realloc(a, (size_t)new_cap * sizeof(uint32_t));
            if (!nb) { std::fprintf(stderr, "realloc postings failed\n"); std::exit(1); }
            a = nb; cap = new_cap;
        }
        a[n++] = v;
    }
};

struct TermEntry {
    uint64_t    hash = 0; 
    const char* term = nullptr;
    uint16_t    len = 0;
    U32List     post;
};

struct TermTable {
    TermEntry* tab = nullptr;
    size_t cap = 0; 
    size_t used = 0;
    Arena arena;

    void init(size_t cap_pow2, size_t arena_bytes) {
        cap = cap_pow2;
        tab = (TermEntry*)std::calloc(cap, sizeof(TermEntry));
        if (!tab) { std::fprintf(stderr, "calloc term table failed\n"); std::exit(1); }
        used = 0;
        arena.init(arena_bytes);
    }

    void clear() {
        for (size_t i=0;i<cap;i++) {
            if (tab[i].hash != 0) tab[i].post.free_mem();
            tab[i].hash = 0; tab[i].term=nullptr; tab[i].len=0;
        }
        used = 0;
        arena.reset();
    }

    void destroy() {
        clear();
        std::free(tab); tab=nullptr; cap=used=0;
        arena.destroy();
    }

    void maybe_grow() {
        if (used * 10 < cap * 7) return;
        size_t new_cap = cap * 2;
        TermEntry* old = tab;
        size_t old_cap = cap;

        tab = (TermEntry*)std::calloc(new_cap, sizeof(TermEntry));
        if (!tab) { std::fprintf(stderr, "calloc rehash failed\n"); std::exit(1); }
        cap = new_cap;
        used = 0;

        for (size_t i=0;i<old_cap;i++) {
            if (old[i].hash != 0) {
                size_t mask = cap - 1;
                size_t pos = (size_t)old[i].hash & mask;
                while (tab[pos].hash != 0) pos = (pos + 1) & mask;
                tab[pos] = old[i];
                used++;
            }
        }
        std::free(old);
    }

    TermEntry* get_or_create(const char* s, int len) {
        if (len <= 0) return nullptr;
        maybe_grow();

        uint64_t h = fnv1a_64(s, len);
        size_t mask = cap - 1;
        size_t pos = (size_t)h & mask;

        while (1) {
            if (tab[pos].hash == 0) {
                const char* stored = arena.add(s, len);
                tab[pos].hash = h;
                tab[pos].term = stored;
                tab[pos].len = (uint16_t)len;
                tab[pos].post = U32List{};
                used++;
                return &tab[pos];
            }
            if (tab[pos].hash == h && tab[pos].len == (uint16_t)len &&
                std::memcmp(tab[pos].term, s, (size_t)len) == 0) {
                return &tab[pos];
            }
            pos = (pos + 1) & mask;
        }
    }

    size_t approx_mem_bytes() const {
        size_t bytes = 0;
        bytes += cap * sizeof(TermEntry);
        bytes += arena.used;
        for (size_t i=0;i<cap;i++) {
            if (tab[i].hash != 0) bytes += (size_t)tab[i].post.cap * sizeof(uint32_t);
        }
        return bytes;
    }
};

struct DocSetEntry {
    uint64_t hash = 0; 
    const char* term = nullptr;
    uint16_t len = 0;
};

struct DocTermSet {
    DocSetEntry* tab = nullptr;
    size_t cap = 0;
    size_t used = 0;
    Arena arena; 

    void init(size_t cap_pow2, size_t arena_bytes) {
        cap = cap_pow2;
        tab = (DocSetEntry*)std::calloc(cap, sizeof(DocSetEntry));
        if (!tab) { std::fprintf(stderr, "calloc DocTermSet failed\n"); std::exit(1); }
        used = 0;
        arena.init(arena_bytes);
    }

    void reset() {

        std::memset(tab, 0, cap * sizeof(DocSetEntry));
        used = 0;
        arena.reset();
    }

    void destroy() {
        std::free(tab); tab=nullptr; cap=used=0;
        arena.destroy();
    }

    int contains_or_add(const char* s, int len) {

        if (len <= 0) return 1;
        if (used * 10 >= cap * 8) {
            return 0;
        }

        uint64_t h = fnv1a_64(s, len);
        size_t mask = cap - 1;
        size_t pos = (size_t)h & mask;

        while (1) {
            if (tab[pos].hash == 0) {
                const char* stored = arena.add(s, len);
                tab[pos].hash = h;
                tab[pos].term = stored;
                tab[pos].len = (uint16_t)len;
                used++;
                return 0; 
            }
            if (tab[pos].hash == h && tab[pos].len == (uint16_t)len &&
                std::memcmp(tab[pos].term, s, (size_t)len) == 0) {
                return 1;
            }
            pos = (pos + 1) & mask;
        }
    }
};


static int extract_json_string(const char* line, const char* key, char* out, int out_cap) {
    char pat[64];
    std::snprintf(pat, sizeof(pat), "\"%s\":", key);

    const char* p = std::strstr(line, pat);
    if (!p) return 0;
    p += std::strlen(pat);

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    int k = 0;
    while (*p && *p != '"' && k < out_cap-1) {
        if (*p == '\\' && p[1]) p++;
        out[k++] = *p++;
    }
    out[k] = '\0';
    return 1;
}

#pragma pack(push,1)
struct DocsHeader {
    char     magic[4];  
    uint32_t version;    
    uint32_t doc_count;
    uint64_t string_pool_bytes;
    uint8_t  reserved[32];
};
struct DocRec {
    uint64_t title_off;
    uint32_t title_len;
    uint64_t url_off;
    uint32_t url_len;
};
#pragma pack(pop)

struct DocsBuilder {
    DocRec* recs = nullptr;
    uint32_t n = 0;
    uint32_t cap = 0;
    Arena pool;

    void init(uint32_t cap_docs, size_t pool_bytes) {
        cap = cap_docs ? cap_docs : 1024;
        recs = (DocRec*)std::malloc((size_t)cap * sizeof(DocRec));
        if (!recs) { std::fprintf(stderr, "malloc docs recs failed\n"); std::exit(1); }
        n = 0;
        pool.init(pool_bytes);
    }

    void ensure() {
        if (n < cap) return;
        uint32_t new_cap = cap * 2;
        DocRec* nb = (DocRec*)std::realloc(recs, (size_t)new_cap * sizeof(DocRec));
        if (!nb) { std::fprintf(stderr, "realloc docs recs failed\n"); std::exit(1); }
        recs = nb;
        cap = new_cap;
    }

    uint32_t add_doc(const char* title, const char* url) {
        ensure();
        uint64_t title_off = (uint64_t)pool.used;
        uint32_t title_len = (uint32_t)std::strlen(title);
        pool.add(title, (int)title_len);

        uint64_t url_off = (uint64_t)pool.used;
        uint32_t url_len = (uint32_t)std::strlen(url);
        pool.add(url, (int)url_len);

        recs[n] = DocRec{ title_off, title_len, url_off, url_len };
        return n++;
    }

    void write_to(const char* path) {
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "open %s failed: %s\n", path, std::strerror(errno)); std::exit(1); }

        DocsHeader h{};
        h.magic[0]='D'; h.magic[1]='O'; h.magic[2]='C'; h.magic[3]='S';
        h.version = 1;
        h.doc_count = n;
        h.string_pool_bytes = (uint64_t)pool.used;
        std::memset(h.reserved, 0, sizeof(h.reserved));

        std::fwrite(&h, sizeof(h), 1, f);
        std::fwrite(recs, sizeof(DocRec), n, f);
        std::fwrite(pool.buf, 1, pool.used, f);
        std::fclose(f);
    }

    void destroy() {
        std::free(recs); recs=nullptr; n=cap=0;
        pool.destroy();
    }
};

#pragma pack(push,1)
struct BlockHeader {
    char magic[4];
    uint32_t term_count;
};
#pragma pack(pop)

static int term_cmp_lex(const void* a, const void* b) {
    const TermEntry* ea = *(const TermEntry* const*)a;
    const TermEntry* eb = *(const TermEntry* const*)b;
    int la = (int)ea->len, lb = (int)eb->len;
    int m = (la < lb) ? la : lb;
    int c = std::memcmp(ea->term, eb->term, (size_t)m);
    if (c != 0) return c;
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

static void write_block(const char* path, TermTable* tt) {
    TermEntry** arr = (TermEntry**)std::malloc(tt->used * sizeof(TermEntry*));
    if (!arr) { std::fprintf(stderr, "malloc arr failed\n"); std::exit(1); }
    size_t k = 0;
    for (size_t i=0;i<tt->cap;i++) if (tt->tab[i].hash != 0) arr[k++] = &tt->tab[i];

    std::qsort(arr, k, sizeof(TermEntry*), term_cmp_lex);

    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "open block %s failed: %s\n", path, std::strerror(errno)); std::exit(1); }

    BlockHeader bh{};
    bh.magic[0]='B'; bh.magic[1]='L'; bh.magic[2]='K'; bh.magic[3]='1';
    bh.term_count = (uint32_t)k;
    std::fwrite(&bh, sizeof(bh), 1, f);

    for (size_t i=0;i<k;i++) {
        TermEntry* e = arr[i];
        uint16_t tlen = e->len;
        uint32_t df = e->post.n;
        std::fwrite(&tlen, sizeof(tlen), 1, f);
        std::fwrite(&df, sizeof(df), 1, f);
        std::fwrite(e->term, 1, tlen, f);
        std::fwrite(e->post.a, sizeof(uint32_t), df, f);
    }

    std::fclose(f);
    std::free(arr);
}

#pragma pack(push,1)
struct LexHeader {
    char magic[4]; 
    uint32_t version;
    uint32_t term_count;
    uint64_t string_pool_bytes;
    uint8_t reserved[32];
};
struct LexRec {
    uint64_t term_off;
    uint16_t term_len;
    uint16_t flags;
    uint32_t df;
    uint64_t postings_off;
    uint32_t postings_len;
    uint32_t reserved;
};
struct PostHeader {
    char magic[4];
    uint32_t version;
    uint8_t reserved[32];
};
#pragma pack(pop)

struct BlockReader {
    FILE* f = nullptr;
    uint32_t remaining = 0;

    char* term = nullptr;
    uint16_t term_len = 0;
    uint32_t df = 0;
    uint32_t* docs = nullptr;

    void open(const char* path) {
        f = std::fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "open %s failed: %s\n", path, std::strerror(errno)); std::exit(1); }
        BlockHeader bh{};
        if (std::fread(&bh, sizeof(bh), 1, f) != 1) { std::fprintf(stderr, "bad block header\n"); std::exit(1); }
        if (std::memcmp(bh.magic, "BLK1", 4) != 0) { std::fprintf(stderr, "bad block magic\n"); std::exit(1); }
        remaining = bh.term_count;
        term = nullptr; docs = nullptr;
        term_len = 0; df = 0;
        next();
    }

    void close() {
        if (f) std::fclose(f);
        f = nullptr;
        std::free(term); term=nullptr;
        std::free(docs); docs=nullptr;
        remaining = 0;
    }

    int has() const { return (f != nullptr) && (term != nullptr); }

    void next() {
        std::free(term); term=nullptr;
        std::free(docs); docs=nullptr;
        term_len = 0; df = 0;

        if (remaining == 0) { return; }

        if (std::fread(&term_len, sizeof(term_len), 1, f) != 1) { std::fprintf(stderr, "read term_len failed\n"); std::exit(1); }
        if (std::fread(&df, sizeof(df), 1, f) != 1) { std::fprintf(stderr, "read df failed\n"); std::exit(1); }

        term = (char*)std::malloc((size_t)term_len + 1);
        if (!term) { std::fprintf(stderr, "malloc term failed\n"); std::exit(1); }
        if (std::fread(term, 1, term_len, f) != term_len) { std::fprintf(stderr, "read term bytes failed\n"); std::exit(1); }
        term[term_len] = '\0';

        docs = (uint32_t*)std::malloc((size_t)df * sizeof(uint32_t));
        if (!docs) { std::fprintf(stderr, "malloc docs failed\n"); std::exit(1); }
        if (df > 0 && std::fread(docs, sizeof(uint32_t), df, f) != df) { std::fprintf(stderr, "read postings failed\n"); std::exit(1); }

        remaining--;
    }
};

static int lex_cmp_str(const char* a, uint16_t alen, const char* b, uint16_t blen) {
    int m = (alen < blen) ? (int)alen : (int)blen;
    int c = std::memcmp(a, b, (size_t)m);
    if (c != 0) return c;
    return (alen < blen) ? -1 : (alen > blen ? 1 : 0);
}

static uint32_t* merge_union_u32(const uint32_t* a, uint32_t na, const uint32_t* b, uint32_t nb, uint32_t* out_n) {
    uint32_t cap = na + nb;
    uint32_t* out = (uint32_t*)std::malloc((size_t)cap * sizeof(uint32_t));
    if (!out) { std::fprintf(stderr, "malloc merge out failed\n"); std::exit(1); }

    uint32_t i=0,j=0,k=0;
    while (i<na && j<nb) {
        uint32_t x=a[i], y=b[j];
        uint32_t v;
        if (x==y) { v=x; i++; j++; }
        else if (x<y) { v=x; i++; }
        else { v=y; j++; }
        if (k==0 || out[k-1]!=v) out[k++]=v;
    }
    while (i<na) { uint32_t v=a[i++]; if (k==0||out[k-1]!=v) out[k++]=v; }
    while (j<nb) { uint32_t v=b[j++]; if (k==0||out[k-1]!=v) out[k++]=v; }

    *out_n = k;
    return out;
}

static char* g_lex_pool_for_sort = nullptr;

static int lexrec_cmp_by_term(const void* pa, const void* pb) {
    const LexRec* a = (const LexRec*)pa;
    const LexRec* b = (const LexRec*)pb;

    const char* ta = g_lex_pool_for_sort + a->term_off;
    const char* tb = g_lex_pool_for_sort + b->term_off;

    uint16_t alen = a->term_len;
    uint16_t blen = b->term_len;

    int m = (alen < blen) ? (int)alen : (int)blen;
    int c = std::memcmp(ta, tb, (size_t)m);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

struct LexBuilder {
    LexRec* recs = nullptr;
    uint32_t n = 0;
    uint32_t cap = 0;
    Arena pool;

    uint64_t sum_term_len = 0;

    void init(uint32_t cap_terms, size_t pool_bytes) {
        cap = cap_terms ? cap_terms : 1024;
        recs = (LexRec*)std::malloc((size_t)cap * sizeof(LexRec));
        if (!recs) { std::fprintf(stderr, "malloc lex recs failed\n"); std::exit(1); }
        n = 0;
        pool.init(pool_bytes);
        sum_term_len = 0;
    }
    void ensure() {
        if (n < cap) return;
        uint32_t new_cap = cap * 2;
        LexRec* nb = (LexRec*)std::realloc(recs, (size_t)new_cap * sizeof(LexRec));
        if (!nb) { std::fprintf(stderr, "realloc lex recs failed\n"); std::exit(1); }
        recs = nb; cap = new_cap;
    }

    void add_term(const char* term, uint16_t tlen, uint64_t postings_off, uint32_t postings_len) {
        ensure();
        uint64_t off = (uint64_t)pool.used;
        pool.add(term, (int)tlen);
        LexRec r{};
        r.term_off = off;
        r.term_len = tlen;
        r.flags = 0;
        r.df = postings_len;
        r.postings_off = postings_off;
        r.postings_len = postings_len;
        r.reserved = 0;
        recs[n++] = r;
        sum_term_len += (uint64_t)tlen;
    }

    void write_to(const char* path) {
        g_lex_pool_for_sort = pool.buf;
        std::qsort(recs, n, sizeof(LexRec), lexrec_cmp_by_term);
        g_lex_pool_for_sort = nullptr;
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "open %s failed: %s\n", path, std::strerror(errno)); std::exit(1); }

        LexHeader h{};
        h.magic[0]='L'; h.magic[1]='E'; h.magic[2]='X'; h.magic[3]='I';
        h.version = 1;
        h.term_count = n;
        h.string_pool_bytes = (uint64_t)pool.used;
        std::memset(h.reserved, 0, sizeof(h.reserved));

        std::fwrite(&h, sizeof(h), 1, f);
        std::fwrite(recs, sizeof(LexRec), n, f);
        std::fwrite(pool.buf, 1, pool.used, f);
        std::fclose(f);
    }

    double avg_term_len() const {
        if (n == 0) return 0.0;
        return (double)sum_term_len / (double)n;
    }

    void destroy() {
        std::free(recs); recs=nullptr; n=cap=0;
        pool.destroy();
        sum_term_len = 0;
    }
};

static void merge_blocks_to_index(const char* blocks_dir, const char* out_lex, const char* out_post) {
    DIR* d = opendir(blocks_dir);
    if (!d) { std::fprintf(stderr, "opendir %s failed: %s\n", blocks_dir, std::strerror(errno)); std::exit(1); }

    size_t cap = 64, n = 0;
    char** names = (char**)std::malloc(cap * sizeof(char*));
    if (!names) { std::fprintf(stderr, "malloc names failed\n"); std::exit(1); }

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* nm = ent->d_name;
        size_t l = std::strlen(nm);
        if (l < 4) continue;
        if (std::strcmp(nm + (l-4), ".blk") != 0) continue;

        if (n == cap) {
            cap *= 2;
            char** nb = (char**)std::realloc(names, cap * sizeof(char*));
            if (!nb) { std::fprintf(stderr, "realloc names failed\n"); std::exit(1); }
            names = nb;
        }
        char* s = (char*)std::malloc(l + 1);
        std::memcpy(s, nm, l+1);
        names[n++] = s;
    }
    closedir(d);

    if (n == 0) { std::fprintf(stderr, "No .blk found in %s\n", blocks_dir); std::exit(1); }

    BlockReader* br = (BlockReader*)std::malloc(n * sizeof(BlockReader));
    if (!br) { std::fprintf(stderr, "malloc br failed\n"); std::exit(1); }
    std::memset(br, 0, n * sizeof(BlockReader));

    for (size_t i=0;i<n;i++) {
        size_t dl = std::strlen(blocks_dir);
        size_t nl = std::strlen(names[i]);
        char* full = (char*)std::malloc(dl + 1 + nl + 1);
        std::memcpy(full, blocks_dir, dl);
        full[dl] = '/';
        std::memcpy(full + dl + 1, names[i], nl + 1);
        br[i].open(full);
        std::free(full);
    }

    FILE* fp = std::fopen(out_post, "wb");
    if (!fp) { std::fprintf(stderr, "open %s failed: %s\n", out_post, std::strerror(errno)); std::exit(1); }
    PostHeader ph{};
    ph.magic[0]='P'; ph.magic[1]='O'; ph.magic[2]='S'; ph.magic[3]='T';
    ph.version = 1;
    std::memset(ph.reserved, 0, sizeof(ph.reserved));
    std::fwrite(&ph, sizeof(ph), 1, fp);
    uint64_t postings_cursor = (uint64_t)sizeof(PostHeader);

    LexBuilder lex;
    lex.init(1024*1024, (size_t)128<<20);

    while (1) {
        ssize_t min_i = -1;
        for (size_t i=0;i<n;i++) {
            if (!br[i].has()) continue;
            if (min_i < 0) min_i = (ssize_t)i;
            else {
                int c = lex_cmp_str(br[i].term, br[i].term_len, br[min_i].term, br[min_i].term_len);
                if (c < 0) min_i = (ssize_t)i;
            }
        }
        if (min_i < 0) break;

        const char* cur_term = br[min_i].term;
        uint16_t cur_len = br[min_i].term_len;

        uint32_t* merged = (uint32_t*)std::malloc((size_t)br[min_i].df * sizeof(uint32_t));
        if (!merged) { std::fprintf(stderr, "malloc merged failed\n"); std::exit(1); }
        std::memcpy(merged, br[min_i].docs, (size_t)br[min_i].df * sizeof(uint32_t));
        uint32_t merged_n = br[min_i].df;

        br[min_i].next();

        for (size_t i=0;i<n;i++) {
            if (!br[i].has()) continue;
            if (br[i].term_len == cur_len && std::memcmp(br[i].term, cur_term, cur_len) == 0) {
                uint32_t out_n = 0;
                uint32_t* out = merge_union_u32(merged, merged_n, br[i].docs, br[i].df, &out_n);
                std::free(merged);
                merged = out;
                merged_n = out_n;
                br[i].next();
            }
        }

        uint64_t off = postings_cursor;
        if (merged_n > 0) {
            std::fwrite(merged, sizeof(uint32_t), merged_n, fp);
            postings_cursor += (uint64_t)merged_n * sizeof(uint32_t);
        }

        lex.add_term(cur_term, cur_len, off, merged_n);

        std::free(merged);
    }

    std::fclose(fp);
    lex.write_to(out_lex);

    std::printf("[INDEX STATS] term_count=%u avg_term_len=%.3f postings_bytes=%llu\n",
        lex.n, lex.avg_term_len(), (unsigned long long)postings_cursor);

    for (size_t i=0;i<n;i++) br[i].close();
    std::free(br);

    for (size_t i=0;i<n;i++) std::free(names[i]);
    std::free(names);

    lex.destroy();
}

static int process_one_doc(
    const char* txt_path,
    uint32_t doc_id,
    TermTable* tt,
    DocTermSet* dset,
    uint64_t* total_bytes,
    uint64_t* total_tokens,
    uint64_t* unique_terms_in_docs_sum
) {
    FILE* f = std::fopen(txt_path, "rb");
    if (!f) {
        std::fprintf(stderr, "WARN: cannot open %s: %s\n", txt_path, std::strerror(errno));
        return -1;
    }

    dset->reset();
    uint64_t unique_in_doc = 0;

    const size_t BUF_SZ = 1 << 20;
    unsigned char* buf = (unsigned char*)std::malloc(BUF_SZ);
    if (!buf) { std::fprintf(stderr, "malloc buf failed\n"); std::exit(1); }

    const int TOK_MAX = 256;
    char tok[TOK_MAX];
    int tok_len = 0;

    size_t nread = 0;
    while ((nread = std::fread(buf, 1, BUF_SZ, f)) > 0) {
        *total_bytes += (uint64_t)nread;
        for (size_t i=0;i<nread;i++) {
            unsigned char c = buf[i];
            if (is_ascii_alnum(c)) {
                c = to_lower_ascii(c);
                if (tok_len < TOK_MAX-1) tok[tok_len++] = (char)c;
            } else {
                if (tok_len > 0) {
                    tok[tok_len] = '\0';
                    (*total_tokens)++;

                    int already = dset->contains_or_add(tok, tok_len);
                    if (!already) {
                        TermEntry* e = tt->get_or_create(tok, tok_len);
                        if (e) e->post.push_unique_sorted(doc_id);
                        unique_in_doc++;
                    }
                    tok_len = 0;
                }
            }
        }
    }
    if (tok_len > 0) {
        tok[tok_len] = '\0';
        (*total_tokens)++;

        int already = dset->contains_or_add(tok, tok_len);
        if (!already) {
            TermEntry* e = tt->get_or_create(tok, tok_len);
            if (e) e->post.push_unique_sorted(doc_id);
            unique_in_doc++;
        }
        tok_len = 0;
    }

    std::free(buf);
    std::fclose(f);

    *unique_terms_in_docs_sum += unique_in_doc;
    return 0;
}

int main(int argc, char** argv) {
    const char* manifest = nullptr;
    const char* corpus_dir = nullptr;
    const char* out_dir = "out";
    uint64_t mem_mb = 512;
    uint64_t report_mb = 200;

    for (int i=1;i<argc;i++) {
        if (std::strcmp(argv[i], "--manifest") == 0 && i+1<argc) manifest = argv[++i];
        else if (std::strcmp(argv[i], "--corpus") == 0 && i+1<argc) corpus_dir = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i+1<argc) out_dir = argv[++i];
        else if (std::strcmp(argv[i], "--mem-mb") == 0 && i+1<argc) mem_mb = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--report-mb") == 0 && i+1<argc) report_mb = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s --manifest manifest.jsonl --corpus ./corpus --out ./out [--mem-mb 512] [--report-mb 200]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }
    if (!manifest || !corpus_dir) {
        std::fprintf(stderr, "Missing --manifest or --corpus\n");
        return 2;
    }

    ensure_dir(out_dir);

    size_t out_len = std::strlen(out_dir);
    char* blocks_dir = (char*)std::malloc(out_len + 1 + 6 + 1);
    std::memcpy(blocks_dir, out_dir, out_len);
    blocks_dir[out_len] = '/';
    std::memcpy(blocks_dir + out_len + 1, "blocks", 6);
    blocks_dir[out_len + 1 + 6] = '\0';
    ensure_dir(blocks_dir);

    DocsBuilder docs;
    docs.init(40000, (size_t)64<<20);

    TermTable tt;
    tt.init((size_t)1<<21, (size_t)128<<20);

    DocTermSet dset;
    dset.init((size_t)1<<17, (size_t)2<<20);

    FILE* fm = std::fopen(manifest, "rb");
    if (!fm) {
        std::fprintf(stderr, "Cannot open manifest %s: %s\n", manifest, std::strerror(errno));
        return 1;
    }

    double t0 = now_sec_monotonic();
    uint64_t total_bytes = 0;
    uint64_t total_tokens = 0;
    uint64_t unique_terms_in_docs_sum = 0;
    uint64_t next_report_bytes = report_mb * 1024ULL * 1024ULL;

    uint32_t doc_id = 0;
    uint32_t block_id = 0;

    char line[1<<20];
    char docid_str[64], title[4096], url[8192];

    uint64_t mem_limit = mem_mb * 1024ULL * 1024ULL;

    while (std::fgets(line, sizeof(line), fm)) {
        docid_str[0]=title[0]=url[0]='\0';

        int ok_id = extract_json_string(line, "doc_id", docid_str, (int)sizeof(docid_str));
        if (!ok_id) continue;

        extract_json_string(line, "title", title, (int)sizeof(title));
        extract_json_string(line, "url", url, (int)sizeof(url));
        if (title[0] == '\0') std::snprintf(title, sizeof(title), "%s", docid_str);
        if (url[0] == '\0') std::snprintf(url, sizeof(url), "%s", "");

        docs.add_doc(title, url);

        size_t cd = std::strlen(corpus_dir);
        size_t di = std::strlen(docid_str);
        char* txt = (char*)std::malloc(cd + 1 + di + 4 + 1);
        std::memcpy(txt, corpus_dir, cd);
        txt[cd] = '/';
        std::memcpy(txt + cd + 1, docid_str, di);
        std::memcpy(txt + cd + 1 + di, ".txt", 4);
        txt[cd + 1 + di + 4] = '\0';

        process_one_doc(txt, doc_id, &tt, &dset, &total_bytes, &total_tokens, &unique_terms_in_docs_sum);
        std::free(txt);

        doc_id++;

        if (total_bytes >= next_report_bytes) {
            double t = now_sec_monotonic();
            double elapsed = t - t0;
            double kb = (double)total_bytes / 1024.0;
            double kbps = (elapsed>0)? (kb/elapsed) : 0.0;
            double avg_unique_per_doc = (doc_id > 0) ? ((double)unique_terms_in_docs_sum / (double)doc_id) : 0.0;

            std::printf("[PROGRESS] docs=%u bytes=%llu (%.1f KB) tokens=%llu avg_unique_terms/doc=%.1f terms_in_block=%llu time=%.2f sec speed=%.1f KB/s mem≈%llu MB\n",
                doc_id,
                (unsigned long long)total_bytes, kb,
                (unsigned long long)total_tokens,
                avg_unique_per_doc,
                (unsigned long long)tt.used,
                elapsed, kbps,
                (unsigned long long)(tt.approx_mem_bytes() / (1024ULL*1024ULL))
            );
            next_report_bytes += report_mb * 1024ULL * 1024ULL;
        }

        if (tt.approx_mem_bytes() >= mem_limit) {
            char blk_path[1024];
            std::snprintf(blk_path, sizeof(blk_path), "%s/block_%04u.blk", blocks_dir, block_id++);
            std::printf("[FLUSH] writing %s terms=%llu\n", blk_path, (unsigned long long)tt.used);
            write_block(blk_path, &tt);
            tt.clear();
        }
    }
    std::fclose(fm);

    if (tt.used > 0) {
        char blk_path[1024];
        std::snprintf(blk_path, sizeof(blk_path), "%s/block_%04u.blk", blocks_dir, block_id++);
        std::printf("[FLUSH] writing %s terms=%llu\n", blk_path, (unsigned long long)tt.used);
        write_block(blk_path, &tt);
        tt.clear();
    }

    char docs_path[1024];
    std::snprintf(docs_path, sizeof(docs_path), "%s/docs.bin", out_dir);
    docs.write_to(docs_path);

    char lex_path[1024], post_path[1024];
    std::snprintf(lex_path, sizeof(lex_path), "%s/lexicon.bin", out_dir);
    std::snprintf(post_path, sizeof(post_path), "%s/postings.bin", out_dir);

    std::printf("[MERGE] blocks -> %s and %s\n", lex_path, post_path);
    merge_blocks_to_index(blocks_dir, lex_path, post_path);

    double t1 = now_sec_monotonic();
    double elapsed = t1 - t0;
    double kb = (double)total_bytes / 1024.0;
    double kbps = (elapsed>0)? (kb/elapsed) : 0.0;
    double avg_unique_per_doc = (doc_id > 0) ? ((double)unique_terms_in_docs_sum / (double)doc_id) : 0.0;

    std::printf("[DONE] docs=%u total_bytes=%llu (%.1f KB) total_tokens=%llu avg_unique_terms/doc=%.1f time=%.2f sec speed=%.1f KB/s\n",
        doc_id,
        (unsigned long long)total_bytes, kb,
        (unsigned long long)total_tokens,
        avg_unique_per_doc,
        elapsed, kbps
    );

    docs.destroy();
    tt.destroy();
    dset.destroy();
    std::free(blocks_dir);

    return 0;
}
