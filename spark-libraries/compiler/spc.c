/* =============================================================================
 *  spc - the Sparks compiler                                      (version 0.1)
 *
 *  A small, readable compiler written in C. It turns a Sparks program (.sp)
 *  into a real native program. There is NO interpreter: your code is checked,
 *  translated to C, and handed to the system C compiler, which makes machine code.
 *
 *  THE PIPELINE  (read this file top to bottom, it follows the same order!)
 *
 *    your .sp file
 *       |  1. LEXER    characters -> tokens
 *       |  2. PARSER   tokens     -> syntax tree (AST)   <- SYNTAX errors are caught here
 *       |  3. CHECKER  the tree   -> names + types       <- NAME / TYPE errors are caught here
 *       |  4. CODEGEN  the tree   -> C source code       (3 and 4 happen in one walk)
 *       |  5. cc/gcc   C source   -> machine code
 *       v
 *    a program you can run
 *
 *  Build:  cc -O2 -o spc spc.c
 *  Use:    ./spc game.sp --run          (see ./spc --help)
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdbool.h>
#ifdef _WIN32
#include <io.h>
#define ISATTY(fd) _isatty(fd)
#define PATH_SEP '\\'
#define EXE_SUFFIX ".exe"
#define NULL_DEV "nul"
#else
#include <unistd.h>
#define ISATTY(fd) isatty(fd)
#define PATH_SEP '/'
#define EXE_SUFFIX ""
#define NULL_DEV "/dev/null"
#endif

#define VERSION    "0.1"
#define MAX_ERRORS 10
#define MAX_PARAMS 12
#define MAX_FIELDS 24

/* =============================================================================
 *  0. TINY HELPERS
 * ============================================================================= */

static void die(const char *msg) { fprintf(stderr, "spc: %s\n", msg); exit(2); }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }
static char *xstrndup(const char *s, size_t n) { char *r = xmalloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

/* A growing string. We build all the generated C code with these. */
typedef struct { char *s; int len, cap; } Buf;
#define BS(b) ((b).s ? (b).s : "")          /* the text inside a Buf ("" if empty) */

static void buf_reserve(Buf *b, int extra) {
    if (b->len + extra + 1 > b->cap) {
        b->cap = (b->len + extra + 1) * 2;
        if (b->cap < 64) b->cap = 64;
        b->s = xrealloc(b->s, b->cap);
    }
}
static void buf_add(Buf *b, const char *s) {
    int n = (int)strlen(s);
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}
static void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    buf_reserve(b, n);
    vsnprintf(b->s + b->len, n + 1, fmt, ap);
    b->len += n;
}
static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); buf_vprintf(b, fmt, ap); va_end(ap);
}

/* "Did you mean ...?" support: how different are two words? */
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = tolower((unsigned char)a[i-1]) != tolower((unsigned char)b[j-1]);
            int m = d[i-1][j] + 1;
            if (d[i][j-1] + 1 < m) m = d[i][j-1] + 1;
            if (d[i-1][j-1] + c < m) m = d[i-1][j-1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}
static const char *closest(const char *name, const char **cands, int n) {
    int L = (int)strlen(name);
    if (L < 2) return NULL;
    int lim = L <= 3 ? 1 : (L <= 7 ? 2 : 3);
    const char *best = NULL; int bd = 99;
    for (int i = 0; i < n; i++) {
        if (!strcmp(name, cands[i])) continue;
        int dd = edit_distance(name, cands[i]);
        if (dd < bd) { bd = dd; best = cands[i]; }
    }
    return (best && bd <= lim && bd < L) ? best : NULL;
}

/* =============================================================================
 *  1. SOURCE FILES, TOKENS AND THE ERROR REPORTER
 * ============================================================================= */

typedef struct { char *path; char *text; } Source;
static Source srcs[64];
static int nsrcs = 0;
static int use_color = 0;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = xmalloc(n + 1);
    size_t got = fread(b, 1, n, f); fclose(f);
    /* drop '\r' (Windows line endings) and a UTF-8 byte-order mark */
    char *w = b; size_t i = 0;
    if (got >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) i = 3;
    for (; i < got; i++) if (b[i] != '\r') *w++ = b[i];
    *w = 0;
    return b;
}

enum {
    T_EOF, T_NEWLINE, T_INDENT, T_DEDENT, T_IDENT, T_INT, T_FLOAT, T_STRING, T_IMPORT,
    /* keywords (must stay in this order: T_LET .. T_NOT) */
    T_LET, T_MUT, T_CONST, T_FN, T_IF, T_ELSE, T_FOR, T_IN, T_LOOP, T_RETURN, T_BREAK,
    T_CONTINUE, T_PRINT, T_STRUCT, T_TRUE, T_FALSE, T_AND, T_OR, T_NOT,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_COMMA, T_COLON, T_DOT, T_DOTDOT, T_ARROW, T_FATARROW,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_NTOK
};
static const char *tk_name[T_NTOK] = {
    "EOF", "NEWLINE", "INDENT", "DEDENT", "IDENT", "INT", "FLOAT", "STRING", "$import",
    "let", "mut", "const", "fn", "if", "else", "for", "in", "loop", "return", "break",
    "continue", "print", "struct", "true", "false", "and", "or", "not",
    "(", ")", "{", "}", ",", ":", ".", "..", "->", "=>",
    "=", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%",
    "+=", "-=", "*=", "/="
};

typedef struct {
    int kind;
    int file, line, col, len;   /* where it is (for error messages) */
    long long ival;             /* value of an INT */
    char *sval;                 /* text of IDENT / STRING / INT / FLOAT */
} Token;

/* ---- the error reporter: every message in the compiler goes through here ---- */

static int n_errors = 0, n_warnings = 0;
static char *reported[512];
static int nreported = 0;

static void diag(int warn, const Token *t, const char *help, const char *fmt, va_list ap) {
    char msg[600];
    vsnprintf(msg, sizeof msg, fmt, ap);
    char key[760];                       /* don't print the same message twice */
    snprintf(key, sizeof key, "%d|%d|%d|%d|%s", warn, t ? t->file : -1, t ? t->line : 0, t ? t->col : 0, msg);
    for (int i = 0; i < nreported; i++) if (!strcmp(reported[i], key)) return;
    if (nreported < 512) reported[nreported++] = xstrdup(key);

    const char *k = use_color ? (warn ? "\033[1;33m" : "\033[1;31m") : "";
    const char *b = use_color ? "\033[1m" : "";
    const char *c = use_color ? "\033[1;36m" : "";
    const char *r = use_color ? "\033[0m" : "";
    fprintf(stderr, "%s%s%s: %s%s%s\n", k, warn ? "warning" : "error", r, b, msg, r);
    if (t) {
        Source *s = &srcs[t->file];
        fprintf(stderr, "  %s-->%s %s:%d:%d\n", c, r, s->path, t->line, t->col);
        const char *p = s->text; int ln = 1;
        while (ln < t->line && *p) { if (*p++ == '\n') ln++; }
        const char *e = p; while (*e && *e != '\n') e++;
        fprintf(stderr, "      %s|%s\n%5d %s|%s %.*s\n      %s|%s ", c, r, t->line, c, r, (int)(e - p), p, c, r);
        for (int i = 1; i < t->col && p + i - 1 < e; i++) fputc(p[i-1] == '\t' ? '\t' : ' ', stderr);
        int room = (int)(e - p) - (t->col - 1); if (room < 1) room = 1;
        int len = t->len < room ? t->len : room; if (len < 1) len = 1;
        fputs(k, stderr); for (int i = 0; i < len; i++) fputc('^', stderr); fputs(r, stderr);
        fputc('\n', stderr);
    }
    if (help) fprintf(stderr, "      %s= help:%s %s\n", c, r, help);
    fputc('\n', stderr);
    if (warn) n_warnings++;
    else if (++n_errors >= MAX_ERRORS) {
        fprintf(stderr, "Too many errors - stopping here. Fix the first ones and try again.\n");
        exit(1);
    }
}
static void error_at(const Token *t, const char *help, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); diag(0, t, help, fmt, ap); va_end(ap);
}
static void warn_at(const Token *t, const char *help, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); diag(1, t, help, fmt, ap); va_end(ap);
}

/* =============================================================================
 *  2. THE LEXER   (characters -> tokens)
 *
 *  Besides words and symbols, the lexer looks at the INDENTATION of every line.
 *  When a line is indented more than the one before, it emits an INDENT token;
 *  when less, one DEDENT token per block that ended. That is how Sparks knows
 *  where blocks start and stop, without any { } characters.
 * ============================================================================= */

typedef struct { Token *v; int n, cap; } Toks;
static void tpush(Toks *t, Token k) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->v = xrealloc(t->v, t->cap * sizeof(Token)); }
    t->v[t->n++] = k;
}
static Token mk(int kind, int file, int line, int col, int len) {
    Token t; memset(&t, 0, sizeof t);
    t.kind = kind; t.file = file; t.line = line; t.col = col; t.len = len;
    return t;
}

static void lex(int file, Toks *out) {
    const char *s = srcs[file].text;
    int i = 0, line = 1, ls = 0;       /* i = position, ls = where this line starts */
    int depth = 0;                     /* how many ( or { are open: newlines are ignored inside */
    int at_start = 1;                  /* are we at the start of a line? */
    int ind[128], nind = 1; ind[0] = 0;/* stack of indentation widths */
    int last_end = 1;                  /* column just after the last token (for NEWLINE position) */

    for (;;) {
        /* ---- start of a line: measure the indentation ---- */
        if (at_start && depth == 0) {
            int j = i, col = 0, tab_seen = 0;
            while (s[j] == ' ' || s[j] == '\t') {
                if (s[j] == '\t') {
                    if (!tab_seen) {
                        Token tt = mk(T_INDENT, file, line, j - ls + 1, 1);
                        error_at(&tt, "use spaces (4 per level) to indent your code", "tabs are not allowed in Sparks code");
                        tab_seen = 1;
                    }
                    col += 4;
                } else col++;
                j++;
            }
            if (s[j] == '\n' || s[j] == '\0' || (s[j] == '/' && s[j+1] == '/')) {   /* blank or comment-only line */
                while (s[j] && s[j] != '\n') j++;
                if (!s[j]) { i = j; break; }
                i = j + 1; line++; ls = i;
                continue;
            }
            i = j;
            Token t = mk(T_INDENT, file, line, i - ls + 1, 1);
            if (col > ind[nind-1]) {
                if (nind >= 127) die("code is nested too deeply");
                ind[nind++] = col;
                tpush(out, t);
            } else if (col < ind[nind-1]) {
                t.kind = T_DEDENT;
                while (nind > 1 && col < ind[nind-1]) { nind--; tpush(out, t); }
                if (col != ind[nind-1])
                    error_at(&t, "line this up with an earlier line of the same block",
                             "this line's indentation doesn't match any block above it");
            }
            at_start = 0; last_end = 1;
        }

        char c = s[i];
        if (c == '\0') break;
        if (c == '\n') {
            i++;
            if (depth == 0) { tpush(out, mk(T_NEWLINE, file, line, last_end, 1)); at_start = 1; }
            line++; ls = i;
            continue;
        }
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '/' && s[i+1] == '/') { while (s[i] && s[i] != '\n') i++; continue; }   /* comment */

        int col = i - ls + 1;
        Token t = mk(T_EOF, file, line, col, 1);

        /* ---- numbers ---- */
        if (isdigit((unsigned char)c)) {
            int j = i;
            while (isdigit((unsigned char)s[j])) j++;
            int isf = 0;
            if (s[j] == '.' && isdigit((unsigned char)s[j+1])) {       /* 0..10 must NOT become the float "0." */
                isf = 1; j++;
                while (isdigit((unsigned char)s[j])) j++;
            }
            if (isalpha((unsigned char)s[j]) || s[j] == '_') {
                int k = j; while (isalnum((unsigned char)s[k]) || s[k] == '_') k++;
                t.len = k - i;
                error_at(&t, "names can't start with a digit, and numbers can't contain letters", "`%.*s` is not a valid number or name", k - i, s + i);
                i = k; continue;
            }
            t.len = j - i; t.sval = xstrndup(s + i, j - i);
            if (isf) t.kind = T_FLOAT;
            else {
                t.kind = T_INT;
                long long v = strtoll(t.sval, NULL, 10);
                if (t.len > 10 || v > 2147483647LL) { error_at(&t, "an int can go up to 2147483647", "this number is too big"); v = 0; }
                t.ival = v;
            }
            tpush(out, t); i = j; last_end = col + t.len;
            continue;
        }

        /* ---- words: names and keywords ---- */
        if (isalpha((unsigned char)c) || c == '_') {
            int j = i;
            while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
            t.len = j - i; t.sval = xstrndup(s + i, t.len); t.kind = T_IDENT;
            for (int k = T_LET; k <= T_NOT; k++) if (!strcmp(t.sval, tk_name[k])) { t.kind = k; break; }
            tpush(out, t); i = j; last_end = col + t.len;
            continue;
        }

        /* ---- $import ---- */
        if (c == '$') {
            int j = i + 1;
            while (isalpha((unsigned char)s[j])) j++;
            t.len = j - i;
            if (j - i == 7 && !strncmp(s + i, "$import", 7)) { t.kind = T_IMPORT; tpush(out, t); last_end = col + t.len; }
            else error_at(&t, "the only directive is $import", "I don't know the directive `%.*s`", j - i, s + i);
            i = j; continue;
        }

        /* ---- text in "quotes" ---- */
        if (c == '"') {
            Buf sb = {0};
            int j = i + 1, ok = 0;
            while (s[j] && s[j] != '\n') {
                if (s[j] == '"') { ok = 1; break; }
                if (s[j] == '\\') {
                    char e = s[j+1]; int ch = -1;
                    if (!e || e == '\n') break;
                    switch (e) {
                        case 'n': ch = '\n'; break;  case 't': ch = '\t'; break;
                        case '\\': ch = '\\'; break; case '"': ch = '"'; break;
                        case 'e': ch = 27; break;
                    }
                    if (ch < 0) {
                        Token et = mk(T_STRING, file, line, j - ls + 1, 2);
                        error_at(&et, "valid escapes: \\n (new line)  \\t (tab)  \\\" (quote)  \\\\ (backslash)  \\e (terminal escape)",
                                 "unknown escape `\\%c` in this text", e);
                        ch = '?';
                    }
                    char tmp[2] = { (char)ch, 0 }; buf_add(&sb, tmp); j += 2; continue;
                }
                char tmp[2] = { s[j], 0 }; buf_add(&sb, tmp); j++;
            }
            if (!ok) {
                t.len = j - i;
                error_at(&t, "end the text with another \" on the same line", "this text never ends (the closing quote is missing)");
            } else j++;
            t.kind = T_STRING; t.len = j - i; t.sval = sb.s ? sb.s : xstrdup("");
            tpush(out, t); i = j; last_end = col + t.len;
            continue;
        }

        /* ---- symbols ---- */
        int k = T_EOF, len = 1; char d = s[i+1];
        switch (c) {
            case '(': k = T_LPAREN; depth++; break;
            case ')': k = T_RPAREN; if (depth > 0) depth--; break;
            case '{': k = T_LBRACE; depth++; break;
            case '}': k = T_RBRACE; if (depth > 0) depth--; break;
            case ',': k = T_COMMA; break;
            case ':': k = T_COLON; break;
            case '.': if (d == '.') { k = T_DOTDOT; len = 2; } else k = T_DOT; break;
            case '-': if (d == '>') { k = T_ARROW; len = 2; } else if (d == '=') { k = T_MINUSEQ; len = 2; } else k = T_MINUS; break;
            case '=': if (d == '=') { k = T_EQ; len = 2; } else if (d == '>') { k = T_FATARROW; len = 2; } else k = T_ASSIGN; break;
            case '!': if (d == '=') { k = T_NE; len = 2; } else k = T_NOT; break;
            case '<': if (d == '=') { k = T_LE; len = 2; } else k = T_LT; break;
            case '>': if (d == '=') { k = T_GE; len = 2; } else k = T_GT; break;
            case '+': if (d == '=') { k = T_PLUSEQ; len = 2; } else k = T_PLUS; break;
            case '*': if (d == '=') { k = T_STAREQ; len = 2; } else k = T_STAR; break;
            case '/': if (d == '=') { k = T_SLASHEQ; len = 2; } else k = T_SLASH; break;
            case '%': k = T_PERCENT; break;
            case '&': if (d == '&') { k = T_AND; len = 2; } break;
            case '|': if (d == '|') { k = T_OR; len = 2; } break;
        }
        if (k == T_EOF) {
            if (c == ';') error_at(&t, "just press Enter instead - every line is one statement", "Sparks doesn't use `;`");
            else if (c == '[' || c == ']') error_at(&t, NULL, "square brackets (lists) are not part of Sparks yet");
            else if (c == '&') error_at(&t, "write `and` (or &&) to mean \"both\"", "a single `&` isn't used in Sparks");
            else if (c == '|') error_at(&t, "write `or` (or ||) to mean \"either\"", "a single `|` isn't used in Sparks");
            else if (c == '\'') error_at(&t, "text goes in double quotes, like \"hello\"", "single quotes aren't used in Sparks");
            else error_at(&t, NULL, "I don't understand the character `%c`", c);
            i++; continue;
        }
        t.kind = k; t.len = len; tpush(out, t);
        i += len; last_end = col + len;
    }

    /* ---- end of file: close the last line and any blocks still open ---- */
    if (!at_start && depth == 0) tpush(out, mk(T_NEWLINE, file, line, last_end, 1));
    for (; nind > 1; nind--) tpush(out, mk(T_DEDENT, file, line, 1, 1));
    tpush(out, mk(T_EOF, file, line, last_end, 1));
}

/* =============================================================================
 *  3. THE SYNTAX TREE (AST)
 * ============================================================================= */

enum {
    N_PROGRAM, N_IMPORT, N_FN, N_STRUCT, N_PARAM, N_BLOCK,
    N_LET, N_ASSIGN, N_EXPRSTMT, N_PRINT, N_IF, N_LOOP, N_FOR, N_RETURN, N_BREAK, N_CONTINUE,
    N_INT, N_FLOAT, N_STR, N_BOOL, N_IDENT, N_UNARY, N_BINARY, N_CALL, N_FIELD, N_STRUCTLIT, N_FIELDINIT,
    N_NKINDS
};
static const char *nk_name[N_NKINDS] = {
    "Program", "Import", "Fn", "Struct", "Param", "Block",
    "Let", "Assign", "ExprStmt", "Print", "If", "Loop", "For", "Return", "Break", "Continue",
    "Int", "Float", "Str", "Bool", "Ident", "Unary", "Binary", "Call", "Field", "StructLit", "FieldInit"
};

typedef struct Node Node;
struct Node {
    int kind;
    Token tok;                 /* where this node is in the source */
    char *name;                /* names, text values */
    long long ival;            /* ints and bools */
    int op;                    /* operator token kind */
    int flag;                  /* N_LET: 0 let / 1 mut / 2 const.  N_EXPRSTMT: 1 = last line of a function */
    Node *a, *b, *c;           /* children, meaning depends on kind */
    Node **list; int n, cap;   /* list of children (statements, arguments, fields...) */
    char *tname; Token ttok;   /* a written-out type, like  x: f32  */
};

static Node *node(int kind, const Token *t) {
    Node *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = kind;
    if (t) n->tok = *t;
    return n;
}
static void push(Node *p, Node *c) {
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 4; p->list = xrealloc(p->list, p->cap * sizeof(Node *)); }
    p->list[p->n++] = c;
}

static void dump_node(Node *n, int d) {
    if (!n) return;
    printf("%*s%s", d * 2, "", nk_name[n->kind]);
    if (n->kind == N_STR) printf(" \"%s\"", n->name);
    else if (n->name) printf(" %s", n->name);
    if (n->kind == N_INT || n->kind == N_BOOL) printf(" %lld", n->ival);
    if (n->kind == N_FLOAT) printf(" %s", n->tok.sval);
    if (n->kind == N_UNARY || n->kind == N_BINARY || n->kind == N_ASSIGN) printf(" '%s'", tk_name[n->op]);
    if (n->kind == N_LET) printf(" (%s)", n->flag == 0 ? "let" : n->flag == 1 ? "mut" : "const");
    if (n->tname) printf(" : %s", n->tname);
    printf("\n");
    dump_node(n->a, d + 1); dump_node(n->b, d + 1); dump_node(n->c, d + 1);
    for (int i = 0; i < n->n; i++) dump_node(n->list[i], d + 1);
}

/* =============================================================================
 *  4. THE PARSER   (tokens -> syntax tree)
 *
 *  Recursive descent: one C function per grammar rule. When the parser finds
 *  something that doesn't fit the grammar, it reports a SYNTAX ERROR, skips the
 *  rest of the broken line (and its indented block) and keeps going, so you
 *  can see several mistakes in one run.
 * ============================================================================= */

static Toks *PT;                 /* the tokens being parsed */
static int ppos;                 /* index of the current token */
static jmp_buf *cur_jb;          /* where to jump after a syntax error */

static Token *cur(void) { return &PT->v[ppos]; }
static int at(int k) { return PT->v[ppos].kind == k; }
static Token *adv(void) { Token *t = &PT->v[ppos]; if (t->kind != T_EOF) ppos++; return t; }
static int accept(int k) { if (at(k)) { adv(); return 1; } return 0; }

static const char *describe(const Token *t) {      /* a token, in words */
    static char b[4][100]; static int k;
    char *r = b[k++ & 3];
    switch (t->kind) {
        case T_EOF:     return "the end of the file";
        case T_NEWLINE: return "the end of the line";
        case T_INDENT:  return "extra indentation";
        case T_DEDENT:  return "the end of the block";
        case T_IDENT:   snprintf(r, 100, "`%s`", t->sval); return r;
        case T_INT: case T_FLOAT: snprintf(r, 100, "the number `%s`", t->sval); return r;
        case T_STRING:  return "a piece of text";
        default:        snprintf(r, 100, "`%s`", tk_name[t->kind]); return r;
    }
}

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif
NORETURN static void perr(const Token *t, const char *help, const char *fmt, ...) {   /* report, then recover */
    va_list ap; va_start(ap, fmt); diag(0, t, help, fmt, ap); va_end(ap);
    if (cur_jb) longjmp(*cur_jb, 1);
    exit(1);
}

/* After an error: skip the rest of the broken line, and the block hanging off it. */
static void sync(void) {
    while (!at(T_EOF) && !at(T_NEWLINE) && !at(T_DEDENT) && !at(T_INDENT)) adv();
    if (at(T_NEWLINE)) adv();
    if (at(T_INDENT)) {
        int d = 0;
        do {
            if (at(T_INDENT)) d++; else if (at(T_DEDENT)) d--;
            if (at(T_EOF)) break;
            adv();
        } while (d > 0);
    }
}

static void expect_eol(Node *first) {            /* a statement must end at the end of the line */
    if (at(T_NEWLINE)) { adv(); return; }
    if (at(T_EOF)) return;
    Token *t = cur();
    const char *help = NULL; char hb[200];
    if (first && first->kind == N_IDENT) {
        const char *kw[] = { "print", "let", "mut", "const", "return", "loop", "for", "if", "else", "break", "continue", "struct", "fn" };
        const char *s = closest(first->name, kw, 13);
        if (s) { snprintf(hb, sizeof hb, "did you mean `%s`? (`%s` is not a Sparks word)", s, first->name); help = hb; }
    }
    if (!help) switch (t->kind) {
        case T_ASSIGN: help = "to compare two values use `==`; a single `=` stores a value"; break;
        case T_LBRACE: case T_RBRACE: help = "Sparks doesn't use { } - group lines with indentation instead"; break;
        case T_RPAREN: help = "there is a `)` with no matching `(` before it"; break;
        case T_IDENT: case T_INT: case T_FLOAT: case T_STRING:
            help = "two values are side by side - did you forget an operator (like `+`) or a comma?"; break;
        default: help = "every statement goes on its own line"; break;
    }
    perr(t, help, "I didn't expect %s here - the line should have ended before it", describe(t));
}

static void expect_close(Token open, int kind, const char *expected) {
    if (at(kind)) { adv(); return; }
    Token *c = cur();
    if (c->kind == T_EOF || c->line != open.line)
        perr(&open, "add the matching closing bracket", "this `%s` is never closed", tk_name[open.kind]);
    perr(c, NULL, "expected %s here, but found %s", expected, describe(c));
}

/* ---------- expressions (lowest precedence first) ---------- */

static Node *parse_expr(void);
static Node *parse_statement(void);

static Node *binop(Node *l, const Token *op, Node *r) {
    Node *n = node(N_BINARY, op); n->op = op->kind; n->a = l; n->b = r; return n;
}

static Node *parse_primary(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_INT:    { Node *n = node(N_INT, t); n->ival = t->ival; adv(); return n; }
    case T_FLOAT:  { Node *n = node(N_FLOAT, t); n->name = t->sval; adv(); return n; }
    case T_STRING: { Node *n = node(N_STR, t); n->name = t->sval; adv(); return n; }
    case T_TRUE: case T_FALSE: { Node *n = node(N_BOOL, t); n->ival = t->kind == T_TRUE; adv(); return n; }
    case T_LPAREN: {
        Token open = *adv();
        Node *e = parse_expr();
        expect_close(open, T_RPAREN, "`)`");
        return e;
    }
    case T_IDENT: {
        Token nm = *adv();
        if (at(T_LPAREN)) {                                   /* function call */
            Token open = *adv();
            Node *n = node(N_CALL, &nm); n->name = nm.sval;
            while (!at(T_RPAREN)) {
                push(n, parse_expr());
                if (!accept(T_COMMA)) break;
            }
            expect_close(open, T_RPAREN, "`,` or `)`");
            return n;
        }
        if (at(T_LBRACE) && isupper((unsigned char)nm.sval[0])) {   /* struct literal:  Name { x: 1, y: 2 } */
            Token open = *adv();
            Node *n = node(N_STRUCTLIT, &nm); n->name = nm.sval;
            while (!at(T_RBRACE)) {
                Token *ft = cur();
                if (!at(T_IDENT)) perr(ft, "fields are written like  x: 1.0", "expected a field name, but found %s", describe(ft));
                Node *fi = node(N_FIELDINIT, ft); fi->name = ft->sval; adv();
                if (!at(T_COLON)) perr(cur(), "fields are written like  x: 1.0", "expected `:` after the field name `%s`, but found %s", fi->name, describe(cur()));
                adv();
                fi->a = parse_expr();
                push(n, fi);
                if (!accept(T_COMMA)) break;
            }
            expect_close(open, T_RBRACE, "`,` or `}`");
            return n;
        }
        Node *n = node(N_IDENT, &nm); n->name = nm.sval;
        return n;
    }
    default: break;
    }
    const char *help = "a value is a number, some text, a name, or something in ( )";
    if (t->kind == T_NEWLINE) help = "the line ended too early - an operator like `+` needs a value on its right";
    else if (t->kind >= T_LET && t->kind <= T_NOT && t->kind != T_NOT) help = "that is a reserved word, it can't be used as a name or a value";
    else if (t->kind == T_RPAREN) help = "there is a `)` with no matching `(`";
    perr(t, help, "I expected a value here, but found %s", describe(t));
    return NULL;
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    while (at(T_DOT)) {
        adv();
        Token *nm = cur();
        if (!at(T_IDENT)) perr(nm, "example: player.score", "expected a field name after `.`, but found %s", describe(nm));
        Node *n = node(N_FIELD, nm); n->name = nm->sval; n->a = e; adv();
        e = n;
    }
    return e;
}
static Node *parse_unary(void) {
    if (at(T_MINUS)) { Token op = *adv(); Node *n = node(N_UNARY, &op); n->op = T_MINUS; n->a = parse_unary(); return n; }
    return parse_postfix();
}
static Node *parse_mul(void) {
    Node *l = parse_unary();
    while (at(T_STAR) || at(T_SLASH) || at(T_PERCENT)) { Token op = *adv(); Node *r = parse_unary(); l = binop(l, &op, r); }
    return l;
}
static Node *parse_add(void) {
    Node *l = parse_mul();
    while (at(T_PLUS) || at(T_MINUS)) { Token op = *adv(); Node *r = parse_mul(); l = binop(l, &op, r); }
    return l;
}
static Node *parse_cmp(void) {
    Node *l = parse_add();
    while (at(T_EQ) || at(T_NE) || at(T_LT) || at(T_LE) || at(T_GT) || at(T_GE)) { Token op = *adv(); Node *r = parse_add(); l = binop(l, &op, r); }
    return l;
}
static Node *parse_not(void) {
    if (at(T_NOT)) { Token op = *adv(); Node *n = node(N_UNARY, &op); n->op = T_NOT; n->a = parse_not(); return n; }
    return parse_cmp();
}
static Node *parse_and(void) {
    Node *l = parse_not();
    while (at(T_AND)) { Token op = *adv(); Node *r = parse_not(); l = binop(l, &op, r); }
    return l;
}
static Node *parse_or(void) {
    Node *l = parse_and();
    while (at(T_OR)) { Token op = *adv(); Node *r = parse_and(); l = binop(l, &op, r); }
    return l;
}
static Node *parse_expr(void) { return parse_or(); }

static Node *parse_condition(void) {
    Node *e = parse_expr();
    if (at(T_ASSIGN)) perr(cur(), "to compare two values use `==`; a single `=` stores a value", "found `=` where a true/false question was expected");
    return e;
}

/* ---------- blocks and statements ---------- */

static Node *parse_block(const char *what) {          /* NEWLINE INDENT statements DEDENT */
    Node *blk = node(N_BLOCK, cur());
    if (!at(T_NEWLINE))
        perr(cur(), "put the body on the next line, indented by 4 spaces", "I didn't expect %s here - the body of %s goes on its own indented lines", describe(cur()), what);
    adv();
    if (!at(T_INDENT)) {
        error_at(cur(), "indent the lines that belong inside (4 spaces) so Sparks knows where the block starts",
                 "expected an indented block after %s", what);
        return blk;
    }
    adv();
    jmp_buf jb, *prev = cur_jb;
    cur_jb = &jb;
    while (!at(T_DEDENT) && !at(T_EOF)) {
        volatile int before = ppos;
        if (setjmp(jb) == 0) {
            Node *s = parse_statement();
            if (s) push(blk, s);
        } else {
            sync();
            if (ppos == before) adv();
        }
    }
    cur_jb = prev;
    if (at(T_DEDENT)) adv();
    return blk;
}

static void parse_type_into(Node *n) {
    Token *t = cur();
    if (!at(T_IDENT)) perr(t, "types look like `int`, `f32`, `str`, `bool` or a struct name", "expected a type name, but found %s", describe(t));
    n->tname = t->sval; n->ttok = *t; adv();
}

static Node *parse_let(void) {
    Token kw = *adv();
    Node *n = node(N_LET, &kw);
    n->flag = kw.kind == T_LET ? 0 : kw.kind == T_MUT ? 1 : 2;
    Token *nm = cur();
    if (!at(T_IDENT)) {
        const char *h = (nm->kind >= T_LET && nm->kind <= T_NOT) ? "that is a reserved word - pick another name" : "example: let score = 0";
        perr(nm, h, "expected a name after `%s`, but found %s", kw.kind == T_LET ? "let" : kw.kind == T_MUT ? "mut" : "const", describe(nm));
    }
    n->name = nm->sval; n->tok = *nm; adv();
    if (accept(T_COLON)) parse_type_into(n);
    if (!at(T_ASSIGN)) perr(cur(), "give it a starting value, like  let score = 0", "expected `=` after the name `%s`, but found %s", n->name, describe(cur()));
    adv();
    n->a = parse_expr();
    expect_eol(NULL);
    return n;
}

static Node *parse_if(void) {
    Token kw = *adv();
    Node *n = node(N_IF, &kw);
    n->a = parse_condition();
    n->b = parse_block("the `if` line");
    if (at(T_ELSE)) {
        adv();
        if (at(T_IF)) n->c = parse_if();                 /* else if ...  */
        else n->c = parse_block("`else`");
    }
    return n;
}

static Node *parse_loop(void) {
    Token kw = *adv();
    Node *n = node(N_LOOP, &kw);
    if (!at(T_NEWLINE)) n->a = parse_condition();        /* plain `loop` runs forever (until `break`) */
    n->b = parse_block("the `loop` line");
    return n;
}

static Node *parse_for(void) {
    Token kw = *adv();
    Node *n = node(N_FOR, &kw);
    Token *v = cur();
    if (!at(T_IDENT)) perr(v, "example: for i in 0..10", "expected a name for the loop counter after `for`, but found %s", describe(v));
    n->name = v->sval; n->tok = *v; adv();
    if (!at(T_IN)) perr(cur(), "example: for i in 0..10", "expected `in` after `for %s`, but found %s", n->name, describe(cur()));
    adv();
    n->a = parse_expr();
    if (!at(T_DOTDOT)) perr(cur(), "a range looks like 0..10 (it counts 0 up to 9)", "expected `..` to make a range, but found %s", describe(cur()));
    adv();
    n->b = parse_expr();
    n->c = parse_block("the `for` line");
    return n;
}

static Node *parse_simple(void) {                        /* assignment, or an expression on its own */
    Node *e = parse_expr();
    if (at(T_ASSIGN) || at(T_PLUSEQ) || at(T_MINUSEQ) || at(T_STAREQ) || at(T_SLASHEQ)) {
        Token op = *adv();
        if (e->kind != N_IDENT && e->kind != N_FIELD)
            perr(&op, "only a variable (or a field like p.x) can be changed", "you can't store a value into this");
        Node *n = node(N_ASSIGN, &op); n->op = op.kind; n->a = e; n->b = parse_expr();
        expect_eol(NULL);
        return n;
    }
    Node *n = node(N_EXPRSTMT, &e->tok); n->a = e;
    expect_eol(e);
    return n;
}

static Node *parse_statement(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_LET: case T_MUT: case T_CONST: return parse_let();
    case T_IF:   return parse_if();
    case T_LOOP: return parse_loop();
    case T_FOR:  return parse_for();
    case T_RETURN: {
        Token kw = *adv(); Node *n = node(N_RETURN, &kw);
        if (!at(T_NEWLINE)) n->a = parse_expr();
        expect_eol(NULL);
        return n;
    }
    case T_BREAK: case T_CONTINUE: {
        Token kw = *adv(); Node *n = node(kw.kind == T_BREAK ? N_BREAK : N_CONTINUE, &kw);
        expect_eol(NULL);
        return n;
    }
    case T_PRINT: {
        Token kw = *adv(); Node *n = node(N_PRINT, &kw);
        if (!at(T_NEWLINE)) { push(n, parse_expr()); while (accept(T_COMMA)) push(n, parse_expr()); }
        expect_eol(NULL);
        return n;
    }
    case T_ELSE:
        perr(t, "`else` must line up exactly with the `if` it belongs to", "this `else` has no `if` before it");
    case T_LBRACE: case T_RBRACE:
        perr(t, "Sparks uses indentation instead of braces: indent the lines that belong inside, by 4 spaces", "Sparks doesn't use `{ }` to group code");
    case T_INDENT:
        perr(t, "nothing above opened a block (`if`, `loop`, `for`, `fn`...), so remove the extra spaces", "this line is indented too far");
    case T_FN:
        perr(t, "define functions at the left edge of the file, outside of other code", "a function can't be defined inside another function");
    case T_STRUCT:
        perr(t, "define structs at the left edge of the file", "a struct can't be defined inside a function");
    case T_IMPORT:
        perr(t, "put $import lines at the very top of the file", "$import can't be used inside a function");
    default: break;
    }
    return parse_simple();
}

/* ---------- top-level declarations ---------- */

static void parse_params(Node *fn, Token open) {
    while (!at(T_RPAREN)) {
        Token *t = cur();
        if (!at(T_IDENT)) perr(t, "parameters look like  a  or  a: int", "expected a parameter name, but found %s", describe(t));
        Node *p = node(N_PARAM, t); p->name = t->sval; adv();
        for (int i = 0; i < fn->n; i++)
            if (!strcmp(fn->list[i]->name, p->name)) perr(t, "give each parameter its own name", "two parameters are both called `%s`", p->name);
        if (accept(T_COLON)) parse_type_into(p);
        push(fn, p);
        if (!accept(T_COMMA)) break;
    }
    expect_close(open, T_RPAREN, "`,` or `)`");
}

static Node *parse_fn(void) {
    Token kw = *adv();
    Node *n = node(N_FN, &kw);
    Token *nm = cur();
    if (!at(T_IDENT)) perr(nm, "example: fn greet(name)", "expected a function name after `fn`, but found %s", describe(nm));
    n->name = nm->sval; n->tok = *nm; adv();
    if (!at(T_LPAREN)) perr(cur(), "example: fn greet(name)", "expected `(` after the function name `%s`, but found %s", n->name, describe(cur()));
    Token open = *adv();
    parse_params(n, open);
    if (accept(T_ARROW)) {
        Token *rt = cur();
        if (!at(T_IDENT)) perr(rt, "example: fn size() -> int", "expected a type after `->`, but found %s", describe(rt));
        n->tname = rt->sval; n->ttok = *rt; adv();
    }
    if (at(T_FATARROW)) {                                /* fn square(n) => n * n */
        Token arrow = *adv();
        Node *blk = node(N_BLOCK, &arrow);
        Node *es = node(N_EXPRSTMT, &arrow);
        es->a = parse_expr();
        push(blk, es);
        expect_eol(NULL);
        n->a = blk;
    } else {
        n->a = parse_block("the `fn` line");
    }
    return n;
}

static void parse_struct_fields(Node *n) {
    jmp_buf jb, *prev = cur_jb;
    cur_jb = &jb;
    while (!at(T_DEDENT) && !at(T_EOF)) {
        volatile int before = ppos;
        if (setjmp(jb) == 0) {
            Token *ft = cur();
            if (!at(T_IDENT)) perr(ft, "fields are written like  x: f32", "expected a field name, but found %s", describe(ft));
            Node *p = node(N_PARAM, ft); p->name = ft->sval; adv();
            if (!at(T_COLON)) perr(cur(), "fields are written like  x: f32", "expected `:` and a type after the field name `%s`, but found %s", p->name, describe(cur()));
            adv();
            parse_type_into(p);
            expect_eol(NULL);
            push(n, p);
        } else {
            sync();
            if (ppos == before) adv();
        }
    }
    cur_jb = prev;
    if (at(T_DEDENT)) adv();
}

static Node *parse_struct(void) {
    adv();
    Token *nm = cur();
    if (!at(T_IDENT)) perr(nm, "example: struct Point", "expected a name after `struct`, but found %s", describe(nm));
    Node *n = node(N_STRUCT, nm); n->name = nm->sval; adv();
    if (!at(T_NEWLINE)) perr(cur(), "the fields go on the next lines, indented", "I didn't expect %s here - a struct's fields go on their own lines", describe(cur()));
    adv();
    if (!at(T_INDENT)) {
        error_at(cur(), "list the fields indented below, like  x: f32", "expected an indented list of fields after `struct %s`", n->name);
        return n;
    }
    adv();
    parse_struct_fields(n);
    return n;
}

static Node *parse_decl(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_IMPORT: {
        adv();
        Token *s = cur();
        if (!at(T_STRING)) perr(s, "example: $import \"io\"", "expected the module name in quotes after `$import`, but found %s", describe(s));
        Node *n = node(N_IMPORT, s); n->name = s->sval; adv();
        expect_eol(NULL);
        return n;
    }
    case T_FN:     return parse_fn();
    case T_STRUCT: return parse_struct();
    case T_LET: case T_MUT: case T_CONST: return parse_let();
    case T_INDENT:
        perr(t, "top-level code starts at the left edge", "this line is indented, but nothing above opened a block");
    case T_IF: case T_LOOP: case T_FOR: case T_PRINT: case T_RETURN: case T_BREAK: case T_CONTINUE:
        perr(t, "put this inside a function - for example write  fn main()  and indent this line under it",
             "`%s` can't be used outside of a function", tk_name[t->kind]);
    default:
        perr(t, "at the top level you can write: fn, struct, let, mut, const or $import",
             "I didn't expect %s here", describe(t));
    }
    return NULL;
}

/* ---------- files and imports ---------- */

static int mod_on[5] = { 1, 0, 0, 0, 0 };              /* which built-in modules were imported */
enum { M_CORE, M_IO, M_MATH, M_GAME, M_SYS };
static const char *mod_names[5] = { "core", "io", "math", "game", "sys" };
static int opt_tokens = 0;
static long total_tokens = 0;

static void parse_file(const char *path, const Token *from, Node *prog);

static void handle_import(Node *d, Node *prog, int file) {
    const char *name = d->name; size_t L = strlen(name);
    if (L > 3 && !strcmp(name + L - 3, ".sp")) {         /* another Sparks file, next to this one */
        const char *me = srcs[file].path;
        const char *slash = strrchr(me, PATH_SEP);
        Buf p = {0};
        if (slash) { char *dir = xstrndup(me, slash - me + 1); buf_add(&p, dir); }
        buf_add(&p, name);
        parse_file(BS(p), &d->tok, prog);
        return;
    }
    for (int m = 1; m < 5; m++) if (!strcmp(name, mod_names[m])) { mod_on[m] = 1; return; }
    error_at(&d->tok, "built-in modules are \"io\", \"math\", \"game\" and \"sys\". To use your own file, write its name ending in .sp",
             "I don't know a module called \"%s\"", name);
}

static void parse_toplevel(Node *prog, int file) {
    jmp_buf jb, *prev = cur_jb;
    cur_jb = &jb;
    while (!at(T_EOF)) {
        volatile int before = ppos;
        if (setjmp(jb) == 0) {
            if (at(T_NEWLINE)) { adv(); continue; }
            Node *d = parse_decl();
            if (d) { if (d->kind == N_IMPORT) handle_import(d, prog, file); else push(prog, d); }
        } else {
            sync();
            if (ppos == before) adv();
        }
    }
    cur_jb = prev;
}

static char *seen_files[64];
static int nseen = 0;

static void dump_tokens(Toks *t) {
    for (int i = 0; i < t->n; i++) {
        Token *k = &t->v[i];
        printf("%4d:%-3d %-9s", k->line, k->col, tk_name[k->kind]);
        if (k->kind == T_IDENT || k->kind == T_INT || k->kind == T_FLOAT) printf(" %s", k->sval);
        else if (k->kind == T_STRING) printf(" \"%s\"", k->sval);
        printf("\n");
    }
}

static void parse_file(const char *path, const Token *from, Node *prog) {
    for (int i = 0; i < nseen; i++) if (!strcmp(seen_files[i], path)) return;     /* already loaded */
    if (nseen >= 64 || nsrcs >= 64) die("too many files");
    seen_files[nseen++] = xstrdup(path);
    char *text = read_file(path);
    if (!text) {
        if (from) error_at(from, "check the spelling, and that the file is in the same folder", "I can't open the file `%s`", path);
        else { fprintf(stderr, "spc: can't open `%s`\n", path); exit(1); }
        return;
    }
    int f = nsrcs++;
    srcs[f].path = xstrdup(path); srcs[f].text = text;
    Toks *toks = xmalloc(sizeof *toks); memset(toks, 0, sizeof *toks);
    lex(f, toks);
    total_tokens += toks->n;
    if (opt_tokens && f == 0) dump_tokens(toks);
    Toks *saved = PT; int savedpos = ppos;
    PT = toks; ppos = 0;
    parse_toplevel(prog, f);
    PT = saved; ppos = savedpos;
}

/* =============================================================================
 *  5. CHECKER + CODE GENERATOR
 *
 *  Now the tree is walked once. For every expression we do two jobs at once:
 *    - CHECK it: do the names exist? do the types fit together?
 *    - TRANSLATE it: write the matching piece of C code.
 *  gen_expr() returns the TYPE of the expression and appends its C code to a buffer.
 *
 *  Functions in Sparks have no written-out parameter types:  fn add(a, b)
 *  So we compile a fresh copy of a function for every combination of argument
 *  types it is called with ("monomorphization", the same trick C++ templates use).
 *  add(1, 2) becomes f_add__int_int, add(1.5, 2.5) becomes f_add__f32_f32.
 * ============================================================================= */

enum { TY_UNKNOWN = -1, TY_ERR = 0, TY_VOID, TY_INT, TY_F32, TY_F64, TY_BOOL, TY_STR, TY_STRUCT0 = 16 };
typedef int Ty;

typedef struct {
    char *name; char *cname; Token tok;
    int nf; char *fname[MAX_FIELDS]; Ty ftype[MAX_FIELDS];
} StructDef;
static StructDef structs[128];
static int nstructs = 0;

static int is_struct(Ty t) { return t >= TY_STRUCT0; }
static int is_num(Ty t) { return t == TY_INT || t == TY_F32 || t == TY_F64; }
static int is_float(Ty t) { return t == TY_F32 || t == TY_F64; }
static const char *ty_name(Ty t) {
    switch (t) {
        case TY_INT: return "int"; case TY_F32: return "f32"; case TY_F64: return "f64";
        case TY_BOOL: return "bool"; case TY_STR: return "str"; case TY_VOID: return "nothing";
        case TY_ERR: return "error"; default: break;
    }
    return is_struct(t) ? structs[t - TY_STRUCT0].name : "?";
}
static const char *ty_c(Ty t) {
    switch (t) {
        case TY_INT: return "int"; case TY_F32: return "float"; case TY_F64: return "double";
        case TY_BOOL: return "bool"; case TY_STR: return "const char *"; case TY_VOID: return "void";
        default: break;
    }
    return is_struct(t) ? structs[t - TY_STRUCT0].cname : "int";
}
/* can a value of type `from` be stored where `to` is expected?  (int -> f32 -> f64 is OK, never backwards) */
static int assignable(Ty from, Ty to) {
    if (from == to || from == TY_ERR || to == TY_ERR) return 1;
    if (from == TY_INT && is_float(to)) return 1;
    if (from == TY_F32 && to == TY_F64) return 1;
    return 0;
}
static Ty promote(Ty a, Ty b) {
    if (a == TY_F64 || b == TY_F64) return TY_F64;
    if (a == TY_F32 || b == TY_F32) return TY_F32;
    return TY_INT;
}
static int find_field(Ty st, const char *name) {
    StructDef *sd = &structs[st - TY_STRUCT0];
    for (int i = 0; i < sd->nf; i++) if (!strcmp(sd->fname[i], name)) return i;
    return -1;
}
static const char *c_keywords[] = { "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "bool", "true", "false", NULL };
static char *csafe(const char *name) {                /* field names that clash with C words get a trailing _ */
    for (int i = 0; c_keywords[i]; i++) if (!strcmp(name, c_keywords[i])) { Buf b = {0}; buf_printf(&b, "%s_", name); return b.s; }
    return xstrdup(name);
}

static Ty resolve_type(const char *name, const Token *tok, int allow_void) {
    if (!strcmp(name, "int") || !strcmp(name, "i32")) return TY_INT;
    if (!strcmp(name, "f32")) return TY_F32;
    if (!strcmp(name, "f64")) return TY_F64;
    if (!strcmp(name, "bool")) return TY_BOOL;
    if (!strcmp(name, "str")) return TY_STR;
    if (allow_void && !strcmp(name, "void")) return TY_VOID;
    for (int i = 0; i < nstructs; i++) if (!strcmp(structs[i].name, name)) return TY_STRUCT0 + i;
    const char *help = NULL; char hb[200];
    if (!strcmp(name, "float")) help = "decimal numbers are `f32` (or `f64` for extra precision)";
    else if (!strcmp(name, "double")) help = "use `f64`";
    else if (!strcmp(name, "string") || !strcmp(name, "String") || !strcmp(name, "char")) help = "text is called `str`";
    else if (!strcmp(name, "boolean") || !strcmp(name, "Bool")) help = "true/false values are called `bool`";
    else if (!strcmp(name, "long") || !strcmp(name, "i64") || !strcmp(name, "Int") || !strcmp(name, "integer")) help = "whole numbers are called `int`";
    else {
        const char *c[140]; int n = 0;
        c[n++] = "int"; c[n++] = "f32"; c[n++] = "f64"; c[n++] = "bool"; c[n++] = "str";
        for (int i = 0; i < nstructs; i++) c[n++] = structs[i].name;
        const char *s = closest(name, c, n);
        if (s) { snprintf(hb, sizeof hb, "did you mean `%s`?", s); help = hb; }
        else help = "the types are int, f32, f64, bool, str - or the name of a struct you defined";
    }
    error_at(tok, help, "I don't know a type called `%s`", name);
    return TY_ERR;
}

static void mismatch(const Token *t, Ty want, Ty got, const char *what) {
    const char *help = NULL;
    if (want == TY_INT && is_float(got)) help = "to drop the decimal part, write  int(value)";
    else if (want == TY_F32 && got == TY_F64) help = "f64 is more precise than f32; convert with  f32(value)";
    else if (want == TY_STR && (is_num(got) || got == TY_BOOL)) help = "turn it into text with  str(value)";
    else if ((is_num(want)) && got == TY_STR) help = "text isn't a number; read a number from the keyboard with input_int(), or convert with int(text)";
    else if (want == TY_BOOL) help = "make a comparison, like  value != 0";
    error_at(t, help, "%s: expected `%s` but found `%s`", what, ty_name(want), ty_name(got));
}

/* ---------- variables and scopes ---------- */

typedef struct { char *name; Ty type; int kind; char *cname; } Var;
enum { V_LET, V_MUT, V_CONST, V_PARAM, V_LOOP };

typedef struct Ctx {
    Var *vars; int nv, cap;           /* all variables visible right now */
    int marks[64]; int nmarks;        /* where each open scope started */
    Ty ret;                           /* what the current function returns (TY_UNKNOWN until we see) */
    Buf *out; int ind;                /* where we write C code, and how deep it is indented */
    int loops;                        /* how many loops we are inside (for `break`) */
    int is_main;
} Ctx;
static Ctx G;                         /* the "global" context: top-level let/mut/const */
static Ctx *C;                        /* the context we are generating code in right now */
static Buf out_structs, out_protos, out_globals, out_funcs, g_init;
static int uniq = 0;

static void L(const char *fmt, ...) {                 /* write one line of C */
    Buf *o = C->out;
    for (int i = 0; i < C->ind; i++) buf_add(o, "    ");
    va_list ap; va_start(ap, fmt); buf_vprintf(o, fmt, ap); va_end(ap);
    buf_add(o, "\n");
}
static void push_scope(void) { if (C->nmarks >= 63) die("blocks are nested too deeply"); C->marks[C->nmarks++] = C->nv; }
static void pop_scope(void) { C->nv = C->marks[--C->nmarks]; }

static Var *find_var(const char *name) {
    for (int i = C->nv - 1; i >= 0; i--) if (!strcmp(C->vars[i].name, name)) return &C->vars[i];
    if (C != &G) for (int i = G.nv - 1; i >= 0; i--) if (!strcmp(G.vars[i].name, name)) return &G.vars[i];
    return NULL;
}
static Var *declare(const Token *t, const char *name, Ty ty, int kind) {
    int lo = C->nmarks ? C->marks[C->nmarks - 1] : 0;
    for (int i = C->nv - 1; i >= lo; i--)
        if (!strcmp(C->vars[i].name, name))
            error_at(t, "pick a different name; to change an existing variable just write  name = value",
                     "`%s` is already defined in this block", name);
    Buf cn = {0};
    if (find_var(name)) buf_printf(&cn, "v_%s__%d", name, ++uniq);   /* shadowing: make the C name unique */
    else buf_printf(&cn, "v_%s", name);
    if (C->nv == C->cap) { C->cap = C->cap ? C->cap * 2 : 16; C->vars = xrealloc(C->vars, C->cap * sizeof(Var)); }
    Var *v = &C->vars[C->nv++];
    v->name = xstrdup(name); v->type = ty; v->kind = kind; v->cname = cn.s;
    return v;
}

/* ---------- functions, instances, built-ins ---------- */

static Node *fns[256];
static int nfns = 0;
static Node *find_fn(const char *name) {
    for (int i = 0; i < nfns; i++) if (!strcmp(fns[i]->name, name)) return fns[i];
    return NULL;
}
typedef struct { Node *fn; Ty args[MAX_PARAMS]; Ty ret; int in_progress; char *cname; } Inst;
static Inst *insts[512];
static int ninsts = 0;
static void gen_instance(Inst *in);

static Inst *get_inst(Node *fn, Ty *key) {
    for (int i = 0; i < ninsts; i++) {
        Inst *in = insts[i];
        if (in->fn != fn) continue;
        int same = 1;
        for (int k = 0; k < fn->n; k++) if (in->args[k] != key[k]) same = 0;
        if (same) return in;
    }
    if (ninsts >= 512) die("too many function copies");
    Inst *in = xmalloc(sizeof *in); memset(in, 0, sizeof *in);
    in->fn = fn;
    Buf nb = {0}; buf_printf(&nb, "f_%s", fn->name);
    for (int k = 0; k < fn->n; k++) { in->args[k] = key[k]; buf_printf(&nb, "__%s", ty_name(key[k])); }
    in->cname = nb.s; in->ret = TY_UNKNOWN;
    insts[ninsts++] = in;
    gen_instance(in);
    return in;
}

enum { R_FLOATLIKE = -10, R_SAMEAS = -11 };
typedef struct { const char *name; int module; int minargs, maxargs; const char *sig; int ret; const char *cfn; } Builtin;
static const Builtin builtins[] = {
    { "len",       M_CORE, 1, 1, "s",  TY_INT,      "sp_len" },
    { "input",     M_IO,   0, 1, "s",  TY_STR,      "sp_input" },
    { "input_int", M_IO,   0, 1, "s",  TY_INT,      "sp_input_int" },
    { "sqrt",      M_MATH, 1, 1, "n",  R_FLOATLIKE, "sqrt" },
    { "sin",       M_MATH, 1, 1, "n",  R_FLOATLIKE, "sin" },
    { "cos",       M_MATH, 1, 1, "n",  R_FLOATLIKE, "cos" },
    { "tan",       M_MATH, 1, 1, "n",  R_FLOATLIKE, "tan" },
    { "floor",     M_MATH, 1, 1, "n",  R_FLOATLIKE, "floor" },
    { "ceil",      M_MATH, 1, 1, "n",  R_FLOATLIKE, "ceil" },
    { "round",     M_MATH, 1, 1, "n",  R_FLOATLIKE, "round" },
    { "pow",       M_MATH, 2, 2, "nn", R_FLOATLIKE, "pow" },
    { "abs",       M_MATH, 1, 1, "n",  R_SAMEAS,    "abs" },
    { "min",       M_MATH, 2, 2, "nn", R_SAMEAS,    "min" },
    { "max",       M_MATH, 2, 2, "nn", R_SAMEAS,    "max" },
    { "rand",      M_GAME, 2, 2, "ii", TY_INT,      "sp_rand" },
    { "sleep",     M_GAME, 1, 1, "i",  TY_VOID,     "sp_sleep" },
    { "clear",     M_GAME, 0, 0, "",   TY_VOID,     "sp_clear" },
    { "time_ms",   M_GAME, 0, 0, "",   TY_INT,      "sp_time_ms" },
    /* Linux syscall module. Integer results use -1 on failure; inspect sys_errno(). */
    { "sys_open",      M_SYS, 3, 3, "sii", TY_INT,  "sp_sys_open" },
    { "sys_read",      M_SYS, 2, 2, "ii",  TY_STR,  "sp_sys_read" },
    { "sys_write",     M_SYS, 2, 2, "is",  TY_INT,  "sp_sys_write" },
    { "sys_close",     M_SYS, 1, 1, "i",   TY_INT,  "sp_sys_close" },
    { "sys_getpid",    M_SYS, 0, 0, "",    TY_INT,  "sp_sys_getpid" },
    { "sys_getppid",   M_SYS, 0, 0, "",    TY_INT,  "sp_sys_getppid" },
    { "sys_getuid",    M_SYS, 0, 0, "",    TY_INT,  "sp_sys_getuid" },
    { "sys_getgid",    M_SYS, 0, 0, "",    TY_INT,  "sp_sys_getgid" },
    { "sys_unlink",    M_SYS, 1, 1, "s",   TY_INT,  "sp_sys_unlink" },
    { "sys_mkdir",     M_SYS, 2, 2, "si",  TY_INT,  "sp_sys_mkdir" },
    { "sys_kill",      M_SYS, 2, 2, "ii",  TY_INT,  "sp_sys_kill" },
    { "sys_errno",     M_SYS, 0, 0, "",    TY_INT,  "sp_sys_errno" },
    { "sys_read_count",M_SYS, 0, 0, "",    TY_INT,  "sp_sys_read_count" },
    { "sys_sleep_ms",  M_SYS, 1, 1, "i",   TY_INT,  "sp_sys_sleep_ms" },
};
#define NBUILTINS ((int)(sizeof builtins / sizeof builtins[0]))
static const Builtin *find_builtin(const char *name) {
    for (int i = 0; i < NBUILTINS; i++) if (!strcmp(builtins[i].name, name)) return &builtins[i];
    return NULL;
}

/* ---------- helpers for expressions ---------- */

static void c_escape(Buf *b, const char *s) {          /* turn text into a C string literal */
    buf_add(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') buf_add(b, "\\\"");
        else if (c == '\\') buf_add(b, "\\\\");
        else if (c == '\n') buf_add(b, "\\n");
        else if (c == '\t') buf_add(b, "\\t");
        else if (c < 32 || c == 127) buf_printf(b, "\\%03o", c);
        else { char t[2] = { (char)c, 0 }; buf_add(b, t); }
    }
    buf_add(b, "\"");
}
static void to_text(Buf *dst, Ty t, const char *expr) {        /* wrap a value so it becomes a C string */
    switch (t) {
        case TY_INT:  buf_printf(dst, "sp_str_int(%s)", expr); break;
        case TY_F32:  buf_printf(dst, "sp_str_f32(%s)", expr); break;
        case TY_F64:  buf_printf(dst, "sp_str_f64(%s)", expr); break;
        case TY_BOOL: buf_printf(dst, "sp_str_bool(%s)", expr); break;
        default:      buf_add(dst, expr); break;
    }
}

static void undefined_name(Node *e) {
    const char *c[300]; int n = 0;
    for (int i = 0; i < C->nv && n < 280; i++) c[n++] = C->vars[i].name;
    if (C != &G) for (int i = 0; i < G.nv && n < 280; i++) c[n++] = G.vars[i].name;
    const char *s = closest(e->name, c, n);
    char hb[200];
    if (s) snprintf(hb, sizeof hb, "did you mean `%s`?", s);
    else snprintf(hb, sizeof hb, "create it first with  let %s = ...  (use `mut` if it will change later)", e->name);
    error_at(&e->tok, hb, "I don't know what `%s` is", e->name);
}

static Ty gen_expr(Node *e, Buf *out);

static Ty gen_binary(Node *e, Buf *out) {
    int op = e->op;
    Buf lb = {0}, rb = {0};
    Ty lt = gen_expr(e->a, &lb), rt = gen_expr(e->b, &rb);
    if (lt == TY_ERR || rt == TY_ERR) return TY_ERR;
    if (lt == TY_VOID || rt == TY_VOID) {
        error_at(&e->tok, "a function that doesn't `return` a value can't be used in a calculation", "one side of `%s` has no value", tk_name[op]);
        return TY_ERR;
    }
    const char *cop = tk_name[op];
    switch (op) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: {
        if (op == T_PLUS && (lt == TY_STR || rt == TY_STR)) {             /* joining text */
            if (is_struct(lt) || is_struct(rt)) {
                error_at(&e->tok, "join the fields instead, like  p.x", "can't join a whole struct into text");
                return TY_ERR;
            }
            Buf a = {0}, b = {0};
            to_text(&a, lt, BS(lb)); to_text(&b, rt, BS(rb));
            buf_printf(out, "sp_cat(%s, %s)", BS(a), BS(b));
            return TY_STR;
        }
        if (!is_num(lt) || !is_num(rt)) {
            const char *h = (lt == TY_STR || rt == TY_STR) ? "text can only be joined with +  (use str(x) to turn a number into text)" : NULL;
            error_at(&e->tok, h, "can't use `%s` between `%s` and `%s`", cop, ty_name(lt), ty_name(rt));
            return TY_ERR;
        }
        if (op == T_PERCENT && (lt != TY_INT || rt != TY_INT)) {
            error_at(&e->tok, "`%` (the remainder) only works on whole numbers", "can't use `%%` on decimal numbers");
            return TY_ERR;
        }
        Ty res = promote(lt, rt);
        if ((op == T_SLASH || op == T_PERCENT) && res == TY_INT)
            buf_printf(out, "%s(%s, %s, %d)", op == T_SLASH ? "sp_idiv" : "sp_imod", BS(lb), BS(rb), e->tok.line);
        else
            buf_printf(out, "(%s %s %s)", BS(lb), cop, BS(rb));
        return res;
    }
    case T_LT: case T_LE: case T_GT: case T_GE:
        if (!is_num(lt) || !is_num(rt)) {
            error_at(&e->tok, "only numbers can be compared with < > <= >=", "can't use `%s` between `%s` and `%s`", cop, ty_name(lt), ty_name(rt));
            return TY_ERR;
        }
        buf_printf(out, "(%s %s %s)", BS(lb), cop, BS(rb));
        return TY_BOOL;
    case T_EQ: case T_NE:
        if ((is_num(lt) && is_num(rt)) || (lt == TY_BOOL && rt == TY_BOOL)) buf_printf(out, "(%s %s %s)", BS(lb), cop, BS(rb));
        else if (lt == TY_STR && rt == TY_STR) buf_printf(out, "(strcmp(%s, %s) %s 0)", BS(lb), BS(rb), cop);
        else {
            error_at(&e->tok, is_struct(lt) ? "compare the fields instead, like  a.x == b.x" : "both sides must be the same kind of value",
                     "can't compare `%s` with `%s`", ty_name(lt), ty_name(rt));
            return TY_ERR;
        }
        return TY_BOOL;
    case T_AND: case T_OR:
        if (lt != TY_BOOL || rt != TY_BOOL) {
            error_at(&e->tok, "`and` / `or` join two true/false questions, like  x > 1 and x < 9",
                     "`%s` needs true/false on both sides, but got `%s` and `%s`", tk_name[op], ty_name(lt), ty_name(rt));
            return TY_ERR;
        }
        buf_printf(out, "(%s %s %s)", BS(lb), op == T_AND ? "&&" : "||", BS(rb));
        return TY_BOOL;
    }
    return TY_ERR;
}

static Ty gen_cast(Node *e, Buf *out) {
    const char *name = e->name;
    if (e->n != 1) { error_at(&e->tok, NULL, "`%s(...)` takes exactly 1 value, but you gave %d", name, e->n); return TY_ERR; }
    Buf ab = {0};
    Ty t = gen_expr(e->list[0], &ab);
    if (t == TY_ERR) return TY_ERR;
    if (t == TY_VOID || is_struct(t)) { error_at(&e->list[0]->tok, NULL, "can't convert `%s` with `%s(...)`", ty_name(t), name); return TY_ERR; }
    if (!strcmp(name, "str")) { to_text(out, t, BS(ab)); return TY_STR; }
    Ty to = !strcmp(name, "int") ? TY_INT : !strcmp(name, "f32") ? TY_F32 : TY_F64;
    if (t == TY_STR) buf_printf(out, to == TY_INT ? "atoi(%s)" : "((%s)atof(%s))", to == TY_INT ? BS(ab) : ty_c(to), BS(ab));
    else buf_printf(out, "((%s)(%s))", ty_c(to), BS(ab));
    return to;
}

static Ty gen_builtin(Node *e, const Builtin *b, Buf *out) {
    if (e->n < b->minargs || e->n > b->maxargs) {
        if (b->minargs == b->maxargs) error_at(&e->tok, NULL, "`%s` needs %d value%s but you gave %d", b->name, b->minargs, b->minargs == 1 ? "" : "s", e->n);
        else error_at(&e->tok, NULL, "`%s` takes %d or %d values but you gave %d", b->name, b->minargs, b->maxargs, e->n);
        return TY_ERR;
    }
    Buf ab[3]; Ty at[3]; int bad = 0;
    memset(ab, 0, sizeof ab);
    for (int i = 0; i < e->n; i++) {
        Node *arg = e->list[i];
        at[i] = gen_expr(arg, &ab[i]);
        char what[100]; snprintf(what, sizeof what, "`%s` got the wrong kind of value", b->name);
        if (at[i] == TY_ERR) bad = 1;
        else if (b->sig[i] == 'i' && at[i] != TY_INT) { mismatch(&arg->tok, TY_INT, at[i], what); bad = 1; }
        else if (b->sig[i] == 's' && at[i] != TY_STR) { mismatch(&arg->tok, TY_STR, at[i], what); bad = 1; }
        else if (b->sig[i] == 'n' && !is_num(at[i])) {
            error_at(&arg->tok, NULL, "`%s` needs a number here, but found `%s`", b->name, ty_name(at[i])); bad = 1;
        }
    }
    if (bad) return TY_ERR;
    Ty rt = b->ret;
    if (rt == R_FLOATLIKE) rt = (at[0] == TY_F64 || (e->n > 1 && at[1] == TY_F64)) ? TY_F64 : TY_F32;
    else if (rt == R_SAMEAS) rt = e->n > 1 ? promote(at[0], at[1]) : at[0];
    const char *cf = b->cfn;
    if (!strcmp(b->name, "abs")) cf = rt == TY_INT ? "sp_abs_i" : "fabs";
    else if (!strcmp(b->name, "min")) cf = rt == TY_INT ? "sp_min_i" : "sp_min_f";
    else if (!strcmp(b->name, "max")) cf = rt == TY_INT ? "sp_max_i" : "sp_max_f";
    buf_printf(out, "%s(", cf);
    if (e->n == 0 && b->maxargs > 0) buf_add(out, "\"\"");
    for (int i = 0; i < e->n; i++) { if (i) buf_add(out, ", "); buf_add(out, BS(ab[i])); }
    buf_add(out, ")");
    return rt;
}

static Ty gen_user_call(Node *e, Node *fn, Buf *out) {
    if (e->n != fn->n) {
        Buf sig = {0};
        for (int i = 0; i < fn->n; i++) buf_printf(&sig, "%s%s", i ? ", " : "", fn->list[i]->name);
        char hb[300]; snprintf(hb, sizeof hb, "it is written  %s(%s)", fn->name, BS(sig));
        error_at(&e->tok, hb, "`%s` needs %d value%s but you gave %d", fn->name, fn->n, fn->n == 1 ? "" : "s", e->n);
        return TY_ERR;
    }
    Buf ab[MAX_PARAMS]; Ty at[MAX_PARAMS], key[MAX_PARAMS]; int bad = 0;
    memset(ab, 0, sizeof ab);
    for (int i = 0; i < e->n; i++) {
        at[i] = gen_expr(e->list[i], &ab[i]);
        if (at[i] == TY_ERR) bad = 1;
        else if (at[i] == TY_VOID) { error_at(&e->list[i]->tok, "that function doesn't `return` a value", "this has no value to hand over"); bad = 1; }
    }
    if (bad) return TY_ERR;
    for (int i = 0; i < fn->n; i++) {
        Node *p = fn->list[i];
        if (p->tname) {
            Ty pt = resolve_type(p->tname, &p->ttok, 0);
            if (pt == TY_ERR) return TY_ERR;
            char what[160]; snprintf(what, sizeof what, "parameter `%s` of `%s` wants a different kind of value", p->name, fn->name);
            if (!assignable(at[i], pt)) { mismatch(&e->list[i]->tok, pt, at[i], what); return TY_ERR; }
            key[i] = pt;
        } else key[i] = at[i];
    }
    Inst *in = get_inst(fn, key);
    if (in->in_progress && in->ret == TY_UNKNOWN) {
        char hb[200]; snprintf(hb, sizeof hb, "say what it gives back, like  fn %s(...) -> int", fn->name);
        error_at(&e->tok, hb, "`%s` calls itself, so I need to be told what it gives back", fn->name);
        return TY_ERR;
    }
    buf_printf(out, "%s(", in->cname);
    for (int i = 0; i < e->n; i++) { if (i) buf_add(out, ", "); buf_add(out, BS(ab[i])); }
    buf_add(out, ")");
    return in->ret == TY_UNKNOWN ? TY_ERR : in->ret;
}

static Ty gen_call(Node *e, Buf *out) {
    const char *name = e->name;
    Node *fn = find_fn(name);
    if (fn) return gen_user_call(e, fn, out);
    if (!strcmp(name, "int") || !strcmp(name, "f32") || !strcmp(name, "f64") || !strcmp(name, "str")) return gen_cast(e, out);
    const Builtin *b = find_builtin(name);
    if (b) {
        if (!mod_on[b->module]) {
            char hb[120]; snprintf(hb, sizeof hb, "add  $import \"%s\"  at the top of your file", mod_names[b->module]);
            error_at(&e->tok, hb, "`%s` lives in the \"%s\" module, which isn't imported yet", name, mod_names[b->module]);
            return TY_ERR;
        }
        return gen_builtin(e, b, out);
    }
    if (!strcmp(name, "print")) { error_at(&e->tok, "no parentheses needed:  print \"hello\"", "`print` is a statement, not a function"); return TY_ERR; }
    if (find_var(name)) { error_at(&e->tok, NULL, "`%s` is a variable, not a function - you can't call it with ( )", name); return TY_ERR; }
    const char *c[400]; int n = 0;
    for (int i = 0; i < nfns && n < 250; i++) c[n++] = fns[i]->name;
    for (int i = 0; i < NBUILTINS; i++) if (mod_on[builtins[i].module]) c[n++] = builtins[i].name;
    c[n++] = "int"; c[n++] = "f32"; c[n++] = "f64"; c[n++] = "str";
    const char *s = closest(name, c, n);
    if (closest(name, (const char *[]){ "print" }, 1)) { error_at(&e->tok, "no parentheses needed:  print \"hello\"", "`%s` isn't a Sparks word - did you mean `print`?", name); return TY_ERR; }
    char hb[200]; const char *help = NULL;
    if (s) { snprintf(hb, sizeof hb, "did you mean `%s`?", s); help = hb; }
    else help = "define it with  fn name(...)  or check the spelling";
    error_at(&e->tok, help, "I don't know a function called `%s`", name);
    return TY_ERR;
}

static Ty gen_expr(Node *e, Buf *out) {
    switch (e->kind) {
    case N_INT:   buf_printf(out, "%lld", e->ival); return TY_INT;
    case N_FLOAT: buf_add(out, e->name); return TY_F32;           /* written as a C double; it fits f32 or f64 */
    case N_BOOL:  buf_add(out, e->ival ? "true" : "false"); return TY_BOOL;
    case N_STR:   c_escape(out, e->name); return TY_STR;
    case N_IDENT: {
        Var *v = find_var(e->name);
        if (v) { buf_add(out, v->cname); return v->type; }
        if (!strcmp(e->name, "PI")) {
            if (mod_on[M_MATH]) { buf_add(out, "3.14159265358979323846"); return TY_F32; }
            error_at(&e->tok, "add  $import \"math\"  at the top of your file", "`PI` lives in the \"math\" module, which isn't imported yet");
            return TY_ERR;
        }
        if (find_fn(e->name)) { error_at(&e->tok, "to run it, add parentheses:  name(...)", "`%s` is a function, not a value", e->name); return TY_ERR; }
        undefined_name(e);
        return TY_ERR;
    }
    case N_UNARY: {
        Buf ab = {0};
        Ty t = gen_expr(e->a, &ab);
        if (t == TY_ERR) return TY_ERR;
        if (e->op == T_MINUS) {
            if (!is_num(t)) { error_at(&e->tok, NULL, "can't put `-` in front of a `%s`", ty_name(t)); return TY_ERR; }
            buf_printf(out, "(-%s)", BS(ab));
            return t;
        }
        if (t != TY_BOOL) { error_at(&e->tok, "`not` flips true to false, like  not done", "`not` needs a true/false value, but found `%s`", ty_name(t)); return TY_ERR; }
        buf_printf(out, "(!%s)", BS(ab));
        return TY_BOOL;
    }
    case N_BINARY: return gen_binary(e, out);
    case N_CALL:   return gen_call(e, out);
    case N_FIELD: {
        Buf ab = {0};
        Ty t = gen_expr(e->a, &ab);
        if (t == TY_ERR) return TY_ERR;
        if (!is_struct(t)) { error_at(&e->tok, "only structs have fields", "this is a `%s`, it has no `.%s`", ty_name(t), e->name); return TY_ERR; }
        int fi = find_field(t, e->name);
        if (fi < 0) {
            StructDef *sd = &structs[t - TY_STRUCT0];
            const char *s = closest(e->name, (const char **)sd->fname, sd->nf);
            char hb[200]; snprintf(hb, sizeof hb, s ? "did you mean `%s`?" : "this struct has no field with that name", s ? s : "");
            error_at(&e->tok, hb, "`%s` has no field called `%s`", sd->name, e->name);
            return TY_ERR;
        }
        buf_printf(out, "%s.%s", BS(ab), csafe(structs[t - TY_STRUCT0].fname[fi]));
        return structs[t - TY_STRUCT0].ftype[fi];
    }
    case N_STRUCTLIT: {
        int si = -1;
        for (int i = 0; i < nstructs; i++) if (!strcmp(structs[i].name, e->name)) si = i;
        if (si < 0) {
            const char *c[130]; int n = 0;
            for (int i = 0; i < nstructs; i++) c[n++] = structs[i].name;
            const char *s = closest(e->name, c, n); char hb[200];
            snprintf(hb, sizeof hb, s ? "did you mean `%s`?" : "define it first with  struct %s", s ? s : e->name);
            error_at(&e->tok, hb, "I don't know a struct called `%s`", e->name);
            return TY_ERR;
        }
        StructDef *sd = &structs[si];
        int given[MAX_FIELDS]; memset(given, 0, sizeof given);
        Buf fb = {0}; int bad = 0;
        for (int i = 0; i < e->n; i++) {
            Node *fi = e->list[i];
            int idx = find_field(TY_STRUCT0 + si, fi->name);
            if (idx < 0) {
                const char *s = closest(fi->name, (const char **)sd->fname, sd->nf); char hb[200];
                snprintf(hb, sizeof hb, s ? "did you mean `%s`?" : "check the struct's definition for its field names", s ? s : "");
                error_at(&fi->tok, hb, "`%s` has no field called `%s`", sd->name, fi->name);
                bad = 1; continue;
            }
            if (given[idx]) { error_at(&fi->tok, NULL, "the field `%s` is given twice", fi->name); bad = 1; continue; }
            given[idx] = 1;
            Buf vb = {0};
            Ty vt = gen_expr(fi->a, &vb);
            if (vt == TY_ERR) { bad = 1; continue; }
            if (vt == TY_VOID) { error_at(&fi->a->tok, NULL, "this has no value to store in `%s`", fi->name); bad = 1; continue; }
            if (!assignable(vt, sd->ftype[idx])) {
                char what[120]; snprintf(what, sizeof what, "the field `%s` wants a different kind of value", fi->name);
                mismatch(&fi->a->tok, sd->ftype[idx], vt, what); bad = 1; continue;
            }
            if (fb.len) buf_add(&fb, ", ");
            buf_printf(&fb, ".%s = %s", csafe(sd->fname[idx]), BS(vb));
        }
        for (int i = 0; i < sd->nf; i++) if (!given[i]) {
            char hb[120]; snprintf(hb, sizeof hb, "add  %s: <value>", sd->fname[i]);
            error_at(&e->tok, hb, "`%s { ... }` is missing the field `%s`", sd->name, sd->fname[i]);
            bad = 1;
        }
        if (bad) return TY_ERR;
        buf_printf(out, "(%s){ %s }", sd->cname, BS(fb));
        return TY_STRUCT0 + si;
    }
    default: break;
    }
    return TY_ERR;
}

/* ---------- statements ---------- */

static int gen_block(Node *b, int scope);

static int is_const_expr(Node *e) {                   /* known while compiling? */
    switch (e->kind) {
        case N_INT: case N_FLOAT: case N_STR: case N_BOOL: return 1;
        case N_IDENT: { Var *v = find_var(e->name); return (v && v->kind == V_CONST) || !strcmp(e->name, "PI"); }
        case N_UNARY: return is_const_expr(e->a);
        case N_BINARY: return is_const_expr(e->a) && is_const_expr(e->b);
        default: return 0;
    }
}

static int has_break(Node *blk) {                     /* does this block contain a `break` for its own loop? */
    if (!blk) return 0;
    for (int i = 0; i < blk->n; i++) {
        Node *s = blk->list[i];
        if (s->kind == N_BREAK) return 1;
        if (s->kind == N_IF) {
            for (Node *x = s; x; ) {
                if (has_break(x->b)) return 1;
                if (x->c && x->c->kind == N_IF) x = x->c;
                else { if (has_break(x->c)) return 1; x = NULL; }
            }
        }
    }
    return 0;
}

static void check_cond(Node *c, Buf *out, const char *what) {
    Ty t = gen_expr(c, out);
    if (t == TY_ERR || t == TY_BOOL) return;
    const char *help = is_num(t) ? "ask a question, like  x != 0  or  x > 5" : NULL;
    error_at(&c->tok, help, "`%s` needs a true/false question, but this is a `%s`", what, ty_name(t));
}

static void gen_let(Node *n) {
    int kind = n->flag == 0 ? V_LET : n->flag == 1 ? V_MUT : V_CONST;
    Ty want = TY_UNKNOWN;
    if (n->tname) want = resolve_type(n->tname, &n->ttok, 0);
    Buf ib = {0};
    Ty t = gen_expr(n->a, &ib);                       /* generate BEFORE declaring: `let x = x + 1` sees the old x */
    if (t == TY_VOID) {
        error_at(&n->a->tok, "that function doesn't `return` a value, so there is nothing to store", "this has no value");
        t = TY_ERR;
    }
    if (kind == V_CONST && t != TY_ERR && !is_const_expr(n->a))
        error_at(&n->a->tok, "use plain numbers, text, true/false, or other `const`s (maths on them is fine)",
                 "a `const` must be known while compiling, but this isn't");
    if (want == TY_ERR) t = TY_ERR;
    else if (want != TY_UNKNOWN && t != TY_ERR) {
        if (!assignable(t, want)) { mismatch(&n->a->tok, want, t, "the value doesn't match the type you wrote"); t = TY_ERR; }
        else t = want;
    }
    Var *v = declare(&n->tok, n->name, t, kind);
    if (t == TY_ERR) return;
    if (C == &G) {                                    /* top level: declare now, set the value when the program starts */
        buf_printf(&out_globals, "static %s %s;\n", ty_c(t), v->cname);
        L("%s = %s;", v->cname, BS(ib));
    } else L("%s %s = %s;", ty_c(t), v->cname, BS(ib));
}

static void gen_assign(Node *n) {
    Node *root = n->a;
    while (root->kind == N_FIELD) root = root->a;
    Buf tb = {0};
    Ty tt = gen_expr(n->a, &tb);
    if (root->kind != N_IDENT) { error_at(&n->tok, "only a variable (or a field of one) can be changed", "can't store a value here"); return; }
    Var *v = find_var(root->name);
    if (v && v->kind != V_MUT && v->type != TY_ERR) {
        char hb[240]; const char *h = NULL;
        switch (v->kind) {
            case V_LET:   snprintf(hb, sizeof hb, "`let` variables never change. Declare it with `mut` instead:  mut %s = ...", v->name); h = hb; break;
            case V_CONST: h = "a `const` is fixed forever"; break;
            case V_PARAM: snprintf(hb, sizeof hb, "parameters are read-only copies. Make your own copy:  mut my_%s = %s", v->name, v->name); h = hb; break;
            default:      h = "the `for` loop controls this variable. Use a separate `mut` variable for your own counter"; break;
        }
        error_at(&root->tok, h, "`%s` can't be changed", v->name);
    }
    Buf vb = {0}; Ty vt;
    if (n->op == T_ASSIGN) vt = gen_expr(n->b, &vb);
    else {                                            /* x += 1   means   x = x + 1 */
        Node *bn = node(N_BINARY, &n->tok);
        bn->op = n->op == T_PLUSEQ ? T_PLUS : n->op == T_MINUSEQ ? T_MINUS : n->op == T_STAREQ ? T_STAR : T_SLASH;
        bn->a = n->a; bn->b = n->b;
        vt = gen_expr(bn, &vb);
    }
    if (tt == TY_ERR || vt == TY_ERR) return;
    if (vt == TY_VOID) { error_at(&n->b->tok, "that function doesn't `return` a value", "this has no value to store"); return; }
    if (!assignable(vt, tt)) { mismatch(&n->b->tok, tt, vt, "can't store this value there"); return; }
    L("%s = %s;", BS(tb), BS(vb));
}

static void emit_return(const Token *tok, Ty t, const char *expr) {
    if (t == TY_ERR) return;
    if (t == TY_VOID) { error_at(tok, NULL, "this doesn't give back a value, so there is nothing to return"); return; }
    if (C->ret == TY_UNKNOWN) C->ret = t;
    else if (!assignable(t, C->ret)) { mismatch(tok, C->ret, t, "this function gives back a different kind of value elsewhere"); return; }
    L("return %s;", expr);
}

static void gen_return(Node *n) {
    if (C->is_main && n->a) { error_at(&n->tok, "just let `main` finish", "`main` can't give back a value"); return; }
    if (!n->a) {
        if (C->ret == TY_UNKNOWN) C->ret = TY_VOID;
        else if (C->ret != TY_VOID && C->ret != TY_ERR)
            error_at(&n->tok, "write  return value", "this function gives back a `%s`, so `return` needs a value", ty_name(C->ret));
        L("return;");
        return;
    }
    Buf eb = {0};
    Ty t = gen_expr(n->a, &eb);
    emit_return(&n->a->tok, t, BS(eb));
}

static int gen_exprstmt(Node *n) {
    Buf eb = {0};
    Ty t = gen_expr(n->a, &eb);
    if (t == TY_ERR) return 0;
    if (n->flag && t != TY_VOID && C->ret != TY_VOID) {     /* last line of a function = its result */
        emit_return(&n->a->tok, t, BS(eb));
        return 1;
    }
    if (n->a->kind == N_CALL) L("%s;", BS(eb));
    else warn_at(&n->a->tok, "to keep a value, store it:  let name = ...", "this value is calculated but never used");
    return 0;
}

static void gen_print(Node *n) {
    Buf fmt = {0}, args = {0}; int bad = 0;
    for (int i = 0; i < n->n; i++) {
        Buf ab = {0};
        Ty t = gen_expr(n->list[i], &ab);
        if (t == TY_ERR) { bad = 1; continue; }
        if (i) buf_add(&fmt, " ");
        switch (t) {
            case TY_INT:  buf_add(&fmt, "%d");    buf_printf(&args, ", %s", BS(ab)); break;
            case TY_F32:  buf_add(&fmt, "%g");    buf_printf(&args, ", (double)(%s)", BS(ab)); break;
            case TY_F64:  buf_add(&fmt, "%.15g"); buf_printf(&args, ", (double)(%s)", BS(ab)); break;
            case TY_BOOL: buf_add(&fmt, "%s");    buf_printf(&args, ", (%s) ? \"true\" : \"false\"", BS(ab)); break;
            case TY_STR:  buf_add(&fmt, "%s");    buf_printf(&args, ", %s", BS(ab)); break;
            case TY_VOID: error_at(&n->list[i]->tok, "that function doesn't `return` a value", "there is nothing to print here"); bad = 1; break;
            default:
                error_at(&n->list[i]->tok, "print its fields one by one, like  print p.x", "can't print a whole `%s` at once", ty_name(t));
                bad = 1; break;
        }
    }
    if (!bad) L("printf(\"%s\\n\"%s);", BS(fmt), BS(args));
}

static int gen_if(Node *n) {
    Buf cb = {0};
    check_cond(n->a, &cb, "if");
    L("if (%s) {", BS(cb));
    C->ind++; int all = gen_block(n->b, 1); C->ind--;
    int has_else = 0;
    for (Node *e = n->c; e; ) {
        if (e->kind == N_IF) {
            Buf cb2 = {0};
            check_cond(e->a, &cb2, "else if");
            L("} else if (%s) {", BS(cb2));
            C->ind++; all &= gen_block(e->b, 1); C->ind--;
            e = e->c;
        } else {
            L("} else {");
            C->ind++; all &= gen_block(e, 1); C->ind--;
            has_else = 1; e = NULL;
        }
    }
    L("}");
    return all && has_else;
}

/* Returns 1 if the statement never "falls through" to the next one (it returns, or loops forever). */
static int gen_stmt(Node *n) {
    switch (n->kind) {
    case N_LET:      gen_let(n); return 0;
    case N_ASSIGN:   gen_assign(n); return 0;
    case N_EXPRSTMT: return gen_exprstmt(n);
    case N_PRINT:    gen_print(n); return 0;
    case N_IF:       return gen_if(n);
    case N_RETURN:   gen_return(n); return 1;
    case N_BREAK:
    case N_CONTINUE:
        if (!C->loops) error_at(&n->tok, "use it inside a `loop` or `for` block", "`%s` only makes sense inside a loop", n->kind == N_BREAK ? "break" : "continue");
        else L(n->kind == N_BREAK ? "break;" : "continue;");
        return 0;
    case N_LOOP: {
        Buf cb = {0};
        if (n->a) check_cond(n->a, &cb, "loop");
        L("while (%s) {", n->a ? BS(cb) : "1");
        C->ind++; C->loops++;
        gen_block(n->b, 1);
        C->loops--; C->ind--;
        L("}");
        return !n->a && !has_break(n->b);
    }
    case N_FOR: {
        Buf lo = {0}, hi = {0};
        Ty lt = gen_expr(n->a, &lo), ht = gen_expr(n->b, &hi);
        if (lt != TY_ERR && lt != TY_INT) mismatch(&n->a->tok, TY_INT, lt, "a `for` range needs whole numbers");
        if (ht != TY_ERR && ht != TY_INT) mismatch(&n->b->tok, TY_INT, ht, "a `for` range needs whole numbers");
        push_scope();
        Var *v = declare(&n->tok, n->name, TY_INT, V_LOOP);
        char *cn = v->cname;
        int id = ++uniq;
        L("for (int %s = %s, sp_end%d = %s; %s < sp_end%d; %s++) {", cn, BS(lo), id, BS(hi), cn, id, cn);
        C->ind++; C->loops++;
        gen_block(n->c, 0);
        C->loops--; C->ind--;
        L("}");
        pop_scope();
        return 0;
    }
    default: break;
    }
    return 0;
}

static int gen_block(Node *b, int scope) {
    if (scope) push_scope();
    int term = 0, warned = 0;
    for (int i = 0; i < b->n; i++) {
        if (term && !warned) { warn_at(&b->list[i]->tok, "remove it, or move it before the `return`", "this code can never run"); warned = 1; }
        term = gen_stmt(b->list[i]);
    }
    if (scope) pop_scope();
    return term;
}

/* ---------- functions ---------- */

static void mark_tail(Node *blk) {                    /* the last expression of a function becomes `return expr` */
    if (!blk || blk->n == 0) return;
    Node *last = blk->list[blk->n - 1];
    if (last->kind == N_EXPRSTMT) { last->flag = 1; return; }
    if (last->kind != N_IF) return;
    Node *x = last;
    while (x->c && x->c->kind == N_IF) x = x->c;
    if (!x->c) return;                                /* an `if` without `else` can't produce a value */
    for (x = last; x; ) {
        mark_tail(x->b);
        if (x->c && x->c->kind == N_IF) x = x->c;
        else { mark_tail(x->c); x = NULL; }
    }
}

static void gen_instance(Inst *in) {
    Node *fn = in->fn;
    Ctx ctx; memset(&ctx, 0, sizeof ctx);
    Buf body = {0};
    ctx.out = &body; ctx.ind = 1; ctx.ret = TY_UNKNOWN;
    ctx.is_main = !strcmp(fn->name, "main");
    Ctx *saved = C; C = &ctx;
    in->in_progress = 1;
    if (fn->tname) { ctx.ret = resolve_type(fn->tname, &fn->ttok, 1); in->ret = ctx.ret; }
    if (ctx.is_main && (fn->n || fn->tname))
        error_at(&fn->tok, "write it as  fn main()", "`main` can't take values or give one back");
    for (int i = 0; i < fn->n; i++) declare(&fn->list[i]->tok, fn->list[i]->name, in->args[i], V_PARAM);
    if (!ctx.is_main) mark_tail(fn->a);
    int term = gen_block(fn->a, 0);
    Ty ret = ctx.ret == TY_UNKNOWN ? TY_VOID : ctx.ret;
    if (!ctx.is_main && ret != TY_VOID && ret != TY_ERR && !term)
        error_at(&fn->tok, "end the function with a value, and make sure every `if` branch has an `else`",
                 "`%s` can finish without giving back a value on some path", fn->name);
    in->ret = ret; in->in_progress = 0;
    Buf sig = {0};
    for (int i = 0; i < fn->n; i++) buf_printf(&sig, "%s%s %s", i ? ", " : "", ty_c(in->args[i]), ctx.vars[i].cname);
    if (!fn->n) buf_add(&sig, "void");
    buf_printf(&out_protos, "static %s %s(%s);\n", ty_c(ret), in->cname, BS(sig));
    buf_printf(&out_funcs, "static %s %s(%s) {\n%s}\n\n", ty_c(ret), in->cname, BS(sig), BS(body));
    C = saved;
}

/* ---------- the whole program ---------- */

static void declare_struct(Node *d) {
    for (int i = 0; i < nstructs; i++)
        if (!strcmp(structs[i].name, d->name)) { error_at(&d->tok, NULL, "a struct called `%s` already exists", d->name); return; }
    if (!isupper((unsigned char)d->name[0]))
        error_at(&d->tok, "struct names start with a capital letter, like  Vector2", "`%s` should start with a capital letter", d->name);
    if (nstructs >= 128) die("too many structs");
    StructDef sd; memset(&sd, 0, sizeof sd);
    sd.name = d->name; sd.tok = d->tok;
    Buf cn = {0}; buf_printf(&cn, "s_%s", d->name); sd.cname = cn.s;
    Buf body = {0};
    for (int i = 0; i < d->n; i++) {
        Node *f = d->list[i];
        for (int k = 0; k < sd.nf; k++) if (!strcmp(sd.fname[k], f->name)) { error_at(&f->tok, NULL, "the field `%s` is defined twice", f->name); }
        Ty t = resolve_type(f->tname, &f->ttok, 0);
        if (t == TY_ERR) continue;
        if (sd.nf >= MAX_FIELDS) { error_at(&f->tok, NULL, "a struct can have at most %d fields", MAX_FIELDS); break; }
        sd.fname[sd.nf] = f->name; sd.ftype[sd.nf] = t; sd.nf++;
        buf_printf(&body, "    %s %s;\n", ty_c(t), csafe(f->name));
    }
    if (sd.nf == 0) { error_at(&d->tok, "add fields like  x: f32", "struct `%s` needs at least one field", d->name); return; }
    structs[nstructs++] = sd;
    buf_printf(&out_structs, "typedef struct {\n%s} %s;\n\n", BS(body), sd.cname);
}

static void register_fn(Node *d) {
    Node *old = find_fn(d->name);
    if (old) { error_at(&d->tok, "give each function its own name", "a function called `%s` already exists (line %d)", d->name, old->tok.line); return; }
    if (d->n > MAX_PARAMS) { error_at(&d->tok, NULL, "a function can take at most %d values", MAX_PARAMS); return; }
    if (nfns >= 256) die("too many functions");
    fns[nfns++] = d;
}

static int has_inst(Node *fn) {
    for (int i = 0; i < ninsts; i++) if (insts[i]->fn == fn) return 1;
    return 0;
}

static void compile_program(Node *prog) {
    for (int i = 0; i < prog->n; i++) {                       /* pass 1: know all structs and function names */
        Node *d = prog->list[i];
        if (d->kind == N_STRUCT) declare_struct(d);
        else if (d->kind == N_FN) register_fn(d);
    }
    G.out = &g_init; G.ind = 1; C = &G;                       /* pass 2: top-level variables, in order */
    for (int i = 0; i < prog->n; i++) if (prog->list[i]->kind == N_LET) gen_let(prog->list[i]);

    Node *m = find_fn("main");                                /* pass 3: start from main; it pulls in what it calls */
    if (!m) {
        error_at(NULL, "every Sparks program starts here:\n         fn main()\n             print \"Hello!\"", "this program has no `fn main()`");
    } else {
        Ty key[MAX_PARAMS];
        get_inst(m, key);
    }
    for (int i = 0; i < nfns; i++) {                          /* pass 4: functions nobody called */
        Node *fn = fns[i];
        if (has_inst(fn)) continue;
        Ty key[MAX_PARAMS]; int ok = 1;
        for (int k = 0; k < fn->n; k++) {
            if (!fn->list[k]->tname) { ok = 0; break; }
            key[k] = resolve_type(fn->list[k]->tname, &fn->list[k]->ttok, 0);
            if (key[k] == TY_ERR) { ok = 0; break; }
        }
        if (ok) get_inst(fn, key);
        else warn_at(&fn->tok, "call it from `main` (or write the parameter types, like  a: int) so I can check what's inside",
                     "`%s` is never called, so I couldn't check its code", fn->name);
    }
}

/* =============================================================================
 *  6. THE RUNTIME  (small helper functions copied into every program)
 * ============================================================================= */

static const char RUNTIME[] =
"#define _GNU_SOURCE 1\n#define _POSIX_C_SOURCE 200809L\n"
"#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdbool.h>\n#include <math.h>\n#include <time.h>\n"
"#ifndef _WIN32\n#include <unistd.h>\n#include <sys/syscall.h>\n#include <sys/types.h>\n#include <sys/stat.h>\n#include <fcntl.h>\n#include <signal.h>\n#include <errno.h>\n#endif\n"
"#ifdef _WIN32\n#include <windows.h>\n#endif\n\n"
"/* ---- Sparks runtime: small helpers the program above uses ---- */\n"
"static int sp_eof = 0;\n"
"static void *sp_alloc(size_t n) { void *p = malloc(n); if (!p) { fputs(\"Out of memory\\n\", stderr); exit(1); } return p; }\n"
"/* Text is never freed (kept simple on purpose). Fine for small programs and games. */\n"
"static const char *sp_cat(const char *a, const char *b) {\n"
"    size_t la = strlen(a), lb = strlen(b);\n"
"    char *r = sp_alloc(la + lb + 1);\n"
"    memcpy(r, a, la); memcpy(r + la, b, lb + 1);\n"
"    return r;\n"
"}\n"
"static const char *sp_str_int(int v) { char *r = sp_alloc(16); snprintf(r, 16, \"%d\", v); return r; }\n"
"static const char *sp_str_f32(double v) { char *r = sp_alloc(32); snprintf(r, 32, \"%g\", v); return r; }\n"
"static const char *sp_str_f64(double v) { char *r = sp_alloc(40); snprintf(r, 40, \"%.15g\", v); return r; }\n"
"static const char *sp_str_bool(bool v) { return v ? \"true\" : \"false\"; }\n"
"static int sp_len(const char *s) { return (int)strlen(s); }\n"
"static int sp_idiv(int a, int b, int line) {\n"
"    if (b == 0) { fprintf(stderr, \"Runtime error on line %d: can't divide by zero\\n\", line); exit(1); }\n"
"    return a / b;\n"
"}\n"
"static int sp_imod(int a, int b, int line) {\n"
"    if (b == 0) { fprintf(stderr, \"Runtime error on line %d: can't divide by zero\\n\", line); exit(1); }\n"
"    return a % b;\n"
"}\n"
"static int sp_abs_i(int a) { return a < 0 ? -a : a; }\n"
"static int sp_min_i(int a, int b) { return a < b ? a : b; }\n"
"static int sp_max_i(int a, int b) { return a > b ? a : b; }\n"
"static double sp_min_f(double a, double b) { return a < b ? a : b; }\n"
"static double sp_max_f(double a, double b) { return a > b ? a : b; }\n"
"static const char *sp_input(const char *prompt) {\n"
"    char buf[4096];\n"
"    fputs(prompt, stdout); fflush(stdout);\n"
"    if (!fgets(buf, sizeof buf, stdin)) { fputs(\"\\n(no more input - stopping the program)\\n\", stdout); exit(0); }\n"
"    size_t n = strlen(buf);\n"
"    while (n > 0 && (buf[n-1] == '\\n' || buf[n-1] == '\\r')) buf[--n] = 0;\n"
"    char *r = sp_alloc(n + 1); memcpy(r, buf, n + 1);\n"
"    return r;\n"
"}\n"
"static int sp_input_int(const char *prompt) {\n"
"    for (;;) {\n"
"        const char *s = sp_input(prompt);\n"
"        char *end; long v = strtol(s, &end, 10);\n"
"        while (*end == ' ') end++;\n"
"        if (end != s && *end == 0) return (int)v;\n"
"        puts(\"Please type a whole number.\");\n"
"    }\n"
"}\n"
"static int sp_rand(int lo, int hi) {\n"
"    if (lo > hi) { int t = lo; lo = hi; hi = t; }\n"
"    return lo + rand() % (hi - lo + 1);\n"
"}\n"
"static long sp_now_raw(void) {\n"
"#ifdef _WIN32\n"
"    return (long)GetTickCount();\n"
"#else\n"
"    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);\n"
"    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;\n"
"#endif\n"
"}\n"
"static long sp_t0 = 0;\n"
"static int sp_time_ms(void) { return (int)(sp_now_raw() - sp_t0); }\n"
"static void sp_sleep(int ms) {\n"
"    fflush(stdout);\n"
"#ifdef _WIN32\n"
"    Sleep(ms);\n"
"#else\n"
"    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;\n"
"    nanosleep(&ts, NULL);\n"
"#endif\n"
"}\n"
"static void sp_clear(void) {\n"
"#ifdef _WIN32\n"
"    system(\"cls\");\n"
"#else\n"
"    fputs(\"\\033[2J\\033[H\", stdout);\n"
"#endif\n"
"    fflush(stdout);\n"
"}\n"
"static void sp_rt_init(void) { srand((unsigned)time(NULL)); sp_t0 = sp_now_raw(); }\n"
"/* ---- experimental Sparks Linux syscall module ---- */\n"
"#ifndef _WIN32\n"
"static char sp_sys_read_buf[4096]; static int sp_sys_read_n = 0;\n"
"static int sp_sys_open(const char *p, int flags, int mode) { return (int)syscall(SYS_openat, AT_FDCWD, p, flags, mode); }\n"
"static const char *sp_sys_read(int fd, int limit) { if (limit < 1) limit = 1; if (limit > 4095) limit = 4095; long n = syscall(SYS_read, fd, sp_sys_read_buf, (size_t)limit); sp_sys_read_n = n < 0 ? -1 : (int)n; if (n < 0) { sp_sys_read_buf[0] = 0; return sp_sys_read_buf; } sp_sys_read_buf[n] = 0; return sp_sys_read_buf; }\n"
"static int sp_sys_write(int fd, const char *s) { return (int)syscall(SYS_write, fd, s, strlen(s)); }\n"
"static int sp_sys_close(int fd) { return (int)syscall(SYS_close, fd); }\n"
"static int sp_sys_getpid(void) { return (int)syscall(SYS_getpid); }\n"
"static int sp_sys_getppid(void) { return (int)syscall(SYS_getppid); }\n"
"static int sp_sys_getuid(void) { return (int)syscall(SYS_getuid); }\n"
"static int sp_sys_getgid(void) { return (int)syscall(SYS_getgid); }\n"
"static int sp_sys_unlink(const char *p) { return (int)syscall(SYS_unlinkat, AT_FDCWD, p, 0); }\n"
"static int sp_sys_mkdir(const char *p, int mode) { return (int)syscall(SYS_mkdirat, AT_FDCWD, p, mode); }\n"
"static int sp_sys_kill(int pid, int sig) { return (int)syscall(SYS_kill, pid, sig); }\n"
"static int sp_sys_errno(void) { return errno; }\n"
"static int sp_sys_read_count(void) { return sp_sys_read_n; }\n"
"static int sp_sys_sleep_ms(int ms) { struct timespec ts; if (ms < 0) { errno = EINVAL; return -1; } ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; return (int)syscall(SYS_nanosleep, &ts, NULL); }\n"
"#else\n"
"static int sp_sys_open(const char *p, int f, int m) { (void)p;(void)f;(void)m; return -1; } static const char *sp_sys_read(int f,int n) { (void)f;(void)n; return \"\"; } static int sp_sys_write(int f,const char*s) { (void)f;(void)s; return -1; } static int sp_sys_close(int f) { (void)f; return -1; } static int sp_sys_getpid(void) { return -1; } static int sp_sys_getppid(void) { return -1; } static int sp_sys_getuid(void) { return -1; } static int sp_sys_getgid(void) { return -1; } static int sp_sys_unlink(const char*p) { (void)p; return -1; } static int sp_sys_mkdir(const char*p,int m) { (void)p;(void)m; return -1; } static int sp_sys_kill(int p,int s) { (void)p;(void)s; return -1; } static int sp_sys_errno(void) { return -1; } static int sp_sys_read_count(void) { return -1; } static int sp_sys_sleep_ms(int m) { (void)m; return -1; }\n"
"#endif\n\n";

static void build_c(Buf *out, const char *src_path) {
    buf_printf(out, "/* Generated by spc %s from %s.\n * This is the C code your Sparks program turned into. Have a look around!\n */\n", VERSION, src_path);
    buf_add(out, RUNTIME);
    buf_add(out, "/* ---- your structs ---- */\n");
    buf_add(out, BS(out_structs));
    buf_add(out, "/* ---- function prototypes ---- */\n");
    buf_add(out, BS(out_protos));
    buf_add(out, "\n/* ---- your global variables ---- */\n");
    buf_add(out, BS(out_globals));
    buf_add(out, "\n/* ---- your functions ---- */\n");
    buf_add(out, BS(out_funcs));
    buf_add(out, "/* ---- program start: set the global variables, then run main ---- */\n");
    buf_printf(out, "static void sp_init_globals(void) {\n%s}\n\n", BS(g_init));
    buf_add(out, "int main(void) {\n    sp_rt_init();\n    sp_init_globals();\n    f_main();\n    return 0;\n}\n");
}

/* =============================================================================
 *  7. THE DRIVER  (reads the command line and runs the 5 stages)
 * ============================================================================= */

static const char *opt_out = NULL;
static int opt_run = 0, opt_ast = 0, opt_emit_c = 0, opt_keep = 0, opt_verbose = 0;

static void usage(void) {
    puts("spc " VERSION " - the Sparks compiler\n\n"
         "Usage: spc <file.sp> [options]\n\n"
         "  -o <name>     name of the program to make (default: the file name without .sp)\n"
         "  -r, --run     compile, then run the program straight away\n"
         "  -v            explain each compiler stage as it happens\n"
         "  --tokens      show the lexer's tokens, then stop\n"
         "  --ast         show the parser's syntax tree, then stop\n"
         "  --emit-c      print the generated C code, then stop\n"
         "  -k, --keep-c  keep the generated C file next to the program\n"
         "  -h, --help    show this help\n\n"
         "spc needs a C compiler (cc, gcc or clang). Set SPC_CC to choose another.");
}

static const char *find_cc(void) {
    const char *cands[4]; int n = 0;
    if (getenv("SPC_CC")) cands[n++] = getenv("SPC_CC");
    cands[n++] = "cc"; cands[n++] = "gcc"; cands[n++] = "clang";
    for (int i = 0; i < n; i++) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "%s --version > %s 2>&1", cands[i], NULL_DEV);
        if (system(cmd) == 0) return cands[i];
    }
    return NULL;
}

static void stage(int n, const char *fmt, ...) {
    if (!opt_verbose) return;
    va_list ap; va_start(ap, fmt);
    printf("[%d/5] ", n); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

static void fail_if_errors(void) {
    if (n_errors == 0) return;
    fprintf(stderr, "Found %d error%s. No program was made - fix %s and try again.\n",
            n_errors, n_errors == 1 ? "" : "s", n_errors == 1 ? "it" : "them");
    exit(1);
}

int main(int argc, char **argv) {
    const char *input = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-o") && i + 1 < argc) opt_out = argv[++i];
        else if (!strcmp(a, "-r") || !strcmp(a, "--run")) opt_run = 1;
        else if (!strcmp(a, "-v")) opt_verbose = 1;
        else if (!strcmp(a, "--tokens")) opt_tokens = 1;
        else if (!strcmp(a, "--ast")) opt_ast = 1;
        else if (!strcmp(a, "--emit-c")) opt_emit_c = 1;
        else if (!strcmp(a, "-k") || !strcmp(a, "--keep-c")) opt_keep = 1;
        else if (a[0] == '-') { fprintf(stderr, "spc: unknown option `%s` (try --help)\n", a); return 2; }
        else if (!input) input = a;
        else { fprintf(stderr, "spc: only one source file at a time (use $import for the rest)\n"); return 2; }
    }
    if (!input) { usage(); return 2; }
#ifndef _WIN32
    use_color = ISATTY(2) && !getenv("NO_COLOR");
#endif

    Node *prog = node(N_PROGRAM, NULL);
    stage(1, "Lexing + 2/5 parsing %s ...", input);
    parse_file(input, NULL, prog);
    if (opt_tokens) return 0;
    if (opt_verbose) { printf("      %ld tokens, %d top-level declarations\n", total_tokens, prog->n); }
    fail_if_errors();
    if (opt_ast) { dump_node(prog, 0); return 0; }

    stage(3, "Checking names and types, and writing C code ...");
    compile_program(prog);
    fail_if_errors();

    Buf c = {0};
    build_c(&c, input);
    if (opt_emit_c) { fputs(BS(c), stdout); return 0; }

    /* decide the output names */
    char exe[1024];
    if (opt_out) snprintf(exe, sizeof exe, "%s", opt_out);
    else {
        snprintf(exe, sizeof exe, "%s", input);
        char *dot = strrchr(exe, '.'), *sl = strrchr(exe, PATH_SEP);
        if (dot && (!sl || dot > sl) && !strcmp(dot, ".sp")) *dot = 0; else strcat(exe, ".out");
        strcat(exe, EXE_SUFFIX);
    }
    char cfile[1100];
    snprintf(cfile, sizeof cfile, opt_keep ? "%s.c" : "%s.spc_tmp.c", exe);
    FILE *f = fopen(cfile, "wb");
    if (!f) { fprintf(stderr, "spc: can't write `%s`\n", cfile); return 2; }
    fputs(BS(c), f); fclose(f);

    const char *cc = find_cc();
    if (!cc) {
        fprintf(stderr, "spc: I couldn't find a C compiler (tried cc, gcc, clang).\n"
                        "     Install one, or set the SPC_CC environment variable. The C code was saved as %s\n", cfile);
        return 2;
    }
    stage(4, "Wrote %d bytes of C code", c.len);
    stage(5, "Compiling the C code with %s ...", cc);
    char cmd[3000];
    snprintf(cmd, sizeof cmd, "%s -O2 -w -o \"%s\" \"%s\" -lm", cc, exe, cfile);
    int rc = system(cmd);
    if (!opt_keep) remove(cfile);
    if (rc != 0) {
        fprintf(stderr, "spc: the C compiler rejected the generated code. That is a bug in spc, not in your program.\n"
                        "     Run again with -k to keep the C file and report it.\n");
        return 2;
    }
    printf("Success! Made %s%s\n", exe, n_warnings ? " (with warnings)" : "");
    if (opt_run) {
        char run[1100];
        const char *pre = strchr(exe, PATH_SEP) ? "" : "." ;
        snprintf(run, sizeof run, "%s%s\"%s\"", pre, (*pre && PATH_SEP == '/') ? "/" : (*pre ? "\\" : ""), exe);
        printf("--- running %s ---\n", exe);
        fflush(stdout);
        return system(run) == 0 ? 0 : 1;
    }
    return 0;
}
