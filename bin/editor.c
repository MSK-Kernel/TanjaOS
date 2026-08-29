#include "../include/fs.h"
#include <stdint.h>

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

#define CTRL_X 24
#define TAB_KEY 9
#define MAX_TEXT 4096

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

static int strlen_editor(const char *s)
{
    int i = 0;
    while (i < MAX_TEXT && s[i]) i++;
    return i;
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
        if (text[i] == '\t')
            col += TAB_WIDTH - (col % TAB_WIDTH);
        else
            col++;
        i++;
    }
    return col;
}

static int is_leading_space_tab(const char *text, int pos)
{
    int start = line_start(text, pos);
    int offset = pos - start;

    if (offset <= 0 || offset % TAB_WIDTH != 0)
        return 0;

    for (int i = start; i < pos; i++) {
        if (text[i] != ' ')
            return 0;
    }
    return 1;
}

static int is_space_block_forward(const char *text, int pos)
{
    int start = line_start(text, pos);
    int end = line_end(text, pos);
    int offset = pos - start;

    if (offset % TAB_WIDTH != 0)
        return 0;
    if (pos + TAB_WIDTH > end)
        return 0;

    for (int i = pos; i < pos + TAB_WIDTH; i++) {
        if (text[i] != ' ')
            return 0;
    }
    return 1;
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
        int width = (text[pos] == '\t') ? (TAB_WIDTH - (col % TAB_WIDTH)) : 1;

        if (target < col + width)
            return snap_to_tab_stop(text, pos);

        col += width;
        pos++;
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

static int visual_rows_of_line(const char *text, int ls)
{
    int width = visual_col(text, line_end(text, ls));
    return width / EDITOR_COLS + 1;
}

static void draw_editor(const char *text, int pos, int scroll_line)
{
    clear_screen();

    print("TanjaOS Editor\n");
    print("Ctrl+X = Save & Exit\n");
    print("---------------------\n");

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
                } else {
                    VGA[screen_row * EDITOR_COLS + col] = VGA_COLOR | (uint8_t)text[j];
                    col++;
                }
                j++;
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
}

static void editor_backspace(char *text, int *pos)
{
    if (*pos <= 0)
        return;

    if (is_leading_space_tab(text, *pos)) {
        delete_bytes(text, pos, TAB_WIDTH);
        return;
    }

    delete_bytes(text, pos, 1);
}

void cmd_editor(char *args)
{
    if (!args || !args[0]) {
        print("Usage: editor <file>\n");
        return;
    }

    char text[MAX_TEXT];
    uint32_t size = 0;
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

    int pos = strlen_editor(text);
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
            fs_write_file(args, text, strlen_editor(text));
            for (int i = 0; i < VGA_TOTAL_CELLS; i++)
                VGA[i] = saved_screen[i];
            cursor = saved_cursor;
            sync_cursor();
            return;
        }

        if (key == KEY_LEFT) {
            if (pos > 0) {
                if (is_leading_space_tab(text, pos)) {
                    pos -= TAB_WIDTH;
                } else {
                    pos--;
                }
            }
            continue;
        }

        if (key == KEY_RIGHT) {
            int len = strlen_editor(text);
            if (pos < len) {
                if (is_space_block_forward(text, pos)) {
                    pos += TAB_WIDTH;
                } else {
                    pos++;
                }
            }
            continue;
        }

        if (key == KEY_UP || key == KEY_DOWN) {
            int wanted = visual_col(text, pos);
            int line = line_number_at(text, pos);

            if (key == KEY_UP) {
                if (line > 0) {
                    int start = line_start_number(text, line - 1);
                    pos = pos_at_visual_col(text, start, wanted);
                }
            } else {
                int current_end = line_end(text, pos);
                if (current_end < strlen_editor(text)) {
                    int start = current_end + 1;
                    pos = pos_at_visual_col(text, start, wanted);
                }
            }
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
