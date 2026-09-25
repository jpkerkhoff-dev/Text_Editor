#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    char *b;
    int len;
} AppBuffer;

enum Key_Binds {
    ARR_UP = 1000,
    ARR_DO,
    ARR_LE,
    ARR_RI
};

#define ABUF_INIT {NULL, 0}

struct termios or_term;

void Disable_Rawmode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &or_term);
}

void Enable_Rawmode(void) {
    if (tcgetattr(STDIN_FILENO, &or_term) == -1) {
        perror("tcgetattr fail");
        exit(1);
    }

    atexit(Disable_Rawmode);

    struct termios raw = or_term;

    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);

    /*
     * Wait until at least one byte is available.
     * This prevents read() from returning 0 after 100 ms
     * and immediately closing the program.
     */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr fail (on raw)");
        exit(1);
    }
}

int Interpret_Key(void) {
    char c;

    /*
     * With VMIN = 1 this normally blocks until
     * a character is available.
     */
    while (read(STDIN_FILENO, &c, 1) != 1) {
    }

    /*
     * Escape sequences are used by arrow keys.
     */
    if (c == '\x1b') {
        char seq[2];

        /*
         * Temporarily use a timeout so pressing ESC by itself
         * does not block forever waiting for more characters.
         */
        struct termios raw;

        if (tcgetattr(STDIN_FILENO, &raw) == -1) {
            return '\x1b';
        }

        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
            read(STDIN_FILENO, &seq[1], 1) != 1) {

            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);

            return '\x1b';
        }

        /*
         * Restore normal blocking raw-mode reads.
         */
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A':
                    return ARR_UP;

                case 'B':
                    return ARR_DO;

                case 'C':
                    return ARR_RI;

                case 'D':
                    return ARR_LE;
            }
        }

        return '\x1b';
    }

    return (unsigned char)c;
}

void Read_Key(void) {
    while (1) {
        int key = Interpret_Key();

        /*
         * Ctrl+Q = ASCII 17
         */
        if (key == 17) {
            printf("Quitting...\r\n");
            break;
        }

        switch (key) {
            case ARR_UP:
                printf("Arrow Up\r\n");
                break;

            case ARR_DO:
                printf("Arrow Down\r\n");
                break;

            case ARR_LE:
                printf("Arrow Left\r\n");
                break;

            case ARR_RI:
                printf("Arrow Right\r\n");
                break;

            default:
                if (key >= 1 && key <= 26) {
                    printf("CTRL + %c\r\n", 'A' + key - 1);
                } else if (iscntrl((unsigned char)key)) {
                    printf("Control character ASCII: %d\r\n", key);
                } else {
                    printf("%c ASCII: %d\r\n", key, key);
                }
                break;
        }
    }
}

int main(void) {
    Enable_Rawmode();
    Read_Key();

    return 0;
}