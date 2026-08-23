// ============================================================
// TanjaOS JIT C compiler ("c" command)
//
// A real, single-pass, x86-32 machine-code-emitting compiler for a
// deliberately small subset of C. No linker, no object files, no
// libc: source goes in, machine code comes out into an executable
// buffer, and main() gets called directly.
//
// Supported: int type only (plus void as a function return type and
// as "(void)" for an explicit empty parameter list - there's no void*
// or void variables), functions (params/return int, must be defined
// before first use - self-recursion is fine), global and local
// variables, fixed-size int arrays (arr[N], both global and local,
// with indexing as lvalue/rvalue), pointers (&, *, including
// *p = value), ++/-- (prefix and postfix, simple variables only),
// compound assignment (+=, -=, *=, /=, %=, on variables and array
// elements), if/else, while, for (including "for (int i = ...; ...;
// ...)"), break/continue, return, the usual arithmetic/comparison/
// logical operators with correct precedence and && / || short-
// circuiting, // and /* */ comments. Built-in I/O: putchar(int),
// print_int(int), print("literal") / print_str("literal") (identical,
// print is just a shorter alias - string literals only work as a
// direct argument to these, there's no general char* string type).
//
// NOT supported: structs/unions/typedefs, the preprocessor, floats,
// multi-dimensional arrays, pointer arithmetic beyond plain
// assignment/deref, compound-assign through a pointer (*p += 1),
// multiple translation units.
//
// This kernel has no paging/memory protection, so a plain static
// buffer is already both writable and executable - no mprotect
// dance needed to run generated code.
// ============================================================

#include <stdint.h>
#include "../include/cc.h"

extern void print(const char* s);
extern void print_dec(uint32_t n);
extern void print_hex(uint32_t n);
extern void putc(char c);

// ------------------------------------------------------------
// Code buffer
// ------------------------------------------------------------

#define CODE_BUF_SIZE 16384
static uint8_t code_buf[CODE_BUF_SIZE];
static uint32_t code_len;

static void emit_u8(uint8_t b) {
    if (code_len < CODE_BUF_SIZE) code_buf[code_len++] = b;
}
static void emit_u32(uint32_t v) {
    emit_u8((uint8_t)(v & 0xFF));
    emit_u8((uint8_t)((v >> 8) & 0xFF));
    emit_u8((uint8_t)((v >> 16) & 0xFF));
    emit_u8((uint8_t)((v >> 24) & 0xFF));
}
static void patch_u32(uint32_t at, uint32_t v) {
    if (at + 4 > CODE_BUF_SIZE) return;
    code_buf[at]     = (uint8_t)(v & 0xFF);
    code_buf[at + 1] = (uint8_t)((v >> 8) & 0xFF);
    code_buf[at + 2] = (uint8_t)((v >> 16) & 0xFF);
    code_buf[at + 3] = (uint8_t)((v >> 24) & 0xFF);
}

// ------------------------------------------------------------
// Error handling: first error wins, checked after every sub-parse.
// ------------------------------------------------------------

static int cc_error_flag = 0;
static int cc_error_line = 0;
static char cc_error_msg[128];

static void cc_seterr(int line, const char* msg) {
    if (cc_error_flag) return;
    cc_error_flag = 1;
    cc_error_line = line;
    int i = 0;
    while (msg[i] && i < 127) { cc_error_msg[i] = msg[i]; i++; }
    cc_error_msg[i] = 0;
}

// ------------------------------------------------------------
// Lexer
// ------------------------------------------------------------

typedef enum {
    T_EOF, T_NUM, T_IDENT, T_STR,
    T_INT, T_VOID, T_IF, T_ELSE, T_WHILE, T_FOR, T_RETURN, T_BREAK, T_CONTINUE,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_AMP,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT,
    T_INC, T_DEC, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ
} token_type_t;

typedef struct {
    token_type_t type;
    int32_t ival;
    char sval[64];
    int line;
    uint32_t start_pos; // byte offset in source where this token began
} token_t;

static const char* src;
static uint32_t src_len;
static uint32_t src_pos;
static int cur_line;
static token_t cur_tok;
static uint32_t token_count; // safety net against pathological input

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int peekc(void) { return src_pos < src_len ? (unsigned char)src[src_pos] : -1; }
static int getc_src(void) {
    if (src_pos >= src_len) return -1;
    int c = (unsigned char)src[src_pos++];
    if (c == '\n') cur_line++;
    return c;
}

static void skip_ws_comments(void) {
    for (;;) {
        int c = peekc();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { getc_src(); continue; }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '/') {
            while (peekc() != -1 && peekc() != '\n') getc_src();
            continue;
        }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '*') {
            getc_src(); getc_src();
            while (peekc() != -1 && !(peekc() == '*' && src_pos + 1 < src_len && src[src_pos + 1] == '/')) getc_src();
            if (peekc() != -1) { getc_src(); getc_src(); }
            continue;
        }
        break;
    }
}

static void next_token(void) {
    token_count++;
    if (token_count > 200000) { cc_seterr(cur_line, "input too complex / possibly unterminated construct"); cur_tok.type = T_EOF; return; }

    skip_ws_comments();
    cur_tok.start_pos = src_pos;
    cur_tok.line = cur_line;
    cur_tok.sval[0] = 0;

    int c = peekc();
    if (c == -1) { cur_tok.type = T_EOF; return; }

    if (c >= '0' && c <= '9') {
        int32_t v = 0;
        while (peekc() >= '0' && peekc() <= '9') { v = v * 10 + (getc_src() - '0'); }
        cur_tok.type = T_NUM; cur_tok.ival = v; return;
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int i = 0;
        while ((peekc() >= 'a' && peekc() <= 'z') || (peekc() >= 'A' && peekc() <= 'Z') ||
               (peekc() >= '0' && peekc() <= '9') || peekc() == '_') {
            if (i < 63) cur_tok.sval[i++] = (char)getc_src(); else getc_src();
        }
        cur_tok.sval[i] = 0;
        if (str_eq(cur_tok.sval, "int")) cur_tok.type = T_INT;
        else if (str_eq(cur_tok.sval, "void")) cur_tok.type = T_VOID;
        else if (str_eq(cur_tok.sval, "if")) cur_tok.type = T_IF;
        else if (str_eq(cur_tok.sval, "else")) cur_tok.type = T_ELSE;
        else if (str_eq(cur_tok.sval, "while")) cur_tok.type = T_WHILE;
        else if (str_eq(cur_tok.sval, "for")) cur_tok.type = T_FOR;
        else if (str_eq(cur_tok.sval, "return")) cur_tok.type = T_RETURN;
        else if (str_eq(cur_tok.sval, "break")) cur_tok.type = T_BREAK;
        else if (str_eq(cur_tok.sval, "continue")) cur_tok.type = T_CONTINUE;
        else cur_tok.type = T_IDENT;
        return;
    }

    if (c == '"') {
        getc_src();
        int i = 0;
        while (peekc() != -1 && peekc() != '"') {
            int ch = getc_src();
            if (ch == '\\') {
                int e = getc_src();
                if (e == 'n') ch = '\n';
                else if (e == 't') ch = '\t';
                else if (e == '0') ch = '\0';
                else if (e == '\\') ch = '\\';
                else if (e == '"') ch = '"';
                else ch = e;
            }
            if (i < 63) cur_tok.sval[i++] = (char)ch;
        }
        if (peekc() == '"') getc_src();
        cur_tok.sval[i] = 0;
        cur_tok.type = T_STR;
        return;
    }

    if (c == '\'') {
        getc_src();
        int ch = getc_src();
        if (ch == '\\') {
            int e = getc_src();
            if (e == 'n') ch = '\n';
            else if (e == 't') ch = '\t';
            else if (e == '0') ch = '\0';
            else if (e == '\\') ch = '\\';
            else if (e == '\'') ch = '\'';
            else ch = e;
        }
        if (peekc() == '\'') getc_src();
        cur_tok.type = T_NUM; cur_tok.ival = ch; return;
    }

    getc_src();
    switch (c) {
        case '(': cur_tok.type = T_LPAREN; return;
        case ')': cur_tok.type = T_RPAREN; return;
        case '{': cur_tok.type = T_LBRACE; return;
        case '}': cur_tok.type = T_RBRACE; return;
        case '[': cur_tok.type = T_LBRACKET; return;
        case ']': cur_tok.type = T_RBRACKET; return;
        case ';': cur_tok.type = T_SEMI; return;
        case ',': cur_tok.type = T_COMMA; return;
        case '+':
            if (peekc() == '+') { getc_src(); cur_tok.type = T_INC; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_PLUSEQ; return; }
            cur_tok.type = T_PLUS; return;
        case '-':
            if (peekc() == '-') { getc_src(); cur_tok.type = T_DEC; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_MINUSEQ; return; }
            cur_tok.type = T_MINUS; return;
        case '*':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_STAREQ; return; }
            cur_tok.type = T_STAR; return;
        case '/':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_SLASHEQ; return; }
            cur_tok.type = T_SLASH; return;
        case '%':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_PERCENTEQ; return; }
            cur_tok.type = T_PERCENT; return;
        case '=':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_EQ; } else cur_tok.type = T_ASSIGN;
            return;
        case '!':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_NE; } else cur_tok.type = T_NOT;
            return;
        case '<':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_LE; } else cur_tok.type = T_LT;
            return;
        case '>':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_GE; } else cur_tok.type = T_GT;
            return;
        case '&':
            if (peekc() == '&') { getc_src(); cur_tok.type = T_ANDAND; return; }
            cur_tok.type = T_AMP; return;
        case '|':
            if (peekc() == '|') { getc_src(); cur_tok.type = T_OROR; return; }
            cc_seterr(cur_line, "unexpected '|'"); cur_tok.type = T_EOF; return;
        default:
            cc_seterr(cur_line, "unexpected character in source");
            cur_tok.type = T_EOF; return;
    }
}

typedef struct { uint32_t pos; int line; token_t tok; } lexer_state_t;
static lexer_state_t lexer_save(void) {
    lexer_state_t s; s.pos = src_pos; s.line = cur_line; s.tok = cur_tok; return s;
}
static void lexer_restore(lexer_state_t s) {
    src_pos = s.pos; cur_line = s.line; cur_tok = s.tok;
}

static void expect(token_type_t t, const char* what) {
    if (cc_error_flag) return;
    if (cur_tok.type != t) { cc_seterr(cur_tok.line, what); return; }
    next_token();
}

// ------------------------------------------------------------
// Symbol tables
// ------------------------------------------------------------

#define MAX_LOCALS 48
typedef struct { char name[32]; int32_t offset; int is_array; int array_len; } local_t;
static local_t locals[MAX_LOCALS];
static int local_count;
static int next_local_offset;
static int next_param_offset;

#define MAX_FUNCS 24
typedef struct { char name[32]; uint32_t addr; int nparams; } func_t;
static func_t funcs[MAX_FUNCS];
static int func_count;
static uint32_t cur_func_start; // for self-recursion

#define MAX_GLOBALS 24
typedef struct { char name[32]; uint32_t addr; int is_array; int array_len; } global_t;
static int32_t globals_data[MAX_GLOBALS];
static global_t globals[MAX_GLOBALS];
static int global_count;

// Separate backing pool for global ARRAYS (globals_data above is one
// slot per scalar global; arrays need N consecutive slots).
#define GLOBAL_ARRAY_POOL_SIZE 1024
static int32_t global_array_pool[GLOBAL_ARRAY_POOL_SIZE];
static int global_array_pool_used;

// break/continue support: tracks the innermost enclosing loop so
// break/continue statements know where to jump. Nested loops save and
// restore the previous context around their body (matches the natural
// LIFO nesting of the recursive-descent parser itself).
#define MAX_BREAK_PATCHES 16
typedef struct {
    uint32_t break_patches[MAX_BREAK_PATCHES];
    int break_count;
    uint32_t continue_patches[MAX_BREAK_PATCHES];
    int continue_count;
    int continue_target_known; // true for while (target = loop_start,
                                // known immediately); false for for
                                // (target = increment code, which is
                                // only known after the body is parsed
                                // - see the for-loop handler)
    uint32_t continue_target;
} loop_ctx_t;
static loop_ctx_t* cur_loop = 0;

static local_t* find_local(const char* name) {
    int i;
    for (i = 0; i < local_count; i++) if (str_eq(locals[i].name, name)) return &locals[i];
    return 0;
}
static global_t* find_global(const char* name) {
    int i;
    for (i = 0; i < global_count; i++) if (str_eq(globals[i].name, name)) return &globals[i];
    return 0;
}
static func_t* find_func(const char* name) {
    int i;
    for (i = 0; i < func_count; i++) if (str_eq(funcs[i].name, name)) return &funcs[i];
    return 0;
}

// ------------------------------------------------------------
// x86 code generation helpers
// ------------------------------------------------------------

static void gen_push_eax(void) { emit_u8(0x50); }
static void gen_pop_ecx(void)  { emit_u8(0x59); }
static void gen_mov_eax_imm32(uint32_t v) { emit_u8(0xB8); emit_u32(v); }
static void gen_mov_eax_ebp_disp32(int32_t d) { emit_u8(0x8B); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_mov_ebp_disp32_eax(int32_t d) { emit_u8(0x89); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_lea_eax_ebp_disp32(int32_t d) { emit_u8(0x8D); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_mov_eax_indirect_eax(void) { emit_u8(0x8B); emit_u8(0x00); } // mov eax,[eax]
static void gen_mov_indirect_ecx_eax(void) { emit_u8(0x89); emit_u8(0x01); } // mov [ecx],eax
static void gen_shl_eax_2(void) { emit_u8(0xC1); emit_u8(0xE0); emit_u8(0x02); } // shl eax,2 (i.e. *4)
static void gen_sub_eax_ecx(void) { emit_u8(0x29); emit_u8(0xC8); } // eax -= ecx (direct order, for compound assign)
static void gen_pop_eax(void) { emit_u8(0x58); }
static void gen_mov_eax_esp0(void) { emit_u8(0x8B); emit_u8(0x04); emit_u8(0x24); } // mov eax,[esp] (peek, no pop)
static void gen_mov_eax_abs(uint32_t addr) { emit_u8(0xA1); emit_u32(addr); }
static void gen_mov_abs_eax(uint32_t addr) { emit_u8(0xA3); emit_u32(addr); }
static void gen_add_eax_ecx(void) { emit_u8(0x01); emit_u8(0xC8); }
static void gen_sub_ecx_eax_then_mov(void) { emit_u8(0x29); emit_u8(0xC1); emit_u8(0x89); emit_u8(0xC8); } // ecx-=eax; eax=ecx
static void gen_imul_eax_ecx(void) { emit_u8(0x0F); emit_u8(0xAF); emit_u8(0xC1); }
static void gen_div_setup_and_idiv(void) {
    // pre: ecx=left, eax=right. want eax=left/right, edx=left%right
    emit_u8(0x89); emit_u8(0xC3); // mov ebx, eax   (ebx = right)
    emit_u8(0x89); emit_u8(0xC8); // mov eax, ecx   (eax = left)
    emit_u8(0x99);                // cdq
    emit_u8(0xF7); emit_u8(0xFB); // idiv ebx
}
static void gen_cmp_ecx_eax(void) { emit_u8(0x39); emit_u8(0xC1); }
static void gen_setcc_movzx(uint8_t setcc_op2) {
    emit_u8(0x0F); emit_u8(setcc_op2); emit_u8(0xC0); // setX al
    emit_u8(0x0F); emit_u8(0xB6); emit_u8(0xC0);      // movzx eax, al
}
static void gen_test_eax_eax(void) { emit_u8(0x85); emit_u8(0xC0); }
static void gen_neg_eax(void) { emit_u8(0xF7); emit_u8(0xD8); }
static void gen_cmp_eax_0(void) { emit_u8(0x3D); emit_u32(0); }

static uint32_t gen_jz_placeholder(void) {
    emit_u8(0x0F); emit_u8(0x84); uint32_t at = code_len; emit_u32(0); return at;
}
static uint32_t gen_jnz_placeholder(void) {
    emit_u8(0x0F); emit_u8(0x85); uint32_t at = code_len; emit_u32(0); return at;
}
static uint32_t gen_jmp_placeholder(void) {
    emit_u8(0xE9); uint32_t at = code_len; emit_u32(0); return at;
}
static void patch_jump_here(uint32_t at) { patch_u32(at, code_len - (at + 4)); }
static void gen_jmp_to(uint32_t target) {
    emit_u8(0xE9); emit_u32(target - (code_len + 4));
}
static void gen_call_abs(uint32_t target) {
    // `target` is an absolute runtime address (either a native kernel
    // function pointer, or code_buf+offset for a JIT'd function/
    // self-recursive call) - code_len on its own is only a
    // buffer-relative offset, so it must be added to code_buf's own
    // base address before subtracting to get a correct rel32.
    emit_u8(0xE8);
    emit_u32(target - ((uint32_t)code_buf + code_len + 4));
}

// ------------------------------------------------------------
// Forward decls
// ------------------------------------------------------------
static void parse_expr(void);
static void parse_stmt(void);

// ------------------------------------------------------------
// Expressions (result always ends up in eax)
// ------------------------------------------------------------

// Loads the VALUE of a named variable into eax. Arrays decay to their
// base address (matching normal C array-to-pointer decay when an
// array name is used as a value, e.g. bare "arr" or as a call arg).
static void gen_var_load_to_eax(const char* name, int line) {
    local_t* l = find_local(name);
    if (l) {
        if (l->is_array) { gen_lea_eax_ebp_disp32(l->offset); return; }
        gen_mov_eax_ebp_disp32(l->offset);
        return;
    }
    global_t* g = find_global(name);
    if (g) {
        if (g->is_array) { gen_mov_eax_imm32(g->addr); return; }
        gen_mov_eax_abs(g->addr);
        return;
    }
    cc_seterr(line, "undefined variable");
}

// Stores eax into a named variable. Errors if it's an array (whole
// arrays aren't assignable, matching real C).
static void gen_var_store_from_eax(const char* name, int line) {
    local_t* l = find_local(name);
    if (l) {
        if (l->is_array) { cc_seterr(line, "cannot assign to an array"); return; }
        gen_mov_ebp_disp32_eax(l->offset);
        return;
    }
    global_t* g = find_global(name);
    if (g) {
        if (g->is_array) { cc_seterr(line, "cannot assign to an array"); return; }
        gen_mov_abs_eax(g->addr);
        return;
    }
    cc_seterr(line, "undefined variable");
}

// Computes the address of arr[index] into eax. Caller has already
// consumed IDENT and the '[' - this parses the index expression and
// the closing ']'.
static void parse_array_index_addr(const char* name, int line) {
    local_t* l = find_local(name);
    global_t* g = l ? 0 : find_global(name);
    if (!l && !g) { cc_seterr(line, "undefined array"); return; }
    int is_array = (l && l->is_array) || (g && g->is_array);
    if (!is_array) { cc_seterr(line, "not an array"); return; }

    if (l) gen_lea_eax_ebp_disp32(l->offset);
    else gen_mov_eax_imm32(g->addr);
    gen_push_eax();
    parse_expr(); // index -> eax
    if (cc_error_flag) return;
    gen_shl_eax_2();
    gen_pop_ecx();
    gen_add_eax_ecx(); // eax = base + index*4
    expect(T_RBRACKET, "expected ']'");
}

// Pure token-scan lookahead (emits no code) to check whether the
// upcoming "[ ... ]" is followed by an assignment operator, without
// risking double-emitting code for an index expression that might
// have side effects (e.g. a function call). Caller must have already
// consumed IDENT and be looking at T_LBRACKET. Always restores the
// lexer to wherever it started.
static int peek_is_array_assign(void) {
    lexer_state_t save = lexer_save();
    next_token(); // consume '['
    int depth = 0;
    for (;;) {
        if (cur_tok.type == T_LBRACKET) depth++;
        else if (cur_tok.type == T_RBRACKET) { if (depth == 0) break; depth--; }
        else if (cur_tok.type == T_EOF) { lexer_restore(save); return 0; }
        next_token();
    }
    next_token(); // consume the matching ']'
    int result = (cur_tok.type == T_ASSIGN || cur_tok.type == T_PLUSEQ || cur_tok.type == T_MINUSEQ ||
                  cur_tok.type == T_STAREQ || cur_tok.type == T_SLASHEQ || cur_tok.type == T_PERCENTEQ);
    lexer_restore(save);
    return result;
}

static void parse_call_args_and_call(func_t* fn, const char* builtin_name) {
    // consumes '(' already done by caller; parses args up to ')'
    int argc = 0;
    int32_t saved_ival[8];
    (void)saved_ival;
    if (cur_tok.type != T_RPAREN) {
        // We must push args in reverse order for our calling convention,
        // but we only get to parse them left-to-right (single pass), and
        // print_str needs special literal handling. To keep this simple
        // and correct without buffering, we evaluate left-to-right but
        // push immediately, then before the call we've pushed in
        // left-to-right order onto a LIFO stack, which is backwards from
        // what we want. Simplest fix: cap args at 8 and evaluate them
        // into temporary stack slots, then push in reverse.
        // Practically: evaluate each arg (eax), push eax (left to right).
        // Then to call with param1 at [ebp+8], we need the LAST pushed
        // value to be param1 - i.e. push order must be reverse. Since we
        // can't re-order after the fact easily without extra stack
        // shuffling, we instead just define OUR calling convention as
        // "first pushed = last param" and assign local param offsets to
        // match (see function definition parsing) - param offsets are
        // assigned in DECLARATION order matching PUSH order at call
        // sites (left-to-right), so this works out consistently as long
        // as both sides agree, which they do since both use this file.
        for (;;) {
            if ((str_eq(builtin_name ? builtin_name : "", "print_str") ||
                 str_eq(builtin_name ? builtin_name : "", "print")) && argc == 0) {
                if (cur_tok.type != T_STR) { cc_seterr(cur_tok.line, "print_str/print requires a string literal argument"); return; }
                // Store string bytes right here in the code buffer as
                // inline data, preceded by a jmp over it so it's never
                // executed, then load its address into eax. Works fine
                // for an empty string too - just emits a single 0 byte.
                uint32_t jmp_at = gen_jmp_placeholder();
                uint32_t str_addr = (uint32_t)code_buf + code_len;
                int i = 0;
                while (cur_tok.sval[i]) { emit_u8((uint8_t)cur_tok.sval[i]); i++; }
                emit_u8(0);
                patch_jump_here(jmp_at);
                gen_mov_eax_imm32(str_addr);
                next_token();
            } else {
                parse_expr();
            }
            if (cc_error_flag) return;
            gen_push_eax();
            argc++;
            if (cur_tok.type == T_COMMA) { next_token(); continue; }
            break;
        }
    }
    expect(T_RPAREN, "expected ')' after arguments");
    if (cc_error_flag) return;

    if (builtin_name) {
        uint32_t target = 0;
        if (str_eq(builtin_name, "putchar")) target = (uint32_t)(void*)putc;
        else if (str_eq(builtin_name, "print_int")) {
            // handled by cc_print_int helper declared below
            extern void cc_print_int(int32_t n);
            target = (uint32_t)(void*)cc_print_int;
        } else if (str_eq(builtin_name, "print_str") || str_eq(builtin_name, "print")) target = (uint32_t)(void*)print;
        gen_call_abs(target);
    } else {
        if (fn->nparams != argc) { cc_seterr(cur_tok.line, "wrong number of arguments"); return; }
        gen_call_abs(fn->addr);
    }
    if (argc > 0) { emit_u8(0x81); emit_u8(0xC4); emit_u32((uint32_t)(argc * 4)); } // add esp, argc*4
}

static void parse_primary(void) {
    if (cc_error_flag) return;
    if (cur_tok.type == T_NUM) {
        gen_mov_eax_imm32((uint32_t)cur_tok.ival);
        next_token();
        return;
    }
    if (cur_tok.type == T_LPAREN) {
        next_token();
        parse_expr();
        expect(T_RPAREN, "expected ')'");
        return;
    }
    if (cur_tok.type == T_MINUS) {
        next_token();
        parse_primary();
        if (cc_error_flag) return;
        gen_neg_eax();
        return;
    }
    if (cur_tok.type == T_NOT) {
        next_token();
        parse_primary();
        if (cc_error_flag) return;
        gen_cmp_eax_0();
        gen_setcc_movzx(0x94); // sete
        return;
    }
    if (cur_tok.type == T_STAR) {
        // pointer dereference (rvalue): *expr
        next_token();
        parse_primary(); // address -> eax
        if (cc_error_flag) return;
        gen_mov_eax_indirect_eax();
        return;
    }
    if (cur_tok.type == T_AMP) {
        // address-of: &ident or &ident[expr]
        next_token();
        int line = cur_tok.line;
        if (cur_tok.type != T_IDENT) { cc_seterr(line, "'&' must be followed by a variable"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        next_token();
        if (cur_tok.type == T_LBRACKET) {
            next_token();
            parse_array_index_addr(name, line); // element address, already in eax
            return;
        }
        local_t* l = find_local(name);
        if (l) { gen_lea_eax_ebp_disp32(l->offset); return; }
        global_t* g = find_global(name);
        if (g) { gen_mov_eax_imm32(g->addr); return; }
        cc_seterr(line, "undefined variable");
        return;
    }
    if (cur_tok.type == T_INC || cur_tok.type == T_DEC) {
        // prefix ++x / --x (simple variables only, not array elements)
        int is_inc = (cur_tok.type == T_INC);
        next_token();
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "'++'/'--' must be followed by a variable"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();
        gen_var_load_to_eax(name, line);
        if (cc_error_flag) return;
        if (is_inc) { emit_u8(0x83); emit_u8(0xC0); emit_u8(0x01); } // add eax,1
        else { emit_u8(0x83); emit_u8(0xE8); emit_u8(0x01); }        // sub eax,1
        gen_var_store_from_eax(name, line);
        return;
    }
    if (cur_tok.type == T_IDENT) {
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();
        if (cur_tok.type == T_LPAREN) {
            next_token();
            func_t* fn = find_func(name);
            const char* builtin = 0;
            if (!fn) {
                if (str_eq(name, "putchar") || str_eq(name, "print_int") || str_eq(name, "print_str") || str_eq(name, "print")) builtin = name;
                else { cc_seterr(line, "call to undefined function"); return; }
            }
            parse_call_args_and_call(fn, builtin);
            return;
        }
        if (cur_tok.type == T_LBRACKET) {
            next_token();
            parse_array_index_addr(name, line); // element address -> eax
            if (cc_error_flag) return;
            gen_mov_eax_indirect_eax(); // load value
            return;
        }
        if (cur_tok.type == T_INC || cur_tok.type == T_DEC) {
            // postfix x++ / x-- (simple variables only)
            int is_inc = (cur_tok.type == T_INC);
            next_token();
            gen_var_load_to_eax(name, line); // old value -> eax (this IS the expression result)
            if (cc_error_flag) return;
            gen_push_eax(); // save old value
            if (is_inc) { emit_u8(0x83); emit_u8(0xC0); emit_u8(0x01); } // add eax,1
            else { emit_u8(0x83); emit_u8(0xE8); emit_u8(0x01); }        // sub eax,1
            gen_var_store_from_eax(name, line);
            if (cc_error_flag) return;
            gen_pop_eax(); // restore old value as the final result
            return;
        }
        gen_var_load_to_eax(name, line);
        return;
    }
    cc_seterr(cur_tok.line, "expected expression");
}

static void parse_term(void) {
    parse_primary();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_STAR) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_imul_eax_ecx();
        } else if (cur_tok.type == T_SLASH) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_div_setup_and_idiv();
        } else if (cur_tok.type == T_PERCENT) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_div_setup_and_idiv();
            emit_u8(0x89); emit_u8(0xD0); // mov eax, edx (remainder)
        } else break;
    }
}

static void parse_additive(void) {
    parse_term();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_PLUS) {
            next_token(); gen_push_eax(); parse_term(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_add_eax_ecx();
        } else if (cur_tok.type == T_MINUS) {
            next_token(); gen_push_eax(); parse_term(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_sub_ecx_eax_then_mov();
        } else break;
    }
}

static void parse_relational(void) {
    parse_additive();
    for (;;) {
        if (cc_error_flag) return;
        token_type_t t = cur_tok.type;
        if (t == T_LT || t == T_GT || t == T_LE || t == T_GE) {
            next_token(); gen_push_eax(); parse_additive(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_cmp_ecx_eax();
            if (t == T_LT) gen_setcc_movzx(0x9C);
            else if (t == T_GT) gen_setcc_movzx(0x9F);
            else if (t == T_LE) gen_setcc_movzx(0x9E);
            else gen_setcc_movzx(0x9D);
        } else break;
    }
}

static void parse_equality(void) {
    parse_relational();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_EQ || cur_tok.type == T_NE) {
            token_type_t t = cur_tok.type;
            next_token(); gen_push_eax(); parse_relational(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_cmp_ecx_eax();
            gen_setcc_movzx(t == T_EQ ? 0x94 : 0x95);
        } else break;
    }
}

static void parse_logic_and(void) {
    parse_equality();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_ANDAND) {
            next_token();
            gen_test_eax_eax();
            uint32_t jz_at = gen_jz_placeholder(); // false -> skip rhs, result 0
            parse_equality(); if (cc_error_flag) return;
            gen_test_eax_eax();
            gen_setcc_movzx(0x95); // eax = (rhs != 0)
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jz_at);
            gen_mov_eax_imm32(0);
            patch_jump_here(jmp_at);
        } else break;
    }
}

static void parse_logic_or(void) {
    parse_logic_and();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_OROR) {
            next_token();
            gen_test_eax_eax();
            uint32_t jnz_at = gen_jnz_placeholder(); // true -> skip rhs, result 1
            parse_logic_and(); if (cc_error_flag) return;
            gen_test_eax_eax();
            gen_setcc_movzx(0x95);
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jnz_at);
            gen_mov_eax_imm32(1);
            patch_jump_here(jmp_at);
        } else break;
    }
}

static void parse_assign(void) {
    if (cur_tok.type == T_IDENT) {
        lexer_state_t save = lexer_save();
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();

        if (cur_tok.type == T_LBRACKET && peek_is_array_assign()) {
            next_token(); // consume '['
            parse_array_index_addr(name, line); // element address -> eax
            if (cc_error_flag) return;
            token_type_t op = cur_tok.type;
            next_token(); // consume the assign/compound-assign operator
            if (op == T_ASSIGN) {
                gen_push_eax(); // save element address
                parse_assign();
                if (cc_error_flag) return;
                gen_pop_ecx();
                gen_mov_indirect_ecx_eax();
            } else {
                gen_push_eax();             // save element address
                gen_mov_eax_esp0();         // peek address without popping
                gen_mov_eax_indirect_eax(); // eax = old value
                gen_push_eax();             // stack: [addr, oldval]
                parse_assign();             // RHS -> eax
                if (cc_error_flag) return;
                gen_pop_ecx();              // ecx = oldval (left operand)
                if (op == T_PLUSEQ) gen_add_eax_ecx();
                else if (op == T_MINUSEQ) gen_sub_ecx_eax_then_mov();
                else if (op == T_STAREQ) gen_imul_eax_ecx();
                else if (op == T_SLASHEQ) gen_div_setup_and_idiv();
                else { gen_div_setup_and_idiv(); emit_u8(0x89); emit_u8(0xD0); } // %=
                gen_pop_ecx();              // ecx = element address
                gen_mov_indirect_ecx_eax();
            }
            return;
        }

        if (cur_tok.type == T_ASSIGN) {
            next_token();
            parse_assign();
            if (cc_error_flag) return;
            gen_var_store_from_eax(name, line);
            return;
        }
        if (cur_tok.type == T_PLUSEQ || cur_tok.type == T_MINUSEQ || cur_tok.type == T_STAREQ ||
            cur_tok.type == T_SLASHEQ || cur_tok.type == T_PERCENTEQ) {
            token_type_t op = cur_tok.type;
            next_token();
            gen_var_load_to_eax(name, line); // old value -> eax
            if (cc_error_flag) return;
            gen_push_eax();
            parse_assign(); // RHS -> eax
            if (cc_error_flag) return;
            gen_pop_ecx(); // ecx = old value (left operand)
            if (op == T_PLUSEQ) gen_add_eax_ecx();
            else if (op == T_MINUSEQ) gen_sub_ecx_eax_then_mov();
            else if (op == T_STAREQ) gen_imul_eax_ecx();
            else if (op == T_SLASHEQ) gen_div_setup_and_idiv();
            else { gen_div_setup_and_idiv(); emit_u8(0x89); emit_u8(0xD0); } // %=
            gen_var_store_from_eax(name, line);
            return;
        }

        lexer_restore(save);
    }

    // Pointer dereference assignment: *IDENT = expr (plain '=' only -
    // compound-assign through a pointer isn't supported in this subset).
    if (cur_tok.type == T_STAR) {
        lexer_state_t save = lexer_save();
        next_token();
        if (cur_tok.type == T_IDENT) {
            char name[64];
            int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
            int line = cur_tok.line;
            next_token();
            if (cur_tok.type == T_ASSIGN) {
                next_token();
                gen_var_load_to_eax(name, line); // pointer's value = target address
                if (cc_error_flag) return;
                gen_push_eax();
                parse_assign(); // RHS -> eax
                if (cc_error_flag) return;
                gen_pop_ecx();
                gen_mov_indirect_ecx_eax();
                return;
            }
        }
        lexer_restore(save);
    }

    parse_logic_or();
}

static void parse_expr(void) { parse_assign(); }

// ------------------------------------------------------------
// Statements
// ------------------------------------------------------------

static void add_local(const char* name, int is_array, int array_len) {
    if (local_count >= MAX_LOCALS) { cc_seterr(cur_tok.line, "too many local variables"); return; }
    if (find_local(name)) { cc_seterr(cur_tok.line, "duplicate variable name"); return; }
    int i = 0; while (name[i] && i < 31) { locals[local_count].name[i] = name[i]; i++; }
    locals[local_count].name[i] = 0;
    int slots = is_array ? array_len : 1;
    next_local_offset -= slots * 4;
    if (next_local_offset < -2048) { cc_seterr(cur_tok.line, "too many/too large local variables (frame too large)"); return; }
    locals[local_count].offset = next_local_offset; // base offset = element 0 (lowest address)
    locals[local_count].is_array = is_array;
    locals[local_count].array_len = array_len;
    local_count++;
}

static void parse_local_decl(void) {
    expect(T_INT, "expected 'int'");
    if (cc_error_flag) return;
    if (cur_tok.type == T_STAR) next_token(); // pointer marker - parsed, no distinct handling needed
    if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected identifier after 'int'"); return; }
    char name[64];
    int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
    next_token();

    if (cur_tok.type == T_LBRACKET) {
        next_token();
        if (cur_tok.type != T_NUM || cur_tok.ival <= 0) { cc_seterr(cur_tok.line, "array size must be a positive constant"); return; }
        int len = cur_tok.ival;
        next_token();
        expect(T_RBRACKET, "expected ']'");
        if (cc_error_flag) return;
        add_local(name, 1, len); // no initializer support for local arrays
        return;
    }

    add_local(name, 0, 0);
    if (cc_error_flag) return;
    if (cur_tok.type == T_ASSIGN) {
        next_token();
        parse_expr();
        if (cc_error_flag) return;
        local_t* l = find_local(name);
        gen_mov_ebp_disp32_eax(l->offset);
    }
}

static void parse_block(void);

static void parse_stmt(void) {
    if (cc_error_flag) return;
    if (cur_tok.type == T_LBRACE) { parse_block(); return; }

    if (cur_tok.type == T_IF) {
        next_token();
        expect(T_LPAREN, "expected '(' after if"); if (cc_error_flag) return;
        parse_expr(); if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;
        gen_test_eax_eax();
        uint32_t jz_at = gen_jz_placeholder();
        parse_stmt(); if (cc_error_flag) return;
        if (cur_tok.type == T_ELSE) {
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jz_at);
            next_token();
            parse_stmt(); if (cc_error_flag) return;
            patch_jump_here(jmp_at);
        } else {
            patch_jump_here(jz_at);
        }
        return;
    }

    if (cur_tok.type == T_WHILE) {
        next_token();
        uint32_t loop_start = code_len;
        expect(T_LPAREN, "expected '(' after while"); if (cc_error_flag) return;
        parse_expr(); if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;
        gen_test_eax_eax();
        uint32_t jz_at = gen_jz_placeholder();

        loop_ctx_t ctx; ctx.break_count = 0; ctx.continue_count = 0;
        ctx.continue_target_known = 1; ctx.continue_target = loop_start;
        loop_ctx_t* prev_loop = cur_loop; cur_loop = &ctx;
        parse_stmt();
        cur_loop = prev_loop;
        if (cc_error_flag) return;

        gen_jmp_to(loop_start);
        patch_jump_here(jz_at);
        { int bi; for (bi = 0; bi < ctx.break_count; bi++) patch_jump_here(ctx.break_patches[bi]); }
        return;
    }

    if (cur_tok.type == T_FOR) {
        next_token();
        expect(T_LPAREN, "expected '(' after for"); if (cc_error_flag) return;
        if (cur_tok.type != T_SEMI) {
            if (cur_tok.type == T_INT) parse_local_decl();
            else parse_expr();
            if (cc_error_flag) return;
        }
        expect(T_SEMI, "expected ';' after for-init"); if (cc_error_flag) return;

        uint32_t cond_start = code_len;
        int has_cond = (cur_tok.type != T_SEMI);
        uint32_t jz_at = 0;
        if (has_cond) {
            parse_expr(); if (cc_error_flag) return;
            gen_test_eax_eax();
            jz_at = gen_jz_placeholder();
        }
        expect(T_SEMI, "expected ';' after for-condition"); if (cc_error_flag) return;

        // Skip the increment expression's tokens for now (no codegen);
        // remember its source range and re-parse it after the body.
        uint32_t incr_start_byte = cur_tok.start_pos;
        int incr_start_line = cur_tok.line;
        int has_incr = (cur_tok.type != T_RPAREN);
        if (has_incr) {
            int depth = 0;
            for (;;) {
                if (cur_tok.type == T_LPAREN) depth++;
                else if (cur_tok.type == T_RPAREN) { if (depth == 0) break; depth--; }
                else if (cur_tok.type == T_EOF) { cc_seterr(cur_tok.line, "unterminated for(...)"); return; }
                next_token();
            }
        }
        expect(T_RPAREN, "expected ')' after for-clauses"); if (cc_error_flag) return;

        // continue jumps here: run the increment, then fall through to
        // re-checking the condition - correct C for-loop continue
        // semantics. This address isn't known until after the body is
        // parsed, so continue statements inside the body get a
        // backpatch placeholder instead of a direct jump (see the
        // continue-statement handler below).
        loop_ctx_t ctx; ctx.break_count = 0; ctx.continue_count = 0;
        ctx.continue_target_known = 0;
        loop_ctx_t* prev_loop = cur_loop; cur_loop = &ctx;
        parse_stmt();
        cur_loop = prev_loop;
        if (cc_error_flag) return;

        uint32_t incr_label = code_len;
        { int ci; for (ci = 0; ci < ctx.continue_count; ci++) patch_jump_here(ctx.continue_patches[ci]); }
        if (has_incr) {
            lexer_state_t save = lexer_save();
            src_pos = incr_start_byte;
            cur_line = incr_start_line;
            next_token();
            parse_expr();
            if (cc_error_flag) return;
            lexer_restore(save);
        }
        (void)incr_label;

        gen_jmp_to(cond_start);
        if (has_cond) patch_jump_here(jz_at);
        { int bi; for (bi = 0; bi < ctx.break_count; bi++) patch_jump_here(ctx.break_patches[bi]); }
        return;
    }

    if (cur_tok.type == T_BREAK) {
        next_token();
        expect(T_SEMI, "expected ';' after break"); if (cc_error_flag) return;
        if (!cur_loop) { cc_seterr(cur_tok.line, "'break' outside a loop"); return; }
        if (cur_loop->break_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many break statements in one loop"); return; }
        cur_loop->break_patches[cur_loop->break_count++] = gen_jmp_placeholder();
        return;
    }

    if (cur_tok.type == T_CONTINUE) {
        next_token();
        expect(T_SEMI, "expected ';' after continue"); if (cc_error_flag) return;
        if (!cur_loop) { cc_seterr(cur_tok.line, "'continue' outside a loop"); return; }
        if (cur_loop->continue_target_known) {
            gen_jmp_to(cur_loop->continue_target);
        } else {
            if (cur_loop->continue_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many continue statements in one loop"); return; }
            cur_loop->continue_patches[cur_loop->continue_count++] = gen_jmp_placeholder();
        }
        return;
    }

    if (cur_tok.type == T_RETURN) {
        next_token();
        if (cur_tok.type != T_SEMI) { parse_expr(); if (cc_error_flag) return; }
        else gen_mov_eax_imm32(0);
        expect(T_SEMI, "expected ';' after return"); if (cc_error_flag) return;
        emit_u8(0xC9); // leave
        emit_u8(0xC3); // ret
        return;
    }

    if (cur_tok.type == T_INT) {
        parse_local_decl();
        expect(T_SEMI, "expected ';' after declaration");
        return;
    }

    if (cur_tok.type == T_SEMI) { next_token(); return; }

    parse_expr();
    expect(T_SEMI, "expected ';'");
}

static void parse_block(void) {
    expect(T_LBRACE, "expected '{'");
    while (!cc_error_flag && cur_tok.type != T_RBRACE && cur_tok.type != T_EOF) {
        parse_stmt();
    }
    expect(T_RBRACE, "expected '}'");
}

// ------------------------------------------------------------
// Top level: functions and globals
// ------------------------------------------------------------

static void parse_function(const char* name) {
    if (func_count >= MAX_FUNCS) { cc_seterr(cur_tok.line, "too many functions"); return; }

    func_t* fn = &funcs[func_count++];
    int i = 0; while (name[i] && i < 31) { fn->name[i] = name[i]; i++; } fn->name[i] = 0;
    fn->addr = (uint32_t)code_buf + code_len;
    cur_func_start = fn->addr;

    local_count = 0;
    next_local_offset = 0;
    next_param_offset = 8;

    // prologue
    emit_u8(0x55);             // push ebp
    emit_u8(0x89); emit_u8(0xE5); // mov ebp, esp
    uint32_t frame_patch = code_len;
    emit_u8(0x81); emit_u8(0xEC); emit_u32(0); // sub esp, <patched later>

    int nparams = 0;
    if (cur_tok.type == T_VOID) {
        // "(void)" - explicit empty parameter list, matching standard
        // C convention for "this function takes no arguments".
        next_token();
    } else if (cur_tok.type != T_RPAREN) {
        for (;;) {
            expect(T_INT, "expected 'int' in parameter list"); if (cc_error_flag) return;
            if (cur_tok.type == T_STAR) next_token(); // pointer param, parsed and ignored (plain int under the hood)
            if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected parameter name"); return; }
            char pname[64];
            int j = 0; while (cur_tok.sval[j]) { pname[j] = cur_tok.sval[j]; j++; } pname[j] = 0;
            next_token();
            if (local_count >= MAX_LOCALS) { cc_seterr(cur_tok.line, "too many parameters"); return; }
            int k = 0; while (pname[k] && k < 31) { locals[local_count].name[k] = pname[k]; k++; }
            locals[local_count].name[k] = 0;
            locals[local_count].offset = next_param_offset;
            locals[local_count].is_array = 0;
            locals[local_count].array_len = 0;
            next_param_offset += 4;
            local_count++;
            nparams++;
            if (cur_tok.type == T_COMMA) { next_token(); continue; }
            break;
        }
    }
    fn->nparams = nparams;
    expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;

    // Call sites push arguments left-to-right (evaluation order), which
    // is the reverse of what's needed for the first-declared parameter
    // to land at [ebp+8]. Fix up the offsets we assigned above (which
    // assumed sequential 8,12,16,...) to match the actual layout:
    // the LAST-pushed (last-declared) parameter is the one closest to
    // the return address.
    {
        int k;
        for (k = 0; k < nparams; k++) {
            locals[k].offset = 4 * nparams + 12 - locals[k].offset;
        }
    }

    parse_block();
    if (cc_error_flag) return;

    // implicit "return 0;" if the function falls through without one
    gen_mov_eax_imm32(0);
    emit_u8(0xC9); // leave
    emit_u8(0xC3); // ret

    // Patch the stack frame size using the actual accumulated offset
    // (not local_count*4 - arrays can use many slots per declaration,
    // so a per-declaration count would undercount the real space needed).
    patch_u32(frame_patch + 2, (uint32_t)(-next_local_offset + 16)); // +16 slack, cheap and safe
}

static void parse_program(void) {
    while (!cc_error_flag && cur_tok.type != T_EOF) {
        int is_void = (cur_tok.type == T_VOID);
        if (cur_tok.type != T_INT && !is_void) { cc_seterr(cur_tok.line, "expected 'int' or 'void' (only int globals/functions supported)"); return; }
        next_token();
        int is_ptr = 0;
        if (cur_tok.type == T_STAR) { is_ptr = 1; next_token(); } // parsed and ignored - plain int under the hood
        (void)is_ptr;
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected identifier"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int decl_line = cur_tok.line;
        next_token();

        if (cur_tok.type == T_LPAREN) {
            next_token();
            parse_function(name);
        } else if (is_void) {
            cc_seterr(decl_line, "'void' is only valid as a function return type");
            return;
        } else if (cur_tok.type == T_LBRACKET) {
            next_token();
            if (cur_tok.type != T_NUM || cur_tok.ival <= 0) { cc_seterr(cur_tok.line, "array size must be a positive constant"); return; }
            int len = cur_tok.ival;
            next_token();
            expect(T_RBRACKET, "expected ']'"); if (cc_error_flag) return;
            if (global_count >= MAX_GLOBALS) { cc_seterr(cur_tok.line, "too many globals"); return; }
            if (global_array_pool_used + len > GLOBAL_ARRAY_POOL_SIZE) { cc_seterr(cur_tok.line, "global arrays too large in total"); return; }
            uint32_t base = (uint32_t)&global_array_pool[global_array_pool_used];
            global_array_pool_used += len;
            int j = 0; while (name[j] && j < 31) { globals[global_count].name[j] = name[j]; j++; }
            globals[global_count].name[j] = 0;
            globals[global_count].addr = base;
            globals[global_count].is_array = 1;
            globals[global_count].array_len = len;
            global_count++;
            expect(T_SEMI, "expected ';' after global array declaration");
        } else {
            if (global_count >= MAX_GLOBALS) { cc_seterr(cur_tok.line, "too many globals"); return; }
            int32_t init = 0;
            if (cur_tok.type == T_ASSIGN) {
                next_token();
                int neg = 0;
                if (cur_tok.type == T_MINUS) { neg = 1; next_token(); }
                if (cur_tok.type != T_NUM) { cc_seterr(cur_tok.line, "global initializer must be a constant"); return; }
                init = neg ? -cur_tok.ival : cur_tok.ival;
                next_token();
            }
            globals_data[global_count] = init;
            int j = 0; while (name[j] && j < 31) { globals[global_count].name[j] = name[j]; j++; }
            globals[global_count].name[j] = 0;
            globals[global_count].addr = (uint32_t)&globals_data[global_count];
            globals[global_count].is_array = 0;
            globals[global_count].array_len = 0;
            global_count++;
            expect(T_SEMI, "expected ';' after global declaration");
        }
    }
}

// ------------------------------------------------------------
// Builtin runtime helpers (called directly from generated code)
// ------------------------------------------------------------

void cc_print_int(int32_t n) {
    if (n < 0) { putc('-'); n = -n; }
    print_dec((uint32_t)n);
}

// ------------------------------------------------------------
// Entry point
// ------------------------------------------------------------

// Debug/test accessor - lets a host-side test harness get the code
// buffer's address and length to inspect (e.g. disassemble) what was
// generated, without needing to execute it. Not used by the kernel.
void* cc_debug_codebuf(void) { return code_buf; }
unsigned int cc_debug_codebuf_size(void) { return CODE_BUF_SIZE; }
unsigned int cc_debug_codelen(void) { return code_len; }

// Returns 0 on success (main() found and ready to call), -1 on
// failure (error already printed). Doesn't execute anything - split
// out from cc_compile_and_run so host-side tooling can inspect
// generated code without ever jumping into it (this kernel has no
// paging, so 32-bit machine code sitting in a plain buffer is safe to
// execute there; on a 64-bit host it would not be).
static int cc_compile(const char* source, unsigned int len, func_t** out_mainfn) {
    src = source; src_len = len; src_pos = 0; cur_line = 1;
    code_len = 0;
    cc_error_flag = 0; cc_error_line = 0; cc_error_msg[0] = 0;
    local_count = 0; func_count = 0; global_count = 0; token_count = 0;

    next_token();
    parse_program();

    if (cc_error_flag) {
        print("Compile error (line ");
        print_dec((uint32_t)cc_error_line);
        print("): ");
        print(cc_error_msg);
        print("\n");
        return -1;
    }

    func_t* mainfn = find_func("main");
    if (!mainfn) {
        print("Compile error: no main() function\n");
        return -1;
    }
    *out_mainfn = mainfn;
    return 0;
}

// Test-only entry point: compiles but never executes anything, safe
// to call from a host-side test harness on any host architecture.
int cc_compile_only(const char* source, unsigned int len) {
    func_t* mainfn;
    return cc_compile(source, len, &mainfn);
}

void cc_compile_and_run(const char* source, unsigned int len) {
    func_t* mainfn;
    if (cc_compile(source, len, &mainfn) != 0) return;

    int (*entry)(void) = (int (*)(void))(void*)mainfn->addr;
    int rc = entry();
    print("Program exited with code ");
    print_dec((uint32_t)rc);
    print("\n");
}
