#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

typedef struct{
	char *b;
	int len;
}AppBuffer;

#define ABUF_INIT {NULL,0}

void AbAppend(AppBuffer *ab, const char *s, int s_len){
	
}

void abFree(){

}


struct termios orsettings_term;

// 1. Declare prototype first so enable_rawmode knows disable_rawmode exists
void disable_rawmode(void);

void disable_rawmode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orsettings_term);
}

void enable_rawmode(void) {
    if (tcgetattr(STDIN_FILENO, &orsettings_term) == -1) {
        perror("tcgetattr fail");
        exit(1);
    }

    atexit(disable_rawmode);

    struct termios raw = orsettings_term;

    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);

    raw.c_cc[VMIN] = 0;   // Return immediately if no bytes available
    raw.c_cc[VTIME] = 1;  // Wait up to 100ms before returning 0

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr fail (new attributes)");
        exit(1);
    }
}

int main(void) {
    enable_rawmode();

    while (1) {
        
    }

    return 0;
}