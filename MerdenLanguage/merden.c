#include "merden.h"

#include "../system/shell/shell.h"

#include "../standarts_lib/stdio.h"


#define MRD_MAX_TOKENS   512
#define MRD_MAX_STR      32
#define MRD_MAX_VARS     32
#define MRD_MAX_SCOPES   8

static int mrd_is_digit(char c) { return c >= '0' && c <= '9'; }
static int mrd_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int mrd_is_alnum(char c) { return mrd_is_alpha(c) || mrd_is_digit(c); }

static int mrd_streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static void mrd_strcpy_bounded(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (src[i] != '\0' && i < max_len - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    TK_EOF, TK_NEWLINE, TK_IDENT, TK_NUMBER, TK_STRING,

    TK_REMEMBER, TK_SET, TK_SAY, TK_WHEN, TK_OTHERWISE, TK_REPEAT, TK_IMPORT,
    TK_TRUE, TK_FALSE,

    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQUAL, TK_EQEQ, TK_NEQ,
    TK_GT, TK_GE, TK_LT, TK_LE,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE
} MTokenKind;

typedef struct {
    MTokenKind kind;
    int  number;                 /* for TK_NUMBER                    */
    char text[MRD_MAX_STR];      /* for TK_IDENT / TK_STRING         */
    int  line;
} MToken;

static MToken mrd_tokens[MRD_MAX_TOKENS];
static int    mrd_token_count = 0;

/* ------------------------------------------------------------------ */
/* Error state — instead of exceptions/longjmp, every executor        */
/* function checks mrd_had_error at its start and bails out           */
/* immediately if it is already set ("poison" propagation).           */
/* ------------------------------------------------------------------ */

static int  mrd_had_error = 0;
static char mrd_error_msg[64];
static int  mrd_error_line = 0;

static void mrd_error(const char *msg, int line)
{
    if (mrd_had_error)
        return; /* keep the first error */
    mrd_had_error = 1;
    mrd_error_line = line;
    mrd_strcpy_bounded(mrd_error_msg, msg, sizeof(mrd_error_msg));
}

static void mrd_print_error(void)
{
    shell_print("MerdenError: ");
    shell_print(mrd_error_msg);
    shell_print(" (line ");
    shell_print_int(mrd_error_line);
    shell_print(")\n");
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

static void mrd_push_token(MTokenKind kind, int number, const char *text, int line)
{
    if (mrd_token_count >= MRD_MAX_TOKENS)
    {
        mrd_error("Script too long (too many tokens)", line);
        return;
    }
    MToken *t = &mrd_tokens[mrd_token_count++];
    t->kind = kind;
    t->number = number;
    t->line = line;
    if (text)
        mrd_strcpy_bounded(t->text, text, MRD_MAX_STR);
    else
        t->text[0] = '\0';
}

static MTokenKind mrd_keyword_kind(const char *word)
{
    if (mrd_streq(word, "remember"))  return TK_REMEMBER;
    if (mrd_streq(word, "set"))       return TK_SET;
    if (mrd_streq(word, "say"))       return TK_SAY;
    if (mrd_streq(word, "when"))      return TK_WHEN;
    if (mrd_streq(word, "otherwise")) return TK_OTHERWISE;
    if (mrd_streq(word, "repeat"))    return TK_REPEAT;
    if (mrd_streq(word, "import"))    return TK_IMPORT;
    if (mrd_streq(word, "true"))      return TK_TRUE;
    if (mrd_streq(word, "false"))     return TK_FALSE;
    return TK_IDENT;
}

static void mrd_tokenize(const char *src)
{
    int i = 0;
    int line = 1;

    mrd_token_count = 0;

    while (src[i] != '\0' && !mrd_had_error)
    {
        char c = src[i];

        if (c == ' ' || c == '\t' || c == '\r')
        {
            i++;
            continue;
        }

        if (c == '\n')
        {
            mrd_push_token(TK_NEWLINE, 0, 0, line);
            line++;
            i++;
            continue;
        }

        if (c == '#')
        {
            while (src[i] != '\0' && src[i] != '\n')
                i++;
            continue;
        }

        if (c == '"')
        {
            int start_line = line;
            char buf[MRD_MAX_STR];
            int len = 0;
            i++; /* opening quote */

            while (src[i] != '\0' && src[i] != '"')
            {
                if (src[i] == '\n')
                {
                    mrd_error("String is not closed with a quotation mark", start_line);
                    return;
                }

                char ch = src[i];
                if (ch == '\\')
                {
                    i++;
                    if (src[i] == '\0')
                    {
                        mrd_error("String is not closed with a quotation mark", start_line);
                        return;
                    }
                    switch (src[i])
                    {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case 'r': ch = '\r'; break;
                        case '"': ch = '"';  break;
                        case '\\': ch = '\\'; break;
                        default:
                            mrd_error("Unknown escape sequence", start_line);
                            return;
                    }
                }

                if (len < MRD_MAX_STR - 1)
                    buf[len++] = ch;
                i++;
            }

            if (src[i] != '"')
            {
                mrd_error("String is not closed with a quotation mark", start_line);
                return;
            }
            buf[len] = '\0';
            i++; /* closing quote */

            mrd_push_token(TK_STRING, 0, buf, start_line);
            continue;
        }

        if (mrd_is_digit(c))
        {
            int start = i;
            int value = 0;
            while (mrd_is_digit(src[i]))
            {
                value = value * 10 + (src[i] - '0');
                i++;
            }
            (void)start;
            mrd_push_token(TK_NUMBER, value, 0, line);
            continue;
        }

        if (mrd_is_alpha(c))
        {
            char buf[MRD_MAX_STR];
            int len = 0;
            while (mrd_is_alnum(src[i]))
            {
                if (len < MRD_MAX_STR - 1)
                    buf[len++] = src[i];
                i++;
            }
            buf[len] = '\0';
            mrd_push_token(mrd_keyword_kind(buf), 0, buf, line);
            continue;
        }

        switch (c)
        {
            case '+': mrd_push_token(TK_PLUS, 0, 0, line); i++; continue;
            case '-': mrd_push_token(TK_MINUS, 0, 0, line); i++; continue;
            case '*': mrd_push_token(TK_STAR, 0, 0, line); i++; continue;
            case '/': mrd_push_token(TK_SLASH, 0, 0, line); i++; continue;
            case '%': mrd_push_token(TK_PERCENT, 0, 0, line); i++; continue;
            case '(': mrd_push_token(TK_LPAREN, 0, 0, line); i++; continue;
            case ')': mrd_push_token(TK_RPAREN, 0, 0, line); i++; continue;
            case '{': mrd_push_token(TK_LBRACE, 0, 0, line); i++; continue;
            case '}': mrd_push_token(TK_RBRACE, 0, 0, line); i++; continue;
            case '=':
                if (src[i + 1] == '=') { mrd_push_token(TK_EQEQ, 0, 0, line); i += 2; }
                else                   { mrd_push_token(TK_EQUAL, 0, 0, line); i += 1; }
                continue;
            case '!':
                if (src[i + 1] == '=') { mrd_push_token(TK_NEQ, 0, 0, line); i += 2; continue; }
                mrd_error("Unknown character: '!'", line);
                return;
            case '>':
                if (src[i + 1] == '=') { mrd_push_token(TK_GE, 0, 0, line); i += 2; }
                else                   { mrd_push_token(TK_GT, 0, 0, line); i += 1; }
                continue;
            case '<':
                if (src[i + 1] == '=') { mrd_push_token(TK_LE, 0, 0, line); i += 2; }
                else                   { mrd_push_token(TK_LT, 0, 0, line); i += 1; }
                continue;
            default:
                mrd_error("Unknown character", line);
                return;
        }
    }

    mrd_push_token(TK_EOF, 0, 0, line);
}

/* ------------------------------------------------------------------ */
/* Variables — flat table with a scope-depth stack (push/pop marks    */
/* instead of nested hash maps).                                      */
/* ------------------------------------------------------------------ */

typedef enum { MVAL_NUMBER, MVAL_STRING, MVAL_BOOL } MValueType;

typedef struct {
    MValueType type;
    int  number;              /* NUMBER / BOOL (0 or 1)              */
    char text[MRD_MAX_STR];   /* STRING                              */
} MValue;

typedef struct {
    char name[MRD_MAX_STR];
    MValue value;
} MVar;

static MVar mrd_vars[MRD_MAX_VARS];
static int  mrd_var_count = 0;

static int  mrd_scope_marks[MRD_MAX_SCOPES];
static int  mrd_scope_depth = 0;

static void mrd_scope_enter(void)
{
    if (mrd_scope_depth >= MRD_MAX_SCOPES)
    {
        mrd_error("Too many nested blocks", mrd_error_line);
        return;
    }
    mrd_scope_marks[mrd_scope_depth++] = mrd_var_count;
}

static void mrd_scope_leave(void)
{
    if (mrd_scope_depth == 0)
        return;
    mrd_var_count = mrd_scope_marks[--mrd_scope_depth];
}

/* Look up a variable starting from the innermost (most recent) entry
 * so shadowing in nested blocks behaves as expected. */
static MVar *mrd_find_var(const char *name)
{
    for (int i = mrd_var_count - 1; i >= 0; i--)
    {
        if (mrd_streq(mrd_vars[i].name, name))
            return &mrd_vars[i];
    }
    return 0;
}

/* Declares a new variable in the CURRENT scope. Returns 0 if a
 * variable with that name already exists in this scope. */
static MVar *mrd_declare_var(const char *name, int line)
{
    int scope_start = (mrd_scope_depth > 0) ? mrd_scope_marks[mrd_scope_depth - 1] : 0;

    for (int i = scope_start; i < mrd_var_count; i++)
    {
        if (mrd_streq(mrd_vars[i].name, name))
        {
            mrd_error("Variable already exists in this block", line);
            return 0;
        }
    }

    if (mrd_var_count >= MRD_MAX_VARS)
    {
        mrd_error("Too many variables", line);
        return 0;
    }

    MVar *v = &mrd_vars[mrd_var_count++];
    mrd_strcpy_bounded(v->name, name, MRD_MAX_STR);
    return v;
}

/* ------------------------------------------------------------------ */
/* Parser / evaluator state                                            */
/* ------------------------------------------------------------------ */

static int mrd_pos = 0;

static MToken *mrd_peek(void)     { return &mrd_tokens[mrd_pos]; }
static MToken *mrd_previous(void) { return &mrd_tokens[mrd_pos - 1]; }

static int mrd_check(MTokenKind kind)
{
    return mrd_peek()->kind == kind;
}

static MToken *mrd_advance(void)
{
    if (mrd_peek()->kind != TK_EOF)
        mrd_pos++;
    return mrd_previous();
}

static int mrd_match(MTokenKind kind)
{
    if (mrd_check(kind))
    {
        mrd_advance();
        return 1;
    }
    return 0;
}

static MToken *mrd_consume(MTokenKind kind, const char *message)
{
    if (mrd_check(kind))
        return mrd_advance();
    mrd_error(message, mrd_peek()->line);
    return mrd_peek();
}

static void mrd_skip_newlines(void)
{
    while (mrd_match(TK_NEWLINE))
        ;
}

static void mrd_consume_statement_end(void)
{
    if (mrd_check(TK_NEWLINE) || mrd_check(TK_RBRACE) || mrd_check(TK_EOF))
        return;
    mrd_error("Each command must end with a new line", mrd_peek()->line);
}

/* Forward declarations */
static MValue mrd_expression(void);
static void   mrd_statement(void);

static MValue mrd_number(int n)  { MValue v; v.type = MVAL_NUMBER; v.number = n; v.text[0] = '\0'; return v; }
static MValue mrd_boolean(int b) { MValue v; v.type = MVAL_BOOL;   v.number = b; v.text[0] = '\0'; return v; }
static MValue mrd_string(const char *s)
{
    MValue v;
    v.type = MVAL_STRING;
    v.number = 0;
    mrd_strcpy_bounded(v.text, s, MRD_MAX_STR);
    return v;
}

/* -------------------------- expressions --------------------------- */

static MValue mrd_primary(void)
{
    if (mrd_had_error) return mrd_number(0);

    if (mrd_match(TK_NUMBER))
        return mrd_number(mrd_previous()->number);

    if (mrd_match(TK_STRING))
        return mrd_string(mrd_previous()->text);

    if (mrd_match(TK_TRUE))
        return mrd_boolean(1);

    if (mrd_match(TK_FALSE))
        return mrd_boolean(0);

    if (mrd_match(TK_IDENT))
    {
        MVar *v = mrd_find_var(mrd_previous()->text);
        if (!v)
        {
            mrd_error("Variable has not been created yet", mrd_previous()->line);
            return mrd_number(0);
        }
        return v->value;
    }

    if (mrd_match(TK_LPAREN))
    {
        MValue v = mrd_expression();
        mrd_consume(TK_RPAREN, "Expected ')' after the expression");
        return v;
    }

    mrd_error("Expected a number, string, variable or '('", mrd_peek()->line);
    return mrd_number(0);
}

static MValue mrd_unary(void)
{
    if (mrd_had_error) return mrd_number(0);

    if (mrd_match(TK_MINUS))
    {
        int line = mrd_previous()->line;
        MValue right = mrd_unary();
        if (mrd_had_error) return mrd_number(0);
        if (right.type != MVAL_NUMBER)
        {
            mrd_error("Unary minus can only be applied to a number", line);
            return mrd_number(0);
        }
        return mrd_number(-right.number);
    }
    return mrd_primary();
}

static MValue mrd_factor(void)
{
    MValue left = mrd_unary();
    while (!mrd_had_error && (mrd_check(TK_STAR) || mrd_check(TK_SLASH) || mrd_check(TK_PERCENT)))
    {
        MTokenKind op = mrd_advance()->kind;
        int line = mrd_previous()->line;
        MValue right = mrd_unary();
        if (mrd_had_error) return mrd_number(0);
        if (left.type != MVAL_NUMBER || right.type != MVAL_NUMBER)
        {
            mrd_error("This operator only works with numbers", line);
            return mrd_number(0);
        }
        int a = left.number, b = right.number;
        if ((op == TK_SLASH || op == TK_PERCENT) && b == 0)
        {
            mrd_error("Division by zero", line);
            return mrd_number(0);
        }
        if (op == TK_STAR)        left = mrd_number(a * b);
        else if (op == TK_SLASH)  left = mrd_number(a / b);
        else                      left = mrd_number(a % b);
    }
    return left;
}

static MValue mrd_term(void)
{
    MValue left = mrd_factor();
    while (!mrd_had_error && (mrd_check(TK_PLUS) || mrd_check(TK_MINUS)))
    {
        MTokenKind op = mrd_advance()->kind;
        int line = mrd_previous()->line;
        MValue right = mrd_factor();
        if (mrd_had_error) return mrd_number(0);
        if (left.type != MVAL_NUMBER || right.type != MVAL_NUMBER)
        {
            mrd_error("This operator only works with numbers", line);
            return mrd_number(0);
        }
        left = mrd_number(op == TK_PLUS ? left.number + right.number
                                         : left.number - right.number);
    }
    return left;
}

static MValue mrd_comparison(void)
{
    MValue left = mrd_term();
    while (!mrd_had_error &&
           (mrd_check(TK_GT) || mrd_check(TK_GE) || mrd_check(TK_LT) || mrd_check(TK_LE)))
    {
        MTokenKind op = mrd_advance()->kind;
        int line = mrd_previous()->line;
        MValue right = mrd_term();
        if (mrd_had_error) return mrd_number(0);
        if (left.type != MVAL_NUMBER || right.type != MVAL_NUMBER)
        {
            mrd_error("This operator only compares numbers", line);
            return mrd_number(0);
        }
        int a = left.number, b = right.number, r;
        switch (op)
        {
            case TK_GT: r = a > b;  break;
            case TK_GE: r = a >= b; break;
            case TK_LT: r = a < b;  break;
            default:    r = a <= b; break;
        }
        left = mrd_boolean(r);
    }
    return left;
}

static MValue mrd_equality(void)
{
    MValue left = mrd_comparison();
    while (!mrd_had_error && (mrd_check(TK_EQEQ) || mrd_check(TK_NEQ)))
    {
        MTokenKind op = mrd_advance()->kind;
        int line = mrd_previous()->line;
        MValue right = mrd_comparison();
        if (mrd_had_error) return mrd_number(0);
        if (left.type != right.type)
        {
            mrd_error("Values of different types cannot be compared", line);
            return mrd_number(0);
        }
        int eq;
        if (left.type == MVAL_STRING)
            eq = mrd_streq(left.text, right.text);
        else
            eq = (left.number == right.number);
        left = mrd_boolean(op == TK_EQEQ ? eq : !eq);
    }
    return left;
}

static MValue mrd_expression(void)
{
    if (mrd_had_error) return mrd_number(0);
    return mrd_equality();
}
static void mrd_print_value(MValue v)
{
    if (v.type == MVAL_NUMBER)
        shell_print_int(v.number);
    else if (v.type == MVAL_STRING)
        shell_print(v.text);
    else
        shell_print(v.number ? "true" : "false");
}

/* Prints a string literal, expanding {identifier} interpolations. */
static void mrd_say_string(const char *s, int line)
{
    int i = 0;
    while (s[i] != '\0')
    {
        if (s[i] == '{')
        {
            char name[MRD_MAX_STR];
            int len = 0;
            int j = i + 1;
            while (s[j] != '\0' && s[j] != '}' && len < MRD_MAX_STR - 1)
                name[len++] = s[j++];
            name[len] = '\0';

            if (s[j] != '}')
            {
                shell_print("{"); /* not a real interpolation, print literally */
                i++;
                continue;
            }

            MVar *v = mrd_find_var(name);
            if (!v)
            {
                mrd_error("Variable has not been created yet", line);
                return;
            }
            mrd_print_value(v->value);
            i = j + 1;
            continue;
        }

        char buf[2] = { s[i], '\0' };
        shell_print(buf);
        i++;
    }
}

/* Skips one {...} block starting at the current token (which must be
 * TK_LBRACE) without executing it. Leaves mrd_pos right after the
 * matching TK_RBRACE. */
static void mrd_skip_block(void)
{
    mrd_consume(TK_LBRACE, "A block in curly braces is expected");
    if (mrd_had_error) return;

    int depth = 1;
    while (depth > 0 && !mrd_check(TK_EOF))
    {
        if (mrd_check(TK_LBRACE)) depth++;
        else if (mrd_check(TK_RBRACE)) depth--;
        if (depth > 0)
            mrd_advance();
    }
    mrd_consume(TK_RBRACE, "Code block is not closed with '}'");
}

/* Executes the statements inside a {...} block starting at the
 * current token. Leaves mrd_pos right after the matching '}'. */
static void mrd_exec_block(void)
{
    mrd_consume(TK_LBRACE, "A block in curly braces is expected");
    if (mrd_had_error) return;

    mrd_scope_enter();
    mrd_skip_newlines();
    while (!mrd_check(TK_RBRACE) && !mrd_check(TK_EOF) && !mrd_had_error)
    {
        mrd_statement();
        mrd_consume_statement_end();
        mrd_skip_newlines();
    }
    mrd_scope_leave();
    mrd_consume(TK_RBRACE, "Code block is not closed with '}'");
}

static void mrd_statement(void)
{
    if (mrd_had_error) return;

    if (mrd_match(TK_REMEMBER))
    {
        MToken *name_tok = mrd_consume(TK_IDENT, "A variable name is expected after 'remember'");
        if (mrd_had_error) return;
        char name[MRD_MAX_STR];
        mrd_strcpy_bounded(name, name_tok->text, MRD_MAX_STR);

        mrd_consume(TK_EQUAL, "The '=' sign is expected after the variable name");
        if (mrd_had_error) return;

        MValue value = mrd_expression();
        if (mrd_had_error) return;

        MVar *v = mrd_declare_var(name, name_tok->line);
        if (!v) return;
        v->value = value;
        return;
    }

    if (mrd_match(TK_SET))
    {
        MToken *name_tok = mrd_consume(TK_IDENT, "A variable name is expected after 'set'");
        if (mrd_had_error) return;
        char name[MRD_MAX_STR];
        mrd_strcpy_bounded(name, name_tok->text, MRD_MAX_STR);
        int line = name_tok->line;

        mrd_consume(TK_EQUAL, "The '=' sign is expected after the variable name");
        if (mrd_had_error) return;

        MValue value = mrd_expression();
        if (mrd_had_error) return;

        MVar *v = mrd_find_var(name);
        if (!v)
        {
            mrd_error("Variable has not been created yet", line);
            return;
        }
        if (v->value.type != value.type)
        {
            mrd_error("The new value has a different type", line);
            return;
        }
        v->value = value;
        return;
    }

    if (mrd_match(TK_IMPORT))
    {
        mrd_error("'import' is not supported in the OS yet", mrd_previous()->line);
        return;
    }

    if (mrd_match(TK_SAY))
    {
        int line = mrd_previous()->line;

        /* String literal with possible {var} interpolation gets special
         * handling; anything else is evaluated and printed as-is. */
        if (mrd_check(TK_STRING))
        {
            MToken *tok = mrd_advance();
            mrd_say_string(tok->text, line);
            if (mrd_had_error) return;
            shell_print("\n");
            return;
        }

        MValue value = mrd_expression();
        if (mrd_had_error) return;
        mrd_print_value(value);
        shell_print("\n");
        return;
    }

    if (mrd_match(TK_WHEN))
    {
        int line = mrd_previous()->line;
        MValue cond = mrd_expression();
        if (mrd_had_error) return;
        if (cond.type != MVAL_BOOL && cond.type != MVAL_NUMBER)
        {
            mrd_error("The 'when' condition must be boolean or numeric", line);
            return;
        }
        int truthy = cond.number != 0;

        int then_start = mrd_pos;
        if (truthy) mrd_exec_block(); else mrd_skip_block();
        if (mrd_had_error) return;

        int checkpoint = mrd_pos;
        mrd_skip_newlines();

        if (mrd_match(TK_OTHERWISE))
        {
            if (truthy) mrd_skip_block(); else mrd_exec_block();
        }
        else
        {
            mrd_pos = checkpoint; /* no otherwise: newlines belong to statement end */
        }
        (void)then_start;
        return;
    }

    if (mrd_match(TK_REPEAT))
    {
        int line = mrd_previous()->line;
        MValue count_val = mrd_expression();
        if (mrd_had_error) return;
        if (count_val.type != MVAL_NUMBER)
        {
            mrd_error("The repeat count must be a number", line);
            return;
        }

        int block_start = mrd_pos;
        int n = count_val.number;

        if (n <= 0)
        {
            mrd_skip_block();
            return;
        }

        for (int i = 0; i < n && !mrd_had_error; i++)
        {
            mrd_pos = block_start;
            mrd_exec_block();
        }
        return;
    }

    mrd_error("Expected a remember, set, say, when, or repeat command", mrd_peek()->line);
}
int merden_run(const char *source)
{
    mrd_had_error = 0;
    mrd_var_count = 0;
    mrd_scope_depth = 0;
    mrd_pos = 0;

    mrd_tokenize(source);
    if (mrd_had_error)
    {
        mrd_print_error();
        return -1;
    }

    mrd_skip_newlines();
    while (!mrd_check(TK_EOF) && !mrd_had_error)
    {
        mrd_statement();
        if (mrd_had_error) break;
        mrd_consume_statement_end();
        mrd_skip_newlines();
    }

    if (mrd_had_error)
    {
        mrd_print_error();
        return -1;
    }

    return 0;
}