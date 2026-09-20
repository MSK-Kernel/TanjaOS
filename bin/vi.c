#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

#define ESC_KEY 27
#define TAB_KEY 9
#define CTRL(c) ((c) & 0x1F)
#define MAX_TEXT (MAX_FILE_SIZE + 1)

#define VI_TOP_ROW 0
#define VI_BOTTOM_ROW 23
#define VI_ROWS (VI_BOTTOM_ROW - VI_TOP_ROW + 1)
#define VI_COLS 80
#define VI_STATUS_ROW 24
#define TAB_WIDTH 4
#define SHIFT_WIDTH 4
#define VGA_HEIGHT 25
#define VGA_TOTAL_CELLS (VI_COLS * VGA_HEIGHT)
#define VGA_COLOR (0x0F << 8)
#define VGA_REVERSE (0x70 << 8)

#define MODE_COMMAND 0
#define MODE_INSERT  1
#define MODE_REPLACE 2

/* ============================================================
 * GAP BUFFER
 * ============================================================ */
typedef struct {
    char data[MAX_TEXT];
    int gap_start;
    int gap_end;
    int len;
} EditorBuffer;

static EditorBuffer eb;

static char text_at(int pos)
{
    if (pos < 0 || pos >= eb.len) return 0;
    if (pos < eb.gap_start) return eb.data[pos];
    return eb.data[pos + (eb.gap_end - eb.gap_start)];
}

static void move_gap(int pos)
{
    int i;
    if (pos < 0) pos = 0;
    if (pos > eb.len) pos = eb.len;
    if (pos == eb.gap_start) return;
    if (pos < eb.gap_start) {
        int n = eb.gap_start - pos;
        for (i = n - 1; i >= 0; i--)
            eb.data[eb.gap_end - n + i] = eb.data[pos + i];
        eb.gap_start -= n;
        eb.gap_end -= n;
    } else {
        int n = pos - eb.gap_start;
        for (i = 0; i < n; i++)
            eb.data[eb.gap_start + i] = eb.data[eb.gap_end + i];
        eb.gap_start += n;
        eb.gap_end += n;
    }
}

static int insert_byte(int pos, char ch)
{
    if (eb.len >= MAX_TEXT - 1) return 0;
    if (eb.gap_end <= eb.gap_start) return 0;
    move_gap(pos);
    eb.data[eb.gap_start++] = ch;
    eb.len++;
    return 1;
}

static void delete_range(int pos, int count)
{
    if (count <= 0 || pos < 0 || pos >= eb.len) return;
    if (pos + count > eb.len) count = eb.len - pos;
    move_gap(pos);
    eb.gap_end += count;
    eb.len -= count;
}

static void flatten(char *out)
{
    int i;
    for (i = 0; i < eb.len; i++) out[i] = text_at(i);
    out[eb.len] = 0;
}

static int insert_bytes(int pos, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!insert_byte(pos + i, s[i])) break;
    return i;
}

/* ============================================================
 * UTF-8 (gap-aware decode)
 * ============================================================ */
static int decode_at(int pos, uint32_t *cp)
{
    unsigned char b0;
    int need, i;
    uint32_t c;

    *cp = UTF8_INVALID;
    if (pos < 0 || pos >= eb.len) return 1;
    b0 = (unsigned char)text_at(pos);
    if (b0 < 0x80) { *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0) { c = b0 & 0x1F; need = 1; if (c < 2) return 1; }
    else if ((b0 & 0xF0) == 0xE0) { c = b0 & 0x0F; need = 2; }
    else if ((b0 & 0xF8) == 0xF0) { c = b0 & 0x07; need = 3; }
    else return 1;
    if (pos + need >= eb.len) return 1;
    for (i = 1; i <= need; i++) {
        unsigned char b = (unsigned char)text_at(pos + i);
        if ((b & 0xC0) != 0x80) return 1;
        c = (c << 6) | (b & 0x3F);
    }
    if ((need == 2 && c < 0x800) || (need == 3 && c < 0x10000) ||
        (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return 1;
    *cp = c;
    return need + 1;
}

static int cp_len(int pos)
{
    uint32_t cp;
    int n = decode_at(pos, &cp);
    return n < 1 ? 1 : n;
}

static int step_forward(int pos)
{
    if (pos >= eb.len) return pos;
    return pos + cp_len(pos);
}

static int step_backward(int pos)
{
    int i;
    if (pos <= 0) return pos;
    i = pos - 1;
    while (i > 0 && ((unsigned char)text_at(i) & 0xC0) == 0x80) i--;
    return i;
}

/* ============================================================
 * LINES / COLUMNS / WRAP
 * ============================================================ */
static int g_cols = VI_COLS;          /* text width (80, or 72 with nu) */
static int g_gutter = 0;              /* 8 when line numbers shown */
static int g_pos = 0, g_scroll = 0;
static int g_mode = MODE_COMMAND;

static int line_start(int pos)
{
    while (pos > 0 && text_at(pos - 1) != '\n') pos--;
    return pos;
}

static int line_end(int pos)
{
    while (pos < eb.len && text_at(pos) != '\n') pos++;
    return pos;
}

static int line_start_number(int wanted)
{
    int line = 0, i = 0;
    while (line < wanted && i < eb.len) { if (text_at(i) == '\n') line++; i++; }
    return i;
}

static int line_end_number(int n)
{
    return line_end(line_start_number(n));
}

static int line_number_at(int pos)
{
    int line = 0, i;
    for (i = 0; i < pos; i++) if (text_at(i) == '\n') line++;
    return line;
}

static int total_lines(void)
{
    if (eb.len == 0) return 1;
    { int lines = 1, i;
      for (i = 0; i < eb.len; i++) if (text_at(i) == '\n') lines++;
      if (text_at(eb.len - 1) == '\n') lines++;
      return lines; }
}

static int visual_col(int pos)
{
    int col = 0, i = line_start(pos);
    while (i < pos && text_at(i) != '\n') {
        if (text_at(i) == '\t') { col += TAB_WIDTH - (col % TAB_WIDTH); i++; }
        else { col++; i += cp_len(i); }
    }
    return col;
}

static int snap_to_tab_stop(int pos)
{
    int start = line_start(pos), col = visual_col(pos), i;
    int all_spaces = 1;
    for (i = start; i < pos; i++) if (text_at(i) != ' ') { all_spaces = 0; break; }
    if (all_spaces) {
        int snapped = (col / TAB_WIDTH) * TAB_WIDTH;
        int p = start, c = 0;
        while (p < pos && c < snapped) { p++; c++; }
        return p;
    }
    return pos;
}

static int pos_at_visual_col(int start, int target)
{
    int pos = start, col = 0;
    while (pos < eb.len && text_at(pos) != '\n') {
        int width, step;
        if (text_at(pos) == '\t') { width = TAB_WIDTH - (col % TAB_WIDTH); step = 1; }
        else { step = cp_len(pos); width = 1; }
        if (target < col + width) return snap_to_tab_stop(pos);
        col += width; pos += step;
    }
    return snap_to_tab_stop(pos);
}

static int visual_rows_of_line(int ls)
{
    int width = visual_col(line_end(ls));
    if (width == 0) return 1;
    return (width - 1) / g_cols + 1;
}

static int move_vertical(int pos, int dir)
{
    int c = visual_col(pos), row = c / g_cols, scol = c % g_cols;
    int ls = line_start(pos), width = visual_col(line_end(ls));
    if (dir < 0) {
        if (row > 0) return pos_at_visual_col(ls, (row - 1) * g_cols + scol);
        { int line = line_number_at(pos); if (line == 0) return pos;
          { int prev = line_start_number(line - 1); int rows = visual_rows_of_line(prev);
            return pos_at_visual_col(prev, (rows - 1) * g_cols + scol); } }
    }
    if ((row + 1) * g_cols < width)
        return pos_at_visual_col(ls, (row + 1) * g_cols + scol);
    { int e = line_end(pos); if (e >= eb.len) return pos; return pos_at_visual_col(e + 1, scol); }
}

static int first_nonblank(int pos)
{
    int ls = line_start(pos);
    while (ls < eb.len && (text_at(ls) == ' ' || text_at(ls) == '\t')) ls++;
    if (ls >= eb.len || text_at(ls) == '\n') return line_start(pos);
    return ls;
}

static int marks[27];                 /* 0..25 = a..z, 26 = last jump */
static int sesslen = 0;
static int sesscount = 1;

static int goto_line(int line, int setjump)
{
    int total = total_lines();
    if (line < 0) line = 0;
    if (line >= total) line = total - 1;
    if (setjump) marks[26] = g_pos;
    g_pos = first_nonblank(line_start_number(line));
    return g_pos;
}

/* ============================================================
 * STATE: options, message, registers, marks, undo, repeat
 * ============================================================ */
static char g_file[MAX_PATH];
static int g_modified = 0;
static int opt_nu = 0, opt_ai = 0, opt_ic = 0;
static const char *vi_msg = 0;

#define REG_CAP 16384
typedef struct { char data[REG_CAP]; int len; int lines; } Reg;
static Reg regs[36];                   /* 0='" 1..26=a..z 27..35=1..9 */
static int reg_index(int c)
{
    if (c == '"') return 0;
    if (c >= 'a' && c <= 'z') return 1 + (c - 'a');
    if (c >= '1' && c <= '9') return 27 + (c - '1');
    return -1;
}
static void reg_store(int c, const char *data, int len, int lines)
{
    int idx = reg_index(c);
    Reg *r;
    if (idx < 0) return;
    r = &regs[idx];
    if (len > REG_CAP) { len = REG_CAP; vi_msg = "register too long, clipped"; }
    r->len = len; r->lines = lines;
    { int i; for (i = 0; i < len; i++) r->data[i] = data[i]; }
}
static void reg_store_both(int c, const char *data, int len, int lines)
{
    reg_store('"', data, len, lines);
    if (c != '"') reg_store(c, data, len, lines);
}
static void reg_shift_numbered(void)
{
    int i;
    for (i = 8; i >= 1; i--) regs[27 + i] = regs[27 + i - 1];
}

static void clamp_marks(void)
{
    int i;
    for (i = 0; i < 27; i++) if (marks[i] > eb.len) marks[i] = eb.len;
}

#define UNDO_DEPTH 6
typedef struct { int len; int pos; int scroll; char data[MAX_TEXT]; } Snap;
static Snap undo_ring[UNDO_DEPTH], redo_ring[UNDO_DEPTH];
static int undo_cnt = 0, undo_head = 0;
static int redo_cnt = 0, redo_head = 0;

static void push_undo(void)
{
    Snap *s = &undo_ring[undo_head];
    flatten(s->data);
    s->len = eb.len; s->pos = g_pos; s->scroll = g_scroll;
    undo_head = (undo_head + 1) % UNDO_DEPTH;
    if (undo_cnt < UNDO_DEPTH) undo_cnt++;
    redo_cnt = 0;                         /* a new change kills redo */
    redo_head = 0;
}
static void snap_restore(Snap *s)
{
    int i;
    eb.len = s->len;
    for (i = 0; i < s->len; i++) eb.data[i] = s->data[i];
    eb.gap_start = s->len; eb.gap_end = MAX_TEXT - 1;
    eb.data[eb.gap_end] = 0;
    g_pos = s->pos; g_scroll = s->scroll;
    if (g_pos > eb.len) g_pos = eb.len;
}
static int do_undo(void)
{
    if (undo_cnt == 0) { vi_msg = "Already at oldest change"; return 0; }
    { Snap *r = &redo_ring[redo_head];
      flatten(r->data);
      r->len = eb.len; r->pos = g_pos; r->scroll = g_scroll;
      redo_head = (redo_head + 1) % UNDO_DEPTH;
      if (redo_cnt < UNDO_DEPTH) redo_cnt++; }
    undo_head = (undo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    undo_cnt--;
    snap_restore(&undo_ring[undo_head]);
    clamp_marks();
    g_modified = 1;
    vi_msg = "undo";
    return 1;
}
static int do_redo(void)
{
    if (redo_cnt == 0) { vi_msg = "Nothing to redo"; return 0; }
    { Snap *s = &undo_ring[undo_head];
      flatten(s->data);
      s->len = eb.len; s->pos = g_pos; s->scroll = g_scroll;
      undo_head = (undo_head + 1) % UNDO_DEPTH;
      if (undo_cnt < UNDO_DEPTH) undo_cnt++; }
    redo_head = (redo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    redo_cnt--;
    snap_restore(&redo_ring[redo_head]);
    clamp_marks();
    g_modified = 1;
    vi_msg = "redo";
    return 1;
}

static void change_begin(void) { push_undo(); g_modified = 1; }

/* record of the last change for '.' */
enum { LC_NONE, LC_INS, LC_OVER, LC_DEL, LC_REPL, LC_JOIN, LC_CASE,
       LC_SHIFT_R, LC_SHIFT_L, LC_SUBST };
typedef struct {
    int kind, count, motion, mode;      /* LC_DEL: motion key + mode */
    char ch;                            /* LC_REPL: the char */
    char text[4096]; int tlen;          /* LC_INS / LC_OVER: typed bytes */
} LastChange;
static LastChange lastch;

/* ============================================================
 * WORD / PARAGRAPH / SENTENCE / CHAR-FIND MOTIONS
 * ============================================================ */
static int chclass(char c)
{
    if (c == ' ' || c == '\t' || c == '\n' || !c) return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') return 1;
    return 2;
}

static int fwd_word_start(int pos, int count, int big)
{
    while (count-- > 0) {
        if (pos >= eb.len) break;
        { int cls = chclass(text_at(pos));
          if (cls != 0) {
              while (pos < eb.len && chclass(text_at(pos)) == cls) pos++;
              if (big) while (pos < eb.len && chclass(text_at(pos)) != 0) pos++;
          }
          while (pos < eb.len && chclass(text_at(pos)) == 0) pos++; }
        if (pos >= eb.len) { pos = eb.len; break; }
    }
    return pos;
}

static int fwd_word_end(int pos, int count, int big)
{
    while (count-- > 0) {
        pos++;
        if (pos >= eb.len) { pos = eb.len; break; }
        while (pos < eb.len && chclass(text_at(pos)) == 0) pos++;
        if (pos >= eb.len) break;
        { int cls = chclass(text_at(pos));
          while (pos + 1 < eb.len && chclass(text_at(pos + 1)) != 0 &&
                 chclass(text_at(pos + 1)) == (big ? chclass(text_at(pos)) : cls)) pos++; }
    }
    return pos;
}

static int back_word_start(int pos, int count, int big)
{
    while (count-- > 0) {
        if (pos <= 0) { pos = 0; break; }
        pos--;
        while (pos > 0 && chclass(text_at(pos)) == 0) pos--;
        if (pos < 0) { pos = 0; break; }
        { int cls = chclass(text_at(pos));
          while (pos > 0 && chclass(text_at(pos - 1)) != 0 &&
                 chclass(text_at(pos - 1)) == (big ? cls : cls)) pos--; }
    }
    return pos;
}

static int para_next(int pos, int count)
{
    while (count-- > 0) {
        int ls = line_start(pos);
        int e = line_end(ls);
        int found = 0;
        while (e < eb.len) {
            int nxt = e + 1;
            if (nxt >= eb.len) break;
            if (line_start(nxt) == nxt && line_end(nxt) == nxt) { pos = nxt; found = 1; break; }
            e = line_end(nxt);
        }
        if (!found) pos = eb.len;
    }
    return pos;
}

static int para_prev(int pos, int count)
{
    while (count-- > 0) {
        int ls = line_start(pos);
        int target = -1;
        if (ls > 0) {
            int p = ls - 1;                        /* the \n above, or empty line */
            { int pl = line_start(p);
              /* skip blank lines upward */
              while (pl == p && pl > 0) { p = pl - 1; pl = line_start(p); }
              if (pl != p || p == 0) {
                  /* p now sits at start of a non-empty line region */
                  int q = line_start(p);
                  while (q > 0) {
                      int prev = q - 1;
                      if (line_start(prev) == prev) { target = prev; break; }
                      q = line_start(prev);
                  }
              }
            }
        }
        pos = target < 0 ? 0 : target;
    }
    return pos;
}

static int is_sentence_end(int pos)
{
    char c = text_at(pos);
    if (c != '.' && c != '!' && c != '?') return 0;
    if (pos + 1 >= eb.len) return 1;
    { char n = text_at(pos + 1);
      return n == ' ' || n == '\t' || n == '\n'; }
}

static int sent_next(int pos, int count)
{
    while (count-- > 0) {
        int found = 0;
        while (pos < eb.len) {
            char c = text_at(pos);
            if (c == '\n') {
                int nxt = pos + 1;
                if (nxt >= eb.len) { pos = eb.len; found = 1; break; }
                if (line_end(nxt) == nxt) { pos = nxt; found = 1; break; }
            }
            if (is_sentence_end(pos)) {
                pos++;
                while (pos < eb.len && (text_at(pos) == ' ' || text_at(pos) == '\t' ||
                                        text_at(pos) == '\n')) pos++;
                found = 1;
                break;
            }
            pos++;
        }
        if (!found) pos = eb.len;
    }
    return pos;
}

static int sent_prev(int pos, int count)
{
    while (count-- > 0) {
        int scan = pos - 1, found = 0;
        while (scan >= 0) {
            if (is_sentence_end(scan)) {
                int e = scan + 1;
                while (e < eb.len && (text_at(e) == ' ' || text_at(e) == '\t' ||
                                      text_at(e) == '\n')) e++;
                pos = e; found = 1; break;
            }
            if (text_at(scan) == '\n' && scan + 1 < pos &&
                line_end(scan + 1) == scan + 1) {
                pos = scan + 1; found = 1; break;
            }
            scan--;
        }
        if (!found) pos = 0;
    }
    return pos;
}

static int find_char_fwd(int pos, char ch, int count, int till, int *ok)
{
    int le = line_end(pos);
    *ok = 0;
    while (count-- > 0) {
        int p = pos + 1;
        while (p < le && text_at(p) != ch) p++;
        if (p >= le) return pos;
        pos = till ? p - 1 : p;
        *ok = 1;
    }
    return pos;
}

static int find_char_back(int pos, char ch, int count, int till, int *ok)
{
    int ls = line_start(pos);
    *ok = 0;
    while (count-- > 0) {
        int p = pos - 1;
        while (p >= ls && text_at(p) != ch) p--;
        if (p < ls) return pos;
        pos = till ? p + 1 : p;
        *ok = 1;
    }
    return pos;
}

static int is_open_bracket(char c)  { return c == '(' || c == '[' || c == '{'; }
static int is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }
static char bracket_match(char c)
{
    switch (c) {
    case '(': return ')';  case '[': return ']';  case '{': return '}';
    case ')': return '(';  case ']': return '[';  case '}': return '{';
    }
    return 0;
}

static int match_bracket(int pos)
{
    char c = text_at(pos), m;
    int depth, i;
    if (!is_open_bracket(c) && !is_close_bracket(c)) {
        int le = line_end(pos);
        for (i = pos; i < le; i++) {
            char t = text_at(i);
            if (is_open_bracket(t) || is_close_bracket(t)) { c = t; pos = i; break; }
        }
        if (!is_open_bracket(c) && !is_close_bracket(c)) return -1;
    }
    m = bracket_match(c);
    depth = 1;
    if (is_open_bracket(c)) {
        for (i = pos + 1; i < eb.len; i++) {
            char t = text_at(i);
            if (t == c) depth++;
            else if (t == m && --depth == 0) return i;
        }
    } else {
        for (i = pos - 1; i >= 0; i--) {
            char t = text_at(i);
            if (t == c) depth++;
            else if (t == m && --depth == 0) return i;
        }
    }
    return -1;
}

/* ============================================================
 * MINI REGEX  (for / ? :s)
 * Supports: literals . [...] * ^ $ \(..\); \-escapes; ic option.
 * Greedy backtracking with a step budget.  Groups recorded in
 * rx_spans for \1..\9 in substitute replacements.
 * ============================================================ */
static int rx_spans[10][2];
static const char *rx_base;
static int rx_budget;

static int rx_fold(int c)
{
    if (opt_ic && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int rx_group_close(const char *pat, int i)
{
    int depth = 1;
    while (pat[i]) {
        if (pat[i] == '\\' && pat[i + 1]) {
            if (pat[i + 1] == '(') depth++;
            else if (pat[i + 1] == ')') { depth--; if (!depth) return i; }
            i += 2;
        } else i++;
    }
    return -1;
}

static int rx_group_id(const char *pat, int pi)
{
    int gid = 0, i = 0;
    while (pat[i] && i < pi) {
        if (pat[i] == '\\' && pat[i + 1]) {
            if (pat[i + 1] == '(') gid++;
            i += 2;
        } else i++;
    }
    return gid + 1;
}

static int rx_class(const char *pat, int *pi, const char *s)
{
    int neg = 0, i = *pi, ok = 0;
    if (pat[i] == '^') { neg = 1; i++; }
    if (pat[i] == ']') { if (s[0] == ']') ok = 1; i++; }
    while (pat[i] && pat[i] != ']') {
        if (pat[i + 1] == '-' && pat[i + 2] && pat[i + 2] != ']') {
            if (rx_fold(s[0]) >= rx_fold(pat[i]) && rx_fold(s[0]) <= rx_fold(pat[i + 2]))
                ok = 1;
            i += 3;
        } else {
            if (rx_fold(s[0]) == rx_fold(pat[i])) ok = 1;
            i++;
        }
    }
    if (pat[i] == ']') i++;
    *pi = i;
    if (!s[0]) return -1;
    if (neg) ok = !ok;
    return ok ? 1 : -1;
}

/* one single-char atom: consumes at most 1 byte of s.
 * returns 1 (match) or -1; *pi advances past the atom. */
static int rx_atom(const char *pat, int *pi, const char *s)
{
    int c = pat[(*pi)++];
    if (c == '.') return s[0] ? 1 : -1;
    if (c == '[') return rx_class(pat, pi, s);
    if (c == '\\' && pat[*pi]) {
        int e = pat[(*pi)++];
        return rx_fold(s[0]) == rx_fold(e) ? 1 : -1;
    }
    return rx_fold(s[0]) == rx_fold(c) ? 1 : -1;
}

/* pattern extent (chars) of the atom starting at pi */
static int rx_atom_extent(const char *pat, int pi)
{
    if (pat[pi] == '[') {
        int q = pi + 1;
        if (pat[q] == '^') q++;
        if (pat[q] == ']') q++;
        while (pat[q] && pat[q] != ']') q++;
        if (pat[q]) q++;
        return q - pi;
    }
    if (pat[pi] == '\\' && pat[pi + 1]) return 2;
    return 1;
}

/* match pat[pi..stop) against s; exact >= 0 = must consume exactly
 * that many bytes.  On success sets *slen. */
static int rxm(const char *pat, int pi, int stop, const char *s, int exact, int *slen)
{
    int c, i;

    if (rx_budget-- <= 0) return 0;
    if (pi == stop) {
        if (exact < 0) { *slen = 0; return 1; }
        return exact == 0;
    }
    c = pat[pi];
    if (c == '^' && pi == 0) {
        if (s != rx_base) return 0;
        return rxm(pat, 1, stop, s, exact, slen);
    }
    if (c == '$' && pi + 1 == stop) {
        if (s[0]) return 0;
        if (exact >= 0 && exact != 0) return 0;
        *slen = 0;
        return 1;
    }
    if (c == '\\' && pi + 1 < stop && pat[pi + 1] == '(') {
        int close = rx_group_close(pat, pi + 2);
        int gid, body_pi, body_stop, after, star, rest_pi;
        int span_off = (int)(s - rx_base);
        if (close < 0) return 0;
        gid = rx_group_id(pat, pi);
        if (gid < 1 || gid > 9) gid = 0;
        body_pi = pi + 2; body_stop = close;
        after = close + 2;
        star = (after < stop && pat[after] == '*');
        rest_pi = star ? after + 1 : after;
        if (!star) {
            int maxe = 0;
            int bl, rl;
            while (s[maxe]) maxe++;
            if (exact >= 0 && exact < maxe) maxe = exact;
            for (i = maxe; i >= 0; i--) {           /* greedy */
                if (!rxm(pat, body_pi, body_stop, s, i, &bl)) continue;
                if (rxm(pat, rest_pi, stop, s + i, exact < 0 ? -1 : exact - i, &rl)) {
                    *slen = i + rl;
                    if (gid) { rx_spans[gid][0] = span_off; rx_spans[gid][1] = span_off + i; }
                    return 1;
                }
            }
            return 0;
        }
        /* \(..\)* : greedy one iteration at a time */
        { int bl = 0;
          if (rxm(pat, body_pi, body_stop, s, exact < 0 ? -1 : exact, &bl)) {
              if (gid) { rx_spans[gid][0] = span_off; rx_spans[gid][1] = span_off + bl; }
              if (rxm(pat, pi, stop, s + bl, exact < 0 ? -1 : exact - bl, slen)) {
                  *slen += bl;
                  return 1;
              }
          }
          if (rxm(pat, rest_pi, stop, s, exact, slen)) {
              if (gid) { rx_spans[gid][0] = span_off; rx_spans[gid][1] = span_off; }
              return 1;
          }
          return 0; }
    }
    { int aext = rx_atom_extent(pat, pi);
      int star = (pat[pi + aext] == '*');
      if (star) {
          int rest_pi = pi + aext + 1;
          int kmax = 0;
          const char *t = s;
          while (t[0]) {
              int q = pi;
              if (rx_atom(pat, &q, t) != 1) break;
              t++; kmax++;
          }
          for (i = kmax; i >= 0; i--) {
              int rl = 0;
              if (exact >= 0 && i > exact) continue;
              if (rxm(pat, rest_pi, stop, s + i, exact < 0 ? -1 : exact - i, &rl)) {
                  *slen = i + rl;
                  return 1;
              }
          }
          return 0;
      }
      { int pp = pi;
        int r = rx_atom(pat, &pp, s);
        int rl = 0;
        if (r != 1) return 0;
        if (exact == 0) return 0;
        if (rxm(pat, pp, stop, s + 1, exact < 0 ? -1 : exact - 1, &rl)) {
            *slen = 1 + rl;
            return 1;
        }
        return 0; }
    }
}

/* leftmost match of pat in line[from..]; returns start idx or -1 */
static int rx_find(const char *pat, const char *line, int from, int *mlen)
{
    int i;
    if (from < 0) from = 0;
    for (i = from; line[i]; i++) {
        rx_budget = 60000;
        rx_base = line;
        *mlen = 0;
        if (rxm(pat, 0, (int)strlen(pat), line + i, -1, mlen)) return i;
        if (pat[0] == '^') break;
    }
    return -1;
}

/* build replacement text for one match */
static int rx_build_rep(const char *rep, const char *line, int mstart, int mend,
                        char *out, int cap)
{
    int o = 0, i;
    for (i = 0; rep[i] && o < cap - 1; i++) {
        if (rep[i] == '&') {
            int k;
            for (k = mstart; k < mend && o < cap - 1; k++) out[o++] = line[k];
        } else if (rep[i] == '\\' && rep[i + 1] >= '1' && rep[i + 1] <= '9') {
            int gid = rep[i + 1] - '0';
            int k;
            i++;
            if (rx_spans[gid][1] >= rx_spans[gid][0])
                for (k = rx_spans[gid][0]; k < rx_spans[gid][1] && o < cap - 1; k++)
                    out[o++] = line[k];
        } else if (rep[i] == '\\' && rep[i + 1]) {
            out[o++] = rep[++i];
        } else {
            out[o++] = rep[i];
        }
    }
    out[o] = 0;
    return o;
}

/* ============================================================
 * SCREEN
 * ============================================================ */
static void draw_text(int scroll_line)
{
    int line = scroll_line;
    int start = line_start_number(line);
    int screen_row = VI_TOP_ROW;

    while (screen_row <= VI_BOTTOM_ROW && start <= eb.len) {
        int j = start;
        int col = g_gutter;
        if (opt_nu) {
            char nb[16];
            int k;
            itoa(line + 1, nb, 10);
            { int l = (int)strlen(nb), pad = 7 - l;
              for (k = 0; k < VI_COLS; k++) VGA[screen_row * VI_COLS + k] = VGA_COLOR | ' ';
              for (k = 0; k < pad; k++) VGA[screen_row * VI_COLS + k] = VGA_REVERSE | ' ';
              for (k = 0; k < l; k++)
                  VGA[screen_row * VI_COLS + pad + k] = VGA_REVERSE | (uint8_t)nb[k];
              VGA[screen_row * VI_COLS + 7] = VGA_COLOR | ' '; }
        }
        do {
            while (j < eb.len && text_at(j) != '\n' && col < VI_COLS) {
                if (text_at(j) == '\t') {
                    int width = TAB_WIDTH - (col % TAB_WIDTH), k;
                    for (k = 0; k < width && col < VI_COLS; k++)
                        VGA[screen_row * VI_COLS + col++] = VGA_COLOR | ' ';
                    j++;
                } else {
                    uint32_t cp; int step = decode_at(j, &cp); char cell;
                    if (step < 1) step = 1;
                    if (cp == UTF8_INVALID) cell = text_at(j);
                    else if (cp < 0x80) cell = (char)cp;
                    else { int m2 = utf8_map_ascii(cp); cell = m2 ? (char)m2 : '?'; }
                    VGA[screen_row * VI_COLS + col++] = VGA_COLOR | (uint8_t)cell;
                    j += step;
                }
            }
            while (col < VI_COLS) VGA[screen_row * VI_COLS + col++] = VGA_COLOR | ' ';
            screen_row++;
            col = g_gutter;
            if (opt_nu && screen_row <= VI_BOTTOM_ROW)
                for (int k = 0; k < 8; k++)
                    VGA[screen_row * VI_COLS + k] = VGA_COLOR | ' ';
        } while (j < eb.len && text_at(j) != '\n' && screen_row <= VI_BOTTOM_ROW);
        if (j >= eb.len) break;
        start = j + 1; line++;
    }
}

static void status_str(const char *s)
{
    int i = 0;
    while (i < VI_COLS) {
        uint16_t cell;
        if (s && s[i]) cell = VGA_REVERSE | (uint8_t)s[i];
        else cell = VGA_REVERSE | ' ';
        VGA[VI_STATUS_ROW * VI_COLS + i] = cell;
        i++;
    }
}

static void ensure_visible(int pos, int *scroll_line)
{
    int current_line = line_number_at(pos);
    if (current_line < *scroll_line) *scroll_line = current_line;
    if (*scroll_line < 0) *scroll_line = 0;
    for (;;) {
        int rows = 0, line = *scroll_line, i = line_start_number(line);
        while (line < current_line) {
            rows += visual_rows_of_line(i);
            i = line_end(i); if (i < eb.len && text_at(i) == '\n') i++; line++;
        }
        rows += visual_col(pos) / g_cols;
        if (rows < VI_ROWS || *scroll_line >= current_line) break;
        (*scroll_line)++;
    }
}

static char g_status[VI_COLS + 1];

static void build_status(const char *prompt)
{
    int i = 0;
    const char *s;

    if (prompt) {                       /* : / ? command line */
        s = prompt;
        while (*s && i < VI_COLS) g_status[i++] = *s++;
        while (i < VI_COLS) g_status[i++] = ' ';
        g_status[i] = 0;
        return;
    }
    if (vi_msg) {
        s = vi_msg;
        while (*s && i < VI_COLS) g_status[i++] = *s++;
        while (i < VI_COLS) g_status[i++] = ' ';
        g_status[i] = 0;
        return;
    }
    /* left: filename [+] */
    s = g_file;
    while (*s && i < 40) g_status[i++] = *s++;
    if (g_modified) { g_status[i++] = ' '; g_status[i++] = '+'; }
    g_status[i++] = ' ';
    /* middle: line/col */
    { char t[16], t2[16];
      itoa(line_number_at(g_pos) + 1, t, 10);
      itoa(visual_col(g_pos) + 1, t2, 10);
      { const char *p = "L"; while (*p && i < 52) g_status[i++] = *p++; }
      s = t;   while (*s && i < 52) g_status[i++] = *s++;
      { const char *p = " C"; while (*p && i < 52) g_status[i++] = *p++; }
      s = t2;  while (*s && i < 52) g_status[i++] = *s++; }
    /* right: mode */
    { const char *marker = (g_mode == MODE_INSERT) ? "-- INSERT --" :
                            (g_mode == MODE_REPLACE) ? "-- REPLACE --" : "";
      int mlen = (int)strlen(marker), pad = VI_COLS - mlen - i - 1;
      if (pad < 1) pad = 1;
      while (pad-- > 0 && i < VI_COLS - 1) g_status[i++] = ' ';
      s = marker;
      while (*s && i < VI_COLS) g_status[i++] = *s++; }
    g_status[i] = 0;
}

static void redraw(const char *prompt)
{
    int current_line, rows_before, line_i, i;
    int c, seg, col_offset, final_row;

    ensure_visible(g_pos, &g_scroll);
    clear_screen();
    draw_text(g_scroll);
    build_status(prompt);
    status_str(g_status);
    vi_msg = 0;
    current_line = line_number_at(g_pos);
    rows_before = 0; line_i = g_scroll; i = line_start_number(line_i);
    while (line_i < current_line) {
        rows_before += visual_rows_of_line(i);
        i = line_end(i); if (i < eb.len && text_at(i) == '\n') i++; line_i++;
    }
    c = visual_col(g_pos); seg = c / g_cols; col_offset = c % g_cols;
    final_row = VI_TOP_ROW + rows_before + seg;
    if (final_row > VI_BOTTOM_ROW) final_row = VI_BOTTOM_ROW;
    cursor = final_row * VI_COLS + g_gutter + col_offset;
    if (cursor >= VGA_TOTAL_CELLS) cursor = VGA_TOTAL_CELLS - 1;
    sync_cursor();
}

/* read a ':' '/' or '?' command.  Returns 1 on Enter, 0 on Esc. */
static char g_prompt[VI_COLS + 1];
static int read_prompt(int lead, char *buf, int cap)
{
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int i = 0, j = 0, key;
        g_prompt[i++] = (char)lead;
        while (buf[j] && i < VI_COLS - 1) g_prompt[i++] = buf[j++];
        while (i < VI_COLS) g_prompt[i++] = ' ';
        g_prompt[i] = 0;
        status_str(g_prompt);
        key = get_key();
        if (key == KEY_ENTER || key == '\n') return 1;
        if (key == ESC_KEY) return 0;
        if (key == KEY_BACKSPACE || key == 8) {
            if (n > 0) buf[--n] = 0;
            continue;
        }
        if (key >= 32 && key <= 126 && n < cap - 1) {
            buf[n++] = (char)key;
            buf[n] = 0;
        }
    }
}

/* ============================================================
 * SEARCH  ( / ? n N )  - pure: does not move the cursor itself
 * ============================================================ */
static char last_pat[128];
static int last_dir = 1;
static char linebuf[33000];

static int extract_line(int pos)
{
    int ls = line_start(pos), le = line_end(pos), n = 0;
    while (ls + n < le && n < (int)sizeof(linebuf) - 1) {
        linebuf[n] = text_at(ls + n);
        n++;
    }
    linebuf[n] = 0;
    return n;
}

/* find the count-th match of pat (or last_pat when pat==0/empty)
 * starting from just after g_pos, direction dir.
 * Returns 1 and sets *hit to the absolute byte position. */
static int search_from(const char *pat, int dir, int count, int *hit)
{
    int cur_line = line_number_at(g_pos);
    int cur_off = g_pos - line_start(g_pos);
    int line, start_off, tries, found = 0, wrapped = 0;

    if (pat && pat[0]) {
        int k;
        for (k = 0; k < (int)sizeof(last_pat) - 1 && pat[k]; k++) last_pat[k] = pat[k];
        last_pat[k] = 0;
    }
    if (!last_pat[0]) { vi_msg = "no previous pattern"; return 0; }
    last_dir = dir;

    line = cur_line;
    start_off = dir > 0 ? cur_off + 1 : cur_off;
    tries = total_lines() * 2 + 4;
    while (tries-- > 0) {
        if (line < 0 || line >= total_lines()) {
            if (wrapped) break;
            wrapped = 1;
            if (dir > 0) { line = 0; start_off = 0;
                           vi_msg = "search hit BOTTOM, continuing at TOP"; }
            else { line = total_lines() - 1; start_off = 1 << 30;
                   vi_msg = "search hit TOP, continuing at BOTTOM"; }
        }
        { int ls = line_start_number(line);
          extract_line(ls);
          if (dir > 0) {
              int mlen, from = start_off;
              if (from > (int)sizeof(linebuf)) from = 0;
              for (;;) {
                  int mstart = rx_find(last_pat, linebuf, from, &mlen);
                  if (mstart < 0) break;
                  if (--count <= 0) { *hit = ls + mstart; found = 1; break; }
                  from = mstart + (mlen > 0 ? mlen : 1);
              }
          } else {
              int offs[64], noffs = 0, scan = 0, i2;
              while (noffs < 64) {
                  int mlen;
                  int mstart = rx_find(last_pat, linebuf, scan, &mlen);
                  if (mstart < 0 || mstart >= start_off) break;
                  offs[noffs++] = mstart;
                  scan = mstart + (mlen > 0 ? mlen : 1);
              }
              for (i2 = noffs - 1; i2 >= 0; i2--) {
                  if (--count <= 0) { *hit = ls + offs[i2]; found = 1; break; }
              }
          } }
        if (found) break;
        if (dir > 0) { line++; start_off = 0; }
        else { line--; start_off = 1 << 30; }
    }
    if (!found) { vi_msg = "Pattern not found"; return 0; }
    return 1;
}

/* ============================================================
 * MOTION RESOLUTION
 * ============================================================ */
#define M_EXCL 0   /* [min(c,t), max(c,t))  - w W b B ( ) { } /pat */
#define M_INCL 1   /* [c, t + cplen(t))    - e E f t ; , % l space */
#define M_BACK 2   /* [t, c)              - h , F */
#define M_EOL  3   /* [c, line_end)        - $ */
#define M_LINE 4   /* linewise            - j k G gg { } */

typedef struct { int ok, target, mode, keepcol, mkey; } Motion;

static int st_count = 0, st_got_count = 0;
static int st_op = 0, st_opcount = 0;
static int st_reg = '"';
static int st_pend = 0;
static int st_findkey = 0;
static char st_findch = 0;

static Motion resolve_motion(int key, int count, int replay)
{
    Motion m;
    m.ok = 0; m.target = g_pos; m.mode = M_EXCL; m.keepcol = 0; m.mkey = key;

    switch (key) {
    case 'h': case KEY_LEFT: {
        int i2;
        m.ok = 1; m.mode = M_BACK; m.target = g_pos;
        for (i2 = 0; i2 < count; i2++) m.target = step_backward(m.target);
        break; }
    case 'l': case KEY_RIGHT: case ' ': {
        int i2;
        m.ok = 1; m.mode = M_EXCL; m.target = g_pos;
        for (i2 = 0; i2 < count && m.target < eb.len; i2++) m.target = step_forward(m.target);
        break; }
    case 'j': case KEY_DOWN:
        m.ok = 1; m.mode = M_LINE; m.keepcol = 1;
        m.target = line_number_at(g_pos) + count; break;
    case 'k': case KEY_UP:
        m.ok = 1; m.mode = M_LINE; m.keepcol = 1;
        m.target = line_number_at(g_pos) - count; break;
    case '0':
        m.ok = 1; m.mode = M_EXCL; m.target = line_start(g_pos); break;
    case '^':
        m.ok = 1; m.mode = M_EXCL; m.target = first_nonblank(g_pos); break;
    case '$':
        m.ok = 1; m.mode = M_EOL; m.target = line_end(g_pos); break;
    case 'w': m.ok = 1; m.mode = M_EXCL; m.target = fwd_word_start(g_pos, count, 0); break;
    case 'W': m.ok = 1; m.mode = M_EXCL; m.target = fwd_word_start(g_pos, count, 1); break;
    case 'e': m.ok = 1; m.mode = M_INCL; m.target = fwd_word_end(g_pos, count, 0); break;
    case 'E': m.ok = 1; m.mode = M_INCL; m.target = fwd_word_end(g_pos, count, 1); break;
    case 'b': m.ok = 1; m.mode = M_EXCL; m.target = back_word_start(g_pos, count, 0); break;
    case 'B': m.ok = 1; m.mode = M_EXCL; m.target = back_word_start(g_pos, count, 1); break;
    case '(': m.ok = 1; m.mode = M_EXCL; m.target = sent_prev(g_pos, count); break;
    case ')': m.ok = 1; m.mode = M_EXCL; m.target = sent_next(g_pos, count); break;
    case '{':
        m.ok = 1; m.mode = M_LINE;
        m.target = line_number_at(para_prev(g_pos, count)); break;
    case '}':
        m.ok = 1; m.mode = M_LINE;
        m.target = line_number_at(para_next(g_pos, count)); break;
    case 'G':
        m.ok = 1; m.mode = M_LINE;
        m.target = (st_got_count || st_opcount) ? count - 1 : total_lines() - 1;
        break;
    case 'g': {
        int k2 = replay ? 'g' : get_key();
        if (k2 == 'g') { m.ok = 1; m.mode = M_LINE; m.target = count - 1; }
        break; }
    case 'f': case 'F': case 't': case 'T': {
        int ch = st_findch;
        if (!replay) {
            ch = get_key();
            if (ch == ESC_KEY || ch < 32 || ch > 126) break;
            st_findkey = key; st_findch = (char)ch;
        } else {
            if (st_findkey != key) break;
        }
        if (key == 'f') { m.ok = 1; m.mode = M_INCL; m.target = find_char_fwd(g_pos, (char)ch, count, 0, &m.ok); }
        else if (key == 'F') { m.ok = 1; m.mode = M_BACK; m.target = find_char_back(g_pos, (char)ch, count, 0, &m.ok); }
        else if (key == 't') { m.ok = 1; m.mode = M_EXCL; m.target = find_char_fwd(g_pos, (char)ch, count, 1, &m.ok); }
        else { m.ok = 1; m.mode = M_EXCL; m.target = find_char_back(g_pos, (char)ch, count, 1, &m.ok); }
        break; }
    case ';': case ',': {
        int ok = 0, t = g_pos, fk = st_findkey;
        if (key == ',') fk = (fk == 'f') ? 'F' : (fk == 'F') ? 'f' :
                             (fk == 't') ? 'T' : 'T';
        if (fk == 'f') t = find_char_fwd(g_pos, st_findch, count, 0, &ok);
        else if (fk == 'F') t = find_char_back(g_pos, st_findch, count, 0, &ok);
        else if (fk == 't') t = find_char_fwd(g_pos, st_findch, count, 1, &ok);
        else if (fk == 'T') t = find_char_back(g_pos, st_findch, count, 1, &ok);
        if (ok) {
            m.ok = 1;
            m.mode = (fk == 'f') ? M_INCL : (fk == 'F') ? M_BACK : M_EXCL;
            m.target = t;
        }
        break; }
    case '%': {
        int t = match_bracket(g_pos);
        if (t >= 0) { m.ok = 1; m.mode = M_INCL; m.target = t; }
        break; }
    case '/': case '?': {
        char pat[128];
        int hit = 0, dir = (key == '/') ? 1 : -1;
        if (replay) {
            if (search_from(0, dir, count, &hit)) { m.ok = 1; m.mode = M_EXCL; m.target = hit; }
            break;
        }
        if (read_prompt(key, pat, sizeof(pat))) {
            if (search_from(pat[0] ? pat : 0, dir, count, &hit)) {
                m.ok = 1; m.mode = M_EXCL; m.target = hit;
            }
        }
        break; }
    default:
        break;
    }
    return m;
}

/* ============================================================
 * OPERATORS  (d c y < >)
 * ============================================================ */
static char opbuf[REG_CAP];
static char savebuf[MAX_TEXT];
static char copybuf[65536];

static int extract_to_opbuf(int from, int to)
{
    int n = 0;
    if (to > from) {
        n = to - from;
        if (n > REG_CAP) n = REG_CAP;
        { int i; for (i = 0; i < n; i++) opbuf[i] = text_at(from + i); }
    }
    return n;
}

static void line_range(int from_line, int to_line, int *lo, int *hi)
{
    int a = from_line, b = to_line, t;
    if (a > b) { t = a; a = b; b = t; }
    *lo = line_start_number(a);
    *hi = line_end_number(b);
    if (*hi < eb.len) (*hi)++;
}

static void begin_insert_session(int capture_count)
{
    g_mode = MODE_INSERT;
    sesslen = 0;
    sesscount = capture_count ? (st_count ? st_count : 1) : 1;
    g_modified = 1;
}

static void op_linewise(int op, int target_line, int reg)
{
    int cur = line_number_at(g_pos);
    int a = cur, b = target_line, t;
    int lo, hi, hi0, n, dlo;

    if (b < 0) b = 0;
    if (b >= total_lines()) b = total_lines() - 1;
    if (a > b) { t = a; a = b; b = t; }
    line_range(a, b, &lo, &hi);
    hi0 = hi;
    n = extract_to_opbuf(lo, hi);
    /* linewise registers must end with a newline */
    if (n > 0 && opbuf[n - 1] != '\n' && n < REG_CAP) opbuf[n++] = '\n';

    if (op == 'y') {
        reg_store_both(reg, opbuf, n, 1);
        g_pos = first_nonblank(lo);
        return;
    }
    if (op == 'c') {
        char ind[128]; int indlen = 0, i;
        int p = lo;
        while (p < eb.len && (text_at(p) == ' ' || text_at(p) == '\t') && indlen < 126)
            ind[indlen++] = text_at(p++);
        change_begin();
        reg_shift_numbered();
        reg_store_both(reg, opbuf, n, 1);
        { int had_nl = (hi0 < eb.len);   /* before shrinking */
          delete_range(lo, hi - lo);
          if (had_nl) insert_byte(lo, '\n'); }   /* keep the line structure */
        g_pos = lo;
        for (i = 0; i < indlen; i++) { insert_byte(g_pos, ind[i]); g_pos++; }
        begin_insert_session(0);
        return;
    }
    /* 'd': deleting the last line also takes the newline before it */
    change_begin();
    dlo = lo;
    if (hi >= eb.len && lo > 0) dlo--;
    reg_shift_numbered();
    reg_store('1', opbuf, n, 1);
    reg_store_both(reg, opbuf, n, 1);
    delete_range(dlo, hi - dlo);
    g_pos = dlo;
    if (g_pos > eb.len) g_pos = eb.len;
    if (op == 'd') {
        lastch.kind = LC_DEL; lastch.motion = 0;
        lastch.mode = M_LINE; lastch.count = 1;
    }
}

static void op_charwise(int op, int from, int to, int reg)
{
    int n = extract_to_opbuf(from, to);
    if (op == 'y') {
        reg_store_both(reg, opbuf, n, 0);
        return;
    }
    change_begin();
    reg_store_both(reg, opbuf, n, 0);
    delete_range(from, to - from);
    if (g_pos > from) g_pos = from;
    if (g_pos > eb.len) g_pos = eb.len;
    if (op == 'c') begin_insert_session(0);
}

static void shift_lines(int op, int target_line)
{
    int cur = line_number_at(g_pos);
    int a = cur, b = target_line, t, i;
    if (b < 0) b = 0;
    if (b >= total_lines()) b = total_lines() - 1;
    if (a > b) { t = a; a = b; b = t; }
    change_begin();
    for (i = a; i <= b; i++) {
        int ls = line_start_number(i);
        int le = line_end(ls);
        if (ls >= le) continue;
        if (op == '>') {
            insert_byte(ls, '\t');
        } else {
            int k, remove = SHIFT_WIDTH;
            for (k = 0; k < remove && ls < le; k++) {
                if (text_at(ls) == '\t') { delete_range(ls, 1); le--; break; }
                if (text_at(ls) == ' ') { delete_range(ls, 1); le--; }
                else break;
            }
        }
    }
    g_pos = first_nonblank(line_start_number(a));
    lastch.kind = (op == '>') ? LC_SHIFT_R : LC_SHIFT_L;
    lastch.count = b - a + 1;
}

static void apply_op(int op, int count, Motion m, int reg)
{
    int from, to;

    if (!m.ok) return;

    if (m.mode == M_LINE) {
        if (op == '<' || op == '>') { shift_lines(op, m.target); return; }
        op_linewise(op, m.target, reg);
        if (op == 'd') {
            lastch.kind = LC_DEL; lastch.motion = m.mkey;
            lastch.mode = m.mode; lastch.count = count;
        }
        return;
    }

    switch (m.mode) {
    case M_EXCL:
        from = g_pos < m.target ? g_pos : m.target;
        to = g_pos < m.target ? m.target : g_pos;
        break;
    case M_INCL:
        from = g_pos;
        to = m.target >= eb.len ? eb.len : m.target + cp_len(m.target);
        break;
    case M_BACK:
        from = m.target;
        to = g_pos;
        break;
    case M_EOL:
        from = g_pos;
        to = line_end(g_pos);
        break;
    default:
        return;
    }
    if (to > eb.len) to = eb.len;
    if (to <= from) return;
    if (op == 'c' && (m.mkey == 'w' || m.mkey == 'W')) {
        /* vi: cw/cW does not eat the whitespace before the next word */
        int allblank = 1, i;
        for (i = from; i < to; i++)
            if (text_at(i) != ' ' && text_at(i) != '\t') { allblank = 0; break; }
        if (!allblank)
            while (to > from && (text_at(to - 1) == ' ' || text_at(to - 1) == '\t')) to--;
    }
    op_charwise(op, from, to, reg);
    if (op == 'd') {
        lastch.kind = LC_DEL; lastch.motion = m.mkey;
        lastch.mode = m.mode; lastch.count = count;
    }
}

/* ============================================================
 * REPLACE / CASE / JOIN / PUT
 * ============================================================ */
static void do_replace(int count, int ch)
{
    int i;
    if (!ch) return;
    change_begin();
    for (i = 0; i < count; i++) {
        if (g_pos >= eb.len || text_at(g_pos) == '\n') break;
        delete_range(g_pos, cp_len(g_pos));
        insert_byte(g_pos, (char)ch);
        g_pos++;
    }
    if (g_pos > eb.len) g_pos = eb.len;
    lastch.kind = LC_REPL; lastch.count = count; lastch.ch = (char)ch;
}

static void do_case(int count)
{
    int i;
    change_begin();
    for (i = 0; i < count; i++) {
        if (g_pos >= eb.len || text_at(g_pos) == '\n') break;
        { char c = text_at(g_pos);
          if (c >= 'a' && c <= 'z') { delete_range(g_pos, 1); insert_byte(g_pos, (char)(c - 32)); }
          else if (c >= 'A' && c <= 'Z') { delete_range(g_pos, 1); insert_byte(g_pos, (char)(c + 32)); } }
        g_pos++;
    }
    lastch.kind = LC_CASE; lastch.count = count;
}

static void do_join(int count)
{
    int i, joins = (count > 1) ? count - 1 : 1;
    change_begin();
    for (i = 0; i < joins; i++) {
        int le = line_end(g_pos);
        if (le >= eb.len) break;
        while (le > g_pos && (text_at(le - 1) == ' ' || text_at(le - 1) == '\t')) {
            delete_range(le - 1, 1);
            le--;
        }
        delete_range(le, 1);
        while (le < eb.len && (text_at(le) == ' ' || text_at(le) == '\t'))
            delete_range(le, 1);
        if (le < eb.len && text_at(le) != '\n' &&
            (le == 0 || text_at(le - 1) != ' '))
            insert_byte(le, ' ');
    }
    lastch.kind = LC_JOIN; lastch.count = count;
}

/* insert whole lines at `at`, splitting at EOF if needed */
static int insert_lines_at(int at, const char *data, int n)
{
    if (at >= eb.len && eb.len > 0) {
        if (text_at(eb.len - 1) != '\n') {
            insert_byte(eb.len, '\n');
            at = eb.len;
        }
    }
    return insert_bytes(at, data, n);
}

static void do_put(int after, int count, int reg)
{
    Reg *r = &regs[reg_index(reg)];
    int i;
    if (reg_index(reg) < 0 || r->len <= 0) { vi_msg = "register empty"; return; }
    change_begin();
    if (r->lines) {
        int at;
        if (after) {
            at = line_end(g_pos);
            if (at < eb.len) at++;
        } else {
            at = line_start(g_pos);
        }
        if (at >= eb.len && eb.len > 0 && text_at(eb.len - 1) != '\n') {
            insert_byte(eb.len, '\n');
            at = eb.len;
        }
        for (i = 0; i < count; i++)
            insert_bytes(at + i * r->len, r->data, r->len);
        g_pos = first_nonblank(at);
    } else {
        int at = after ? step_forward(g_pos) : g_pos;
        if (at > eb.len) at = eb.len;
        for (i = 0; i < count; i++)
            insert_bytes(at + i * r->len, r->data, r->len);
        g_pos = at + count * r->len;
        g_pos = step_backward(g_pos);
        if (g_pos < 0) g_pos = 0;
    }
}

/* ============================================================
 * SUBSTITUTE / EX COMMANDS
 * ============================================================ */
static char sub_pat[128], sub_rep[128];
static int sub_glob = 0, sub_have = 0;
static char newbuf[65536];

static int sub_line(int ls)
{
    int le = line_end(ls);
    int lsz = le - ls, o = 0, subs = 0, scan;
    int mstart, mlen;

    if (lsz > 32768) return 0;
    { int i; for (i = 0; i < lsz; i++) linebuf[i] = text_at(ls + i); }
    linebuf[lsz] = 0;

    scan = 0;
    while (linebuf[scan]) {
        mstart = rx_find(sub_pat, linebuf, scan, &mlen);
        if (mstart < 0) break;
        { int k;
          for (k = scan; k < mstart && o < 65535; k++) newbuf[o++] = linebuf[k]; }
        o += rx_build_rep(sub_rep, linebuf, mstart, mstart + mlen, newbuf + o, 65536 - o);
        subs++;
        if (mlen > 0) scan = mstart + mlen;
        else {
            if (linebuf[scan] && o < 65535) newbuf[o++] = linebuf[scan];
            scan++;
        }
        if (!sub_glob) break;
    }
    if (!subs) return 0;
    { int k;
      for (k = scan; linebuf[k] && o < 65535; k++) newbuf[o++] = linebuf[k]; }
    newbuf[o] = 0;
    delete_range(ls, le - ls);
    insert_bytes(ls, newbuf, o);
    return subs;
}

static void substitute_range(int a1, int a2)
{
    int nsubs = 0, nlines = 0, i;
    static char msg[64];
    change_begin();
    for (i = a1; i <= a2; i++) {
        int ls = line_start_number(i);
        int s = sub_line(ls);
        if (s) { nsubs += s; nlines++; }
    }
    if (nsubs) {
        char t[16];
        itoa(nsubs, t, 10);
        strcpy(msg, t);
        strcat(msg, " subs on ");
        itoa(nlines, t, 10);
        strcat(msg, t);
        strcat(msg, " lines");
        vi_msg = msg;
        lastch.kind = LC_SUBST;
    } else {
        vi_msg = "Pattern not found";
    }
}

static void subst_current(void)
{
    if (!sub_have) { vi_msg = "no previous substitute"; return; }
    substitute_range(line_number_at(g_pos), line_number_at(g_pos));
}

static int write_file(const char *name)
{
    flatten(savebuf);
    if (fs_write_file(name, savebuf, (uint32_t)eb.len) != 0) {
        vi_msg = "write error: file too large or filesystem full";
        return 0;
    }
    if (strcmp(name, g_file) == 0) g_modified = 0;
    return 1;
}

static int load_file(const char *name, int force)
{
    uint32_t size = 0;
    if (g_modified && !force) {
        vi_msg = "No write since last change (use :e!)";
        return 0;
    }
    if (!fs_file_exists(name)) {
        if (fs_create_file(name) != 0) { vi_msg = "no such directory"; return 0; }
        size = 0;
    } else if (fs_read_file_prefix(name, savebuf, MAX_TEXT, &size) != 0) {
        vi_msg = "cannot read file";
        return 0;
    }
    if (size > (uint32_t)(MAX_TEXT - 1)) { vi_msg = "file too large"; return 0; }
    eb.len = (int)size;
    { uint32_t i; for (i = 0; i < size; i++) eb.data[i] = savebuf[i]; }
    eb.gap_start = eb.len; eb.gap_end = MAX_TEXT - 1;
    eb.data[eb.gap_end] = 0;
    strncpy(g_file, name, sizeof(g_file));
    g_file[sizeof(g_file) - 1] = 0;
    g_pos = 0; g_scroll = 0; g_modified = 0;
    undo_cnt = 0; undo_head = 0; redo_cnt = 0; redo_head = 0;
    clamp_marks();
    return 1;
}

/* parse one address; returns line number, or -1 if none present */
static int parse_addr(const char *s, int *pi, int total)
{
    int v = -1, got = 0;
    while (s[*pi] == ' ') (*pi)++;
    if (s[*pi] == '.') { v = line_number_at(g_pos); (*pi)++; got = 1; }
    else if (s[*pi] == '$') { v = total - 1; (*pi)++; got = 1; }
    else if (s[*pi] == '\'') {
        int c = s[*pi + 1];
        if (c >= 'a' && c <= 'z') { v = line_number_at(marks[c - 'a']); *pi += 2; got = 1; }
    }
    else if (s[*pi] >= '0' && s[*pi] <= '9') {
        v = 0;
        while (s[*pi] >= '0' && s[*pi] <= '9') { v = v * 10 + (s[*pi] - '0'); (*pi)++; }
        got = 1;
        if (v > 0) v--;
    }
    for (;;) {
        while (s[*pi] == ' ') (*pi)++;
        if (s[*pi] == '+' || s[*pi] == '-') {
            int n = 0, sign = (s[*pi] == '+') ? 1 : -1;
            (*pi)++;
            while (s[*pi] >= '0' && s[*pi] <= '9') { n = n * 10 + (s[*pi] - '0'); (*pi)++; }
            if (n == 0) n = 1;
            if (!got) v = line_number_at(g_pos);
            v += sign * n;
            got = 1;
        } else break;
    }
    return got ? v : -1;
}

/* run an ex command; returns 1 to quit */
static int ex_run(char *cmd)
{
    int total = total_lines();
    int pi = 0, a1 = -1, a2 = -1, pct = 0;
    char c;

    while (cmd[pi] == ' ') pi++;
    if (!cmd[pi]) return 0;

    if (cmd[pi] == '%') { a1 = 0; a2 = total - 1; pct = 1; pi++; }
    else {
        a1 = parse_addr(cmd, &pi, total);
        if (a1 >= 0) {
            while (cmd[pi] == ' ') pi++;
            if (cmd[pi] == ',' || cmd[pi] == ';') {
                pi++;
                a2 = parse_addr(cmd, &pi, total);
                if (a2 < 0) a2 = a1;
            } else a2 = a1;
        }
    }
    while (cmd[pi] == ' ') pi++;

    /* bare range: goto line a2 */
    if (!cmd[pi]) {
        if (pct || a1 >= 0) goto_line(a2, 1);
        return 0;
    }

    c = cmd[pi];

    /* :set ... */
    if (c == 's' && cmd[pi + 1] == 'e' && cmd[pi + 2] == 't') {
        static char args[64];
        int k = pi + 3, ai2 = 0;
        while (cmd[k] == ' ') k++;
        while (cmd[k] && ai2 < 63) args[ai2++] = cmd[k++];
        args[ai2] = 0;
        if (args[0] == 0) {
            vi_msg = "nu off  ai off  ic off  (try :set nu)";
        }
        else if (!strcmp(args, "nu") || !strcmp(args, "number")) {
            opt_nu = 1; g_gutter = 8; g_cols = VI_COLS - 8;
        }
        else if (!strcmp(args, "nonu") || !strcmp(args, "nonumber")) {
            opt_nu = 0; g_gutter = 0; g_cols = VI_COLS;
        }
        else if (!strcmp(args, "ai") || !strcmp(args, "autoindent")) opt_ai = 1;
        else if (!strcmp(args, "noai") || !strcmp(args, "noautoindent")) opt_ai = 0;
        else if (!strcmp(args, "ic") || !strcmp(args, "ignorecase")) opt_ic = 1;
        else if (!strcmp(args, "noic") || !strcmp(args, "noignorecase")) opt_ic = 0;
        else {
            static char msg[64];
            strcpy(msg, "unknown option: ");
            strcat(msg, args);
            vi_msg = msg;
        }
        return 0;
    }

    /* :s[ubstitute] */
    if (c == 's') {
        int q = pi + 1, s1, s2;
        char delim;
        static char newpat[128], newrep[128];
        while (cmd[q] == ' ') q++;
        if (cmd[q] == 0) { subst_current(); return 0; }
        if (a1 < 0) { a1 = line_number_at(g_pos); a2 = a1; }
        if (a2 < 0) a2 = a1;
        if (a1 > a2) { int t = a1; a1 = a2; a2 = t; }
        if (a1 < 0) a1 = 0;
        if (a2 >= total) a2 = total - 1;
        delim = cmd[q];
        if (delim == '\\' || (delim >= 'a' && delim <= 'z') || (delim >= 'A' && delim <= 'Z') ||
            (delim >= '0' && delim <= '9')) {
            vi_msg = "bad substitute";
            return 0;
        }
        q++;
        s1 = 0;
        while (cmd[q] && cmd[q] != delim) {
            if (cmd[q] == '\\' && cmd[q + 1] == delim) newpat[s1++] = delim, q += 2;
            else newpat[s1++] = cmd[q++];
            if (s1 >= 126) break;
        }
        newpat[s1] = 0;
        sub_glob = 0;
        if (cmd[q] == delim) {
            q++;
            s2 = 0;
            while (cmd[q] && cmd[q] != delim) {
                if (cmd[q] == '\\' && cmd[q + 1] == delim) newrep[s2++] = delim, q += 2;
                else newrep[s2++] = cmd[q++];
                if (s2 >= 126) break;
            }
            newrep[s2] = 0;
            if (cmd[q] == delim) {
                q++;
                while (cmd[q]) {
                    if (cmd[q] == 'g') sub_glob = 1;
                    q++;
                }
            }
        } else {
            newrep[0] = 0;
        }
        if (s1 > 0) {
            strncpy(sub_pat, newpat, sizeof(sub_pat));
            sub_pat[sizeof(sub_pat) - 1] = 0;
            strncpy(sub_rep, newrep, sizeof(sub_rep));
            sub_rep[sizeof(sub_rep) - 1] = 0;
            sub_have = 1;
        }
        if (!sub_have) { vi_msg = "no pattern"; return 0; }
        substitute_range(a1, a2);
        return 0;
    }

    if (a1 < 0) a1 = line_number_at(g_pos);
    if (a2 < 0) a2 = a1;
    { int t; if (a1 > a2) { t = a1; a1 = a2; a2 = t; } }

    switch (c) {
    case 'q': {
        int force = 0, k = pi + 1;
        static char w1[8];
        while (cmd[k] == ' ' || cmd[k] == '!') { if (cmd[k] == '!') force = 1; k++; }
        { int n = 0;
          while (cmd[k] && cmd[k] != ' ' && n < 7) w1[n++] = cmd[k++];
          w1[n] = 0; }
        (void)w1;
        if (g_modified && !force) { vi_msg = "No write since last change (:q! to force)"; return 0; }
        return 1; }
    case 'w': {
        int k = pi + 1, waq = 0;
        char name[MAX_PATH]; int ni = 0;
        while (cmd[k] == ' ' || cmd[k] == '!' || cmd[k] == 'q') {
            if (cmd[k] == 'q') waq = 1;
            k++;
        }
        while (cmd[k] && cmd[k] != ' ' && ni < MAX_PATH - 1) name[ni++] = cmd[k++];
        name[ni] = 0;
        if (!write_file(ni ? name : g_file)) return 0;
        vi_msg = "written";
        if (waq) return 1;
        return 0; }
    case 'x': {
        if (!write_file(g_file)) return 0;
        return 1; }
    case 'e': {
        int force = 0, k = pi + 1;
        char name[MAX_PATH]; int ni = 0;
        while (cmd[k] == ' ' || cmd[k] == '!') { if (cmd[k] == '!') force = 1; k++; }
        while (cmd[k] && cmd[k] != ' ' && ni < MAX_PATH - 1) name[ni++] = cmd[k++];
        name[ni] = 0;
        if (ni) { if (load_file(name, force)) vi_msg = "loaded"; }
        else { if (load_file(g_file, force)) vi_msg = "reloaded"; }
        return 0; }
    case 'r': {
        int k = pi + 1, at;
        char name[MAX_PATH]; int ni = 0;
        uint32_t size = 0;
        while (cmd[k] == ' ') k++;
        while (cmd[k] && cmd[k] != ' ' && ni < MAX_PATH - 1) name[ni++] = cmd[k++];
        name[ni] = 0;
        if (!ni) { vi_msg = "file name required"; return 0; }
        if (!fs_file_exists(name)) { vi_msg = "file not found"; return 0; }
        if (fs_read_file_prefix(name, savebuf, MAX_TEXT, &size) != 0) {
            vi_msg = "cannot read file"; return 0;
        }
        at = line_end_number(a1);
        if (at < eb.len) at++;
        change_begin();
        insert_lines_at(at, savebuf, (int)size);
        if (at >= eb.len && eb.len > 0 && text_at(eb.len - 1) != '\n') g_pos = eb.len;
        else g_pos = at;
        return 0; }
    case 'd':
    case 'y': {
        int lo, hi, n;
        if (a1 < 0) a1 = line_number_at(g_pos);
        if (a2 < 0) a2 = a1;
        if (a1 > a2) { int t = a1; a1 = a2; a2 = t; }
        line_range(a1, a2, &lo, &hi);
        n = extract_to_opbuf(lo, hi);
        if (n > 0 && opbuf[n - 1] != '\n' && n < REG_CAP) opbuf[n++] = '\n';
        if (c == 'y') {
            reg_store_both('"', opbuf, n, 1);
            g_pos = first_nonblank(lo);
            vi_msg = "yanked lines";
            return 0;
        }
        change_begin();
        reg_shift_numbered();
        reg_store('1', opbuf, n, 1);
        reg_store_both('"', opbuf, n, 1);
        { int dlo = lo;
          if (hi >= eb.len && lo > 0) dlo--;
          delete_range(dlo, hi - dlo);
          g_pos = dlo; }
        if (g_pos > eb.len) g_pos = eb.len;
        return 0; }
    case 't':
    case 'm': {
        int k = pi + 1, dst;
        int lo, hi, n, insert_at;
        while (cmd[k] == ' ') k++;
        { int q2 = k;
          dst = parse_addr(cmd, &q2, total);
          k = q2; }
        if (dst < 0) dst = line_number_at(g_pos);
        if (a1 < 0) a1 = line_number_at(g_pos);
        if (a2 < 0) a2 = a1;
        line_range(a1, a2, &lo, &hi);
        n = hi - lo;
        if (n > (int)sizeof(copybuf)) { vi_msg = "range too large"; return 0; }
        { int i2; for (i2 = 0; i2 < n; i2++) copybuf[i2] = text_at(lo + i2); }
        if (c == 'm' && dst >= a1 - 1 && dst <= a2) { vi_msg = "move into itself"; return 0; }
        change_begin();
        if (c == 'm') {
            int n2;
            reg_shift_numbered();
            n2 = extract_to_opbuf(lo, hi);
            reg_store_both('"', opbuf, n2, 1);
            delete_range(lo, n);
            if (dst >= a1) dst -= (a2 - a1 + 1);
        }
        insert_at = line_end_number(dst);
        if (insert_at < eb.len) insert_at++;
        insert_lines_at(insert_at, copybuf, n);
        if (insert_at >= eb.len) insert_at = eb.len;
        g_pos = first_nonblank(insert_at);
        return 0; }
    case '>':
    case '<': {
        int i;
        change_begin();
        for (i = a1; i <= a2 && i < total; i++) {
            int ls = line_start_number(i);
            int le = line_end(ls);
            if (ls >= le) continue;
            if (c == '>') insert_byte(ls, '\t');
            else {
                int k2, remove = SHIFT_WIDTH;
                for (k2 = 0; k2 < remove && ls < le; k2++) {
                    if (text_at(ls) == '\t') { delete_range(ls, 1); le--; break; }
                    if (text_at(ls) == ' ') { delete_range(ls, 1); le--; }
                    else break;
                }
            }
        }
        g_pos = first_nonblank(line_start_number(a1));
        return 0; }
    case '=': {
        static char msg[48];
        char t[16];
        itoa(a2 + 1, t, 10);
        strcpy(msg, "line ");
        strcat(msg, t);
        vi_msg = msg;
        return 0; }
    case '!':
        vi_msg = ":! not supported";
        return 0;
    default:
        break;
    }

    { static char msg[64];
      int k = pi, n = 0;
      strcpy(msg, "not an editor command: ");
      while (cmd[k] && cmd[k] != ' ' && n < 38) msg[strlen(msg)] = cmd[k++], n++;
      msg[strlen(msg)] = 0;
      vi_msg = msg; }
    return 0;
}

/* ============================================================
 * INSERT / REPLACE MODE
 * ============================================================ */
static char sessbuf[4096];

static void g_enter_insert(int capture)
{
    push_undo();
    g_modified = 1;
    begin_insert_session(capture);
}

static void g_enter_replace(void)
{
    push_undo();
    g_modified = 1;
    g_mode = MODE_REPLACE;
    sesslen = 0;
    sesscount = st_count ? st_count : 1;
}

static void end_session(int kind)
{
    int i;
    lastch.kind = kind;
    lastch.tlen = sesslen;
    for (i = 0; i < sesslen && i < 4096; i++) lastch.text[i] = sessbuf[i];
    lastch.count = sesscount;
}

static void sess_capture(char c)
{
    if (sesslen < 4095) sessbuf[sesslen++] = c;
}

static void insert_key(int key)
{
    if (key == ESC_KEY) {
        g_mode = MODE_COMMAND;
        g_pos = step_backward(g_pos);
        if (sesscount > 1 && sesslen > 0) {
            int i, j;
            for (i = 0; i < sesscount - 1; i++)
                for (j = 0; j < sesslen; j++)
                    insert_byte(g_pos, sessbuf[j]);
            g_pos += (sesscount - 1) * sesslen;
        }
        end_session(LC_INS);
        return;
    }
    if (key == KEY_BACKSPACE || key == 8) {
        int old = g_pos;
        g_pos = step_backward(g_pos);
        if (old != g_pos) {
            delete_range(g_pos, old - g_pos);
            if (sesslen > 0) sesslen--;
        }
        return;
    }
    if (key == CTRL('w')) {
        int t = back_word_start(g_pos, 1, 0);
        if (t < g_pos) {
            delete_range(t, g_pos - t);
            g_pos = t;
        }
        return;
    }
    if (key == KEY_ENTER || key == '\n') {
        if (insert_byte(g_pos, '\n')) {
            g_pos++;
            sess_capture('\n');
            if (opt_ai) {
                int ls = line_start(g_pos - 1);
                while (ls < eb.len && (text_at(ls) == ' ' || text_at(ls) == '\t') &&
                       ls < g_pos) {
                    char c2 = text_at(ls);
                    insert_byte(g_pos, c2);
                    sess_capture(c2);
                    g_pos++;
                    ls++;
                }
            }
        }
        return;
    }
    if (key == TAB_KEY) {
        if (insert_byte(g_pos, '\t')) { g_pos++; sess_capture('\t'); }
        return;
    }
    if (key >= 32 && key <= 126) {
        if (insert_byte(g_pos, (char)key)) { g_pos++; sess_capture((char)key); }
        return;
    }
    if (key == KEY_LEFT)  { g_pos = step_backward(g_pos); return; }
    if (key == KEY_RIGHT) { g_pos = step_forward(g_pos); return; }
    if (key == KEY_UP || key == KEY_DOWN) {
        g_pos = move_vertical(g_pos, key == KEY_UP ? -1 : 1);
        return;
    }
}

static void replace_key(int key)
{
    if (key == ESC_KEY) {
        g_mode = MODE_COMMAND;
        end_session(LC_OVER);
        return;
    }
    if (key == KEY_BACKSPACE || key == 8) {
        g_pos = step_backward(g_pos);
        return;
    }
    if (key == KEY_ENTER || key == '\n') {
        if (insert_byte(g_pos, '\n')) { g_pos++; sess_capture('\n'); }
        return;
    }
    if (key == TAB_KEY) key = '\t';
    if ((key >= 32 && key <= 126) || key == '\t') {
        if (g_pos < eb.len && text_at(g_pos) != '\n')
            delete_range(g_pos, cp_len(g_pos));
        insert_byte(g_pos, (char)key);
        g_pos++;
        sess_capture((char)key);
        return;
    }
    if (key == KEY_LEFT)  { g_pos = step_backward(g_pos); return; }
    if (key == KEY_RIGHT) { g_pos = step_forward(g_pos); return; }
    if (key == KEY_UP || key == KEY_DOWN) {
        g_pos = move_vertical(g_pos, key == KEY_UP ? -1 : 1);
        return;
    }
}

/* ============================================================
 * REPEAT (.)
 * ============================================================ */
static void replay_last(void)
{
    int i, j;
    switch (lastch.kind) {
    case LC_INS:
        change_begin();
        for (i = 0; i < lastch.count; i++)
            for (j = 0; j < lastch.tlen; j++)
                insert_byte(g_pos, lastch.text[j]);
        g_pos += lastch.tlen * lastch.count;
        if (g_pos > eb.len) g_pos = eb.len;
        break;
    case LC_OVER:
        change_begin();
        for (i = 0; i < lastch.count; i++)
            for (j = 0; j < lastch.tlen; j++) {
                if (g_pos < eb.len && text_at(g_pos) != '\n')
                    delete_range(g_pos, cp_len(g_pos));
                insert_byte(g_pos, lastch.text[j]);
                g_pos++;
            }
        break;
    case LC_DEL: {
        Motion m;
        m.ok = 0; m.target = g_pos; m.mode = lastch.mode; m.keepcol = 0;
        m.mkey = lastch.motion;
        if (lastch.motion == 'd') {
            m.ok = 1; m.mode = M_LINE;
            m.target = line_number_at(g_pos) + lastch.count - 1;
            { int t;
              int cur = line_number_at(g_pos), tgt = m.target;
              if (cur > tgt) { t = cur; cur = tgt; tgt = t; } (void)cur; (void)tgt; }
            apply_op('d', lastch.count, m, '"');
        } else {
            m = resolve_motion(lastch.motion, lastch.count, 1);
            if (m.ok) apply_op('d', lastch.count, m, '"');
        }
        break; }
    case LC_REPL:
        do_replace(lastch.count, lastch.ch);
        break;
    case LC_JOIN:
        do_join(lastch.count);
        break;
    case LC_CASE:
        do_case(lastch.count);
        break;
    case LC_SHIFT_R:
    case LC_SHIFT_L: {
        int cur = line_number_at(g_pos);
        int lo2 = cur, hi2 = cur + lastch.count - 1, t;
        change_begin();
        if (lo2 > hi2) { t = lo2; lo2 = hi2; hi2 = t; }
        for (i = lo2; i <= hi2; i++) {
            int ls = line_start_number(i);
            int le = line_end(ls);
            if (ls >= le) continue;
            if (lastch.kind == LC_SHIFT_R) insert_byte(ls, '\t');
            else {
                for (j = 0; j < SHIFT_WIDTH && ls < le; j++) {
                    if (text_at(ls) == '\t') { delete_range(ls, 1); le--; break; }
                    if (text_at(ls) == ' ') { delete_range(ls, 1); le--; }
                    else break;
                }
            }
        }
        break; }
    case LC_SUBST:
        subst_current();
        break;
    default:
        break;
    }
}

/* ============================================================
 * NORMAL MODE
 * ============================================================ */
static int norm_key(int key)
{
    Motion m;
    int count = st_count ? st_count : 1;

    if (key == ESC_KEY) {
        st_pend = 0; st_op = 0; st_count = 0; st_got_count = 0;
        return 0;
    }

    /* ----- pending two-key states ----- */
    if (st_pend == '"') {
        if (reg_index(key) >= 0) st_reg = key;
        st_pend = 0;
        return 0;
    }
    if (st_pend == 'm') {
        if (key >= 'a' && key <= 'z') marks[key - 'a'] = g_pos;
        st_pend = 0;
        return 0;
    }
    if (st_pend == '\'' || st_pend == '`') {
        int t = -1;
        if (key == '\'' || key == '`') t = marks[26];
        else if (key >= 'a' && key <= 'z') t = marks[key - 'a'];
        if (t >= 0 && t <= eb.len) {
            marks[26] = g_pos;
            if (st_pend == '\'') g_pos = first_nonblank(t);
            else g_pos = t;
        }
        st_pend = 0;
        return 0;
    }
    if (st_pend == 'z') {
        int cur = line_number_at(g_pos);
        if (key == KEY_ENTER || key == 't') g_scroll = cur;
        else if (key == 'z') g_scroll = cur - VI_ROWS / 2;
        else if (key == 'b' || key == '-') g_scroll = cur - VI_ROWS + 1;
        if (g_scroll < 0) g_scroll = 0;
        st_pend = 0;
        return 0;
    }
    if (st_pend == 'Z') {
        st_pend = 0;
        if (key == 'Z') return ex_run("x");
        return 0;
    }
    if (st_pend == 'g') {
        st_pend = 0;
        if (key != 'g') return 0;
        if (st_op) {
            m.ok = 1; m.mode = M_LINE; m.target = count - 1; m.keepcol = 0; m.mkey = 'g';
            apply_op(st_op, count * (st_opcount ? st_opcount : 1), m, st_reg);
            st_op = 0; st_opcount = 0; st_reg = '"'; st_count = 0; st_got_count = 0;
        } else {
            goto_line(count - 1, 1);
            st_count = 0; st_got_count = 0;
        }
        return 0;
    }

    /* ----- counts ----- */
    if ((key >= '1' && key <= '9') || (key == '0' && st_count > 0)) {
        st_count = st_count * 10 + (key - '0');
        st_got_count = 1;
        return 0;
    }

    /* ----- operator waiting for a motion ----- */
    if (st_op) {
        int eff = count * (st_opcount ? st_opcount : 1);
        if (key == st_op && (key == 'd' || key == 'c' || key == 'y' ||
                             key == '<' || key == '>')) {
            m.ok = 1; m.mode = M_LINE; m.keepcol = 0; m.mkey = key;
            m.target = line_number_at(g_pos) + eff - 1;
        } else {
            m = resolve_motion(key, eff, 0);
        }
        if (m.ok) {
            apply_op(st_op, eff, m, st_reg);
            st_op = 0; st_opcount = 0; st_reg = '"'; st_count = 0; st_got_count = 0;
        }
        return 0;
    }

    /* ----- cursor motions ----- */
    switch (key) {
    case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
    case 'h': case 'l': case ' ': case 'j': case 'k':
    case '0': case '^': case '$': case 'w': case 'W': case 'b': case 'B':
    case 'e': case 'E': case '(': case ')': case '{': case '}':
    case 'f': case 'F': case 't': case 'T': case ';': case ',': case '%': {
        m = resolve_motion(key, count, 0);
        if (m.ok) {
            if (m.mode == M_LINE) {
                if (m.keepcol) {
                    int i;
                    if (key == 'j' || key == KEY_DOWN)
                        for (i = 0; i < count; i++) g_pos = move_vertical(g_pos, 1);
                    else
                        for (i = 0; i < count; i++) g_pos = move_vertical(g_pos, -1);
                } else {
                    marks[26] = g_pos;
                    goto_line(m.target, 0);
                }
            } else if (m.mode == M_EOL) {
                int e = line_end(g_pos);
                if (e > line_start(g_pos)) e = step_backward(e);
                g_pos = e;
            } else {
                g_pos = m.target;
            }
        }
        st_count = 0; st_got_count = 0;
        return 0; }

    case 'G':
        marks[26] = g_pos;
        goto_line(st_count ? st_count - 1 : total_lines() - 1, 0);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'g': st_pend = 'g'; return 0;
    case KEY_ENTER: case '\n':
        goto_line(line_number_at(g_pos) + count, 0);
        st_count = 0; st_got_count = 0;
        return 0;

    case CTRL('f'): case CTRL('b'): case CTRL('d'): case CTRL('u'): {
        int step = (key == CTRL('f') || key == CTRL('b')) ? VI_ROWS - 2 : (VI_ROWS - 2) / 2;
        if (key == CTRL('f') || key == CTRL('d')) g_scroll += step;
        else g_scroll -= step;
        if (g_scroll < 0) g_scroll = 0;
        { int maxs = total_lines() - 1;
          if (g_scroll > maxs) g_scroll = maxs; }
        goto_line(g_scroll, 0);
        st_count = 0; st_got_count = 0;
        return 0; }
    case 'z': st_pend = 'z'; return 0;
    case 'Z': st_pend = 'Z'; return 0;
    case CTRL('g'): {
        static char msg[80];
        char t[16];
        strcpy(msg, g_file);
        strcat(msg, g_modified ? " [Modified] line " : " line ");
        itoa(line_number_at(g_pos) + 1, t, 10);
        strcat(msg, t);
        strcat(msg, " of ");
        itoa(total_lines(), t, 10);
        strcat(msg, t);
        vi_msg = msg;
        return 0; }

    case 'H': goto_line(g_scroll, 0); st_count = 0; st_got_count = 0; return 0;
    case 'M': goto_line(g_scroll + VI_ROWS / 2, 0); st_count = 0; st_got_count = 0; return 0;
    case 'L': {
        int line = g_scroll, i = line_start_number(g_scroll), rows = 0;
        int last = g_scroll;
        while (rows < VI_ROWS - 1) {
            rows += visual_rows_of_line(i);
            if (rows >= VI_ROWS) break;
            i = line_end(i); if (i < eb.len && text_at(i) == '\n') i++;
            last = ++line;
            if (line >= total_lines()) { last = total_lines() - 1; break; }
        }
        goto_line(last, 0);
        st_count = 0; st_got_count = 0;
        return 0; }

    /* ----- operators ----- */
    case 'd': case 'c': case 'y': case '<': case '>':
        st_op = key; st_opcount = st_count; st_count = 0; st_got_count = 0;
        return 0;

    /* ----- quick changes (all built from motions) ----- */
    case 'x': {
        int p = g_pos, n = 0;
        while (n < count && p < eb.len && text_at(p) != '\n') { p = step_forward(p); n++; }
        if (n > 0) {
            m.ok = 1; m.mode = M_EXCL; m.target = p; m.keepcol = 0; m.mkey = 'l';
            apply_op('d', n, m, st_reg);
        }
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0; }
    case 'X': {
        int p = g_pos, n = 0;
        while (n < count && p > 0) { p = step_backward(p); n++; }
        if (n > 0) {
            m.ok = 1; m.mode = M_BACK; m.target = p; m.keepcol = 0; m.mkey = 'h';
            apply_op('d', n, m, st_reg);
        }
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0; }
    case 'D':
        m.ok = 1; m.mode = M_EOL; m.target = line_end(g_pos); m.keepcol = 0; m.mkey = '$';
        apply_op('d', 1, m, st_reg);
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0;
    case 'C':
        m.ok = 1; m.mode = M_EOL; m.target = line_end(g_pos); m.keepcol = 0; m.mkey = '$';
        apply_op('c', 1, m, st_reg);
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0;
    case 's': {
        int p = g_pos, n = 0;
        while (n < count && p < eb.len && text_at(p) != '\n') { p = step_forward(p); n++; }
        if (n > 0) {
            m.ok = 1; m.mode = M_EXCL; m.target = p; m.keepcol = 0; m.mkey = 'l';
            apply_op('c', n, m, st_reg);
        } else {
            begin_insert_session(0);
        }
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0; }
    case 'S':
        m.ok = 1; m.mode = M_LINE; m.keepcol = 0; m.mkey = 'd';
        m.target = line_number_at(g_pos) + count - 1;
        apply_op('c', count, m, st_reg);
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0;
    case 'Y':
        m.ok = 1; m.mode = M_LINE; m.keepcol = 0; m.mkey = 'y';
        m.target = line_number_at(g_pos) + count - 1;
        apply_op('y', count, m, st_reg);
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0;

    case 'r': {
        int ch = get_key();
        if (ch == ESC_KEY) return 0;
        if (ch == KEY_ENTER || ch == '\n') ch = '\n';
        if (ch == TAB_KEY) ch = '\t';
        if ((ch >= 32 && ch <= 126) || ch == '\n' || ch == '\t')
            do_replace(count, ch);
        st_count = 0; st_got_count = 0;
        return 0; }
    case 'R':
        g_enter_replace();
        st_count = 0; st_got_count = 0;
        return 0;
    case '~':
        do_case(count);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'J':
        do_join(count);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'p': case 'P':
        do_put(key == 'p', count, st_reg);
        st_count = 0; st_got_count = 0; st_reg = '"';
        return 0;

    case '"': st_pend = '"'; return 0;
    case 'm': st_pend = 'm'; return 0;
    case '\'': case '`': st_pend = key; return 0;

    case '.':
        replay_last();
        st_count = 0; st_got_count = 0;
        return 0;
    case '&':
        subst_current();
        st_count = 0; st_got_count = 0;
        return 0;

    case 'u': do_undo(); st_count = 0; return 0;
    case CTRL('r'): do_redo(); st_count = 0; return 0;

    case 'i': g_enter_insert(1); st_count = 0; st_got_count = 0; return 0;
    case 'I':
        g_pos = first_nonblank(g_pos);
        g_enter_insert(1);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'a':
        if (g_pos < eb.len && text_at(g_pos) != '\n') g_pos = step_forward(g_pos);
        g_enter_insert(1);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'A':
        g_pos = line_end(g_pos);
        g_enter_insert(1);
        st_count = 0; st_got_count = 0;
        return 0;
    case 'o': case 'O': {
        char ind[128]; int indlen = 0, k;
        int ls = line_start(g_pos);
        int p = ls;
        while (p < eb.len && (text_at(p) == ' ' || text_at(p) == '\t') && indlen < 126)
            ind[indlen++] = text_at(p++);
        g_enter_insert(1);
        if (key == 'o') {
            int le = line_end(g_pos);
            insert_byte(le, '\n');
            g_pos = le + 1;
        } else {
            insert_byte(ls, '\n');
            g_pos = ls;
        }
        sess_capture('\n');
        if (opt_ai)
            for (k = 0; k < indlen; k++) {
                insert_byte(g_pos, ind[k]);
                sess_capture(ind[k]);
                g_pos++;
            }
        st_count = 0; st_got_count = 0;
        return 0; }

    case '/': case '?': {
        char pat[128];
        int hit = 0;
        if (read_prompt(key, pat, sizeof(pat))) {
            if (search_from(pat[0] ? pat : 0, key == '/' ? 1 : -1, count, &hit)) {
                marks[26] = g_pos;
                g_pos = hit;
            }
        }
        st_count = 0; st_got_count = 0;
        return 0; }
    case 'n': case 'N': {
        int hit = 0, dir = (key == 'n') ? last_dir : -last_dir;
        if (search_from(0, dir, count, &hit)) {
            marks[26] = g_pos;
            g_pos = hit;
        }
        st_count = 0; st_got_count = 0;
        return 0; }

    case ':': {
        char excmd2[80];
        int quit;
        if (!read_prompt(':', excmd2, sizeof(excmd2))) return 0;
        quit = ex_run(excmd2);
        st_count = 0; st_got_count = 0;
        return quit; }

    default:
        st_count = 0; st_got_count = 0;
        return 0;
    }
}

/* ============================================================
 * MAIN
 * ============================================================ */
void main(char *args)
{
    uint16_t saved_screen[VGA_TOTAL_CELLS];
    int saved_cursor, i;
    char fname[MAX_PATH];
    int fn = 0;
    const char *s = args;

    if (!args || !args[0]) { print("Usage: vi <file>\n"); return; }
    while (*s && *s != ' ' && fn < MAX_PATH - 1) fname[fn++] = *s++;
    fname[fn] = 0;

    strncpy(g_file, fname, sizeof(g_file));
    g_file[sizeof(g_file) - 1] = 0;
    if (!load_file(g_file, 1)) {
        print("vi: cannot open '");
        print(g_file);
        print("'\n");
        return;
    }
    g_modified = 0;

    for (i = 0; i < VGA_TOTAL_CELLS; i++) saved_screen[i] = VGA[i];
    saved_cursor = cursor;

    g_mode = MODE_COMMAND;
    st_count = 0; st_op = 0; st_pend = 0; st_reg = '"';
    undo_cnt = 0; undo_head = 0; redo_cnt = 0; redo_head = 0;
    for (i = 0; i < 27; i++) marks[i] = 0;
    move_gap(g_pos);

    for (;;) {
        int key, quit = 0;
        if (g_mode == MODE_INSERT) {
            redraw(0);
            key = get_key();
            if (key < 0) continue;
            insert_key(key);
        } else if (g_mode == MODE_REPLACE) {
            redraw(0);
            key = get_key();
            if (key < 0) continue;
            replace_key(key);
        } else {
            redraw(0);
            key = get_key();
            if (key < 0) continue;
            quit = norm_key(key);
            if (quit) break;
        }
    }

    for (i = 0; i < VGA_TOTAL_CELLS; i++) VGA[i] = saved_screen[i];
    cursor = saved_cursor;
    sync_cursor();
}
