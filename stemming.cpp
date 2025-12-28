#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <time.h>

#include <dirent.h>
#include <sys/stat.h>

extern "C" int stem_word_en(char* w, int len);

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
static int ends_with_txt(const char* name) {
    size_t n = std::strlen(name);
    return (n >= 4 && std::strcmp(name + (n - 4), ".txt") == 0);
}


static inline int is_consonant(const char* b, int i) {
    char ch = b[i];
    if (ch=='a'||ch=='e'||ch=='i'||ch=='o'||ch=='u') return 0;
    if (ch=='y') {
        if (i==0) return 1;
        return !is_consonant(b, i-1);
    }
    return 1;
}

static int m_measure(const char* b, int j) {
    int n = 0;
    int i = 0;
    while (1) {
        if (i > j) return n;
        if (!is_consonant(b, i)) break;
        i++;
    }
    i++;
    while (1) {
        while (1) {
            if (i > j) return n;
            if (is_consonant(b, i)) break;
            i++;
        }
        i++;
        n++;
        while (1) {
            if (i > j) return n;
            if (!is_consonant(b, i)) break;
            i++;
        }
        i++;
    }
}

static int vowel_in_stem(const char* b, int j) {
    for (int i=0;i<=j;i++) if (!is_consonant(b,i)) return 1;
    return 0;
}

static int doublec(const char* b, int j) {
    if (j < 1) return 0;
    if (b[j] != b[j-1]) return 0;
    return is_consonant(b, j);
}

static int cvc(const char* b, int i) {
    if (i < 2) return 0;
    if (!is_consonant(b,i) || is_consonant(b,i-1) || !is_consonant(b,i-2)) return 0;
    char ch = b[i];
    if (ch=='w' || ch=='x' || ch=='y') return 0;
    return 1;
}

static int ends_with(const char* b, int k, const char* s, int slen) {
    if (slen > k+1) return 0;
    return std::memcmp(b + (k+1-slen), s, (size_t)slen) == 0;
}

static void set_to(char* b, int* k, const char* s, int slen, int* j) {
    int start = *j + 1;
    std::memcpy(b + start, s, (size_t)slen);
    *k = start + slen - 1;
    b[*k + 1] = '\0';
}

static void r(char* b, int* k, const char* s, int slen, int* j) {
    if (m_measure(b, *j) > 0) set_to(b, k, s, slen, j);
}

static void step1ab(char* b, int* k, int* j) {

    if (ends_with(b, *k, "sses", 4)) { *k -= 2; b[*k+1]='\0'; }
    else if (ends_with(b, *k, "ies", 3)) { *k -= 2; b[*k+1]='\0'; } 
    else if (ends_with(b, *k, "ss", 2)) { /* do nothing */ }
    else if (ends_with(b, *k, "s", 1)) { *k -= 1; b[*k+1]='\0'; }

    int flag = 0;
    if (ends_with(b, *k, "eed", 3)) {
        *j = *k - 3;
        if (m_measure(b, *j) > 0) { *k -= 1; b[*k+1]='\0'; }
    } else if ((ends_with(b, *k, "ed", 2) && (*j = *k - 2, vowel_in_stem(b, *j))) ||
               (ends_with(b, *k, "ing", 3) && (*j = *k - 3, vowel_in_stem(b, *j)))) {
        *k = *j;
        b[*k+1]='\0';
        flag = 1;
    }

    if (flag) {
        if (ends_with(b, *k, "at", 2)) { *j = *k - 2; set_to(b, k, "ate", 3, j); }
        else if (ends_with(b, *k, "bl", 2)) { *j = *k - 2; set_to(b, k, "ble", 3, j); }
        else if (ends_with(b, *k, "iz", 2)) { *j = *k - 2; set_to(b, k, "ize", 3, j); }
        else if (doublec(b, *k)) {
            char ch = b[*k];
            if (ch!='l' && ch!='s' && ch!='z') { (*k)--; b[*k+1]='\0'; }
        } else if (m_measure(b, *k) == 1 && cvc(b, *k)) {
            *j = *k;
            set_to(b, k, "e", 1, j);
        }
    }

    if (ends_with(b, *k, "y", 1)) {
        *j = *k - 1;
        if (vowel_in_stem(b, *j)) { b[*k] = 'i'; }
    }
}

static void step2(char* b, int* k, int* j) {
    struct Rule { const char* suf; int slen; const char* rep; int rlen; };
    static const Rule rules[] = {
        {"ational",7,"ate",3},
        {"tional",6,"tion",4},
        {"enci",4,"ence",4},
        {"anci",4,"ance",4},
        {"izer",4,"ize",3},
        {"abli",4,"able",4},
        {"alli",4,"al",2},
        {"entli",5,"ent",3},
        {"eli",3,"e",1},
        {"ousli",5,"ous",3},
        {"ization",7,"ize",3},
        {"ation",5,"ate",3},
        {"ator",4,"ate",3},
        {"alism",5,"al",2},
        {"iveness",7,"ive",3},
        {"fulness",7,"ful",3},
        {"ousness",7,"ous",3},
        {"aliti",5,"al",2},
        {"iviti",5,"ive",3},
        {"biliti",6,"ble",3},
        {"logi",4,"log",3},
    };

    for (size_t i=0;i<sizeof(rules)/sizeof(rules[0]);i++) {
        if (ends_with(b, *k, rules[i].suf, rules[i].slen)) {
            *j = *k - rules[i].slen;
            if (m_measure(b, *j) > 0) {
                set_to(b, k, rules[i].rep, rules[i].rlen, j);
            }
            return;
        }
    }
}

static void step3(char* b, int* k, int* j) {
    struct Rule { const char* suf; int slen; const char* rep; int rlen; };
    static const Rule rules[] = {
        {"icate",5,"ic",2},
        {"ative",5,"",0},
        {"alize",5,"al",2},
        {"iciti",5,"ic",2},
        {"ical",4,"ic",2},
        {"ful",3,"",0},
        {"ness",4,"",0},
    };
    for (size_t i=0;i<sizeof(rules)/sizeof(rules[0]);i++) {
        if (ends_with(b, *k, rules[i].suf, rules[i].slen)) {
            *j = *k - rules[i].slen;
            r(b, k, rules[i].rep, rules[i].rlen, j);
            return;
        }
    }
}

static void step4(char* b, int* k, int* j) {
    static const char* sufs[] = {
        "al","ance","ence","er","ic","able","ible","ant","ement","ment","ent",
        "ion","ou","ism","ate","iti","ous","ive","ize"
    };
    static const int lens[] = {
        2,4,4,2,2,4,4,3,5,4,3,
        3,2,3,3,3,3,3,3
    };
    for (size_t i=0;i<sizeof(lens)/sizeof(lens[0]);i++) {
        const char* suf = sufs[i];
        int slen = lens[i];
        if (ends_with(b, *k, suf, slen)) {
            *j = *k - slen;
            if (std::strcmp(suf, "ion") == 0) {
                if (*j >= 0) {
                    char ch = b[*j];
                    if (ch != 's' && ch != 't') return;
                } else return;
            }
            if (m_measure(b, *j) > 1) {
                *k = *j;
                b[*k+1]='\0';
            }
            return;
        }
    }
}

static void step5(char* b, int* k, int* j) {
    // step5a
    if (ends_with(b, *k, "e", 1)) {
        *j = *k - 1;
        int m = m_measure(b, *j);
        if (m > 1 || (m == 1 && !cvc(b, *j))) {
            (*k)--;
            b[*k+1]='\0';
        }
    }
    // step5b
    if (m_measure(b, *k) > 1 && doublec(b, *k) && b[*k] == 'l') {
        (*k)--;
        b[*k+1]='\0';
    }
}

static int porter_stem_inplace(char* b, int len) {
    if (len <= 2) { b[len] = '\0'; return len; }
    int has_letter = 0;
    for (int i=0;i<len;i++) {
        char c = b[i];
        if (c >= 'a' && c <= 'z') { has_letter = 1; break; }
    }
    if (!has_letter) { b[len]='\0'; return len; }

    int k = len - 1;
    int j = 0;

    step1ab(b, &k, &j);
    step2(b, &k, &j);
    step3(b, &k, &j);
    step4(b, &k, &j);
    step5(b, &k, &j);

    int out_len = k + 1;
    if (out_len < 0) out_len = 0;
    b[out_len] = '\0';
    return out_len;
}
extern "C" int stem_word_en(char* w, int len) {
    return porter_stem_inplace(w, len);
}

static uint64_t file_size(FILE* f) {
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    return (sz < 0) ? 0 : (uint64_t)sz;
}

#ifndef STEMMER_LIB
int main(int argc, char** argv) {
    const char* dir = nullptr;
    uint32_t report_mb = 50;

    for (int i=1;i<argc;i++) {
        if (std::strcmp(argv[i], "--dir") == 0 && i+1 < argc) dir = argv[++i];
        else if (std::strcmp(argv[i], "--report-mb") == 0 && i+1 < argc) report_mb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s --dir <corpus_dir> [--report-mb 50]\n", argv[0]);
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

    DIR* d = opendir(dir);
    if (!d) {
        std::fprintf(stderr, "opendir failed: %s (%s)\n", dir, std::strerror(errno));
        return 1;
    }

    const size_t BUF_SZ = 1 << 20; 
    char* buf = (char*)std::malloc(BUF_SZ);
    if (!buf) { std::fprintf(stderr, "malloc buf failed\n"); return 1; }

    char tok[256];

    uint64_t bytes_total = 0;
    uint64_t tokens_raw = 0;
    uint64_t tokens_stem = 0;
    uint64_t sum_raw_len = 0;
    uint64_t sum_stem_len = 0;
    uint64_t changed = 0;

    uint64_t next_report = (uint64_t)report_mb * 1024ULL * 1024ULL;
    double t0 = now_sec_monotonic();

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (!ends_with_txt(ent->d_name)) continue;

        char path[2048];
        std::snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        FILE* f = std::fopen(path, "rb");
        if (!f) continue;

        (void)file_size(f);

        size_t rd;
        int in_tok = 0;
        int tlen = 0;

        while ((rd = std::fread(buf, 1, BUF_SZ, f)) > 0) {
            bytes_total += (uint64_t)rd;

            for (size_t i=0;i<rd;i++) {
                unsigned char c = (unsigned char)buf[i];
                if (is_ascii_alnum(c)) {
                    if (tlen < 255) tok[tlen++] = (char)to_lower_ascii(c);
                    in_tok = 1;
                } else {
                    if (in_tok && tlen > 0) {
                        tok[tlen] = '\0';

                        tokens_raw++;
                        sum_raw_len += (uint64_t)tlen;

                        char tmp[256];
                        std::memcpy(tmp, tok, (size_t)tlen + 1);

                        int newlen = stem_word_en(tmp, tlen);
                        tokens_stem++;
                        sum_stem_len += (uint64_t)newlen;

                        if (newlen != tlen || std::memcmp(tmp, tok, (size_t)((tlen<newlen)?tlen:newlen)) != 0) {
                            changed++;
                        }
                    }
                    in_tok = 0;
                    tlen = 0;
                }
            }

            if (bytes_total >= next_report) {
                double t = now_sec_monotonic() - t0;
                double kb = (double)bytes_total / 1024.0;
                double speed = (t > 0.0) ? (kb / t) : 0.0;

                double avg_raw = (tokens_raw ? (double)sum_raw_len / (double)tokens_raw : 0.0);
                double avg_stem = (tokens_stem ? (double)sum_stem_len / (double)tokens_stem : 0.0);
                double chp = (tokens_raw ? (100.0 * (double)changed / (double)tokens_raw) : 0.0);

                std::printf("[PROGRESS] bytes=%llu (%.1f KB) time=%.3f sec speed=%.1f KB/s | raw_tokens=%llu avg_raw=%.3f | stem_tokens=%llu avg_stem=%.3f | changed=%llu (%.2f%%)\n",
                    (unsigned long long)bytes_total, kb, t, speed,
                    (unsigned long long)tokens_raw, avg_raw,
                    (unsigned long long)tokens_stem, avg_stem,
                    (unsigned long long)changed, chp
                );
                std::fflush(stdout);

                next_report += (uint64_t)report_mb * 1024ULL * 1024ULL;
            }
        }

        if (in_tok && tlen > 0) {
            tok[tlen] = '\0';

            tokens_raw++;
            sum_raw_len += (uint64_t)tlen;

            char tmp[256];
            std::memcpy(tmp, tok, (size_t)tlen + 1);

            int newlen = stem_word_en(tmp, tlen);
            tokens_stem++;
            sum_stem_len += (uint64_t)newlen;

            if (newlen != tlen || std::memcmp(tmp, tok, (size_t)((tlen<newlen)?tlen:newlen)) != 0) {
                changed++;
            }
        }

        std::fclose(f);
    }

    closedir(d);
    std::free(buf);

    double t = now_sec_monotonic() - t0;
    double kb = (double)bytes_total / 1024.0;
    double speed = (t > 0.0) ? (kb / t) : 0.0;

    double avg_raw = (tokens_raw ? (double)sum_raw_len / (double)tokens_raw : 0.0);
    double avg_stem = (tokens_stem ? (double)sum_stem_len / (double)tokens_stem : 0.0);
    double chp = (tokens_raw ? (100.0 * (double)changed / (double)tokens_raw) : 0.0);

    std::printf("[FINAL] bytes=%llu (%.1f KB) time=%.3f sec speed=%.1f KB/s | raw_tokens=%llu avg_raw=%.3f | stem_tokens=%llu avg_stem=%.3f | changed=%llu (%.2f%%)\n",
        (unsigned long long)bytes_total, kb, t, speed,
        (unsigned long long)tokens_raw, avg_raw,
        (unsigned long long)tokens_stem, avg_stem,
        (unsigned long long)changed, chp
    );

    return 0;
}
#endif