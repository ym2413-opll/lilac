/*
 * syntax.c - Markdown code block syntax highlighter implementation.
 * Pure C, zero external dependencies.
 */
#include "syntax.h"
#include <stdlib.h>
#include <string.h>

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
    tb->tokens = (SynToken *)malloc(tb->cap * sizeof(SynToken));
}

static void tb_add(TokBuf *tb, int start, int len, SynTokType type)
{
    if (len <= 0) return;
    if (tb->count == tb->cap) {
        tb->cap *= 2;
        tb->tokens = (SynToken *)realloc(tb->tokens, tb->cap * sizeof(SynToken));
    }
    tb->tokens[tb->count].start = start;
    tb->tokens[tb->count].len = len;
    tb->tokens[tb->count].type = type;
    tb->count++;
}

static int is_alpha(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'$';
}

static int is_alnum(wchar_t c)
{
    return is_alpha(c) || (c >= L'0' && c <= L'9');
}

static int is_digit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

static int is_hex(wchar_t c)
{
    return is_digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

static int match_kw(const wchar_t *text, int len, const char *kw)
{
    int i;
    if ((wchar_t)kw[0] != text[0]) return 0;
    for (i = 1; i < len; i++) {
        if (!kw[i] || (wchar_t)kw[i] != text[i]) return 0;
    }
    return kw[len] == '\0';
}

static int match_kw_ci(const wchar_t *text, int len, const char *kw)
{
    int i;
    for (i = 0; i < len; i++) {
        wchar_t c = text[i];
        char k = kw[i];
        if (!k) return 0;
        if (c >= L'a' && c <= L'z') c -= 32;
        if (k >= 'a' && k <= 'z') k -= 32;
        if ((wchar_t)k != c) return 0;
    }
    return kw[len] == '\0';
}

static int is_in_list(const wchar_t *text, int len, const char *const *list)
{
    int i;
    for (i = 0; list[i] != NULL; i++) {
        if (match_kw(text, len, list[i])) return 1;
    }
    return 0;
}

static int is_in_list_ci(const wchar_t *text, int len, const char *const *list)
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

static SynLang detect_lang(const wchar_t *lang)
{
    wchar_t buf[32];
    int len = 0;
    if (!lang) return LANG_GENERIC;
    while (*lang == L' ' || *lang == L'\t') lang++;
    if (!*lang) return LANG_GENERIC;

    while (*lang && *lang != L' ' && *lang != L'\t' && *lang != L'{' && *lang != L':' && len < 31) {
        wchar_t c = *lang++;
        if (c >= L'A' && c <= L'Z') c += 32;
        buf[len++] = c;
    }
    buf[len] = L'\0';
    if (len == 0) return LANG_GENERIC;

    if (wcscmp(buf, L"text") == 0 || wcscmp(buf, L"txt") == 0 ||
        wcscmp(buf, L"plain") == 0 || wcscmp(buf, L"plaintext") == 0 ||
        wcscmp(buf, L"none") == 0) {
        return LANG_PLAIN;
    }

    if (wcscmp(buf, L"c") == 0 || wcscmp(buf, L"h") == 0) return LANG_C;
    if (wcscmp(buf, L"cpp") == 0 || wcscmp(buf, L"c++") == 0 || wcscmp(buf, L"cc") == 0 ||
        wcscmp(buf, L"cxx") == 0 || wcscmp(buf, L"hpp") == 0 || wcscmp(buf, L"hxx") == 0) return LANG_CPP;
    if (wcscmp(buf, L"cs") == 0 || wcscmp(buf, L"csharp") == 0 || wcscmp(buf, L"c#") == 0) return LANG_CSHARP;
    if (wcscmp(buf, L"java") == 0) return LANG_JAVA;
    if (wcscmp(buf, L"rust") == 0 || wcscmp(buf, L"rs") == 0) return LANG_RUST;
    if (wcscmp(buf, L"go") == 0 || wcscmp(buf, L"golang") == 0) return LANG_GO;
    if (wcscmp(buf, L"js") == 0 || wcscmp(buf, L"javascript") == 0 ||
        wcscmp(buf, L"ts") == 0 || wcscmp(buf, L"typescript") == 0 ||
        wcscmp(buf, L"jsx") == 0 || wcscmp(buf, L"tsx") == 0 ||
        wcscmp(buf, L"mjs") == 0 || wcscmp(buf, L"cjs") == 0) return LANG_JS;
    if (wcscmp(buf, L"python") == 0 || wcscmp(buf, L"py") == 0 || wcscmp(buf, L"python3") == 0) return LANG_PYTHON;
    if (wcscmp(buf, L"json") == 0 || wcscmp(buf, L"jsonc") == 0 || wcscmp(buf, L"json5") == 0) return LANG_JSON;
    if (wcscmp(buf, L"html") == 0 || wcscmp(buf, L"htm") == 0 ||
        wcscmp(buf, L"xml") == 0 || wcscmp(buf, L"svg") == 0 || wcscmp(buf, L"xhtml") == 0) return LANG_HTML;
    if (wcscmp(buf, L"css") == 0 || wcscmp(buf, L"scss") == 0 || wcscmp(buf, L"sass") == 0 || wcscmp(buf, L"less") == 0) return LANG_CSS;
    if (wcscmp(buf, L"sql") == 0 || wcscmp(buf, L"mysql") == 0 || wcscmp(buf, L"pgsql") == 0 ||
        wcscmp(buf, L"postgres") == 0 || wcscmp(buf, L"sqlite") == 0) return LANG_SQL;
    if (wcscmp(buf, L"sh") == 0 || wcscmp(buf, L"bash") == 0 || wcscmp(buf, L"zsh") == 0 || wcscmp(buf, L"shell") == 0) return LANG_SHELL;
    if (wcscmp(buf, L"ps1") == 0 || wcscmp(buf, L"powershell") == 0 || wcscmp(buf, L"ps") == 0) return LANG_POWERSHELL;
    if (wcscmp(buf, L"bat") == 0 || wcscmp(buf, L"batch") == 0 || wcscmp(buf, L"cmd") == 0) return LANG_BATCH;
    if (wcscmp(buf, L"yaml") == 0 || wcscmp(buf, L"yml") == 0) return LANG_YAML;
    if (wcscmp(buf, L"diff") == 0 || wcscmp(buf, L"patch") == 0) return LANG_DIFF;
    if (wcscmp(buf, L"md") == 0 || wcscmp(buf, L"markdown") == 0) return LANG_MARKDOWN;
    if (wcscmp(buf, L"kotlin") == 0 || wcscmp(buf, L"kt") == 0 || wcscmp(buf, L"kts") == 0) return LANG_KOTLIN;
    if (wcscmp(buf, L"lua") == 0) return LANG_LUA;

    return LANG_GENERIC;
}

/* ---------------- Language Tokenizers ---------------- */

static void tokenize_c_family(TokBuf *tb, const wchar_t *text, int len, SynLang lang)
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
        wchar_t c = text[i];

        /* Whitespace */
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            i++;
            continue;
        }

        /* Line comment // */
        if (c == L'/' && i + 1 < len && text[i + 1] == L'/') {
            int start = i;
            i += 2;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Block comment /* */
        if (c == L'/' && i + 1 < len && text[i + 1] == L'*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == L'*' && text[i + 1] == L'/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Preprocessor # */
        if (c == L'#') {
            int start = i;
            int isLineStart = 1;
            int k = i - 1;
            while (k >= 0 && text[k] != L'\n' && text[k] != L'\r') {
                if (text[k] != L' ' && text[k] != L'\t') { isLineStart = 0; break; }
                k--;
            }
            if (isLineStart || lang == LANG_C || lang == LANG_CPP || lang == LANG_CSHARP) {
                i++;
                while (i < len && (is_alpha(text[i]) || text[i] == L'_')) i++;
                tb_add(tb, start, i - start, SYN_PREPROC);
                continue;
            } else if (lang == LANG_GENERIC) {
                /* Script style # comment in generic */
                i++;
                while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
                tb_add(tb, start, i - start, SYN_COMMENT);
                continue;
            }
        }

        /* Annotations / Decorators: @name */
        if (c == L'@' && (lang == LANG_JAVA || lang == LANG_CSHARP || lang == LANG_JS || lang == LANG_KOTLIN || lang == LANG_GENERIC)) {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == L':')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        /* Triple quotes """ (Kotlin, Java text blocks) */
        if (c == L'"' && i + 2 < len && text[i + 1] == L'"' && text[i + 2] == L'"' &&
            (lang == LANG_KOTLIN || lang == LANG_JAVA || lang == LANG_GENERIC)) {
            int start = i;
            i += 3;
            while (i + 2 < len && !(text[i] == L'"' && text[i + 1] == L'"' && text[i + 2] == L'"')) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Strings: double quotes "..." */
        if (c == L'"') {
            int start = i++;
            while (i < len && text[i] != L'"') {
                if (text[i] == L'\\' && i + 1 < len) i++;
                else if (text[i] == L'\n' && lang != LANG_JS && lang != LANG_GO) break;
                i++;
            }
            if (i < len && text[i] == L'"') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Strings: single quotes '...' */
        if (c == L'\'') {
            int start = i++;
            while (i < len && text[i] != L'\'') {
                if (text[i] == L'\\' && i + 1 < len) i++;
                else if (text[i] == L'\n') break;
                i++;
            }
            if (i < len && text[i] == L'\'') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* JS/TS template strings or Go raw strings: `...` */
        if (c == L'`' && (lang == LANG_JS || lang == LANG_GO || lang == LANG_GENERIC)) {
            int start = i++;
            while (i < len && text[i] != L'`') {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == L'`') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == L'.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == L'0' && i + 1 < len && (text[i + 1] == L'x' || text[i + 1] == L'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == L'_')) i++;
            } else if (c == L'0' && i + 1 < len && (text[i + 1] == L'b' || text[i + 1] == L'B')) {
                i += 2;
                while (i < len && (text[i] == L'0' || text[i] == L'1' || text[i] == L'_')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == L'_' || text[i] == L'.')) i++;
                if (i < len && (text[i] == L'e' || text[i] == L'E')) {
                    i++;
                    if (i < len && (text[i] == L'+' || text[i] == L'-')) i++;
                    while (i < len && is_digit(text[i])) i++;
                }
            }
            while (i < len && (is_alpha(text[i]) || text[i] == L'%')) i++;
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
            while (p < len && (text[p] == L' ' || text[p] == L'\t')) p++;
            if (p < len && text[p] == L'(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }

            /* Generic capitalize convention: TypeName */
            if (text[start] >= L'A' && text[start] <= L'Z' && (lang == LANG_RUST || lang == LANG_JAVA || lang == LANG_CSHARP || lang == LANG_KOTLIN)) {
                tb_add(tb, start, tokLen, SYN_TYPE);
                continue;
            }

            continue;
        }

        /* Operators */
        if (wcschr(L"+-*/%=&|!^~?:<>", c)) {
            int start = i++;
            while (i < len && wcschr(L"+-*/%=&|!^~?:<>", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

static void tokenize_python(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];

        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            i++;
            continue;
        }

        /* Comment # */
        if (c == L'#') {
            int start = i++;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Triple quotes """ or ''' */
        if ((c == L'"' || c == L'\'') && i + 2 < len && text[i + 1] == c && text[i + 2] == c) {
            int start = i;
            i += 3;
            while (i + 2 < len && !(text[i] == c && text[i + 1] == c && text[i + 2] == c)) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* String literals "..." or '...' */
        if (c == L'"' || c == L'\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                else if (text[i] == L'\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Decorator: @name */
        if (c == L'@') {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == L'.')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == L'.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == L'0' && i + 1 < len && (text[i + 1] == L'x' || text[i + 1] == L'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == L'_')) i++;
            } else if (c == L'0' && i + 1 < len && (text[i + 1] == L'b' || text[i + 1] == L'B')) {
                i += 2;
                while (i < len && (text[i] == L'0' || text[i] == L'1' || text[i] == L'_')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == L'_' || text[i] == L'.')) i++;
                if (i < len && (text[i] == L'e' || text[i] == L'E')) {
                    i++;
                    if (i < len && (text[i] == L'+' || text[i] == L'-')) i++;
                    while (i < len && is_digit(text[i])) i++;
                }
            }
            if (i < len && (text[i] == L'j' || text[i] == L'J')) i++;
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
            if ((tokLen == 1 || tokLen == 2) && i < len && (text[i] == L'"' || text[i] == L'\'')) {
                wchar_t p0 = text[start];
                if (p0 == L'r' || p0 == L'f' || p0 == L'b' || p0 == L'u' || p0 == L'R' || p0 == L'F' || p0 == L'B') {
                    wchar_t sc = text[i];
                    /* Scan string with prefix */
                    if (i + 2 < len && text[i + 1] == sc && text[i + 2] == sc) {
                        i += 3;
                        while (i + 2 < len && !(text[i] == sc && text[i + 1] == sc && text[i + 2] == sc)) {
                            if (text[i] == L'\\' && i + 1 < len) i++;
                            i++;
                        }
                        if (i + 2 < len) i += 3; else i = len;
                    } else {
                        i++;
                        while (i < len && text[i] != sc) {
                            if (text[i] == L'\\' && i + 1 < len) i++;
                            else if (text[i] == L'\n') break;
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
            while (p < len && (text[p] == L' ' || text[p] == L'\t')) p++;
            if (p < len && text[p] == L'(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }

            continue;
        }

        if (wcschr(L"+-*/%=&|!^~?:<>", c)) {
            int start = i++;
            while (i < len && wcschr(L"+-*/%=&|!^~?:<>", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

static void tokenize_json(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }

        if (c == L'"') {
            int start = i++;
            int isKey = 0;
            int p;
            while (i < len && text[i] != L'"') {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == L'"') i++;

            p = i;
            while (p < len && (text[p] == L' ' || text[p] == L'\t' || text[p] == L'\r' || text[p] == L'\n')) p++;
            if (p < len && text[p] == L':') isKey = 1;

            tb_add(tb, start, i - start, isKey ? SYN_KEYWORD : SYN_STRING);
            continue;
        }

        if (is_digit(c) || c == L'-') {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == L'.' || text[i] == L'e' || text[i] == L'E' || text[i] == L'+' || text[i] == L'-')) i++;
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

        if (c == L'{' || c == L'}' || c == L'[' || c == L']' || c == L':' || c == L',') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_html(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];

        if (c == L'<' && i + 3 < len && text[i + 1] == L'!' && text[i + 2] == L'-' && text[i + 3] == L'-') {
            int start = i;
            i += 4;
            while (i + 2 < len && !(text[i] == L'-' && text[i + 1] == L'-' && text[i + 2] == L'>')) i++;
            if (i + 2 < len) i += 3; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == L'<' && i + 1 < len && (text[i + 1] == L'!' || text[i + 1] == L'?')) {
            int start = i;
            while (i < len && text[i] != L'>') i++;
            if (i < len) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        if (c == L'<') {
            int startTag = i++;
            tb_add(tb, startTag, 1, SYN_OPERATOR);
            if (i < len && text[i] == L'/') {
                tb_add(tb, i, 1, SYN_OPERATOR);
                i++;
            }
            /* Tag name */
            if (i < len && is_alpha(text[i])) {
                int nameStart = i;
                while (i < len && (is_alnum(text[i]) || text[i] == L'-' || text[i] == L':')) i++;
                tb_add(tb, nameStart, i - nameStart, SYN_KEYWORD);
            }

            /* Tag attributes until > */
            while (i < len && text[i] != L'>') {
                wchar_t ac = text[i];
                if (ac == L' ' || ac == L'\t' || ac == L'\r' || ac == L'\n') { i++; continue; }
                if (ac == L'/' && i + 1 < len && text[i + 1] == L'>') {
                    tb_add(tb, i, 2, SYN_OPERATOR);
                    i += 2;
                    break;
                }
                if (ac == L'=') {
                    tb_add(tb, i, 1, SYN_OPERATOR);
                    i++;
                    continue;
                }
                if (ac == L'"' || ac == L'\'') {
                    int sStart = i++;
                    while (i < len && text[i] != ac) i++;
                    if (i < len && text[i] == ac) i++;
                    tb_add(tb, sStart, i - sStart, SYN_STRING);
                    continue;
                }
                if (is_alpha(ac)) {
                    int aStart = i;
                    while (i < len && (is_alnum(text[i]) || text[i] == L'-' || text[i] == L':')) i++;
                    tb_add(tb, aStart, i - aStart, SYN_TYPE);
                    continue;
                }
                i++;
            }
            if (i < len && text[i] == L'>') {
                tb_add(tb, i, 1, SYN_OPERATOR);
                i++;
            }
            continue;
        }

        if (c == L'&') {
            int start = i++;
            while (i < len && is_alnum(text[i])) i++;
            if (i < len && text[i] == L';') i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        i++;
    }
}

static void tokenize_css(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }

        if (c == L'/' && i + 1 < len && text[i + 1] == L'*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == L'*' && text[i + 1] == L'/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == L'@') {
            int start = i++;
            while (i < len && (is_alnum(text[i]) || text[i] == L'-')) i++;
            tb_add(tb, start, i - start, SYN_PREPROC);
            continue;
        }

        if (c == L'"' || c == L'\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        if (c == L'#' && i + 1 < len && is_hex(text[i + 1])) {
            int start = i++;
            while (i < len && is_hex(text[i])) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (is_digit(c)) {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == L'.')) i++;
            while (i < len && (is_alpha(text[i]) || text[i] == L'%')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        if (is_alpha(c) || c == L'-') {
            int start = i;
            int p;
            while (i < len && (is_alnum(text[i]) || text[i] == L'-')) i++;
            p = i;
            while (p < len && (text[p] == L' ' || text[p] == L'\t')) p++;
            if (p < len && text[p] == L':') {
                tb_add(tb, start, i - start, SYN_KEYWORD);
            }
            continue;
        }

        if (c == L'{' || c == L'}' || c == L':' || c == L';' || c == L',' || c == L'>') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_sql(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }

        /* Comment -- */
        if (c == L'-' && i + 1 < len && text[i + 1] == L'-') {
            int start = i;
            i += 2;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Comment /* */
        if (c == L'/' && i + 1 < len && text[i + 1] == L'*') {
            int start = i;
            i += 2;
            while (i + 1 < len && !(text[i] == L'*' && text[i + 1] == L'/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* String '...' */
        if (c == L'\'') {
            int start = i++;
            while (i < len && text[i] != L'\'') {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == L'\'') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Number */
        if (is_digit(c)) {
            int start = i++;
            while (i < len && (is_digit(text[i]) || text[i] == L'.')) i++;
            tb_add(tb, start, i - start, SYN_NUMBER);
            continue;
        }

        /* Word */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            while (i < len && (is_alnum(text[i]) || text[i] == L'_')) i++;
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

        if (wcschr(L"=<>!+-*/,();", c)) {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_shell(TokBuf *tb, const wchar_t *text, int len, SynLang lang)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }

        /* Comment # */
        if (c == L'#' && lang != LANG_BATCH) {
            int start = i++;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* Batch comment REM or :: */
        if (lang == LANG_BATCH) {
            if (c == L':' && i + 1 < len && text[i + 1] == L':') {
                int start = i;
                while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
                tb_add(tb, start, i - start, SYN_COMMENT);
                continue;
            }
        }

        /* Strings "..." and '...' */
        if (c == L'"' || c == L'\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                else if (text[i] == L'\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Variables: $VAR, ${VAR}, %VAR% */
        if (c == L'$' || (lang == LANG_BATCH && c == L'%')) {
            int start = i++;
            if (i < len && text[i] == L'{') {
                while (i < len && text[i] != L'}') i++;
                if (i < len && text[i] == L'}') i++;
            } else if (c == L'%' && lang == LANG_BATCH) {
                while (i < len && text[i] != L'%') i++;
                if (i < len && text[i] == L'%') i++;
            } else {
                while (i < len && (is_alnum(text[i]) || text[i] == L'_')) i++;
            }
            tb_add(tb, start, i - start, SYN_TYPE);
            continue;
        }

        /* Identifiers */
        if (is_alpha(c)) {
            int start = i;
            int tokLen;
            while (i < len && (is_alnum(text[i]) || text[i] == L'_' || text[i] == L'-')) i++;
            tokLen = i - start;

            if (lang == LANG_BATCH) {
                if (match_kw_ci(text + start, tokLen, "rem")) {
                    while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
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

        if (wcschr(L"|&;><=(){}[]", c)) {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_yaml(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }

        if (c == L'#') {
            int start = i++;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        if (c == L'"' || c == L'\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        if (is_digit(c) || c == L'-') {
            if (is_digit(c) || (i + 1 < len && is_digit(text[i + 1]))) {
                int start = i++;
                while (i < len && (is_digit(text[i]) || text[i] == L'.' || text[i] == L'_')) i++;
                tb_add(tb, start, i - start, SYN_NUMBER);
                continue;
            }
        }

        if (is_alpha(c) || c == L'_') {
            int start = i;
            int p;
            while (i < len && (is_alnum(text[i]) || text[i] == L'_' || text[i] == L'-')) i++;
            p = i;
            while (p < len && (text[p] == L' ' || text[p] == L'\t')) p++;
            if (p < len && text[p] == L':') {
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

        if (c == L':' || c == L'-' || c == L'{' || c == L'}' || c == L'[' || c == L']') {
            tb_add(tb, i, 1, SYN_OPERATOR);
            i++;
            continue;
        }

        i++;
    }
}

static void tokenize_diff(TokBuf *tb, const wchar_t *text, int len)
{
    int lineStart = 0;
    while (lineStart < len) {
        int lineEnd = lineStart;
        wchar_t first;
        while (lineEnd < len && text[lineEnd] != L'\n' && text[lineEnd] != L'\r') lineEnd++;
        first = (lineStart < lineEnd) ? text[lineStart] : 0;

        if (first == L'+' && !(lineStart + 2 < lineEnd && text[lineStart + 1] == L'+' && text[lineStart + 2] == L'+')) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_TYPE); /* Green / Add */
        } else if (first == L'-' && !(lineStart + 2 < lineEnd && text[lineStart + 1] == L'-' && text[lineStart + 2] == L'-')) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_KEYWORD); /* Red / Remove */
        } else if (first == L'@' && lineStart + 1 < lineEnd && text[lineStart + 1] == L'@') {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_PREPROC); /* Chunk header */
        } else if (lineStart + 4 <= lineEnd && (wcsncmp(text + lineStart, L"diff", 4) == 0 || wcsncmp(text + lineStart, L"index", 5) == 0)) {
            tb_add(tb, lineStart, lineEnd - lineStart, SYN_COMMENT);
        }

        if (lineEnd < len && text[lineEnd] == L'\r') lineEnd++;
        if (lineEnd < len && text[lineEnd] == L'\n') lineEnd++;
        lineStart = lineEnd;
    }
}

static void tokenize_markdown(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L'#') {
            int start = i++;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_KEYWORD);
            continue;
        }
        if (c == L'>') {
            int start = i++;
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }
        if (c == L'`') {
            int start = i++;
            while (i < len && text[i] != L'`') i++;
            if (i < len && text[i] == L'`') i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }
        i++;
    }
}

static void tokenize_lua(TokBuf *tb, const wchar_t *text, int len)
{
    int i = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            i++;
            continue;
        }

        /* Comment -- */
        if (c == L'-' && i + 1 < len && text[i + 1] == L'-') {
            int start = i;
            i += 2;
            /* Check if multi-line comment: --[=*...[ */
            if (i < len && text[i] == L'[') {
                int numEq = 0;
                int p = i + 1;
                while (p < len && text[p] == L'=') { numEq++; p++; }
                if (p < len && text[p] == L'[') {
                    i = p + 1;
                    while (i < len) {
                        if (text[i] == L']') {
                            int closeEq = 0;
                            int q = i + 1;
                            while (q < len && text[q] == L'=') { closeEq++; q++; }
                            if (q < len && text[q] == L']' && closeEq == numEq) {
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
            while (i < len && text[i] != L'\n' && text[i] != L'\r') i++;
            tb_add(tb, start, i - start, SYN_COMMENT);
            continue;
        }

        /* String: Multi-line literal [=*...[ */
        if (c == L'[') {
            int numEq = 0;
            int p = i + 1;
            while (p < len && text[p] == L'=') { numEq++; p++; }
            if (p < len && text[p] == L'[') {
                int start = i;
                i = p + 1;
                while (i < len) {
                    if (text[i] == L']') {
                        int closeEq = 0;
                        int q = i + 1;
                        while (q < len && text[q] == L'=') { closeEq++; q++; }
                        if (q < len && text[q] == L']' && closeEq == numEq) {
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
        if (c == L'"' || c == L'\'') {
            int start = i++;
            while (i < len && text[i] != c) {
                if (text[i] == L'\\' && i + 1 < len) i++;
                else if (text[i] == L'\n') break;
                i++;
            }
            if (i < len && text[i] == c) i++;
            tb_add(tb, start, i - start, SYN_STRING);
            continue;
        }

        /* Numbers */
        if (is_digit(c) || (c == L'.' && i + 1 < len && is_digit(text[i + 1]))) {
            int start = i;
            if (c == L'0' && i + 1 < len && (text[i + 1] == L'x' || text[i + 1] == L'X')) {
                i += 2;
                while (i < len && (is_hex(text[i]) || text[i] == L'.' || text[i] == L'p' || text[i] == L'P' || text[i] == L'+' || text[i] == L'-')) i++;
            } else {
                while (i < len && (is_digit(text[i]) || text[i] == L'.')) i++;
                if (i < len && (text[i] == L'e' || text[i] == L'E')) {
                    i++;
                    if (i < len && (text[i] == L'+' || text[i] == L'-')) i++;
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
            while (p < len && (text[p] == L' ' || text[p] == L'\t')) p++;
            if (p < len && text[p] == L'(') {
                tb_add(tb, start, tokLen, SYN_FUNCTION);
                continue;
            }
            continue;
        }

        /* Operators */
        if (c == L'.' && i + 1 < len && text[i + 1] == L'.') {
            int start = i;
            while (i < len && text[i] == L'.') i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        if (wcschr(L"+-*/%^#&~|<>=:", c)) {
            int start = i++;
            while (i < len && wcschr(L"=<>~/", text[i])) i++;
            tb_add(tb, start, i - start, SYN_OPERATOR);
            continue;
        }

        i++;
    }
}

/* ---------------- Public API ---------------- */

SynToken *syntax_highlight(const wchar_t *lang, const wchar_t *text, int textlen, int *outCount)
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
