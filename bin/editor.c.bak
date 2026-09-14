#include "../include/fs.h"
#include "../include/utf8.h"
#include <stdint.h>

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

#define CTRL_X 24
#define TAB_KEY 9
#define MAX_TEXT (MAX_FILE_SIZE + 1)

#define EDITOR_TOP_ROW 3
#define EDITOR_BOTTOM_ROW 23
#define EDITOR_ROWS (EDITOR_BOTTOM_ROW - EDITOR_TOP_ROW + 1)
#define EDITOR_COLS 80
#define TAB_WIDTH 4
#define VGA_HEIGHT 25
#define VGA_TOTAL_CELLS (EDITOR_COLS * VGA_HEIGHT)

extern void print(const char*);
extern void clear_screen(void);
extern void putc(char);
extern int get_key(void);

extern int cursor;
extern void sync_cursor(void);
extern uint16_t *VGA;

#define VGA_COLOR (0x0F << 8)

/* The editor already owns the complete text buffer.  Keep its length cached
 * instead of rescanning up to 64 KiB every time the cursor moves or a frame
 * is redrawn.  The old strlen-on-every-operation behavior made large files
 * feel like a hang because several helpers called it recursively. */
static int editor_text_len = 0;

static int strlen_editor(const char *s)
{
    (void)s;
    return editor_text_len;
}

static int line_start(const char *text, int pos)
{
    while (pos > 0 && text[pos - 1] != '\n')
        pos--;
    return pos;
}

static int line_end(const char *text, int pos)
{
    int len = strlen_editor(text);
    while (pos < len && text[pos] != '\n')
        pos++;
    return pos;
}

static int visual_col(const char *text, int pos)
{
    int col = 0;
    int i = line_start(text, pos);

    while (i < pos && text[i] != '\n') {
        if (text[i] == '\t') {
            col += TAB_WIDTH - (col % TAB_WIDTH);
            i++;
        } else {
            /* One UTF-8 sequence = one screen cell, so the visual
             * column matches what draw_editor() actually renders. */
            uint32_t cp;
            int step = utf8_decode(&text[i], pos - i, &cp);
            if (step < 1) step = 1;
            col++;
            i += step;
        }
    }
    return col;
}

static int snap_to_tab_stop(const char *text, int pos)
{
    int start = line_start(text, pos);
    int col = visual_col(text, pos);

    int all_spaces = 1;
    for (int i = start; i < pos; i++) {
        if (text[i] != ' ') {
            all_spaces = 0;
            break;
        }
    }

    if (all_spaces) {
        int snapped_col = (col / TAB_WIDTH) * TAB_WIDTH;
        return start + snapped_col;
    }

    return pos;
}

static int pos_at_visual_col(const char *text, int start, int target)
{
    int len = strlen_editor(text);
    int pos = start;
    int col = 0;

    while (pos < len && text[pos] != '\n') {
        int width;
        int step = 1;

        if (text[pos] == '\t') {
            width = TAB_WIDTH - (col % TAB_WIDTH);
        } else {
            /* Walk whole UTF-8 sequences, not single bytes, so a
             * multi-byte character is one cell here too. */
            uint32_t cp;
            step = utf8_decode(&text[pos], len - pos, &cp);
            if (step < 1) step = 1;
            width = 1;
        }

        if (target < col + width)
            return snap_to_tab_stop(text, pos);

        col += width;
        pos += step;
    }

    return snap_to_tab_stop(text, pos);
}

static int line_number_at(const char *text, int pos)
{
    int line = 0;
    for (int i = 0; i < pos && text[i]; i++)
        if (text[i] == '\n') line++;
    return line;
}

static int line_start_number(const char *text, int wanted)
{
    int line = 0;
    int i = 0;

    while (line < wanted && text[i]) {
        if (text[i] == '\n') line++;
        i++;
    }
    return i;
}

/* Move one character (UTF-8 codepoint) forward.  This keeps the
 * cursor aligned to sequence starts so the renderer never lands
 * in the middle of a multi-byte character. */
static int step_forward(const char *text, int pos)
{
    int len = strlen_editor(text);
    uint32_t cp;
    int step;

    if (pos >= len)
        return pos;
    step = utf8_decode(&text[pos], len - pos, &cp);
    if (step < 1) step = 1;
    return pos + step;
}

/* Move one character (UTF-8 codepoint) backward: scan back past
 * UTF-8 continuation bytes to the start of the sequence. */
static int step_backward(const char *text, int pos)
{
    int i;

    if (pos <= 0)
        return pos;

    i = pos - 1;
    while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80)
        i--;
    return i;
}

/* Wrap-aware vertical movement.
 *
 * The editor word-wraps long lines onto several screen rows, so
 * "down" must mean "down one VISUAL row", not "down one logical
 * line": when the line under the cursor continues on the next
 * screen row, the cursor moves onto that wrapped continuation
 * instead of skipping ahead to the next real line.  Likewise "up"
 * from the first row of a wrapped line moves to the last screen
 * row of the previous line, at the same on-screen column. */
static int move_vertical(const char *text, int pos, int dir)
{
    int len = strlen_editor(text);
    int c = visual_col(text, pos);
    int row = c / EDITOR_COLS;
    int scol = c % EDITOR_COLS;   /* on-screen column to preserve */
    int ls = line_start(text, pos);
    int width = visual_col(text, line_end(text, pos));

    if (dir < 0) {
        /* UP */
        if (row > 0) {
            /* Still inside a wrapped line: move to the row above. */
            return pos_at_visual_col(text, ls, (row - 1) * EDITOR_COLS + scol);
        }
        {
            int line = line_number_at(text, pos);
            if (line == 0)
                return pos;
            {
                int prev_start = line_start_number(text, line - 1);
                int prev_width = visual_col(text, line_end(text, prev_start));
                int prev_rows = prev_width / EDITOR_COLS + 1;
                /* Land on the last screen row of the previous line,
                 * clamped to its end if it is shorter than scol. */
                return pos_at_visual_col(text, prev_start,
                                         (prev_rows - 1) * EDITOR_COLS + scol);
            }
        }
    } else {
        /* DOWN */
        if ((row + 1) * EDITOR_COLS < width) {
            /* The current line continues on the next screen row:
             * step onto the wrapped continuation instead of
             * skipping to the next real line. */
            return pos_at_visual_col(text, ls, (row + 1) * EDITOR_COLS + scol);
        }
        {
            int cur_end = line_end(text, pos);
            if (cur_end >= len)
                return pos;   /* last line, nowhere to go */
            /* Last row of this line: move to the next real line,
             * keeping the same on-screen column. */
            return pos_at_visual_col(text, cur_end + 1, scol);
        }
    }
}

static int visual_rows_of_line(const char *text, int ls)
{
    int width = visual_col(text, line_end(text, ls));
    /* A line that exactly fills its last screen row (e.g. exactly
     * 80 columns) still renders as just that one row: the next byte
     * is a newline, so no empty continuation row is drawn.  Only
     * content BEYOND a full row creates another row, hence the
     * (width-1) here.  The old `width / EDITOR_COLS + 1` counted a
     * phantom extra row for every exact multiple of 80, which made
     * the cursor-row math (and wrap-aware Up) land one row off. */
    if (width == 0)
        return 1;
    return (width - 1) / EDITOR_COLS + 1;
}

static void draw_editor(const char *text, int pos, int scroll_line)
{
    clear_screen();

    print("TanjaOS Editor\n");
    print("Ctrl+X = Save & Exit\n");
    print("--------------------\n");

    int line = scroll_line;
    int start = line_start_number(text, line);
    int screen_row = EDITOR_TOP_ROW;

    while (screen_row <= EDITOR_BOTTOM_ROW) {
        int j = start;

        do {
            int col = 0;
            while (text[j] && text[j] != '\n' && col < EDITOR_COLS) {
                if (text[j] == '\t') {
                    int width = TAB_WIDTH - (col % TAB_WIDTH);
                    for (int k = 0; k < width && col < EDITOR_COLS; k++) {
                        VGA[screen_row * EDITOR_COLS + col] = VGA_COLOR | ' ';
                        col++;
                    }
                    j++;
                } else {
                    /* Decode one UTF-8 sequence and render it as a
                     * single ASCII cell: “ ” become ", — becomes -,
                     * ʼ becomes ', other non-ASCII becomes '?', and
                     * invalid bytes draw raw like before.  A sequence
                     * is never split across two screen rows. */
                    uint32_t cp;
                    int step = utf8_decode(&text[j], editor_text_len - j, &cp);
                    char cell;
                    if (step < 1) step = 1;
                    if (cp == UTF8_INVALID) {
                        cell = text[j];
                    } else if (cp < 0x80) {
                        cell = (char)cp;
                    } else {
                        int m = utf8_map_ascii(cp);
                        cell = m ? (char)m : '?';
                    }
                    VGA[screen_row * EDITOR_COLS + col] = VGA_COLOR | (uint8_t)cell;
                    col++;
                    j += step;
                }
            }
            while (col < EDITOR_COLS) {
                VGA[screen_row * EDITOR_COLS + col] = VGA_COLOR | ' ';
                col++;
            }
            screen_row++;
        } while (text[j] && text[j] != '\n' && screen_row <= EDITOR_BOTTOM_ROW);

        if (!text[j])
            break;
        start = j + 1;
        line++;
    }

    int current_line = line_number_at(text, pos);
    if (current_line >= scroll_line) {
        int rows_before = 0;
        int line_i = scroll_line;
        int i = line_start_number(text, line_i);
        while (line_i < current_line) {
            rows_before += visual_rows_of_line(text, i);
            i = line_end(text, i);
            if (text[i] == '\n') i++;
            line_i++;
        }

        int c = visual_col(text, pos);
        int seg = c / EDITOR_COLS;
        int col_offset = c % EDITOR_COLS;

        int final_row = EDITOR_TOP_ROW + rows_before + seg;
        if (final_row > EDITOR_BOTTOM_ROW) final_row = EDITOR_BOTTOM_ROW;

        cursor = final_row * EDITOR_COLS + col_offset;
    }

    sync_cursor();
}

static void ensure_visible(const char *text, int pos, int *scroll_line)
{
    int current_line = line_number_at(text, pos);

    if (current_line < *scroll_line)
        *scroll_line = current_line;

    if (*scroll_line < 0)
        *scroll_line = 0;

    for (;;) {
        int rows = 0;
        int line = *scroll_line;
        int i = line_start_number(text, line);

        while (line < current_line) {
            rows += visual_rows_of_line(text, i);
            i = line_end(text, i);
            if (text[i] == '\n') i++;
            line++;
        }
        rows += visual_col(text, pos) / EDITOR_COLS;

        if (rows < EDITOR_ROWS)
            break;
        if (*scroll_line >= current_line)
            break;

        (*scroll_line)++;
    }
}

static void insert_byte(char *text, int *pos, char ch)
{
    int len = strlen_editor(text);
    if (len >= MAX_TEXT - 1)
        return;

    for (int i = len; i >= *pos; i--)
        text[i + 1] = text[i];

    text[*pos] = ch;
    (*pos)++;
    editor_text_len++;
}

static void insert_wrapped(char *text, int *pos, char ch)
{

    insert_byte(text, pos, ch);
}

static void delete_bytes(char *text, int *pos, int count)
{
    if (*pos < count)
        count = *pos;

    int len = strlen_editor(text);
    for (int i = *pos - count; i <= len - count; i++) {
        text[i] = text[i + count];
    }
    *pos -= count;
    editor_text_len -= count;
    if (editor_text_len < 0) editor_text_len = 0;
}

static void editor_backspace(char *text, int *pos)
{
    int start;

    if (*pos <= 0)
        return;

    /* Delete the entire previous character, not just one byte: a
     * multi-byte UTF-8 sequence (e.g. “ stored as three bytes)
     * disappears in one keystroke.  A tab is stored as a single
     * '\t' byte, so it still deletes in one keystroke too.
     * Literal spaces (e.g. from pressing space) are always deleted
     * one at a time, no matter how many are in a row. */
    start = step_backward(text, *pos);
    delete_bytes(text, pos, *pos - start);
}

void cmd_editor(char *args)
{
    if (!args || !args[0]) {
        print("Usage: editor <file>\n");
        return;
    }

    static char text[MAX_TEXT];
    uint32_t size = sizeof(text);
    text[0] = 0;

    if (fs_file_exists(args)) {
        if (fs_read_file(args, text, &size) != 0) {
            print("editor: cannot read '");
            print(args);
            print("'\n");
            return;
        }
    } else {
        if (fs_create_file(args) != 0) {
            print("editor: cannot open '");
            print(args);
            print("': No such directory\n");
            return;
        }
    }

    if (size >= MAX_TEXT)
        text[MAX_TEXT - 1] = 0;
    else
        text[size] = 0;

    editor_text_len = (int)size;
    if (editor_text_len >= MAX_TEXT) editor_text_len = MAX_TEXT - 1;

    /* Open every file at its beginning, not at EOF. */
    int pos = 0;
    int scroll_line = 0;

    uint16_t saved_screen[VGA_TOTAL_CELLS];
    for (int i = 0; i < VGA_TOTAL_CELLS; i++)
        saved_screen[i] = VGA[i];
    int saved_cursor = cursor;

    while (1) {
        ensure_visible(text, pos, &scroll_line);
        draw_editor(text, pos, scroll_line);

        int key = get_key();

        if (key == CTRL_X) {
            if (fs_write_file(args, text, (uint32_t)strlen_editor(text)) != 0) {
                print("editor: cannot save '");
                print(args);
                print("': file is too large or filesystem is full\\n");
                continue;
            }
            for (int i = 0; i < VGA_TOTAL_CELLS; i++)
                VGA[i] = saved_screen[i];
            cursor = saved_cursor;
            sync_cursor();
            return;
        }

        if (key == KEY_LEFT) {
            pos = step_backward(text, pos);
            continue;
        }

        if (key == KEY_RIGHT) {
            pos = step_forward(text, pos);
            continue;
        }

        if (key == KEY_UP || key == KEY_DOWN) {
            /* Wrap-aware: moves one VISUAL row at a time, so wrapped
             * continuations of a long line are visited like real
             * lines instead of being skipped over. */
            pos = move_vertical(text, pos, (key == KEY_UP) ? -1 : 1);
            continue;
        }

        if (key == KEY_ENTER || key == '\n') {
            insert_byte(text, &pos, '\n');
            continue;
        }

        if (key == KEY_BACKSPACE || key == 8) {
            editor_backspace(text, &pos);
            continue;
        }

        if (key == TAB_KEY) {
            insert_wrapped(text, &pos, '\t');
            continue;
        }

        if (key >= 32 && key <= 126) {
            insert_wrapped(text, &pos, (char)key);
        }
    }
}
