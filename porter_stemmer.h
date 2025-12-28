#pragma once
#include <cstring>

static int is_consonant(const char* s, int i) {
    char ch = s[i];
    if (ch=='a'||ch=='e'||ch=='i'||ch=='o'||ch=='u') return 0;
    if (ch=='y') return (i==0) ? 1 : !is_consonant(s, i-1);
    return 1;
}

static int measure_m(const char* s, int end) {
    int n = 0;
    int i = 0;
    while (1) {
        if (i > end) return n;
        if (!is_consonant(s, i)) break;
        i++;
    }
    i++;
    while (1) {
        while (1) {
            if (i > end) return n;
            if (is_consonant(s, i)) break;
            i++;
        }
        i++;
        n++;
        while (1) {
            if (i > end) return n;
            if (!is_consonant(s, i)) break;
            i++;
        }
        i++;
    }
}

static int contains_vowel(const char* s, int end) {
    for (int i = 0; i <= end; i++) {
        if (!is_consonant(s, i)) return 1;
    }
    return 0;
}

static int double_consonant(const char* s, int end) {
    if (end < 1) return 0;
    if (s[end] != s[end-1]) return 0;
    return is_consonant(s, end);
}

static int cvc(const char* s, int end) {
    if (end < 2) return 0;
    if (!is_consonant(s, end) || is_consonant(s, end-1) || !is_consonant(s, end-2)) return 0;
    char ch = s[end];
    if (ch=='w' || ch=='x' || ch=='y') return 0;
    return 1;
}

static int ends_with(const char* s, int len, const char* suffix) {
    int sl = (int)std::strlen(suffix);
    if (sl > len) return 0;
    return std::memcmp(s + (len - sl), suffix, sl) == 0;
}

static void set_to(char* s, int* len, const char* repl, int cut_suffix_len) {
    int repl_len = (int)std::strlen(repl);
    int new_len = (*len - cut_suffix_len) + repl_len;
    std::memmove(s + (*len - cut_suffix_len), repl, repl_len);
    *len = new_len;
    s[*len] = '\0';
}

static void step1a(char* s, int* len) {
    if (ends_with(s, *len, "sses")) { set_to(s, len, "ss", 4); return; }
    if (ends_with(s, *len, "ies"))  { set_to(s, len, "i", 3);  return; }
    if (ends_with(s, *len, "ss"))   { return; }
    if (ends_with(s, *len, "s"))    { set_to(s, len, "", 1);   return; }
}

static void step1b(char* s, int* len) {
    int did = 0;
    if (ends_with(s, *len, "eed")) {
        int base = *len - 4;
        if (base >= 0 && measure_m(s, base) > 0) { set_to(s, len, "ee", 3); }
        return;
    }
    if (ends_with(s, *len, "ed")) {
        int base = *len - 3;
        if (base >= 0 && contains_vowel(s, base)) {
            set_to(s, len, "", 2);
            did = 1;
        }
    } else if (ends_with(s, *len, "ing")) {
        int base = *len - 4;
        if (base >= 0 && contains_vowel(s, base)) {
            set_to(s, len, "", 3);
            did = 1;
        }
    }
    if (!did) return;

    if (ends_with(s, *len, "at")) { set_to(s, len, "ate", 2); return; }
    if (ends_with(s, *len, "bl")) { set_to(s, len, "ble", 2); return; }
    if (ends_with(s, *len, "iz")) { set_to(s, len, "ize", 2); return; }

    if (double_consonant(s, *len - 1)) {
        char ch = s[*len - 1];
        if (ch!='l' && ch!='s' && ch!='z') {
            s[*len - 1] = '\0';
            (*len)--;
            return;
        }
    }
    if (measure_m(s, *len - 1) == 1 && cvc(s, *len - 1)) {
        set_to(s, len, "e", 0);
        return;
    }
}

static void step1c(char* s, int* len) {
    if (ends_with(s, *len, "y")) {
        int base = *len - 2;
        if (base >= 0 && contains_vowel(s, base)) {
            s[*len - 1] = 'i';
        }
    }
}

static void step2(char* s, int* len) {
    struct Rule { const char* suf; const char* rep; };
    static const Rule rules[] = {
        {"ational","ate"}, {"tional","tion"}, {"enci","ence"}, {"anci","ance"},
        {"izer","ize"}, {"abli","able"}, {"alli","al"}, {"entli","ent"},
        {"eli","e"}, {"ousli","ous"}, {"ization","ize"}, {"ation","ate"},
        {"ator","ate"}, {"alism","al"}, {"iveness","ive"}, {"fulness","ful"},
        {"ousness","ous"}, {"aliti","al"}, {"iviti","ive"}, {"biliti","ble"},
        {nullptr,nullptr}
    };
    for (int i=0; rules[i].suf; i++) {
        int sl = (int)std::strlen(rules[i].suf);
        if (ends_with(s, *len, rules[i].suf)) {
            int base = *len - sl - 1;
            if (base >= 0 && measure_m(s, base) > 0) {
                set_to(s, len, rules[i].rep, sl);
            }
            return;
        }
    }
}

static void step3(char* s, int* len) {
    struct Rule { const char* suf; const char* rep; };
    static const Rule rules[] = {
        {"icate","ic"}, {"ative",""}, {"alize","al"}, {"iciti","ic"},
        {"ical","ic"}, {"ful",""}, {"ness",""},
        {nullptr,nullptr}
    };
    for (int i=0; rules[i].suf; i++) {
        int sl = (int)std::strlen(rules[i].suf);
        if (ends_with(s, *len, rules[i].suf)) {
            int base = *len - sl - 1;
            if (base >= 0 && measure_m(s, base) > 0) {
                set_to(s, len, rules[i].rep, sl);
            }
            return;
        }
    }
}

static void step4(char* s, int* len) {
    static const char* sufs[] = {
        "al","ance","ence","er","ic","able","ible","ant","ement","ment","ent",
        "ion","ou","ism","ate","iti","ous","ive","ize", nullptr
    };
    for (int i=0; sufs[i]; i++) {
        int sl = (int)std::strlen(sufs[i]);
        if (ends_with(s, *len, sufs[i])) {
            int base = *len - sl - 1;
            if (base < 0) return;
            if (std::strcmp(sufs[i], "ion") == 0) {
                if (s[*len - sl - 1] != 's' && s[*len - sl - 1] != 't') return;
            }
            if (measure_m(s, base) > 1) {
                set_to(s, len, "", sl);
            }
            return;
        }
    }
}

static void step5(char* s, int* len) {
    if (ends_with(s, *len, "e")) {
        int base = *len - 2;
        int m = (base >= 0) ? measure_m(s, base) : 0;
        if (m > 1 || (m == 1 && !cvc(s, base))) {
            (*len)--;
            s[*len] = '\0';
        }
    }
    if (*len >= 2 && ends_with(s, *len, "ll")) {
        int base = *len - 1;
        if (measure_m(s, base) > 1) {
            (*len)--;
            s[*len] = '\0';
        }
    }
}

static int porter_stem(char* s, int len) {
    if (len <= 2) return len;
    step1a(s, &len);
    step1b(s, &len);
    step1c(s, &len);
    step2(s, &len);
    step3(s, &len);
    step4(s, &len);
    step5(s, &len);
    return len;
}
