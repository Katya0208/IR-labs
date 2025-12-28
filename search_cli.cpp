#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <time.h>

#include "stemmer_api.h"

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

static void* read_whole_file(const char* path, size_t* out_size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "Cannot open %s: %s\n", path, std::strerror(errno));
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return nullptr; }

    void* buf = std::malloc((size_t)sz);
    if (!buf) { std::fprintf(stderr, "malloc failed\n"); std::fclose(f); return nullptr; }

    if (std::fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        std::fprintf(stderr, "read failed %s\n", path);
        std::free(buf);
        std::fclose(f);
        return nullptr;
    }
    std::fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

struct Index {
    DocsHeader* dh = nullptr;
    DocRec*     docs = nullptr;
    char*       doc_pool = nullptr;

    LexHeader* lh = nullptr;
    LexRec*    lex = nullptr;
    char*      term_pool = nullptr;

    char* postings_file = nullptr;
    size_t postings_size = 0;

    void* docs_file = nullptr;
    size_t docs_size = 0;
    void* lex_file = nullptr;
    size_t lex_size = 0;

    uint32_t doc_count() const { return dh ? dh->doc_count : 0; }
    uint32_t term_count() const { return lh ? lh->term_count : 0; }

    const char* doc_title(uint32_t id, uint32_t* out_len) const {
        const DocRec& r = docs[id];
        *out_len = r.title_len;
        return doc_pool + r.title_off;
    }
    const char* doc_url(uint32_t id, uint32_t* out_len) const {
        const DocRec& r = docs[id];
        *out_len = r.url_len;
        return doc_pool + r.url_off;
    }

    int find_term(const char* t, uint16_t tlen, uint32_t* out_idx) const {
        uint32_t lo = 0, hi = term_count();
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const LexRec& r = lex[mid];
            const char* s = term_pool + r.term_off;
            uint16_t slen = r.term_len;

            int m = (tlen < slen) ? tlen : slen;
            int c = std::memcmp(t, s, (size_t)m);
            if (c == 0) {
                if (tlen < slen) c = -1;
                else if (tlen > slen) c = 1;
            }
            if (c == 0) { *out_idx = mid; return 1; }
            if (c < 0) hi = mid;
            else lo = mid + 1;
        }
        return 0;
    }

    const uint32_t* postings_ptr(const LexRec& r) const {
        uint64_t need = r.postings_off + (uint64_t)r.postings_len * 4ULL;
        if (need > (uint64_t)postings_size) return nullptr;
        return (const uint32_t*)(postings_file + r.postings_off);
    }

    int load(const char* index_dir) {
        char p_docs[1024], p_lex[1024], p_post[1024];
        std::snprintf(p_docs, sizeof(p_docs), "%s/docs.bin", index_dir);
        std::snprintf(p_lex,  sizeof(p_lex),  "%s/lexicon.bin", index_dir);
        std::snprintf(p_post, sizeof(p_post), "%s/postings.bin", index_dir);

        docs_file = read_whole_file(p_docs, &docs_size);
        if (!docs_file) return 0;
        lex_file = read_whole_file(p_lex, &lex_size);
        if (!lex_file) return 0;
        postings_file = (char*)read_whole_file(p_post, &postings_size);
        if (!postings_file) return 0;

        dh = (DocsHeader*)docs_file;
        if (docs_size < sizeof(DocsHeader) || std::memcmp(dh->magic, "DOCS", 4) != 0 || dh->version != 1) {
            std::fprintf(stderr, "Bad docs.bin\n"); return 0;
        }
        docs = (DocRec*)((char*)docs_file + sizeof(DocsHeader));
        doc_pool = (char*)docs + (size_t)dh->doc_count * sizeof(DocRec);

        lh = (LexHeader*)lex_file;
        if (lex_size < sizeof(LexHeader) || std::memcmp(lh->magic, "LEXI", 4) != 0 || lh->version != 1) {
            std::fprintf(stderr, "Bad lexicon.bin\n"); return 0;
        }
        lex = (LexRec*)((char*)lex_file + sizeof(LexHeader));
        term_pool = (char*)lex + (size_t)lh->term_count * sizeof(LexRec);

        PostHeader* ph = (PostHeader*)postings_file;
        if (postings_size < sizeof(PostHeader) || std::memcmp(ph->magic, "POST", 4) != 0 || ph->version != 1) {
            std::fprintf(stderr, "Bad postings.bin\n"); return 0;
        }

        return 1;
    }

    void destroy() {
        std::free(docs_file); docs_file=nullptr; docs_size=0;
        std::free(lex_file);  lex_file=nullptr;  lex_size=0;
        std::free(postings_file); postings_file=nullptr; postings_size=0;
    }
};

struct U32Vec {
    uint32_t* a = nullptr;
    uint32_t n = 0;
    uint32_t cap = 0;

    void clear() { n = 0; }
    void free_mem() { std::free(a); a=nullptr; n=cap=0; }

    void reserve(uint32_t need) {
        if (cap >= need) return;
        uint32_t nc = cap ? cap : 8;
        while (nc < need) nc *= 2;
        uint32_t* nb = (uint32_t*)std::realloc(a, (size_t)nc * sizeof(uint32_t));
        if (!nb) { std::fprintf(stderr, "realloc U32Vec failed\n"); std::exit(1); }
        a = nb; cap = nc;
    }
    void push(uint32_t v) {
        if (n == cap) reserve(cap ? cap*2 : 8);
        a[n++] = v;
    }
};

static void op_and(const uint32_t* a, uint32_t na, const uint32_t* b, uint32_t nb, U32Vec* out) {
    out->clear();
    out->reserve((na < nb) ? na : nb);
    uint32_t i=0,j=0;
    while (i<na && j<nb) {
        uint32_t x=a[i], y=b[j];
        if (x==y) { out->push(x); i++; j++; }
        else if (x<y) i++;
        else j++;
    }
}
static void op_or(const uint32_t* a, uint32_t na, const uint32_t* b, uint32_t nb, U32Vec* out) {
    out->clear();
    out->reserve(na + nb);
    uint32_t i=0,j=0;
    while (i<na && j<nb) {
        uint32_t x=a[i], y=b[j];
        if (x==y) { out->push(x); i++; j++; }
        else if (x<y) { out->push(x); i++; }
        else { out->push(y); j++; }
    }
    while (i<na) out->push(a[i++]);
    while (j<nb) out->push(b[j++]);
}
static void op_not(uint32_t doc_count, const uint32_t* a, uint32_t na, U32Vec* out) {
    out->clear();
    out->reserve(doc_count > na ? (doc_count - na) : 0);
    uint32_t i=0;
    for (uint32_t d=0; d<doc_count; d++) {
        while (i<na && a[i] < d) i++;
        if (i<na && a[i]==d) continue;
        out->push(d);
    }
}

enum TokType { T_TERM, T_AND, T_OR, T_NOT, T_LP, T_RP, T_END, T_BAD };

struct Tok {
    TokType type;
    char text[256];
    uint16_t len;
};

struct TokStream {
    const char* s;
    size_t i;
    size_t n;

    void init(const char* line) { s=line; i=0; n=std::strlen(line); }

    void skip_spaces() {
        while (i<n && (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) i++;
    }

    Tok next() {
        skip_spaces();
        Tok t{}; t.type=T_END; t.len=0; t.text[0]='\0';
        if (i>=n) return t;

        char c = s[i];
        if (c=='(') { i++; t.type=T_LP; return t; }
        if (c==')') { i++; t.type=T_RP; return t; }
        if (c=='!') { i++; t.type=T_NOT; return t; }
        if (c=='&') {
            if (i+1<n && s[i+1]=='&') i+=2;
            else i+=1;
            t.type=T_AND;
            return t;
        }
        if (c=='|') {
            if (i+1<n && s[i+1]=='|') i+=2;
            else i+=1;
            t.type=T_OR;
            return t;
        }

        if (is_ascii_alnum((unsigned char)c)) {
            int k=0;
            while (i<n && is_ascii_alnum((unsigned char)s[i])) {
                unsigned char cc=(unsigned char)s[i++];
                cc = to_lower_ascii(cc);
                if (k<255) t.text[k++] = (char)cc;
            }
            t.text[k]='\0';
            t.len=(uint16_t)k;
            t.type=T_TERM;
            return t;
        }
        i++;
        t.type=T_BAD;
        return t;
    }
};

static int precedence(TokType t) {
    if (t==T_NOT) return 3;
    if (t==T_AND) return 2;
    if (t==T_OR)  return 1;
    return 0;
}
static int is_right_assoc(TokType t) { return (t==T_NOT); }

struct RpnItem {
    TokType type;
    char text[256];
    uint16_t len;
};

struct RpnVec {
    RpnItem* a=nullptr;
    uint32_t n=0, cap=0;
    void clear(){ n=0; }
    void free_mem(){ std::free(a); a=nullptr; n=cap=0; }
    void push(const RpnItem& it){
        if (n==cap){
            uint32_t nc = cap?cap*2:64;
            RpnItem* nb=(RpnItem*)std::realloc(a,(size_t)nc*sizeof(RpnItem));
            if(!nb){ std::fprintf(stderr,"realloc RpnVec failed\n"); std::exit(1); }
            a=nb; cap=nc;
        }
        a[n++]=it;
    }
};

struct TokStack {
    TokType* a=nullptr;
    uint32_t n=0, cap=0;
    void free_mem(){ std::free(a); a=nullptr; n=cap=0; }
    void push(TokType t){
        if(n==cap){
            uint32_t nc=cap?cap*2:32;
            TokType* nb=(TokType*)std::realloc(a,(size_t)nc*sizeof(TokType));
            if(!nb){ std::fprintf(stderr,"realloc TokStack failed\n"); std::exit(1); }
            a=nb; cap=nc;
        }
        a[n++]=t;
    }
    TokType pop(){ return a[--n]; }
    TokType top() const { return a[n-1]; }
    int empty() const { return n==0; }
};

static int is_value_token(TokType t){ return t==T_TERM || t==T_RP; }
static int can_start_value(TokType t){ return t==T_TERM || t==T_LP || t==T_NOT; }

static void normalize_term(char* s, uint16_t* len) {
    int n = stem_word_en(s, (int)*len);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s[n] = '\0';
    *len = (uint16_t)n;
}

static void to_rpn(const char* line, RpnVec* out) {
    out->clear();
    TokStream ts; ts.init(line);
    TokStack ops;

    Tok prev{}; prev.type=T_END;

    while(1){
        Tok tok = ts.next();
        if(tok.type==T_BAD) continue;
        if(tok.type==T_END) break;

        if(is_value_token(prev.type) && can_start_value(tok.type)){
            TokType op=T_AND;
            while(!ops.empty()){
                TokType top=ops.top();
                if(top==T_LP) break;
                int p1=precedence(top), p2=precedence(op);
                if(p1>p2 || (p1==p2 && !is_right_assoc(op))){
                    RpnItem it{}; it.type=top; out->push(it);
                    ops.pop();
                } else break;
            }
            ops.push(op);
        }

        if(tok.type==T_TERM){
            normalize_term(tok.text, &tok.len);
            if (tok.len > 0) {
                RpnItem it{}; it.type=T_TERM; it.len=tok.len;
                std::memcpy(it.text, tok.text, tok.len+1);
                out->push(it);
            } else {
            }
        } else if(tok.type==T_LP){
            ops.push(T_LP);
        } else if(tok.type==T_RP){
            while(!ops.empty() && ops.top()!=T_LP){
                RpnItem it{}; it.type=ops.pop();
                out->push(it);
            }
            if(!ops.empty() && ops.top()==T_LP) ops.pop(); 
        } else if(tok.type==T_AND || tok.type==T_OR || tok.type==T_NOT){
            TokType op=tok.type;
            while(!ops.empty()){
                TokType top=ops.top();
                if(top==T_LP) break;
                int p1=precedence(top), p2=precedence(op);
                if(p1>p2 || (p1==p2 && !is_right_assoc(op))){
                    RpnItem it{}; it.type=top; out->push(it);
                    ops.pop();
                } else break;
            }
            ops.push(op);
        }

        prev=tok;
    }

    while(!ops.empty()){
        TokType top=ops.pop();
        if(top==T_LP) continue;
        RpnItem it{}; it.type=top;
        out->push(it);
    }

    ops.free_mem();
}

struct Res { uint32_t* a=nullptr; uint32_t n=0; };

struct ResStack {
    Res* a=nullptr; uint32_t n=0, cap=0;
    void free_all(){
        for(uint32_t i=0;i<n;i++) std::free(a[i].a);
        std::free(a); a=nullptr; n=cap=0;
    }
    void push(uint32_t* arr, uint32_t n_){
        if(n==cap){
            uint32_t nc=cap?cap*2:32;
            Res* nb=(Res*)std::realloc(a,(size_t)nc*sizeof(Res));
            if(!nb){ std::fprintf(stderr,"realloc ResStack failed\n"); std::exit(1); }
            a=nb; cap=nc;
        }
        a[n++]=Res{arr,n_};
    }
    int empty() const { return n==0; }
    Res pop_safe(){
        if (n==0) return Res{nullptr,0};
        return a[--n];
    }
};

static uint32_t* copy_list(const uint32_t* p, uint32_t n){
    if(n==0) return nullptr;
    uint32_t* a=(uint32_t*)std::malloc((size_t)n*sizeof(uint32_t));
    if(!a){ std::fprintf(stderr,"malloc copy_list failed\n"); std::exit(1); }
    std::memcpy(a,p,(size_t)n*sizeof(uint32_t));
    return a;
}

static void eval_rpn(const Index& idx, const RpnVec& rpn, Res* out_res) {
    ResStack st;
    U32Vec tmp;

    for(uint32_t i=0;i<rpn.n;i++){
        const RpnItem& it=rpn.a[i];

        if(it.type==T_TERM){
            uint32_t lex_i=0;
            if(!idx.find_term(it.text,it.len,&lex_i)){
                st.push(nullptr,0);
            } else {
                const LexRec& r=idx.lex[lex_i];
                const uint32_t* p=idx.postings_ptr(r);
                if(!p || r.postings_len==0) st.push(nullptr,0);
                else st.push(copy_list(p,r.postings_len), r.postings_len);
            }
        }
        else if(it.type==T_NOT){
            Res a = st.pop_safe();
            op_not(idx.doc_count(), a.a, a.n, &tmp);
            std::free(a.a);
            st.push(copy_list(tmp.a,tmp.n), tmp.n);
        }
        else if(it.type==T_AND || it.type==T_OR){
            Res b = st.pop_safe();
            Res a = st.pop_safe();

            if(it.type==T_AND){
                if (a.n==0 || b.n==0) {
                    std::free(a.a); std::free(b.a);
                    st.push(nullptr,0);
                } else {
                    op_and(a.a,a.n,b.a,b.n,&tmp);
                    std::free(a.a); std::free(b.a);
                    st.push(copy_list(tmp.a,tmp.n), tmp.n);
                }
            } else {
                if (a.n==0 && b.n==0) {
                    std::free(a.a); std::free(b.a);
                    st.push(nullptr,0);
                } else if (a.n==0) {
                    st.push(b.a, b.n);
                    std::free(a.a);
                } else if (b.n==0) {
                    st.push(a.a, a.n);
                    std::free(b.a);
                } else {
                    op_or(a.a,a.n,b.a,b.n,&tmp);
                    std::free(a.a); std::free(b.a);
                    st.push(copy_list(tmp.a,tmp.n), tmp.n);
                }
            }
        }
    }
    Res res = st.pop_safe();
    while(!st.empty()){
        Res x = st.pop_safe();
        std::free(x.a);
    }
    std::free(st.a);
    tmp.free_mem();

    *out_res = res;
}

static void chomp(char* s) {
    size_t n = std::strlen(s);
    while (n>0 && (s[n-1]=='\n' || s[n-1]=='\r')) { s[n-1]='\0'; n--; }
}

int main(int argc, char** argv){
    const char* index_dir="./out";
    uint32_t limit=50;
    uint32_t offset=0;
    int stats_only=0;
    int print_doccount=0;

    for(int i=1;i<argc;i++){
        if(std::strcmp(argv[i],"--index")==0 && i+1<argc) index_dir=argv[++i];
        else if(std::strcmp(argv[i],"--limit")==0 && i+1<argc) limit=(uint32_t)std::strtoul(argv[++i],nullptr,10);
        else if(std::strcmp(argv[i],"--offset")==0 && i+1<argc) offset=(uint32_t)std::strtoul(argv[++i],nullptr,10);
        else if(std::strcmp(argv[i],"--stats-only")==0) stats_only=1;
        else if(std::strcmp(argv[i],"--print-doccount")==0) print_doccount=1;
        else if(std::strcmp(argv[i],"--help")==0){
            std::printf("Usage: %s --index <dir> [--limit 50] [--offset 0] [--stats-only] [--print-doccount]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr,"Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    Index idx;
    if(!idx.load(index_dir)){
        std::fprintf(stderr,"Index load failed\n");
        return 1;
    }

    if (print_doccount) {
        std::printf("%u\n", idx.doc_count());
        idx.destroy();
        return 0;
    }

    char line[8192];
    while(std::fgets(line,sizeof(line),stdin)){
        chomp(line);
        int only_ws=1;
        for(size_t i=0;i<std::strlen(line);i++){
            if(!(line[i]==' '||line[i]=='\t')) { only_ws=0; break; }
        }
        if(only_ws) continue;

        double t0=now_sec_monotonic();

        RpnVec rpn;
        to_rpn(line,&rpn);

        Res res{};
        eval_rpn(idx,rpn,&res);

        double t1=now_sec_monotonic();
        double elapsed=t1-t0;

        uint32_t shown=0;
        if (!stats_only) {
            for(uint32_t i=offset;i<res.n && shown<limit;i++){
                uint32_t id=res.a[i];
                if(id>=idx.doc_count()) continue;
                uint32_t tl=0, ul=0;
                const char* title=idx.doc_title(id,&tl);
                const char* url=idx.doc_url(id,&ul);
                std::printf("%u\t%.*s\t%.*s\n", id, (int)tl, title, (int)ul, url);
                shown++;
            }
        } else {
            if (offset < res.n) {
                uint32_t left = res.n - offset;
                shown = (left < limit) ? left : limit;
            } else shown = 0;
        }

        std::printf("[STATS] query=\"%s\" hits=%u shown=%u offset=%u time=%.6f sec\n",
            line, res.n, shown, offset, elapsed);

        std::free(res.a);
        rpn.free_mem();
    }

    idx.destroy();
    return 0;
}
