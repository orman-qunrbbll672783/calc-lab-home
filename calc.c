// Orman Gurbanli 251ADB255
// https://github.com/orman-qunrbbll672783/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic, claude.ai) AI assistant wrote this program; conversation: https://claude.ai/chat/4adfdbf2-2298-4e63-a6f8-1f4908528462

/*
 * AI (Claude, Anthropic): prompt "can you please tell me how to complete this
 * programming languages task? do it for me get everything ready you will just
 * tell me where and what to put", with the lab description attached.
 * Link: see line 4.
 * Claude generated the whole file (tokenizer, parser, evaluation, command
 * line, file handling) and its comments, so this note covers every part of it.
 *
 * Overview
 *   1. main() reads the command line and decides which files to evaluate and
 *      where the results go.
 *   2. read_file() loads one input file into a single buffer.
 *   3. evaluate() walks that buffer line by line. Each expression line is
 *      parsed and computed at the same time by a recursive descent parser:
 *         expr    := term  { ('+' | '-') term }
 *         term    := power { ('*' | '/') power }
 *         power   := primary [ '**' power ]
 *         primary := NUMBER | '(' expr ')'
 *   4. The results (or a single ERROR:<pos>) are written to the output file.
 */

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Used to build output names. Lower case, because the lab's example
 * (Anna Berzina) produces task1_anna_berzina_221234.txt. */
#define STUDENT_FIRST "orman"
#define STUDENT_LAST  "gurbanli"
#define STUDENT_ID    "251ADB255"

/* ------------------------------------------------------------------------
 * Values: integer or float
 *
 * How integer and float arithmetic is chosen: every number and every
 * intermediate result carries an is_float flag. A literal decides it first:
 * digits only (12) is an integer, digits '.' digits (12.5) is a float. After
 * that, the rule "if either operand is a float, the operation is done in
 * double" means a result is a float exactly when at least one operand was.
 * So an integer result can only come from integers, and 7 / 2 stays C integer
 * division (3) while 7 / 2.0 becomes 3.5. The flag also picks the printf
 * format at the end: %lld for integers, %.15g for floats.
 * ------------------------------------------------------------------------ */
typedef struct {
    int is_float;   /* 0: the value is in i, 1: the value is in d */
    long long i;
    double d;
} Value;

static double as_double(Value v)
{
    return v.is_float ? v.d : (double)v.i;
}

/* ------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------ */
typedef enum {
    TOK_NUM,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_POW,      /* the two characters "**" */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_END,      /* no more tokens on this line */
    TOK_BAD       /* a character that cannot start any token */
} TokType;

typedef struct {
    TokType type;
    size_t pos;   /* 1-based position of the token's first character in the file */
    Value val;    /* only used when type == TOK_NUM */
} Token;

/* One operand of a chain like a ** b ** c (see parse_power). */
typedef struct {
    Value val;
    size_t start;   /* position where this operand starts */
    size_t op_pos;  /* position of the "**" in front of it (not used for the first) */
} PowItem;

typedef struct {
    char *buf;        /* the whole input file */
    size_t i;         /* 0-based index of the next character to read */
    size_t line_end;  /* 0-based index of this line's '\n', or the file length */
    Token tok;        /* the token the parser is looking at right now */
    size_t err_pos;   /* 1-based position of the first error */
    PowItem *stack;   /* operands of ** chains, shared by all nesting levels */
    size_t sp;        /* number of items in use */
    size_t cap;       /* number of items allocated */
} Parser;

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* ------------------------------------------------------------------------
 * Tokenizer
 *
 * How positions are tracked: the tokenizer never copies a line out of the
 * file. It reads straight from the buffer that holds the whole file, using
 * p->i, the 0-based index of the next unread character in that file. Since
 * positions are 1-based and count every character of the file (newlines and
 * comment lines included), the position of a character is simply its index
 * + 1. No separate line/column counter is needed, so nothing can drift.
 *
 * The tokenizer stops at p->line_end (the '\n' of the current line or the
 * end of the file). When nothing is left it returns TOK_END with position
 * line_end + 1, which is exactly the position of the '\n', or file length
 * + 1 when the last line has no '\n' - the place the lab wants "missing
 * operand" and "missing )" errors reported.
 *
 * An unknown character becomes a TOK_BAD token instead of an immediate
 * error. The parser then reports it when it actually needs a token at that
 * spot, so every error is "the start of the token the parser was looking at
 * when it could not continue", as the lab defines it.
 * ------------------------------------------------------------------------ */
static void next_token(Parser *p)
{
    char *s = p->buf;
    size_t i = p->i;
    size_t end = p->line_end;

    /* spaces and tabs may appear anywhere between tokens */
    while (i < end && (s[i] == ' ' || s[i] == '\t'))
        i++;

    p->tok.pos = i + 1;   /* index -> 1-based position */

    if (i >= end) {
        p->tok.type = TOK_END;
        p->i = i;
        return;
    }

    char c = s[i];

    if (is_digit(c)) {
        size_t start = i;
        unsigned long long v = 0;   /* unsigned: no undefined behaviour even on overflow */
        while (i < end && is_digit(s[i])) {
            v = v * 10u + (unsigned long long)(s[i] - '0');
            i++;
        }
        /* A float must be digits '.' digits. "5." and "1.5e3" are not
         * accepted here: the '.' or the 'e' is left for the next token,
         * which will be TOK_BAD, so the error lands on that character. */
        if (i + 1 < end && s[i] == '.' && is_digit(s[i + 1])) {
            i++;
            while (i < end && is_digit(s[i]))
                i++;
            /* strtod would also read an exponent ("1.5e3"), which our
             * grammar forbids, so the token is cut off with a temporary '\0'.
             * This avoids copying the number. The buffer has one spare byte
             * at the end, so s[i] is always valid. */
            char saved = s[i];
            s[i] = '\0';
            p->tok.val.is_float = 1;
            p->tok.val.d = strtod(s + start, NULL);
            p->tok.val.i = 0;
            s[i] = saved;
        } else {
            p->tok.val.is_float = 0;
            p->tok.val.i = (long long)v;
            p->tok.val.d = 0.0;
        }
        p->tok.type = TOK_NUM;
        p->i = i;
        return;
    }

    p->i = i + 1;
    switch (c) {
    case '+': p->tok.type = TOK_PLUS;   break;
    case '-': p->tok.type = TOK_MINUS;  break;
    case '/': p->tok.type = TOK_SLASH;  break;
    case '(': p->tok.type = TOK_LPAREN; break;
    case ')': p->tok.type = TOK_RPAREN; break;
    case '*':
        /* "**" only when the two stars touch; "* *" is two operators in a row */
        if (i + 1 < end && s[i + 1] == '*') {
            p->tok.type = TOK_POW;
            p->i = i + 2;
        } else {
            p->tok.type = TOK_STAR;
        }
        break;
    default:
        /* includes '#' that is not the first character of a line, '.',
         * letters, '\r' and everything else the rules do not allow */
        p->tok.type = TOK_BAD;
        break;
    }
}

/* Remember where the first error is and tell the caller to stop. */
static int fail(Parser *p, size_t pos)
{
    p->err_pos = pos;
    return 0;
}

/* ------------------------------------------------------------------------
 * Arithmetic
 *
 * Why each evaluation error is reported where it is:
 *  - division by zero: at the start of the right operand, because that
 *    operand is what is wrong. rhs_pos is taken right after the operator is
 *    consumed, so for 10 / (2 - 2) it is the '(' even though the zero only
 *    appears after the whole parenthesis is computed. A float zero divisor
 *    (1.5 / 0.0) is checked the same way, before dividing.
 *  - negative integer exponent: at the start of the exponent, for the same
 *    reason (the exponent is the bad operand).
 *  - a float result that is infinite or NaN: at the operator, because both
 *    operands are fine on their own; it is the operation that fails.
 *
 * Integer + - * are done in unsigned long long and converted back. That
 * gives the normal two's complement result and, unlike signed overflow,
 * is never undefined behaviour (the tests never overflow anyway).
 * ------------------------------------------------------------------------ */
static int apply_op(Parser *p, TokType op, size_t op_pos, size_t rhs_pos,
                    Value *acc, Value rhs)
{
    if (!acc->is_float && !rhs.is_float) {
        unsigned long long a = (unsigned long long)acc->i;
        unsigned long long b = (unsigned long long)rhs.i;
        switch (op) {
        case TOK_PLUS:  acc->i = (long long)(a + b); return 1;
        case TOK_MINUS: acc->i = (long long)(a - b); return 1;
        case TOK_STAR:  acc->i = (long long)(a * b); return 1;
        default:        /* TOK_SLASH */
            if (rhs.i == 0)
                return fail(p, rhs_pos);
            if (rhs.i == -1)
                acc->i = (long long)(0ULL - a);   /* avoids LLONG_MIN / -1 */
            else
                acc->i = acc->i / rhs.i;          /* C truncates toward zero, as required */
            return 1;
        }
    }

    double a = as_double(*acc);
    double b = as_double(rhs);
    double r;
    switch (op) {
    case TOK_PLUS:  r = a + b; break;
    case TOK_MINUS: r = a - b; break;
    case TOK_STAR:  r = a * b; break;
    default:        /* TOK_SLASH */
        if (b == 0.0)
            return fail(p, rhs_pos);
        r = a / b;
        break;
    }
    if (!isfinite(r))
        return fail(p, op_pos);
    acc->is_float = 1;
    acc->d = r;
    return 1;
}

static int apply_pow(Parser *p, size_t op_pos, size_t exp_pos, Value *base, Value ex)
{
    if (!base->is_float && !ex.is_float) {
        if (ex.i < 0)
            return fail(p, exp_pos);
        /* Exponentiation by squaring: O(log exponent) multiplications, so a
         * big exponent like 1 ** 1000000000 is still instant. Starting from
         * r = 1 makes x ** 0 == 1, including 0 ** 0. */
        unsigned long long b = (unsigned long long)base->i;
        unsigned long long e = (unsigned long long)ex.i;
        unsigned long long r = 1;
        while (e > 0) {
            if (e & 1u)
                r *= b;
            b *= b;
            e >>= 1;
        }
        base->i = (long long)r;
        return 1;
    }

    double r = pow(as_double(*base), as_double(ex));
    if (!isfinite(r))   /* e.g. (0 - 8) ** 0.5 is NaN, 10.0 ** 400 is inf */
        return fail(p, op_pos);
    base->is_float = 1;
    base->d = r;
    return 1;
}

/* ------------------------------------------------------------------------
 * Parser
 *
 * Why the grammar gives the right precedence: each precedence level has its
 * own function, and a function only builds its operands by calling the
 * next-higher level. parse_term() gets its operands from parse_power(), so
 * in 3 + 2 * 5 the "2 * 5" is finished inside parse_term() before
 * parse_expr() ever sees the '+'. The lowest level (+ -) is the outermost
 * call and the highest (**) is the innermost, so tighter operators are
 * always computed first. Parentheses restart at parse_expr(), which is why
 * they override precedence.
 *
 * Why + - * / are left-associative: parse_expr() and parse_term() use a
 * loop that folds each new operand into the running result immediately,
 * so 10 - 3 - 2 is (10 - 3) - 2.
 *
 * The parser evaluates while it parses, so an evaluation error (division
 * by zero etc.) is found at the moment the parser reaches it, in the same
 * left-to-right order as syntax errors.
 * ------------------------------------------------------------------------ */
static int parse_expr(Parser *p, Value *out);

/* primary := NUMBER | '(' expr ')'
 * Every syntax error ends up here or at a ')' / end-of-line check, and in
 * all cases it is reported at p->tok.pos, the token we are looking at:
 *  - a TOK_BAD token: the invalid character itself;
 *  - an operator where a number was expected: the second operator in a row
 *    (7 + * 3), or a leading '-' (there are no unary operators);
 *  - TOK_END where a number was expected: the line's '\n' (missing operand);
 *  - ')' where a number was expected: as in 2 * (), the ')' is reported. */
static int parse_primary(Parser *p, Value *out)
{
    if (p->tok.type == TOK_NUM) {
        *out = p->tok.val;
        next_token(p);
        return 1;
    }
    if (p->tok.type == TOK_LPAREN) {
        next_token(p);
        if (!parse_expr(p, out))
            return 0;
        /* missing ')': we are looking at the end of the line (or something
         * else that cannot continue the expression), so report that token */
        if (p->tok.type != TOK_RPAREN)
            return fail(p, p->tok.pos);
        next_token(p);
        return 1;
    }
    return fail(p, p->tok.pos);
}

static void push_operand(Parser *p, Value v, size_t start, size_t op_pos)
{
    if (p->sp == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 64;
        PowItem *ns = realloc(p->stack, ncap * sizeof *ns);
        if (ns == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        p->stack = ns;
        p->cap = ncap;
    }
    p->stack[p->sp].val = v;
    p->stack[p->sp].start = start;
    p->stack[p->sp].op_pos = op_pos;
    p->sp++;
}

/* power := primary [ '**' power ]
 *
 * Why ** is right-associative: the grammar rule is right-recursive (the
 * part after '**' is a whole power again), so a ** b ** c means
 * a ** (b ** c), and 2 ** 3 ** 2 == 2 ** 9 == 512.
 *
 * Written literally, that recursion would use one C stack frame per '**'.
 * Lines can be megabytes long, so a long chain like 1 ** 1 ** 1 ** ... could
 * overflow the stack. Instead the same thing is done with an explicit
 * stack: first all operands of the chain are read left to right (pushing
 * them), then they are combined from the right end back to the left, which
 * is exactly the order the recursive version would compute them in:
 *     operands 2, 3, 2  ->  acc = 2;  acc = 3 ** acc = 9;  acc = 2 ** acc = 512
 * The stack is shared by all nesting levels: each call remembers where its
 * own items start (base) and removes them before returning, so a chain
 * inside parentheses, e.g. 2 ** (3 ** 2), simply pushes above its caller's
 * items. */
static int parse_power(Parser *p, Value *out)
{
    size_t base = p->sp;
    size_t start = p->tok.pos;
    Value v;

    if (!parse_primary(p, &v))
        return 0;
    push_operand(p, v, start, 0);

    while (p->tok.type == TOK_POW) {
        size_t op_pos = p->tok.pos;
        next_token(p);
        start = p->tok.pos;
        if (!parse_primary(p, &v))
            return 0;
        push_operand(p, v, start, op_pos);
    }

    size_t k = p->sp - 1;
    Value acc = p->stack[k].val;
    /* acc is "everything right of the current '**'", so its start is where
     * the exponent starts - the position for a negative exponent error */
    size_t acc_start = p->stack[k].start;
    while (k > base) {
        k--;
        Value b = p->stack[k].val;
        size_t op_pos = p->stack[k + 1].op_pos;
        if (!apply_pow(p, op_pos, acc_start, &b, acc))
            return 0;
        acc = b;
        acc_start = p->stack[k].start;
    }

    p->sp = base;
    *out = acc;
    return 1;
}

/* term := power { ('*' | '/') power } */
static int parse_term(Parser *p, Value *out)
{
    if (!parse_power(p, out))
        return 0;
    while (p->tok.type == TOK_STAR || p->tok.type == TOK_SLASH) {
        TokType op = p->tok.type;
        size_t op_pos = p->tok.pos;
        next_token(p);
        size_t rhs_pos = p->tok.pos;   /* start of the right operand */
        Value rhs;
        if (!parse_power(p, &rhs))
            return 0;
        if (!apply_op(p, op, op_pos, rhs_pos, out, rhs))
            return 0;
    }
    return 1;
}

/* expr := term { ('+' | '-') term } */
static int parse_expr(Parser *p, Value *out)
{
    if (!parse_term(p, out))
        return 0;
    while (p->tok.type == TOK_PLUS || p->tok.type == TOK_MINUS) {
        TokType op = p->tok.type;
        size_t op_pos = p->tok.pos;
        next_token(p);
        size_t rhs_pos = p->tok.pos;
        Value rhs;
        if (!parse_term(p, &rhs))
            return 0;
        if (!apply_op(p, op, op_pos, rhs_pos, out, rhs))
            return 0;
    }
    return 1;
}

/* Evaluates the expression in buf[start, line_end). After a complete
 * expression the next token must be the end of the line; anything else
 * (an extra ')', a second number, an invalid character such as a '#' in
 * the middle of a line) is reported at that token. */
static int eval_line(Parser *p, size_t start, size_t line_end, Value *result)
{
    p->i = start;
    p->line_end = line_end;
    p->sp = 0;
    next_token(p);
    if (!parse_expr(p, result))
        return 0;
    if (p->tok.type != TOK_END)
        return fail(p, p->tok.pos);
    return 1;
}

/* ------------------------------------------------------------------------
 * Output buffer
 *
 * Results are collected in memory instead of being written straight away,
 * because one error anywhere in the file means the output must contain
 * only ERROR:<pos> and no partial results. The buffer doubles its size
 * when full, so appending n bytes costs O(n) in total, even for files of
 * several megabytes (no repeated copying).
 * ------------------------------------------------------------------------ */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} OutBuf;

static void out_append(OutBuf *o, const char *s, size_t n)
{
    if (o->len + n > o->cap) {
        size_t ncap = o->cap ? o->cap : 4096;
        while (ncap < o->len + n)
            ncap *= 2;
        char *nd = realloc(o->data, ncap);
        if (nd == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        o->data = nd;
        o->cap = ncap;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
}

static void out_append_value(OutBuf *o, Value v)
{
    char tmp[64];   /* %lld needs at most 20 chars, %.15g at most about 23 */
    int n;
    if (v.is_float)
        n = snprintf(tmp, sizeof tmp, "%.15g\n", v.d);
    else
        n = snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    out_append(o, tmp, (size_t)n);
}

/* ------------------------------------------------------------------------
 * Evaluating a whole file
 *
 * How large files stay fast: the file is read into memory once, and
 * everything else works on that one buffer by index. memchr() finds each
 * line end, the tokenizer reads characters in place, and no line or token
 * is ever copied out. Every character is looked at a constant number of
 * times, so the time is linear in the file size, and line length does not
 * matter (a 5 MB single line is handled like 5 MB of short lines).
 *
 * Returns 1 if every expression was fine, 0 if there was an error (its
 * position is stored in *err_pos). Evaluation stops at the first error,
 * because only that one is reported.
 * ------------------------------------------------------------------------ */
static int evaluate(char *buf, size_t len, OutBuf *out, size_t *err_pos)
{
    Parser p;
    memset(&p, 0, sizeof p);
    p.buf = buf;

    int ok = 1;
    size_t line_start = 0;
    while (line_start < len) {
        char *nl = memchr(buf + line_start, '\n', len - line_start);
        size_t line_end = nl ? (size_t)(nl - buf) : len;

        size_t k = line_start;
        while (k < line_end && (buf[k] == ' ' || buf[k] == '\t'))
            k++;

        /* blank lines give no output; a '#' as the first non-blank
         * character makes the line a comment. A '#' later in a line is
         * not checked here, so the tokenizer rejects it as invalid. */
        if (k < line_end && buf[k] != '#') {
            Value v;
            if (!eval_line(&p, k, line_end, &v)) {
                *err_pos = p.err_pos;
                ok = 0;
                break;
            }
            out_append_value(out, v);
        }
        line_start = line_end + 1;
    }

    free(p.stack);
    return ok;
}

/* ------------------------------------------------------------------------
 * Files and folders
 * ------------------------------------------------------------------------ */

/* Reads the whole file into one malloc'd buffer with a spare byte at the
 * end (used by the tokenizer's temporary '\0'). The size from stat() is
 * used for the first allocation, so a normal file is read with a single
 * allocation and no copying. If the file turns out bigger (or stat gave no
 * size), the buffer doubles, which still keeps the total copying linear. */
static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "calc: cannot open input file '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    size_t cap = 4096;
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0)
        cap = (size_t)st.st_size + 2;   /* +1 spare byte, +1 so a full read sees EOF */

    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        fclose(f);
        return NULL;
    }

    size_t len = 0;
    for (;;) {
        size_t want = cap - len - 1;   /* always keep one spare byte */
        if (want == 0) {
            char *nb = realloc(buf, cap * 2);
            if (nb == NULL) {
                fprintf(stderr, "calc: out of memory\n");
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap *= 2;
            continue;
        }
        size_t n = fread(buf + len, 1, want, f);
        len += n;
        if (n < want)   /* end of file or a read error */
            break;
    }

    if (ferror(f)) {
        fprintf(stderr, "calc: cannot read input file '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    buf[len] = '\0';
    *len_out = len;
    return buf;
}

/* Last component of a path, without trailing '/' and, if strip_txt is set,
 * without a final ".txt". "labs/task1.txt" -> "task1", "labs/" -> "labs". */
static char *base_name(const char *path, int strip_txt)
{
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    if (strip_txt && end - start >= 4 && memcmp(path + end - 4, ".txt", 4) == 0)
        end -= 4;

    char *s = malloc(end - start + 1);
    if (s == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    memcpy(s, path + start, end - start);
    s[end - start] = '\0';
    return s;
}

static char *join_path(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *s = malloc(a + b + 2);
    if (s == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    memcpy(s, dir, a);
    s[a] = '/';
    memcpy(s + a + 1, name, b + 1);
    return s;
}

/* Creates the output folder if needed. The lab says its parent exists, so
 * one mkdir() is enough; an existing folder is simply reused. */
static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 1;
        fprintf(stderr, "calc: output path '%s' exists and is not a folder\n", path);
        return 0;
    }
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

/* Evaluates one input file and writes <out_dir>/<base>_<name>_<lastname>_<id>.txt.
 * An ERROR:<pos> result is a normal result; only I/O problems return 0. */
static int process_file(const char *in_path, const char *out_dir)
{
    size_t len = 0;
    char *buf = read_file(in_path, &len);
    if (buf == NULL)
        return 0;

    OutBuf out = { NULL, 0, 0 };
    size_t err_pos = 0;
    int ok = evaluate(buf, len, &out, &err_pos);
    free(buf);

    char *base = base_name(in_path, 1);
    const char *suffix = "_" STUDENT_FIRST "_" STUDENT_LAST "_" STUDENT_ID ".txt";
    size_t blen = strlen(base);
    char *file = malloc(blen + strlen(suffix) + 1);
    if (file == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    memcpy(file, base, blen);
    strcpy(file + blen, suffix);
    char *out_path = join_path(out_dir, file);
    free(base);
    free(file);

    int result = 1;
    FILE *f = fopen(out_path, "w");
    if (f == NULL) {
        fprintf(stderr, "calc: cannot write output file '%s': %s\n", out_path, strerror(errno));
        result = 0;
    } else {
        if (ok) {
            if (out.len > 0)
                fwrite(out.data, 1, out.len, f);
        } else {
            fprintf(f, "ERROR:%zu\n", err_pos);
        }
        /* fclose also flushes, so a full disk is noticed here */
        int write_error = ferror(f);
        if (fclose(f) != 0 || write_error) {
            fprintf(stderr, "calc: cannot write output file '%s'\n", out_path);
            result = 0;
        }
    }

    free(out.data);
    free(out_path);
    return result;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Evaluates every regular file ending in ".txt" directly inside dir.
 * The names are collected first and only then processed, so output files
 * written into the same folder (-d labs -o labs) are not picked up as new
 * inputs. Sorting makes the order the same on every system. */
static int process_dir(const char *dir, const char *out_dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 0;
    }

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t L = strlen(e->d_name);
        if (L <= 4 || strcmp(e->d_name + L - 4, ".txt") != 0)
            continue;
        char *full = join_path(dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {   /* skip subfolders */
            free(full);
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **nn = realloc(names, ncap * sizeof *nn);
            if (nn == NULL) {
                fprintf(stderr, "calc: out of memory\n");
                exit(1);
            }
            names = nn;
            cap = ncap;
        }
        names[n++] = full;
    }
    closedir(d);

    if (n > 1)
        qsort(names, n, sizeof *names, compare_names);

    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (!process_file(names[i], out_dir))
            ok = 0;
        free(names[i]);
    }
    free(names);
    return ok;
}

static void usage(void)
{
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

/* ------------------------------------------------------------------------
 * main: command line
 * Exit code 0 when every output file was written (ERROR:<pos> results
 * included), 1 for no arguments, an unknown option, a missing input file
 * or folder, or an output that cannot be written.
 * ------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *dir = NULL, *out_dir = NULL, *input = NULL;

    if (argc < 2) {
        fprintf(stderr, "calc: no arguments\n");
        usage();
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        const char *arg = argv[a];
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--dir") == 0) {
            if (a + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a folder name\n", arg);
                usage();
                return 1;
            }
            dir = argv[++a];
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output-dir") == 0) {
            if (a + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a folder name\n", arg);
                usage();
                return 1;
            }
            out_dir = argv[++a];
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", arg);
            usage();
            return 1;
        } else {
            if (input != NULL) {
                fprintf(stderr, "calc: only one input file can be given\n");
                usage();
                return 1;
            }
            input = arg;
        }
    }

    if (dir != NULL && input != NULL) {
        fprintf(stderr, "calc: give either -d DIR or an input file, not both\n");
        usage();
        return 1;
    }
    if (dir == NULL && input == NULL) {
        fprintf(stderr, "calc: no input file or folder given\n");
        usage();
        return 1;
    }

    /* Check the input before creating any output folder, so a typo in the
     * input name does not leave an empty folder behind. */
    const char *src = dir != NULL ? dir : input;
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "calc: cannot open %s '%s': %s\n",
                dir != NULL ? "folder" : "input file", src, strerror(errno));
        return 1;
    }
    if (dir != NULL && !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is not a folder\n", dir);
        return 1;
    }
    if (dir == NULL && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is a folder; use -d to evaluate a folder\n", input);
        return 1;
    }

    /* Default output folder: <input_base>_<name>_<studentid> in the current
     * folder, where input_base is the file name without .txt or, with -d,
     * the folder's own name. */
    char *default_out = NULL;
    if (out_dir == NULL) {
        char *base = base_name(src, dir == NULL);
        const char *suffix = "_" STUDENT_FIRST "_" STUDENT_ID;
        size_t blen = strlen(base);
        default_out = malloc(blen + strlen(suffix) + 1);
        if (default_out == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            free(base);
            return 1;
        }
        memcpy(default_out, base, blen);
        strcpy(default_out + blen, suffix);
        free(base);
        out_dir = default_out;
    }

    int ok = ensure_dir(out_dir);
    if (ok)
        ok = dir != NULL ? process_dir(dir, out_dir) : process_file(input, out_dir);

    free(default_out);
    return ok ? 0 : 1;
}
