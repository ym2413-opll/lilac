/*
 * syntax.c - Markdown code block syntax highlighter implementation (UTF-8).
 * Pure C, zero external dependencies.
 */
#include "syntax.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

typedef enum SynLang {
    LANG_PLAIN = -1,
    LANG_GENERIC = 0,
    LANG_C,
    LANG_CPP,
    LANG_CSHARP,
    LANG_JAVA,
    LANG_RUST,
    LANG_GO,
    LANG_JS,
    LANG_PYTHON,
    LANG_JSON,
    LANG_HTML,
    LANG_CSS,
    LANG_SQL,
    LANG_SHELL,
    LANG_POWERSHELL,
    LANG_BATCH,
    LANG_YAML,
    LANG_DIFF,
    LANG_MARKDOWN,
    LANG_KOTLIN,
    LANG_LUA
} SynLang;

/* GitHub Light & Dark syntax palette */
static const SynColors g_synLight = {
    0x1F2328, /* text */
    0xCF222E, /* keyword: crimson red */
    0x953800, /* type: brown/orange */
    0x0A3069, /* string: navy blue */
    0x0550AE, /* number: blue */
    0x6E7781, /* comment: muted gray */
    0x8250DF, /* preproc: purple */
    0x0550AE, /* builtin: blue */
    0x8250DF, /* function: purple */
    0x0550AE  /* operator: blue */
};

static const SynColors g_synDark = {
    0xE6EDF3, /* text */
    0xFF7B72, /* keyword: coral red */
    0xFFA657, /* type: orange */
    0xA5D6FF, /* string: sky light blue */
    0x79C0FF, /* number: sky blue */
    0x8B949E, /* comment: muted gray */
    0xD2A8FF, /* preproc: light purple */
    0x79C0FF, /* builtin: sky blue */
    0xD2A8FF, /* function: light purple */
    0x79C0FF  /* operator: sky blue */
};

const SynColors *syntax_get_colors(int dark)
{
    return dark ? &g_synDark : &g_synLight;
}

uint32_t syntax_token_color(const SynColors *colors, SynTokType type)
{
    if (!colors) return 0;
    switch (type) {
    case SYN_KEYWORD:   return colors->keyword;
    case SYN_TYPE:      return colors->type;
    case SYN_STRING:    return colors->string;
    case SYN_NUMBER:    return colors->number;
    case SYN_COMMENT:   return colors->comment;
    case SYN_PREPROC:   return colors->preproc;
    case SYN_BUILTIN:   return colors->builtin;
    case SYN_FUNCTION:  return colors->function;
    case SYN_OPERATOR:  return colors->operator_col;
    case SYN_DEFAULT:
    default:            return colors->text;
    }
}

typedef struct TokBuf {
    SynToken *tokens;
    int count;
    int cap;
} TokBuf;

static void tb_init(TokBuf *tb)
{
    tb->cap = 64;
    tb->count = 0;
    tb->tokens = (SynToken *)malloc((size_t)tb->cap * sizeof(SynToken));
    if (!tb->tokens) tb->cap = 0;
}

static void tb_add(TokBuf *tb, int start, int len, SynTokType type)
{
    if (len <= 0) return;
    if (tb->count >= tb->cap) {
        int newCap;
        SynToken *nt;
        if (tb->cap <= 0) newCap = 64;
        else if (tb->cap > INT_MAX / 2) return;
        else newCap = tb->cap * 2;
        if ((size_t)newCap > SIZE_MAX / sizeof(SynToken)) return;
        nt = (SynToken *)realloc(tb->tokens, (size_t)newCap * sizeof(SynToken));
        if (!nt) return;
        tb->tokens = nt;
        tb->cap = newCap;
    }
    tb->tokens[tb->count].start = start;
    tb->tokens[tb->count].len = len;
    tb->tokens[tb->count].type = type;
    tb->count++;
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static int is_alnum(char c)
{
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int match_kw(const char *text, int len, const char *kw)
{
    int i;
    if (kw[0] != text[0]) return 0;
    for (i = 1; i < len; i++) {
        if (!kw[i] || kw[i] != text[i]) return 0;
    }
    return kw[len] == '\0';
}

static int match_kw_ci(const char *text, int len, const char *kw)
{
    int i;
    for (i = 0; i < len; i++) {
        char c = text[i];
        char k = kw[i];
        if (!k) return 0;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (k >= 'a' && k <= 'z') k -= 32;
        if (k != c) return 0;
    }
    return kw[len] == '\0';
}

static int is_in_list(const char *text, int len, const char *const *list)
{
    int i;
    for (i = 0; list[i] != NULL; i++) {
        if (match_kw(text, len, list[i])) return 1;
    }
    return 0;
}

static int is_in_list_ci(const char *text, int len, const char *const *list)
{
    int i;
    for (i = 0; list[i] != NULL; i++) {
        if (match_kw_ci(text, len, list[i])) return 1;
    }
    return 0;
}

/* ---------------- Keyword Tables ---------------- */

/* C / C++ */
static const char *const g_c_keywords[] = {
    "auto", "break", "case", "catch", "class", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
    "co_yield", "decltype", "default", "delete", "do", "dynamic_cast", "else",
    "enum", "explicit", "export", "extern", "final", "for", "friend", "goto",
    "if", "inline", "mutable", "namespace", "new", "noexcept", "operator",
    "override", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "restrict", "return", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "throw", "try", "typedef",
    "typeid", "typename", "union", "using", "virtual", "volatile", "while", NULL
};
static const char *const g_c_types[] = {
    "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
    "int", "long", "short", "signed", "unsigned", "void", "wchar_t",
    "size_t", "ssize_t", "intptr_t", "uintptr_t", "ptrdiff_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "string", "vector", "map", "set", "pair",
    "tuple", "unique_ptr", "shared_ptr", "weak_ptr", NULL
};
static const char *const g_c_builtins[] = {
    "true", "false", "NULL", "nullptr", "this", NULL
};

/* C# */
static const char *const g_cs_keywords[] = {
    "abstract", "as", "async", "await", "base", "break", "case", "catch",
    "checked", "class", "const", "continue", "default", "delegate", "do",
    "else", "enum", "event", "explicit", "extern", "finally", "fixed",
    "for", "foreach", "get", "goto", "if", "implicit", "in", "init",
    "interface", "internal", "is", "lock", "namespace", "new", "operator",
    "out", "override", "params", "private", "protected", "public", "readonly",
    "record", "ref", "return", "sealed", "set", "sizeof", "stackalloc",
    "static", "struct", "switch", "throw", "try", "typeof", "unchecked",
    "unsafe", "using", "value", "var", "virtual", "void", "volatile",
    "while", "with", "yield", NULL
};
static const char *const g_cs_types[] = {
    "bool", "byte", "sbyte", "char", "decimal", "double", "float",
    "int", "uint", "nint", "nuint", "long", "ulong", "short", "ushort",
    "object", "string", "dynamic", "Task", "List", "Dictionary", "Action", "Func", NULL
};
static const char *const g_cs_builtins[] = {
    "true", "false", "null", "this", NULL
};

/* Java */
static const char *const g_java_keywords[] = {
    "abstract", "assert", "break", "case", "catch", "class", "const",
    "continue", "default", "do", "else", "enum", "extends", "final",
    "finally", "for", "goto", "if", "implements", "import", "instanceof",
    "interface", "native", "new", "package", "permits", "private",
    "protected", "public", "record", "return", "sealed", "static",
    "strictfp", "super", "switch", "synchronized", "throw", "throws",
    "transient", "try", "var", "volatile", "while", NULL
};
static const char *const g_java_types[] = {
    "boolean", "byte", "char", "double", "float", "int", "long", "short",
    "void", "String", "Object", "Integer", "Boolean", "Long", "Double",
    "Float", "List", "Map", "Set", "ArrayList", "HashMap", NULL
};
static const char *const g_java_builtins[] = {
    "true", "false", "null", "this", "super", NULL
};

/* Rust */
static const char *const g_rust_keywords[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn",
    "else", "enum", "extern", "fn", "for", "if", "impl", "in", "let",
    "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
    "static", "struct", "super", "trait", "type", "unsafe", "use",
    "where", "while", NULL
};
static const char *const g_rust_types[] = {
    "bool", "char", "i8", "i16", "i32", "i64", "i128", "isize",
    "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64",
    "str", "String", "Vec", "Option", "Result", "Box", "Rc", "Arc", NULL
};
static const char *const g_rust_builtins[] = {
    "true", "false", "self", "Self", "Some", "None", "Ok", "Err", NULL
};

/* Go */
static const char *const g_go_keywords[] = {
    "break", "case", "chan", "const", "continue", "default", "defer",
    "else", "fallthrough", "for", "func", "go", "goto", "if", "import",
    "interface", "map", "package", "range", "return", "select", "struct",
    "switch", "type", "var", NULL
};
static const char *const g_go_types[] = {
    "bool", "byte", "complex64", "complex128", "error", "float32",
    "float64", "int", "int8", "int16", "int32", "int64", "rune",
    "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", NULL
};
static const char *const g_go_builtins[] = {
    "true", "false", "nil", "iota", "append", "cap", "close", "complex",
    "copy", "delete", "imag", "len", "make", "new", "panic", "print",
    "println", "real", "recover", NULL
};

/* JS / TS */
static const char *const g_js_keywords[] = {
    "abstract", "as", "async", "await", "break", "case", "catch", "class",
    "const", "continue", "debugger", "declare", "default", "delete", "do",
    "else", "enum", "export", "extends", "finally", "for", "from",
    "function", "get", "if", "implements", "import", "in", "instanceof",
    "interface", "is", "keyof", "let", "namespace", "new", "of", "package",
    "private", "protected", "public", "readonly", "return", "set", "static",
    "super", "switch", "throw", "try", "type", "typeof", "var", "void",
    "while", "with", "yield", NULL
};
static const char *const g_js_types[] = {
    "any", "bigint", "boolean", "never", "number", "object", "string",
    "symbol", "unknown", "void", "Array", "Promise", "Record", "Function",
    "Map", "Set", NULL
};
static const char *const g_js_builtins[] = {
    "true", "false", "null", "undefined", "NaN", "Infinity", "this",
    "window", "document", "console", "global", "globalThis", NULL
};

/* Python */
static const char *const g_py_keywords[] = {
    "and", "as", "assert", "async", "await", "break", "case", "class",
    "continue", "def", "del", "elif", "else", "except", "finally",
    "for", "from", "global", "if", "import", "in", "is", "lambda",
    "match", "nonlocal", "not", "or", "pass", "raise", "return",
    "try", "while", "with", "yield", NULL
};
static const char *const g_py_types[] = {
    "bool", "bytearray", "bytes", "complex", "dict", "float", "frozenset",
    "int", "list", "object", "range", "set", "str", "tuple", NULL
};
static const char *const g_py_builtins[] = {
    "True", "False", "None", "self", "cls", "print", "len", "open",
    "range", "enumerate", "zip", "map", "filter", "isinstance", "issubclass", "type", NULL
};

/* SQL */
static const char *const g_sql_keywords[] = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "UPDATE", "DELETE",
    "CREATE", "TABLE", "DROP", "ALTER", "INDEX", "VIEW", "JOIN",
    "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "ON",
    "GROUP", "BY", "ORDER", "HAVING", "LIMIT", "OFFSET", "AS",
    "AND", "OR", "NOT", "IN", "IS", "NULL", "LIKE", "UNION",
    "ALL", "DISTINCT", "BETWEEN", "EXISTS", "CASE", "WHEN", "THEN",
    "ELSE", "END", "SET", "VALUES", "PRIMARY", "KEY", "FOREIGN",
    "REFERENCES", "DEFAULT", "CHECK", "UNIQUE", "CONSTRAINT", "DATABASE",
    "SCHEMA", "USE", "DESC", "ASC", "WITH", "RECURSIVE", NULL
};
static const char *const g_sql_types[] = {
    "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT", "VARCHAR", "CHAR",
    "TEXT", "BOOLEAN", "DATE", "DATETIME", "TIMESTAMP", "TIME",
    "DECIMAL", "NUMERIC", "FLOAT", "DOUBLE", "REAL", "BLOB", NULL
};
static const char *const g_sql_builtins[] = {
    "TRUE", "FALSE", "NULL", NULL
};

/* Shell / Bash */
static const char *const g_sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "in", "do", "done",
    "while", "until", "case", "esac", "function", "return", "exit",
    "echo", "export", "local", "alias", "read", "cd", "test", "set",
    "unset", "shift", "source", "eval", NULL
};
static const char *const g_sh_builtins[] = {
    "true", "false", NULL
};

/* PowerShell */
static const char *const g_ps_keywords[] = {
    "if", "else", "elseif", "for", "foreach", "in", "while", "do",
    "until", "switch", "break", "continue", "return", "function",
    "filter", "param", "try", "catch", "finally", "throw", "trap",
    "exit", "class", "using", NULL
};
static const char *const g_ps_builtins[] = {
    "$true", "$false", "$null", NULL
};

/* Batch */
static const char *const g_bat_keywords[] = {
    "echo", "set", "if", "else", "goto", "call", "exit", "for",
    "in", "do", "shift", "pause", "rem", "cd", "dir", "copy",
    "del", "mkdir", "rmdir", "not", "exist", "errorlevel", NULL
};

/* Kotlin */
static const char *const g_kt_keywords[] = {
    "abstract", "actual", "annotation", "as", "break", "by", "catch", "class",
    "companion", "const", "constructor", "continue", "crossinline", "data",
    "delegate", "do", "dynamic", "else", "enum", "expect", "external", "final",
    "finally", "for", "fun", "get", "if", "import", "in", "infix", "init",
    "inline", "inner", "interface", "internal", "is", "lateinit", "noinline",
    "object", "open", "operator", "out", "override", "package", "private",
    "protected", "public", "reified", "return", "sealed", "set", "suspend",
    "tailrec", "throw", "try", "typealias", "val", "var", "vararg", "value",
    "when", "where", "while", "yield", NULL
};
static const char *const g_kt_types[] = {
    "Any", "Array", "Boolean", "Byte", "Char", "Double", "Float", "Int",
    "Long", "Nothing", "Number", "Short", "String", "UByte", "UInt", "ULong",
    "UShort", "Unit", "List", "Map", "Set", "MutableList", "MutableMap",
    "MutableSet", "Sequence", "Pair", "Triple", "ArrayList", "HashMap", NULL
};
static const char *const g_kt_builtins[] = {
    "true", "false", "null", "this", "super", "it", "field", NULL
};

/* Lua */
static const char *const g_lua_keywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while", NULL
};
static const char *const g_lua_builtins[] = {
    "nil", "true", "false", "_G", "_VERSION", "self",
    "assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs",
    "load", "loadfile", "next", "pairs", "pcall", "print", "rawequal",
    "rawget", "rawlen", "rawset", "require", "select", "setmetatable",
    "tonumber", "tostring", "type", "warn", "xpcall", NULL
};
static const char *const g_lua_types[] = {
    "string", "table", "math", "io", "os", "debug", "coroutine", "utf8", "package", NULL
};

/* Generic / Fallback */
static const char *const g_gen_keywords[] = {
    "if", "else", "elif", "for", "while", "do", "loop", "return",
    "function", "func", "fn", "def", "class", "struct", "interface",
    "import", "export", "from", "var", "let", "const", "switch",
    "case", "break", "continue", "try", "catch", "finally", "throw",
    "yield", "async", "await", "package", "public", "private", "protected", NULL
};
static const char *const g_gen_builtins[] = {
    "true", "false", "null", "nil", "undefined", "None", "this", "self", NULL
};

static SynLang detect_lang(const char *lang)
{
    char buf[32];
    int len = 0;
    if (!lang) return LANG_GENERIC;
    while (*lang == ' ' || *lang == '\t') lang++;
    if (!*lang) return LANG_GENERIC;

    while (*lang && *lang != ' ' && *lang != '\t' && *lang != '{' && *lang != ':' && len < 31) {
        char c = *lang++;
        if (c >= 'A' && c <= 'Z') c += 32;
        buf[len++] = c;
    }
    buf[len] = '\0';
    if (len == 0) return LANG_GENERIC;

    if (strcmp(buf, "text") == 0 || strcmp(buf, "txt") == 0 ||
        strcmp(buf, "plain") == 0 || strcmp(buf, "plaintext") == 0 ||
        strcmp(buf, "none") == 0) {
        return LANG_PLAIN;
    }

    if (strcmp(buf, "c") == 0 || strcmp(buf, "h") == 0) return LANG_C;
    if (strcmp(buf, "cpp") == 0 || strcmp(buf, "c++") == 0 || strcmp(buf, "cc") == 0 ||
        strcmp(buf, "cxx") == 0 || strcmp(buf, "hpp") == 0 || strcmp(buf, "hxx") == 0) return LANG_CPP;
    if (strcmp(buf, "cs") == 0 || strcmp(buf, "csharp") == 0 || strcmp(buf, "c#") == 0) return LANG_CSHARP;
    if (strcmp(buf, "java") == 0) return LANG_JAVA;
    if (strcmp(buf, "rust") == 0 || strcmp(buf, "rs") == 0) return LANG_RUST;
    if (strcmp(buf, "go") == 0 || strcmp(buf, "golang") == 0) return LANG_GO;
    if (strcmp(buf, "js") == 0 || strcmp(buf, "javascript") == 0 ||
        strcmp(buf, "ts") == 0 || strcmp(buf, "typescript") == 0 ||
        strcmp(buf, "jsx") == 0 || strcmp(buf, "tsx") == 0 ||
        strcmp(buf, "mjs") == 0 || strcmp(buf, "cjs") == 0) return LANG_JS;
    if (strcmp(buf, "python") == 0 || strcmp(buf, "py") == 0 || strcmp(buf, "python3") == 0) return LANG_PYTHON;
    if (strcmp(buf, "json") == 0 || strcmp(buf, "jsonc") == 0 || strcmp(buf, "json5") == 0) return LANG_JSON;
    if (strcmp(buf, "html") == 0 || strcmp(buf, "htm") == 0 ||
        strcmp(buf, "xml") == 0 || strcmp(buf, "svg") == 0 || strcmp(buf, "xhtml") == 0) return LANG_HTML;
    if (strcmp(buf, "css") == 0 || strcmp(buf, "scss") == 0 || strcmp(buf, "sass") == 0 || strcmp(buf, "less") == 0) return LANG_CSS;
    if (strcmp(buf, "sql") == 0 || strcmp(buf, "mysql") == 0 || strcmp(buf, "pgsql") == 0 ||
        strcmp(buf, "postgres") == 0 || strcmp(buf, "sqlite") == 0) return LANG_SQL;
    if (strcmp(buf, "sh") == 0 || strcmp(buf, "bash") == 0 || strcmp(buf, "zsh") == 0 || strcmp(buf, "shell") == 0) return LANG_SHELL;
    if (strcmp(buf, "ps1") == 0 || strcmp(buf, "powershell") == 0 || strcmp(buf, "ps") == 0) return LANG_POWERSHELL;
    if (strcmp(buf, "bat") == 0 || strcmp(buf, "batch") == 0 || strcmp(buf, "cmd") == 0) return LANG_BATCH;
    if (strcmp(buf, "yaml") == 0 || strcmp(buf, "yml") == 0) return LANG_YAML;
    if (strcmp(buf, "diff") == 0 || strcmp(buf, "patch") == 0) return LANG_DIFF;
    if (strcmp(buf, "md") == 0 || strcmp(buf, "markdown") == 0) return LANG_MARKDOWN;
    if (strcmp(buf, "kotlin") == 0 || strcmp(buf, "kt") == 0 || strcmp(buf, "kts") == 0) return LANG_KOTLIN;
    if (strcmp(buf, "lua") == 0) return LANG_LUA;

    return LANG_GENERIC;
}

/* ---------------- Language Tokenizers ---------------- */

static void tokenize_c_family(TokBuf *tb, const char *text, int len, SynLang lang)
{
    int i = 0;
    const char *const *kws = g_gen_keywords;
    const char *const *types = NULL;
    const char *const *builtins = g_gen_builtins;

    switch (lang) {
    case LANG_C:
    case LANG_CPP:
        kws = g_c_keywords; types = g_c_types; builtins = g_c_builtins; break;
    case LANG_CSHARP:
        kws = g_cs_keywords; types = g_cs_types; builtins = g_cs_builtins; break;
    case LANG_JAVA:
        kws = g_java_keywords; types = g_java_types; builtins = g_java_builtins; break;
    case LANG_RUST:
        kws = g_rust_keywords; types = g_rust_types; builtins = g_rust_builtins; break;
    case LANG_GO:
        kws = g_go_keywords; types = g_go_types; builtins = g_go_builtins; break;
    case LANG_JS:
        kws = g_js_keywords; types = g_js_types; builtins = g_js_builtins; break;
    case LANG_KOTLIN:
        kws = g_kt_keywords; types = g_kt_types; builtins = g_kt_builtins; break;
    default:
        break;
    }

    while (i < len) {
        char c = text[i];

        /* Whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }

        /* Line comment // */
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            int start = i;
            i += 2;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Block comment /* */
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Preprocessor # */
        if (c == '#') {
            int start = i;
            int isLineStart = 1;
            int k = i - 1;
            while (k >= 0 && text[k] != '\n' && text[k] != '\r') {
                if (text[k] != ' ' && text[k] != '\t') { isLineStart = 0; break; }
                k--;
            }
            if (isLineStart || lang == LANG_C || lang == LANG_CPP || lang == LANG_CSHARP) {
                i++;
                while (i < len && (is_alpha(text[i]) || text[i] == '_')) i++;
                tb_add(tb, start, i - start, SYN_PREPROC);
                continue;
            } else if (lang == LANG_GENERIC) {
                /* Script style # comment in generic */
                i++;
                while (i < len && text[i] != '\n' && text[i] != '\r') i++;
                tb_add(tb, start, i - start, SYN_COMMENT);
                continue;
            }
        }

        /* Annotations / Decorators: @name */
        if (c == '@' && (lang == LANG_JAVA || lang == LANG_CSHARP || lang == LANG_JS || lang == LANG_KOTLIN || lang == LANG_GENERIC)) {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == ':')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        /* Triple quotes """ (Kotlin, Java text blocks) */
        if (c == '"' && i + 2 < len && text[i + 1] == '"' && text[i + 2] == '"' &&
            (lang == LANG_KOTLIN || lang == LANG_JAVA || lang == LANG_GENERIC)) {
            int start = i;
            i += 3;
            while (i + 2 < len && !(text[i] == '"' && text[i + 1] == '"' && text[i + 2] == '"')) {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Strings: double quotes "..." */
        if (c == '"') {
            int start = i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len) i++;
                else if (text[i] == '\n' && lang != LANG_JS && lang != LANG_GO) break;
                i++;
            }
            if (i < len && text[i] == '"') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Strings: single quotes '...' */
        if (c == '\'') {
            int start = i++;
            while (i < len && text[i] != '\'') {
                if (text[i] == '\\' && i + 1 < len) i++;
                else if (text[i] == '\n') break;
                i++;
            }
            if (i < len && text[i] == '\'') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* JS/TS template strings or Go raw strings: `...` */
        if (c == '`' && (lang == LANG_JS || lang == LANG_GO || lang == LANG_GENERIC)) {
            int start = i++;
            while (i < len && text[i] != '`') {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == '`') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == '0' && i + 1 < len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == '_')) i++;
            } else if (c == '0' && i + 1 < len && (text[i + 1] == 'b' || text[i + 1] == 'B')) {
                i += 2;
                while (i < len && (text[i] == '0' || text[i] == '1' || text[i] == '_')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == '_' || text[i] == '.')) i++;
                if (i < len && (text[i] == 'e' || text[i] == 'E')) {
                    i++;
                    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
                    while (i < len && is_digit(text[i])) i++;
                }
            }
            while (i < len && (is_alpha(text[i]) || text[i] == '%')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        /* Identifiers: Keywords, Types, Builtins, Functions */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            int p;
            while (i < len && is_alnum(text[i])) i++;
            tokLen = i - start;

            if (is_in_list(text + start, tokLen, kws)) {
                tb_add(tb, start, tokLen, SYN_KEYWORD);
                continue;
            }
            if (types && is_in_list(text + start, tokLen, types)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }
            if (builtins && is_in_list(text + start, tokLen, builtins)) {
                tb_add(tb, start, tokLen, SYN_BUILTIN);
                continue;
            }

            /* Check if followed by ( -> function call */
            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p < len && text[p] == '(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }

            /* Generic capitalize convention: TypeName */
            if (text[start] >= 'A' && text[start] <= 'Z' && (lang == LANG_RUST || lang == LANG_JAVA || lang == LANG_CSHARP || lang == LANG_KOTLIN)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }

            continue;
        }

        /* Operators */
        if (c && strchr("+-*/%=&|!^~?:<>", c)) {
            int start = i++;
            while (i < len && text[i] && strchr("+-*/%=&|!^~?:<>", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

static void tokenize_python(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }

        /* Comment # */
        if (c == '#') {
            int start = i++;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Triple quotes """ or ''' */
        if ((c == '"' || c == '\'') && i + 2 < len && text[i + 1] == c && text[i + 2] == c) {
            int start = i;
            i += 3;
            while (i + 2 < len && !(text[i] == c && text[i + 1] == c && text[i + 2] == c)) {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* String literals "..." or '...' */
        if (c == '"' || c == '\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == '\\' && i + 1 < len) i++;
                else if (text[i] == '\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Decorator: @name */
        if (c == '@') {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == '.')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == '0' && i + 1 < len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == '_')) i++;
            } else if (c == '0' && i + 1 < len && (text[i + 1] == 'b' || text[i + 1] == 'B')) {
                i += 2;
                while (i < len && (text[i] == '0' || text[i] == '1' || text[i] == '_')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == '_' || text[i] == '.')) i++;
                if (i < len && (text[i] == 'e' || text[i] == 'E')) {
                    i++;
                    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
                    while (i < len && is_digit(text[i])) i++;
                }
            }
            if (i < len && (text[i] == 'j' || text[i] == 'J')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        /* Identifiers */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            int p;
            while (i < len && is_alnum(text[i])) i++;
            tokLen = i - start;

            /* Check prefix for string e.g. f"...", r"...", b"..." */
            if ((tokLen == 1 || tokLen == 2) && i < len && (text[i] == '"' || text[i] == '\'')) {
                char p0 = text[start];
                if (p0 == 'r' || p0 == 'f' || p0 == 'b' || p0 == 'u' || p0 == 'R' || p0 == 'F' || p0 == 'B') {
                    char sc = text[i];
                    if (i + 2 < len && text[i + 1] == sc && text[i + 2] == sc) {
                        i += 3;
                        while (i + 2 < len && !(text[i] == sc && text[i + 1] == sc && text[i + 2] == sc)) {
                            if (text[i] == '\\' && i + 1 < len) i++;
                            i++;
                        }
                        if (i + 2 < len) i += 3; else i = len;
                    } else {
                        i++;
                        while (i < len && text[i] != sc) {
                            if (text[i] == '\\' && i + 1 < len) i++;
                            else if (text[i] == '\n') break;
                            i++;
                        }
                        if (i < len && text[i] == sc) i++;
                    }
                    tb_add(tb, start, i - start, SYN_STRING);
                    continue;
                }
            }

            if (is_in_list(text + start, tokLen, g_py_keywords)) {
                tb_add(tb, start, tokLen, SYN_KEYWORD);
                continue;
            }
            if (is_in_list(text + start, tokLen, g_py_builtins)) {
                tb_add(tb, start, tokLen, SYN_BUILTIN);
                continue;
            }
            if (is_in_list(text + start, tokLen, g_py_types)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }

            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p < len && text[p] == '(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }

            continue;
        }

        if (c && strchr("+-*/%=&|!^~?:<>", c)) {
            int start = i++;
            while (i < len && text[i] && strchr("+-*/%=&|!^~?:<>", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

static void tokenize_json(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }

        if (c == '"') {
            int start = i++;
            int isKey = 0;
            int p;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == '"') i++;

            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) p++;
            if (p < len && text[p] == ':') isKey = 1;

            tb_add(tb, start, i - start, isKey ? SYN_KEYWORD : SYN_STRING);
            continue;
        }

        if (is_digit(c) || c == '-') {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == '.' || text[i] == 'e' || text[i] == 'E' || text[i] == '+' || text[i] == '-')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (is_alpha(c)) {
            int start = i;
            while (i < len && is_alnum(text[i])) i++;
            if (match_kw(text + start, i - start, "true") ||
                match_kw(text + start, i - start, "false") ||
                match_kw(text + start, i - start, "null")) {
                tb_add(tb, start, i - start, SYN_BUILTIN);
            }
            continue;
        }

        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_html(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];

        if (c == '<' && i + 3 < len && text[i + 1] == '!' && text[i + 2] == '-' && text[i + 3] == '-') {
            int start = i;
            i += 4;
            while (i + 2 < len && !(text[i] == '-' && text[i + 1] == '-' && text[i + 2] == '>')) i++;
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == '<' && i + 1 < len && (text[i + 1] == '!' || text[i + 1] == '?')) {
            int start = i;
            while (i < len && text[i] != '>') i++;
            if (i < len) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        if (c == '<') {
            int startTag = i++;
            tb_add(tb, startTag, 1, SYN_OPERATOR);
            if (i < len && text[i] == '/') {
                tb_add(tb, i, 1, SYN_OPERATOR);
                i++;
            }
            /* Tag name */
            if (i < len && is_alpha(text[i])) {
                int nameStart = i;
                while (i < len && (is_alnum(text[i]) || text[i] == '-' || text[i] == ':')) i++;
                tb_add(tb, nameStart, i - nameStart, SYN_KEYWORD);
            }

            /* Tag attributes until > */
            while (i < len && text[i] != '>') {
                char ac = text[i];
                if (ac == ' ' || ac == '\t' || ac == '\r' || ac == '\n') { i++; continue; }
                if (ac == '/' && i + 1 < len && text[i + 1] == '>') {
                    tb_add(tb, i, 2, SYN_OPERATOR);
                    i += 2;
                    break;
                }
                if (ac == '=') {
                    tb_add(tb, i, 1, SYN_OPERATOR);
                    i++;
                    continue;
                }
                if (ac == '"' || ac == '\'') {
                    int sStart = i++;
                    while (i < len && text[i] != ac) i++;
                    if (i < len && text[i] == ac) i++;
                    tb_add(tb, sStart, i - sStart, SYN_STRING);
                    continue;
                }
                if (is_alpha(ac)) {
                    int aStart = i;
                    while (i < len && (is_alnum(text[i]) || text[i] == '-' || text[i] == ':')) i++;
                    tb_add(tb, aStart, i - aStart, SYN_TYPE);
                    continue;
                }
                i++;
            }
            if (i < len && text[i] == '>') {
                tb_add(tb, i, 1, SYN_OPERATOR);
                i++;
            }
            continue;
        }

        if (c == '&') {
            int start = i++;
            while (i < len && is_alnum(text[i])) i++;
            if (i < len && text[i] == ';') i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        i++;
    }
}

static void tokenize_css(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }

        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == '@') {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == '-')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        if (c == '"' || c == '\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        if (c == '#' && i + 1 < len && is_hex(text[i + 1])) {
            int start = i++;
            while (i < len && is_hex(text[i])) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (is_digit(c)) {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == '.')) i++;
            while (i < len && (is_alpha(text[i]) || text[i] == '%')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (is_alpha(c) || c == '-') {
            int start = i;
            int p;
            while (i < len && (is_alnum(text[i]) || text[i] == '-')) i++;
            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p < len && text[p] == ':') {
                tb_add(tb, start, i - start, SYN_KEYWORD);
            }
            continue;
        }

        if (c == '{' || c == '}' || c == ':' || c == ';' || c == ',' || c == '>') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_sql(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }

        /* Comment -- */
        if (c == '-' && i + 1 < len && text[i + 1] == '-') {
            int start = i;
            i += 2;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Comment /* */
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* String '...' */
        if (c == '\'') {
            int start = i++;
            while (i < len && text[i] != '\'') {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == '\'') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Number */
        if (is_digit(c)) {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == '.')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        /* Word */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            while (i < len && (is_alnum(text[i]) || text[i] == '_')) i++;
            tokLen = i - start;

            if (is_in_list_ci(text + start, tokLen, g_sql_keywords)) {
                tb_add(tb, start, tokLen, SYN_KEYWORD);
                continue;
            }
            if (is_in_list_ci(text + start, tokLen, g_sql_types)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }
            if (is_in_list_ci(text + start, tokLen, g_sql_builtins)) {
                tb_add(tb, start, tokLen, SYN_BUILTIN);
                continue;
            }
            continue;
        }

        if (c && strchr("=<>!+-*/,();", c)) {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_shell(TokBuf *tb, const char *text, int len, SynLang lang)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }

        /* Comment # */
        if (c == '#' && lang != LANG_BATCH) {
            int start = i++;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Batch comment REM or :: */
        if (lang == LANG_BATCH) {
            if (c == ':' && i + 1 < len && text[i + 1] == ':') {
                int start = i;
                while (i < len && text[i] != '\n' && text[i] != '\r') i++;
                tb_add(tb, start, i - start, SYN_COMMENT);
                continue;
            }
        }

        /* Strings "..." and '...' */
        if (c == '"' || c == '\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == '\\' && i + 1 < len) i++;
                else if (text[i] == '\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Variables: $VAR, ${VAR}, %VAR% */
        if (c == '$' || (lang == LANG_BATCH && c == '%')) {
            int start = i++;
            if (i < len && text[i] == '{') {
                while (i < len && text[i] != '}') i++;
                if (i < len && text[i] == '}') i++;
            } else if (c == '%' && lang == LANG_BATCH) {
                while (i < len && text[i] != '%') i++;
                if (i < len && text[i] == '%') i++;
            } else {
                while (i < len && (is_alnum(text[i]) || text[i] == '_')) i++;
            }
            tb_add(tb, start, i - start, SYN_TYPE);
            continue;
        }

        /* Identifiers */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            while (i < len && (is_alnum(text[i]) || text[i] == '_' || text[i] == '-')) i++;
            tokLen = i - start;

            if (lang == LANG_BATCH) {
                if (match_kw_ci(text + start, tokLen, "rem")) {
                    while (i < len && text[i] != '\n' && text[i] != '\r') i++;
                    tb_add(tb, start, i - start, SYN_COMMENT);
                    continue;
                }
                if (is_in_list_ci(text + start, tokLen, g_bat_keywords)) {
                    tb_add(tb, start, tokLen, SYN_KEYWORD);
                    continue;
                }
            } else if (lang == LANG_POWERSHELL) {
                if (is_in_list_ci(text + start, tokLen, g_ps_keywords)) {
                    tb_add(tb, start, tokLen, SYN_KEYWORD);
                    continue;
                }
                if (is_in_list_ci(text + start, tokLen, g_ps_builtins)) {
                    tb_add(tb, start, tokLen, SYN_BUILTIN);
                    continue;
                }
            } else {
                if (is_in_list(text + start, tokLen, g_sh_keywords)) {
                    tb_add(tb, start, tokLen, SYN_KEYWORD);
                    continue;
                }
                if (is_in_list(text + start, tokLen, g_sh_builtins)) {
                    tb_add(tb, start, tokLen, SYN_BUILTIN);
                    continue;
                }
            }
            continue;
        }

        /* Numbers */
        if (is_digit(c)) {
            int start = i++;
            while (i < len && is_digit(text[i])) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (c && strchr("|&;><=(){}[]", c)) {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_yaml(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }

        if (c == '#') {
            int start = i++;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == '"' || c == '\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        if (is_digit(c) || c == '-') {
            if (is_digit(c) || (i + 1 < len && is_digit(text[i + 1]))) {
                int start = i++;
                while (i < len && (is_digit(text[i]) || text[i] == '.' || text[i] == '_')) i++;
                tb_add(tb, start, i - start, SYN_NUMBER);
                continue;
            }
        }

        if (is_alpha(c) || c == '_') {
            int start = i;
            int p;
            while (i < len && (is_alnum(text[i]) || text[i] == '_' || text[i] == '-')) i++;
            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p < len && text[p] == ':') {
                tb_add(tb, start, i - start, SYN_KEYWORD);
                continue;
            }
            if (match_kw(text + start, i - start, "true") || match_kw(text + start, i - start, "false") ||
                match_kw(text + start, i - start, "yes") || match_kw(text + start, i - start, "no") ||
                match_kw(text + start, i - start, "null")) {
                tb_add(tb, start, i - start, SYN_BUILTIN);
                continue;
            }
            continue;
        }

        if (c == ':' || c == '-' || c == '{' || c == '}' || c == '[' || c == ']') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_diff(TokBuf *tb, const char *text, int len)
{
    int lineStart = 0;
    while (lineStart < len) {
        int lineEnd = lineStart;
        char first;
        while (lineEnd < len && text[lineEnd] != '\n' && text[lineEnd] != '\r') lineEnd++;
        first = (lineStart < lineEnd) ? text[lineStart] : 0;

        if (first == '+' && !(lineStart + 2 < lineEnd && text[lineStart + 1] == '+' && text[lineStart + 2] == '+')) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_TYPE); /* Green / Add */
        } else if (first == '-' && !(lineStart + 2 < lineEnd && text[lineStart + 1] == '-' && text[lineStart + 2] == '-')) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_KEYWORD); /* Red / Remove */
        } else if (first == '@' && lineStart + 1 < lineEnd && text[lineStart + 1] == '@') {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_PREPROC); /* Chunk header */
        } else if (lineStart + 4 <= lineEnd && (strncmp(text + lineStart, "diff", 4) == 0 || (lineStart + 5 <= lineEnd && strncmp(text + lineStart, "index", 5) == 0))) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_COMMENT);
        }

        if (lineEnd < len && text[lineEnd] == '\r') lineEnd++;
        if (lineEnd < len && text[lineEnd] == '\n') lineEnd++;
        lineStart = lineEnd;
    }
}

static void tokenize_markdown(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == '#') {
            int start = i++;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_KEYWORD);
            continue;
        }
        if (c == '>') {
            int start = i++;
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }
        if (c == '`') {
            int start = i++;
            while (i < len && text[i] != '`') i++;
            if (i < len && text[i] == '`') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }
        i++;
    }
}

static void tokenize_lua(TokBuf *tb, const char *text, int len)
{
    int i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }

        /* Comment -- */
        if (c == '-' && i + 1 < len && text[i + 1] == '-') {
            int start = i;
            i += 2;
            /* Check if multi-line comment: --[=*...[ */
            if (i < len && text[i] == '[') {
                int numEq = 0;
                int p = i + 1;
                while (p < len && text[p] == '=') { numEq++; p++; }
                if (p < len && text[p] == '[') {
                    i = p + 1;
                    while (i < len) {
                        if (text[i] == ']') {
                            int closeEq = 0;
                            int q = i + 1;
                            while (q < len && text[q] == '=') { closeEq++; q++; }
                            if (q < len && text[q] == ']' && closeEq == numEq) {
                                i = q + 1;
                                break;
                            }
                        }
                        i++;
                    }
                    tb_add(tb, start, i - start, SYN_COMMENT);
                    continue;
                }
            }
            /* Single-line comment */
            while (i < len && text[i] != '\n' && text[i] != '\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* String: Multi-line literal [=*...[ */
        if (c == '[') {
            int numEq = 0;
            int p = i + 1;
            while (p < len && text[p] == '=') { numEq++; p++; }
            if (p < len && text[p] == '[') {
                int start = i;
                i = p + 1;
                while (i < len) {
                    if (text[i] == ']') {
                        int closeEq = 0;
                        int q = i + 1;
                        while (q < len && text[q] == '=') { closeEq++; q++; }
                        if (q < len && text[q] == ']' && closeEq == numEq) {
                            i = q + 1;
                            break;
                        }
                    }
                    i++;
                }
                tb_add(tb, start, i - start, SYN_STRING);
                continue;
            }
        }

        /* String: "..." or '...' */
        if (c == '"' || c == '\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == '\\' && i + 1 < len) i++;
                else if (text[i] == '\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == '0' && i + 1 < len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == '.' || text[i] == 'p' || text[i] == 'P' || text[i] == '+' || text[i] == '-')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == '.')) i++;
                if (i < len && (text[i] == 'e' || text[i] == 'E')) {
                    i++;
                    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
                    while (i < len && is_digit(text[i])) i++;
                }
            }
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        /* Words / Identifiers */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            int p;
            while (i < len && is_alnum(text[i])) i++;
            tokLen = i - start;

            if (is_in_list(text + start, tokLen, g_lua_keywords)) {
                tb_add(tb, start, tokLen, SYN_KEYWORD);
                continue;
            }
            if (is_in_list(text + start, tokLen, g_lua_builtins)) {
                tb_add(tb, start, tokLen, SYN_BUILTIN);
                continue;
            }
            if (is_in_list(text + start, tokLen, g_lua_types)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }

            p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p < len && text[p] == '(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }
            continue;
        }

        /* Operators */
        if (c == '.' && i + 1 < len && text[i + 1] == '.') {
            int start = i;
            while (i < len && text[i] == '.') i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        if (c && strchr("+-*/%^#&~|<>=:", c)) {
            int start = i++;
            while (i < len && text[i] && strchr("=<>~/", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

/* ---------------- Public API ---------------- */

SynToken *syntax_highlight(const char *lang, const char *text, int textlen, int *outCount)
{
    SynLang l;
    TokBuf tb;

    if (!outCount) return NULL;
    *outCount = 0;
    if (!text || textlen <= 0) return NULL;

    l = detect_lang(lang);
    if (l == LANG_PLAIN) {
        return NULL; /* explicit plain text */
    }

    tb_init(&tb);

    switch (l) {
    case LANG_LUA:
        tokenize_lua(&tb, text, textlen);
        break;
    case LANG_PYTHON:
        tokenize_python(&tb, text, textlen);
        break;
    case LANG_JSON:
        tokenize_json(&tb, text, textlen);
        break;
    case LANG_HTML:
        tokenize_html(&tb, text, textlen);
        break;
    case LANG_CSS:
        tokenize_css(&tb, text, textlen);
        break;
    case LANG_SQL:
        tokenize_sql(&tb, text, textlen);
        break;
    case LANG_SHELL:
    case LANG_POWERSHELL:
    case LANG_BATCH:
        tokenize_shell(&tb, text, textlen, l);
        break;
    case LANG_YAML:
        tokenize_yaml(&tb, text, textlen);
        break;
    case LANG_DIFF:
        tokenize_diff(&tb, text, textlen);
        break;
    case LANG_MARKDOWN:
        tokenize_markdown(&tb, text, textlen);
        break;
    case LANG_KOTLIN:
    case LANG_C:
    case LANG_CPP:
    case LANG_CSHARP:
    case LANG_JAVA:
    case LANG_RUST:
    case LANG_GO:
    case LANG_JS:
    case LANG_GENERIC:
    default:
        tokenize_c_family(&tb, text, textlen, l);
        break;
    }

    *outCount = tb.count;
    return tb.tokens;
}
