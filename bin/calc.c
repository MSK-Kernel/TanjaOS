#include "bin.h"

// Helper to read a line of text from the keyboard
static void read_line(char* buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        int key = get_key();
        if (key == KEY_ENTER || key == '\n' || key == '\r') {
            putc('\n');
            break;
        } else if (key == KEY_BACKSPACE || key == '\b') {
            if (i > 0) {
                i--;
                putc('\b');
                putc(' ');
                putc('\b');
            }
        } else if (key >= 32 && key <= 126) {
            buf[i++] = (char)key;
            putc((char)key);
        }
    }
    buf[i] = '\0';
}

void cmd_calc(char* args) {
    char buf[32];

    print("\nTanjaOS Calculator\n");
    print("------------------\n");
    print("(+) Addition\n");
    print("(-) Substraction\n");
    print("(*) Multiplication\n");
    print("(/) Division\n");
    print("(%) Modulo\n\n");
    print("Enter an operator: ");
    read_line(buf, sizeof(buf));
    char op = buf[0];

    print("Enter first number: ");
    read_line(buf, sizeof(buf));
    int num1 = atoi(buf);

    print("Enter second number: ");
    read_line(buf, sizeof(buf));
    int num2 = atoi(buf);

    print("Result: ");
    switch (op) {
        case '+':
            print_dec(num1 + num2);
            break;

	case '-':
            if (num1 < num2) {
                putc('-');
                print_dec(num2 - num1);
            } else {
                print_dec(num1 - num2);
            }
            break;

	case '*':
            print_dec(num1 * num2);
            break;

	case '/':
            if (num2 != 0) {
                print_dec(num1 / num2);
            } else {
                print("error: Cannot divide by 0");
            }
            break;

	case '%':
            if (num2 != 0) {
                print_dec(num1 % num2);
            } else {
                print("error: Cannot modulo by 0");
            }
            break;

	default:
            print("error: Invalid operator");
            break;
    }
    print("\n\n");
}
