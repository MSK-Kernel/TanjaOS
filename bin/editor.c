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

/* Visual column: tabs occupy four terminal cells, but remain one byte in
 * the editable buffer. This makes TAB behave like a real editor tab stop
 * while keeping source files compact and preserving actual tab characters. */
static int line_start(const char *text, int pos);

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

/* Return the first byte position on the line whose visual column is closest
 * to target. We deliberately stop before the character that would put the
 * cursor beyond target, so left/right and up/down feel like a normal editor. */
static int pos_at_visual_col(const char *text, int start, int target)
{
    int len = strlen_editor(text);
    int pos = start;
    int col = 0;

    while (pos < len && text[pos] != '\n') {
        int width;

        if (text[pos] == '\t')
            width = TAB_WIDTH - (col % TAB_WIDTH);
        else
            width = 1;

        /*
         * If the desired visual column falls inside this character,
         * place the cursor at this byte position rather than changing
         * the contents of the line.
         */
        if (target < col + width)
            return pos;

        col += width;
        pos++;
    }

    return pos;
}

static int line_number_at(const char *text, int pos)
{
    int line = 0;
    int i;
    for (i = 0; i < pos && text[i]; i++)
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

/* Draw only the visible part of the file. The old editor printed the entire
 * file and then tried to derive a cursor offset from byte positions. That
 * breaks as soon as the file exceeds the VGA screen, and it also counts tabs
 * as one cell. A real viewport fixes both problems. */
static void draw_editor(const char *text, int pos, int scroll_line)
{
    clear_screen();

    print("TanjaOS Editor\n");
    print("Ctrl+X = Save & Exit\n");
    print("---------------------\n");

    int line = scroll_line;
    int start = line_start_number(text, line);
    int current_line = line_number_at(text, pos);
    int screen_row = EDITOR_TOP_ROW;

    while (screen_row <= EDITOR_BOTTOM_ROW) {
        int i = start;
        int col = 0;

        if (!text[i] && line != current_line)
            break;

        /* Draw this source line directly into VGA memory. */
        while (i < MAX_TEXT && text[i] && text[i] != '\n') {
            if (text[i] == '\t') {
                int width = TAB_WIDTH - (col % TAB_WIDTH);

                for (int k = 0; k < width && col < EDITOR_COLS; k++) {
                    VGA[screen_row * EDITOR_COLS + col] =
                        VGA_COLOR | ' ';
                    col++;
                }
            } else {
                if (col < EDITOR_COLS) {
                    VGA[screen_row * EDITOR_COLS + col] =
                        VGA_COLOR | (uint8_t)text[i];
                    col++;
                }
            }

            i++;

            if (col >= EDITOR_COLS)
                break;
        }

        /* Clear the rest of the screen row. */
        while (col < EDITOR_COLS) {
            VGA[screen_row * EDITOR_COLS + col] =
                VGA_COLOR | ' ';
            col++;
        }

        /*
         * If this is the line containing the text cursor,
         * calculate its VISUAL column and put the hardware cursor there.
         */
        if (line == current_line) {
            int c = visual_col(text, pos);

            if (c >= EDITOR_COLS)
                c = EDITOR_COLS - 1;

            cursor = screen_row * EDITOR_COLS + c;
        }

        /* Move to the next source line. */
        while (i < MAX_TEXT && text[i] && text[i] != '\n')
            i++;

        if (i < MAX_TEXT && text[i] == '\n')
            i++;

        start = i;
        line++;
        screen_row++;

        if (!text[start] && line > current_line)
            break;
    }

    /*
     * Handle the cursor when it is on the blank line at EOF.
     */
    if (current_line >= scroll_line) {
        int row = EDITOR_TOP_ROW + (current_line - scroll_line);

        if (row > EDITOR_BOTTOM_ROW)
            row = EDITOR_BOTTOM_ROW;

        int c = visual_col(text, pos);

        if (c >= EDITOR_COLS)
            c = EDITOR_COLS - 1;

        cursor = row * EDITOR_COLS + c;
    }

    sync_cursor();
}

static void ensure_visible(const char *text, int pos, int *scroll_line)
{
    int line = line_number_at(text, pos);

    if (line < *scroll_line)
        *scroll_line = line;

    if (line >= *scroll_line + EDITOR_ROWS)
        *scroll_line = line - EDITOR_ROWS + 1;

    if (*scroll_line < 0)
        *scroll_line = 0;
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

/* Keep normal typing from running off the right edge of the VGA text area.
 * A source line can be much longer than the screen, but while editing we
 * automatically start a new source line when the cursor reaches column 80.
 * This is especially important with key-repeat: every repeated character
 * continues naturally onto the next line instead of getting stuck at the
 * right edge. */
static void insert_wrapped(char *text, int *pos, char ch)
{
    int col = visual_col(text, *pos);

    if (ch != '\n') {
        int width = (ch == '\t')
            ? TAB_WIDTH - (col % TAB_WIDTH)
            : 1;

        if (col >= EDITOR_COLS || col + width > EDITOR_COLS) {
            insert_byte(text, pos, '\n');
        }
    }

    insert_byte(text, pos, ch);
}

/* Backspace treats a stored TAB as one logical editing unit. It therefore
 * removes the complete four-cell indentation with one keypress. */
static void editor_backspace(char *text, int *pos)
{
    if (*pos <= 0)
        return;

    int p = *pos - 1;
    int len = strlen_editor(text);

    for (int i = p; i < len; i++)
        text[i] = text[i + 1];

    *pos = p;
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

    while (1) {
        ensure_visible(text, pos, &scroll_line);
        draw_editor(text, pos, scroll_line);

        int key = get_key();

        if (key == CTRL_X) {
            fs_write_file(args, text, strlen_editor(text));
            print("\n");
            return;
        }

        if (key == KEY_LEFT) {
            if (pos > 0)
                pos--;
            continue;
        }

        if (key == KEY_RIGHT) {
            if (pos < strlen_editor(text))
                pos++;
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
            /* Store a real tab, not four literal spaces. */
            insert_wrapped(text, &pos, '\t');
            continue;
        }

        if (key >= 32 && key <= 126) {
            insert_wrapped(text, &pos, (char)key);
        }
    }
}
