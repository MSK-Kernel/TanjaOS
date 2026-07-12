#include "../include/fs.h"
#include <stdint.h>

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

#define CTRL_X 24

#define MAX_TEXT 1024

extern void print(const char*);
extern void clear_screen(void);
extern void putc(char);
extern int get_key(void);

extern int cursor;
extern void sync_cursor(void);


static int strlen_editor(char *s)
{
    int i = 0;
    while (s[i])
        i++;

    return i;
}


static int get_column(char *text, int pos)
{
    int col = 0;

    while (pos > 0 && text[pos-1] != '\n')
    {
        col++;
        pos--;
    }

    return col;
}


static int line_start(char *text, int pos)
{
    while (pos > 0 && text[pos-1] != '\n')
        pos--;

    return pos;
}


static void draw_editor(char *text, int pos)
{
    clear_screen();

    print("TanjaOS Editor\n");
    print("Use [Ctrl] + [X] to save and exit\n");
    print("__________________________________\n\n");

    for (int i = 0; text[i]; i++)
        putc(text[i]);


    int screen_pos = 0;

    for (int i = 0; i < pos; i++)
    {
        if (text[i] == '\n')
            screen_pos = ((screen_pos / 80) + 1) * 80;
        else
            screen_pos++;
    }


    /*
       Put hardware cursor exactly where text will be inserted
    */
    cursor = (80 * 4) + screen_pos;

    sync_cursor();
}



void cmd_editor(char *args)
{
    if (!args || !args[0])
    {
        print("Usage: editor <file>\n");
        return;
    }


    char text[MAX_TEXT];
    uint32_t size = 0;

    text[0] = 0;


    if (fs_file_exists(args))
        fs_read_file(args, text, &size);
    else
        fs_create_file(args);


    int pos = 0;


    while (1)
    {
        draw_editor(text, pos);


        int key = get_key();



        /* CTRL + X */
        if (key == CTRL_X)
        {
            fs_write_file(args, text, strlen_editor(text));

            return;
        }



        /* Insert newline */
        else if (key == KEY_ENTER || key == '\n')
        {
            int len = strlen_editor(text);

            if (len < MAX_TEXT-1)
            {
                for (int i=len; i>=pos; i--)
                    text[i+1] = text[i];

                text[pos] = '\n';

                pos++;
            }
        }



        /* Backspace */
        else if (key == KEY_BACKSPACE || key == 8)
        {
            if (pos > 0)
            {
                int len = strlen_editor(text);

                for (int i=pos-1; i<len; i++)
                    text[i] = text[i+1];

                pos--;
            }
        }



        /* Left */
        else if (key == KEY_LEFT)
        {
            if (pos > 0)
                pos--;
        }



        /* Right */
        else if (key == KEY_RIGHT)
        {
            if (pos < strlen_editor(text))
                pos++;
        }



        /* Up */
        else if (key == KEY_UP)
        {
            int col = get_column(text,pos);

            int start = line_start(text,pos);


            if (start > 0)
            {
                start--;

                while (start > 0 && text[start-1] != '\n')
                    start--;


                pos = start;


                while (col > 0 &&
                       text[pos] &&
                       text[pos] != '\n')
                {
                    pos++;
                    col--;
                }
            }
        }



        /* Down */
        else if (key == KEY_DOWN)
        {
            int col = get_column(text,pos);

            int len = strlen_editor(text);


            while (pos < len && text[pos] != '\n')
                pos++;


            if (pos < len)
            {
                pos++;


                while (col > 0 &&
                       text[pos] &&
                       text[pos] != '\n')
                {
                    pos++;
                    col--;
                }
            }
        }



        /* Normal character */
        else if (key >= 32 && key <= 126)
        {
            int len = strlen_editor(text);

            if (len < MAX_TEXT-1)
            {
                for (int i=len; i>=pos; i--)
                    text[i+1] = text[i];

                text[pos] = key;

                pos++;
            }
        }
    }
}
