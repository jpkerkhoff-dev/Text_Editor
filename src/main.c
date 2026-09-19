#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

typedef struct{
  char *b;
  int len;
}AppBuffer;

enum Key_Binds{
    Arrow_Up,
    Arrow_Down,
    Arrow_Left,
    Arrow_Right
};

#define ABUF_INIT {NULL,0}

//void AbAppend(AppBuffer *ab, const char *s, int s_len){}

//void abFree(){}

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

    raw.c_cc[VMIN] = 0;   // Return immediately if no bytes available
    raw.c_cc[VTIME] = 1;  // Wait up to 100ms before returning 0

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr fail (on raw)");
        exit(1);
    }
}

int Editor_Read_Key(void){
    char c;

    while(read(STDIN_FILENO,&c,1) != 1){
        if(c == '\x1b'){
           char next[2];

           if(read(STDIN_FILENO,&next[0],1) !=1){
                return '\x1b';
           }
           if(read(STDIN_FILENO,&next[1],1)!=1){
                return '\x1b';
           }
           if(next[0] == '['){
                switch(next[1]){
                    case 'A': return "Arrow_Up";         
                    case 'B': return "Arrow_Down";
                    case 'C': return "Arrow_Right";
                    case 'D': return "Arrow_Left";
                }
           }
           return '\x1b';
    }
  }
    return c;
}

int Editor_Interpret_Key(void){
      int key = Editor_Read_Key;

      switch(key){
        case Arrow_Up:
          cursor_y--;
          break;
        case Arrow_Down:
          cursor_y++;
          break;
        case Arrow_Right:
          cursor_x++;
          break;
        case Arrow_Left:
          cursor_x--;
          break;
      }
}

int main(void) {
  Enable_Rawmode();

  while (1) {
    
  }

return 0;
}
