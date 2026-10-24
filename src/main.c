#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TAB_STOP 4
#define STATUS_MESSAGE_SECONDS 5
#define ABUF_INIT { NULL, 0 }

typedef struct {
    char *data;
    size_t len;
} AppendBuffer;

typedef enum {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_STRING,
    HL_COMMENT,
    HL_KEYWORD,
    HL_TYPE,
    HL_PREPROCESSOR
} Highlight;

typedef enum {
    SYNTAX_NONE = 0,
    SYNTAX_C
} SyntaxType;

typedef struct {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} EditorRow;

typedef struct {
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;

    int numrows;
    EditorRow *row;

    int dirty;
    int end_with_newline;
    int quit_times;
    int alternate_screen;
    int rawmode;

    char *filename;
    SyntaxType syntax;

    char statusmsg[1024];
    time_t statusmsg_time;
} EditorConfig;

enum EditorKey {
    KEY_NULL = 0,
    CTRL_C = 3,
    CTRL_H = 8,
    TAB = 9,
    ENTER = 13,
    CTRL_Q = 17,
    CTRL_S = 19,
    ESC = 27,
    BACKSPACE = 127,

    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DELETE_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

static EditorConfig E;
static struct termios original_termios;

static const char *const c_keywords[] = {
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex",
    "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local", "alignas", "alignof", "and", "asm", "auto",
    "bitand", "bitor", "break", "case", "catch", "class", "concept",
    "const", "consteval", "constexpr", "constinit", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "for", "friend", "goto", "if", "inline", "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "restrict", "return",
    "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "using", "virtual",
    "volatile", "while", "NULL"
};

static const char *const c_types[] = {
    "bool", "char", "double", "FILE", "float", "int", "long",
    "ptrdiff_t", "short", "signed", "size_t", "ssize_t", "uint8_t",
    "uint16_t", "uint32_t", "uint64_t", "uintptr_t", "unsigned",
    "void", "wchar_t", "int8_t", "int16_t", "int32_t", "int64_t"
};

static void editorSetStatusMessage(const char *fmt, ...);
static void editorRefreshScreen(void);
static void editorUpdateSyntaxAll(void);
static void editorUpdateRow(EditorRow *row);
static void editorInsertRow(int at, const char *s, size_t len);
static void editorInsertChar(int c);
static void editorInsertNewline(void);
static void editorBackspace(void);
static void editorDeleteAtCursor(void);
static void editorSave(void);

static void writeAll(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

static void appendBytes(AppendBuffer *ab, const char *s, size_t len)
{
    if (len == 0) {
        return;
    }
    if (len > SIZE_MAX - ab->len) {
        fprintf(stderr, "Screen buffer is too large\n");
        exit(EXIT_FAILURE);
    }

    char *new_data = realloc(ab->data, ab->len + len);
    if (new_data == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }

    memcpy(new_data + ab->len, s, len);
    ab->data = new_data;
    ab->len += len;
}

static void appendString(AppendBuffer *ab, const char *s)
{
    appendBytes(ab, s, strlen(s));
}

static void freeAppendBuffer(AppendBuffer *ab)
{
    free(ab->data);
    ab->data = NULL;
    ab->len = 0;
}

static int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_row == 0 || ws.ws_col == 0) {
        return -1;
    }

    *rows = (int)ws.ws_row;
    *cols = (int)ws.ws_col;
    return 0;
}

static void editorUpdateWindowSize(void)
{
    int rows;
    int cols;
    if (getWindowSize(&rows, &cols) == -1) {
        return;
    }

    E.screenrows = rows - 2;
    if (E.screenrows < 1) {
        E.screenrows = 1;
    }
    E.screencols = cols > 0 ? cols : 1;
}

static void editorDisableRawMode(void)
{
    if (E.rawmode) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        E.rawmode = 0;
    }

    if (E.alternate_screen) {
        static const char leave_screen[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
        writeAll(STDOUT_FILENO, leave_screen, sizeof(leave_screen) - 1);
        E.alternate_screen = 0;
    } else {
        static const char reset_terminal[] = "\x1b[0m\x1b[?25h";
        writeAll(STDOUT_FILENO, reset_terminal, sizeof(reset_terminal) - 1);
    }
}

static int editorEnableRawMode(void)
{
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        return -1;
    }

    struct termios raw = original_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return -1;
    }

    E.rawmode = 1;
    if (atexit(editorDisableRawMode) != 0) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        E.rawmode = 0;
        return -1;
    }
    return 0;
}

static void editorEnterAlternateScreen(void)
{
    static const char enter_screen[] = "\x1b[?1049h\x1b[?25l";
    writeAll(STDOUT_FILENO, enter_screen, sizeof(enter_screen) - 1);
    E.alternate_screen = 1;
}

static void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

static void editorFreeRow(EditorRow *row)
{
    free(row->chars);
    free(row->render);
    free(row->hl);
    row->chars = NULL;
    row->render = NULL;
    row->hl = NULL;
}

static void editorFreeRows(void)
{
    for (int i = 0; i < E.numrows; i++) {
        editorFreeRow(&E.row[i]);
    }
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
}

static void editorFree(void)
{
    editorFreeRows();
    free(E.filename);
    E.filename = NULL;
}

static int rowCxToRx(const EditorRow *row, int cx)
{
    int rx = 0;
    if (cx > row->size) {
        cx = row->size;
    }
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

static void editorUpdateRow(EditorRow *row)
{
    size_t tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    size_t render_size = (size_t)row->size + tabs * (TAB_STOP - 1) + 1;
    if (render_size > (size_t)INT_MAX) {
        fprintf(stderr, "Line is too large to render\n");
        exit(EXIT_FAILURE);
    }

    char *new_render = malloc(render_size);
    if (new_render == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            new_render[idx++] = ' ';
            while (idx % TAB_STOP != 0) {
                new_render[idx++] = ' ';
            }
        } else {
            new_render[idx++] = row->chars[j];
        }
    }
    new_render[idx] = '\0';

    free(row->render);
    row->render = new_render;
    row->rsize = idx;

    unsigned char *new_hl = realloc(row->hl, (size_t)(row->rsize > 0 ? row->rsize : 1));
    if (new_hl == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    row->hl = new_hl;
    memset(row->hl, HL_NORMAL, (size_t)(row->rsize > 0 ? row->rsize : 1));
}

static int wordInList(const char *word, size_t len,
                      const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strlen(list[i]) == len && memcmp(word, list[i], len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int isCKeyword(const char *word, size_t len)
{
    return wordInList(word, len, c_keywords,
                      sizeof(c_keywords) / sizeof(c_keywords[0]));
}

static int isCType(const char *word, size_t len)
{
    return wordInList(word, len, c_types,
                      sizeof(c_types) / sizeof(c_types[0]));
}

static void editorSelectSyntaxHighlight(void)
{
    E.syntax = SYNTAX_NONE;
    if (E.filename == NULL) {
        return;
    }

    const char *slash = strrchr(E.filename, '/');
    const char *dot = strrchr(E.filename, '.');
    if (dot == NULL || (slash != NULL && dot < slash)) {
        return;
    }

    if (strcasecmp(dot, ".c") == 0 || strcasecmp(dot, ".h") == 0 ||
        strcasecmp(dot, ".cc") == 0 || strcasecmp(dot, ".cpp") == 0 ||
        strcasecmp(dot, ".cxx") == 0 || strcasecmp(dot, ".hpp") == 0 ||
        strcasecmp(dot, ".hh") == 0 || strcasecmp(dot, ".hxx") == 0) {
        E.syntax = SYNTAX_C;
    }
}

static void editorUpdateSyntaxAll(void)
{
    int in_comment = 0;

    for (int row_index = 0; row_index < E.numrows; row_index++) {
        EditorRow *row = &E.row[row_index];
        if (row->rsize > 0) {
            memset(row->hl, HL_NORMAL, (size_t)row->rsize);
        }

        if (E.syntax != SYNTAX_C) {
            row->hl_open_comment = 0;
            in_comment = 0;
            continue;
        }

        int first_nonspace = 0;
        while (first_nonspace < row->rsize &&
               isspace((unsigned char)row->render[first_nonspace])) {
            first_nonspace++;
        }
        int preprocessor = !in_comment && first_nonspace < row->rsize &&
                           row->render[first_nonspace] == '#';

        int i = 0;
        while (i < row->rsize) {
            if (in_comment) {
                row->hl[i] = HL_COMMENT;
                if (i + 1 < row->rsize && row->render[i] == '*' &&
                    row->render[i + 1] == '/') {
                    row->hl[i + 1] = HL_COMMENT;
                    i += 2;
                    in_comment = 0;
                } else {
                    i++;
                }
                continue;
            }

            if (preprocessor) {
                row->hl[i++] = HL_PREPROCESSOR;
                continue;
            }

            if (i + 1 < row->rsize && row->render[i] == '/' &&
                row->render[i + 1] == '/') {
                memset(row->hl + i, HL_COMMENT, (size_t)(row->rsize - i));
                break;
            }

            if (i + 1 < row->rsize && row->render[i] == '/' &&
                row->render[i + 1] == '*') {
                row->hl[i] = HL_COMMENT;
                row->hl[i + 1] = HL_COMMENT;
                i += 2;
                in_comment = 1;
                continue;
            }

            if (row->render[i] == '"' || row->render[i] == '\'') {
                char quote = row->render[i];
                row->hl[i++] = HL_STRING;
                while (i < row->rsize) {
                    row->hl[i] = HL_STRING;
                    if (row->render[i] == '\\' && i + 1 < row->rsize) {
                        row->hl[i + 1] = HL_STRING;
                        i += 2;
                        continue;
                    }
                    if (row->render[i++] == quote) {
                        break;
                    }
                }
                continue;
            }

            if (isdigit((unsigned char)row->render[i]) &&
                (i == 0 || (!isalnum((unsigned char)row->render[i - 1]) &&
                            row->render[i - 1] != '_'))) {
                int start = i++;
                while (i < row->rsize &&
                       (isalnum((unsigned char)row->render[i]) ||
                        row->render[i] == '.' || row->render[i] == '_')) {
                    i++;
                }
                memset(row->hl + start, HL_NUMBER, (size_t)(i - start));
                continue;
            }

            if (isalpha((unsigned char)row->render[i]) || row->render[i] == '_') {
                int start = i++;
                while (i < row->rsize &&
                       (isalnum((unsigned char)row->render[i]) ||
                        row->render[i] == '_')) {
                    i++;
                }
                size_t len = (size_t)(i - start);
                if (isCKeyword(row->render + start, len)) {
                    memset(row->hl + start, HL_KEYWORD, len);
                } else if (isCType(row->render + start, len)) {
                    memset(row->hl + start, HL_TYPE, len);
                }
                continue;
            }

            i++;
        }

        row->hl_open_comment = in_comment;
    }
}

static void editorInsertRow(int at, const char *s, size_t len)
{
    if (at < 0 || at > E.numrows || len > (size_t)INT_MAX) {
        return;
    }

    if ((size_t)E.numrows >= SIZE_MAX / sizeof(EditorRow) - 1) {
        fprintf(stderr, "Too many lines\n");
        exit(EXIT_FAILURE);
    }

    EditorRow *new_rows = realloc(E.row, sizeof(EditorRow) * (size_t)(E.numrows + 1));
    if (new_rows == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    E.row = new_rows;

    if (at < E.numrows) {
        memmove(&E.row[at + 1], &E.row[at],
                sizeof(EditorRow) * (size_t)(E.numrows - at));
    }

    for (int j = at + 1; j <= E.numrows; j++) {
        E.row[j].idx = j;
    }

    EditorRow *row = &E.row[at];
    memset(row, 0, sizeof(*row));
    row->idx = at;
    row->size = (int)len;
    row->chars = malloc(len + 1);
    if (row->chars == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    if (len > 0) {
        memcpy(row->chars, s, len);
    }
    row->chars[len] = '\0';
    editorUpdateRow(row);

    E.numrows++;
    E.dirty++;
    for (int j = at; j < E.numrows; j++) {
        E.row[j].idx = j;
    }
}

static void editorDeleteRow(int at)
{
    if (at < 0 || at >= E.numrows) {
        return;
    }

    editorFreeRow(&E.row[at]);
    if (at < E.numrows - 1) {
        memmove(&E.row[at], &E.row[at + 1],
                sizeof(EditorRow) * (size_t)(E.numrows - at - 1));
    }
    E.numrows--;
    for (int j = at; j < E.numrows; j++) {
        E.row[j].idx = j;
    }
    E.dirty++;
}

static void editorRowInsertChar(EditorRow *row, int at, int c)
{
    if (at < 0 || at > row->size || row->size == INT_MAX) {
        return;
    }

    char *new_chars = realloc(row->chars, (size_t)row->size + 2);
    if (new_chars == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    row->chars = new_chars;

    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->chars[at] = (char)c;
    row->size++;
    editorUpdateRow(row);
}

static void editorRowDeleteChar(EditorRow *row, int at)
{
    if (at < 0 || at >= row->size) {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(row);
}

static void editorInsertChar(int c)
{
    if (E.numrows == 0) {
        editorInsertRow(0, "", 0);
    }
    if (E.cy < 0 || E.cy >= E.numrows) {
        return;
    }

    EditorRow *row = &E.row[E.cy];
    if (E.cx < 0) {
        E.cx = 0;
    }
    if (E.cx > row->size) {
        E.cx = row->size;
    }

    editorRowInsertChar(row, E.cx, c);
    E.cx++;
    E.dirty++;
    editorUpdateSyntaxAll();
}

static void editorInsertNewline(void)
{
    if (E.numrows == 0) {
        editorInsertRow(0, "", 0);
        E.cy = 0;
        E.cx = 0;
        editorUpdateSyntaxAll();
        return;
    }

    if (E.cy < 0 || E.cy >= E.numrows) {
        return;
    }

    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        EditorRow *row = &E.row[E.cy];
        int old_cx = E.cx;
        size_t tail_len = (size_t)(row->size - old_cx);
        editorInsertRow(E.cy + 1, row->chars + old_cx, tail_len);

        row = &E.row[E.cy];
        row->size = old_cx;
        row->chars[old_cx] = '\0';
        editorUpdateRow(row);
        E.cy++;
        E.cx = 0;
    }

    E.dirty++;
    editorUpdateSyntaxAll();
}

static void editorJoinWithNextRow(void)
{
    if (E.cy < 0 || E.cy >= E.numrows - 1) {
        return;
    }

    EditorRow *row = &E.row[E.cy];
    EditorRow *next = &E.row[E.cy + 1];
    if (next->size > INT_MAX - row->size) {
        editorSetStatusMessage("Cannot join lines: line is too large.");
        return;
    }

    int old_size = row->size;
    int new_size = row->size + next->size;
    char *new_chars = realloc(row->chars, (size_t)new_size + 1);
    if (new_chars == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    row->chars = new_chars;
    memcpy(row->chars + old_size, next->chars, (size_t)next->size + 1);
    row->size = new_size;
    editorUpdateRow(row);
    editorDeleteRow(E.cy + 1);
    E.dirty++;
    editorUpdateSyntaxAll();
}

static void editorBackspace(void)
{
    if (E.numrows == 0 || E.cy < 0 || E.cy >= E.numrows) {
        return;
    }

    if (E.cx > 0) {
        EditorRow *row = &E.row[E.cy];
        editorRowDeleteChar(row, E.cx - 1);
        E.cx--;
        E.dirty++;
        editorUpdateSyntaxAll();
    } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
        editorJoinWithNextRow();
    }
}

static void editorDeleteAtCursor(void)
{
    if (E.numrows == 0 || E.cy < 0 || E.cy >= E.numrows) {
        return;
    }

    EditorRow *row = &E.row[E.cy];
    if (E.cx < row->size) {
        editorRowDeleteChar(row, E.cx);
        E.dirty++;
        editorUpdateSyntaxAll();
    } else if (E.cy < E.numrows - 1) {
        editorJoinWithNextRow();
    }
}

static void editorMoveCursor(int key)
{
    if (E.numrows == 0) {
        E.cx = 0;
        E.cy = 0;
        return;
    }

    if (E.cy >= E.numrows) {
        E.cy = E.numrows - 1;
    }
    if (E.cy < 0) {
        E.cy = 0;
    }

    EditorRow *row = &E.row[E.cy];
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx < row->size) {
                E.cx++;
            } else if (E.cy < E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows - 1) {
                E.cy++;
            }
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = row->size;
            break;
        case PAGE_UP:
            E.cy = E.rowoff;
            for (int i = 0; i < E.screenrows; i++) {
                if (E.cy > 0) {
                    E.cy--;
                }
            }
            break;
        case PAGE_DOWN:
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy >= E.numrows) {
                E.cy = E.numrows - 1;
            }
            for (int i = 0; i < E.screenrows; i++) {
                if (E.cy < E.numrows - 1) {
                    E.cy++;
                }
            }
            break;
        default:
            break;
    }

    if (E.cy < 0) {
        E.cy = 0;
    }
    if (E.cy >= E.numrows) {
        E.cy = E.numrows - 1;
    }
    if (E.cx > E.row[E.cy].size) {
        E.cx = E.row[E.cy].size;
    }
    if (E.cx < 0) {
        E.cx = 0;
    }
}

static int editorReadKey(void)
{
    char c;
    for (;;) {
        ssize_t nread = read(STDIN_FILENO, &c, 1);
        if (nread == 1) {
            break;
        }
        if (nread == -1 && errno == EINTR) {
            continue;
        }
        if (nread == -1) {
            return KEY_NULL;
        }
    }

    if (c != '\x1b') {
        return (unsigned char)c;
    }

    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &raw) == -1) {
        return ESC;
    }
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        return ESC;
    }

    char seq[3] = { 0, 0, 0 };
    int key = ESC;
    ssize_t nread = read(STDIN_FILENO, &seq[0], 1);
    if (nread == 1) {
        if (seq[0] == '[') {
            nread = read(STDIN_FILENO, &seq[1], 1);
            if (nread == 1) {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    nread = read(STDIN_FILENO, &seq[2], 1);
                    if (nread == 1 && seq[2] == '~') {
                        switch (seq[1]) {
                            case '1': key = HOME_KEY; break;
                            case '3': key = DELETE_KEY; break;
                            case '4': key = END_KEY; break;
                            case '5': key = PAGE_UP; break;
                            case '6': key = PAGE_DOWN; break;
                            case '7': key = HOME_KEY; break;
                            case '8': key = END_KEY; break;
                            default: key = ESC; break;
                        }
                    }
                } else {
                    switch (seq[1]) {
                        case 'A': key = ARROW_UP; break;
                        case 'B': key = ARROW_DOWN; break;
                        case 'C': key = ARROW_RIGHT; break;
                        case 'D': key = ARROW_LEFT; break;
                        case 'H': key = HOME_KEY; break;
                        case 'F': key = END_KEY; break;
                        default: key = ESC; break;
                    }
                }
            }
        } else if (seq[0] == 'O') {
            nread = read(STDIN_FILENO, &seq[1], 1);
            if (nread == 1) {
                if (seq[1] == 'H') key = HOME_KEY;
                if (seq[1] == 'F') key = END_KEY;
            }
        }
    }

    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    return key;
}

static char *editorPrompt(const char *prompt)
{
    size_t capacity = 128;
    size_t len = 0;
    char *buf = malloc(capacity);
    if (buf == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    buf[0] = '\0';

    for (;;) {
        editorSetStatusMessage("%s%s", prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();

        if (c == ENTER || c == '\n') {
            if (len > 0) {
                E.statusmsg[0] = '\0';
                return buf;
            }
        } else if (c == ESC || c == CTRL_C) {
            E.statusmsg[0] = '\0';
            free(buf);
            return NULL;
        } else if (c == BACKSPACE || c == CTRL_H) {
            if (len > 0) {
                buf[--len] = '\0';
            }
        } else if (c >= 32 && c <= 255) {
            if (len + 1 >= capacity) {
                if (capacity > SIZE_MAX / 2) {
                    free(buf);
                    editorSetStatusMessage("Filename is too long.");
                    return NULL;
                }
                size_t new_capacity = capacity * 2;
                char *new_buf = realloc(buf, new_capacity);
                if (new_buf == NULL) {
                    perror("realloc");
                    free(buf);
                    return NULL;
                }
                buf = new_buf;
                capacity = new_capacity;
            }
            buf[len++] = (char)c;
            buf[len] = '\0';
        }
    }
}

static int editorOpen(const char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);
    if (E.filename == NULL) {
        perror("strdup");
        return -1;
    }
    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        if (errno == ENOENT) {
            E.end_with_newline = 1;
            E.dirty = 0;
            editorUpdateSyntaxAll();
            editorSetStatusMessage("New file: %s", filename);
            return 0;
        }
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t line_len;
    E.end_with_newline = 0;

    while ((line_len = getline(&line, &capacity, fp)) != -1) {
        int had_newline = line_len > 0 && line[line_len - 1] == '\n';
        if (had_newline) {
            line_len--;
        }
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }

        editorInsertRow(E.numrows, line, (size_t)line_len);
        E.end_with_newline = had_newline;
    }

    int read_error = ferror(fp);
    int saved_errno = errno;
    free(line);
    fclose(fp);

    if (read_error) {
        errno = saved_errno;
        return -1;
    }

    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.dirty = 0;
    editorUpdateSyntaxAll();
    editorSetStatusMessage("Opened %s", filename);
    return 0;
}

static void editorSave(void)
{
    if (E.filename == NULL) {
        char *filename = editorPrompt("Save as: ");
        if (filename == NULL) {
            editorSetStatusMessage("Save cancelled.");
            return;
        }
        free(E.filename);
        E.filename = filename;
        editorSelectSyntaxHighlight();
        editorUpdateSyntaxAll();
    }

    FILE *fp = fopen(E.filename, "wb");
    if (fp == NULL) {
        editorSetStatusMessage("Save failed: %s", strerror(errno));
        return;
    }

    size_t bytes_written = 0;
    int failed = 0;
    for (int i = 0; i < E.numrows; i++) {
        EditorRow *row = &E.row[i];
        if (row->size > 0) {
            size_t n = fwrite(row->chars, 1, (size_t)row->size, fp);
            bytes_written += n;
            if (n != (size_t)row->size) {
                failed = 1;
                break;
            }
        }

        if (i < E.numrows - 1 || (i == E.numrows - 1 && E.end_with_newline)) {
            if (fwrite("\n", 1, 1, fp) != 1) {
                failed = 1;
                break;
            }
            bytes_written++;
        }
    }

    if (fclose(fp) != 0) {
        failed = 1;
    }

    if (failed) {
        editorSetStatusMessage("Save failed: %s", strerror(errno));
        return;
    }

    E.dirty = 0;
    editorSetStatusMessage("Saved %zu bytes to %s", bytes_written, E.filename);
}

static int editorLineNumberWidth(void)
{
    int n = E.numrows > 0 ? E.numrows : 1;
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    return digits < 4 ? 4 : digits;
}

static int editorGutterWidth(void)
{
    return editorLineNumberWidth() + 2;
}

static int editorTextWidth(void)
{
    int width = E.screencols - editorGutterWidth();
    return width > 0 ? width : 1;
}

static const char *highlightColor(unsigned char hl)
{
    switch (hl) {
        case HL_NUMBER: return "\x1b[95m";
        case HL_STRING: return "\x1b[92m";
        case HL_COMMENT: return "\x1b[90m";
        case HL_KEYWORD: return "\x1b[94m";
        case HL_TYPE: return "\x1b[96m";
        case HL_PREPROCESSOR: return "\x1b[93m";
        default: return "\x1b[39m";
    }
}

static void editorScroll(void)
{
    if (E.numrows == 0) {
        E.cx = 0;
        E.cy = 0;
        E.rowoff = 0;
        E.coloff = 0;
        E.rx = 0;
        return;
    }

    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.numrows) E.cy = E.numrows - 1;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;

    E.rx = rowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    int text_width = editorTextWidth();
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + text_width) {
        E.coloff = E.rx - text_width + 1;
    }
    if (E.rowoff < 0) E.rowoff = 0;
    if (E.coloff < 0) E.coloff = 0;
}

static void editorDrawLineNumber(AppendBuffer *ab, int file_row)
{
    int width = editorLineNumberWidth();
    char num[64];
    appendString(ab, "\x1b[90m");
    if (file_row >= 0 && file_row < E.numrows) {
        int n = snprintf(num, sizeof(num), "%*d ", width, file_row + 1);
        if (n > 0) appendBytes(ab, num, (size_t)n);
    } else {
        int count = width + 1;
        for (int i = 0; i < count; i++) appendString(ab, " ");
    }
    appendString(ab, "|");
    appendString(ab, "\x1b[39m");
}

static void editorDrawRow(AppendBuffer *ab, const EditorRow *row)
{
    int text_width = editorTextWidth();
    int start = E.coloff;
    if (start > row->rsize) {
        start = row->rsize;
    }
    int end = row->rsize;
    if (end > E.coloff + text_width) {
        end = E.coloff + text_width;
    }

    unsigned char current_hl = HL_NORMAL;
    appendString(ab, highlightColor(current_hl));
    for (int i = start; i < end; i++) {
        unsigned char hl = row->hl[i];
        if (hl != current_hl) {
            current_hl = hl;
            appendString(ab, highlightColor(current_hl));
        }
        appendBytes(ab, &row->render[i], 1);
    }
    appendString(ab, "\x1b[39m");
}

static void editorDrawRows(AppendBuffer *ab)
{
    for (int y = 0; y < E.screenrows; y++) {
        int file_row = y + E.rowoff;
        editorDrawLineNumber(ab, file_row);

        if (file_row >= 0 && file_row < E.numrows) {
            editorDrawRow(ab, &E.row[file_row]);
        } else {
            appendString(ab, "\x1b[90m~\x1b[39m");
        }

        appendString(ab, "\x1b[K");
        if (y < E.screenrows - 1) {
            appendString(ab, "\r\n");
        }
    }
}

static void editorDrawStatusBar(AppendBuffer *ab)
{
    int width = E.screencols;
    if (width <= 0) {
        return;
    }

    char *line = malloc((size_t)width);
    if (line == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    memset(line, ' ', (size_t)width);

    const char *name = E.filename != NULL ? E.filename : "[No Name]";
    const char *filetype = E.syntax == SYNTAX_C ? "C" : "Text";
    char left[1024];
    char right[128];
    (void)snprintf(left, sizeof(left), " %.700s%s | %d lines | %s",
                   name, E.dirty ? " [+]" : "", E.numrows, filetype);
    (void)snprintf(right, sizeof(right), "Ln %d, Col %d ", E.cy + 1, E.cx + 1);

    size_t left_len = strlen(left);
    if (left_len > (size_t)width) left_len = (size_t)width;
    memcpy(line, left, left_len);

    size_t right_len = strlen(right);
    if (right_len > (size_t)width) right_len = (size_t)width;
    if (right_len > 0) {
        memcpy(line + width - (int)right_len, right + strlen(right) - right_len, right_len);
    }

    char pos[32];
    int pos_len = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 1);
    if (pos_len > 0) appendBytes(ab, pos, (size_t)pos_len);
    appendString(ab, "\x1b[7m");
    appendBytes(ab, line, (size_t)width);
    appendString(ab, "\x1b[0m");
    free(line);
}

static void editorDrawMessageBar(AppendBuffer *ab)
{
    char pos[32];
    int pos_len = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 2);
    if (pos_len > 0) appendBytes(ab, pos, (size_t)pos_len);
    appendString(ab, "\x1b[K");

    time_t now = time(NULL);
    if (E.statusmsg[0] != '\0' && now - E.statusmsg_time < STATUS_MESSAGE_SECONDS) {
        size_t len = strlen(E.statusmsg);
        if (len > (size_t)E.screencols) len = (size_t)E.screencols;
        appendBytes(ab, E.statusmsg, len);
    }
}

static void editorRefreshScreen(void)
{
    editorUpdateWindowSize();
    editorScroll();

    AppendBuffer ab = ABUF_INIT;
    appendString(&ab, "\x1b[?25l\x1b[H");
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    int gutter = editorGutterWidth();
    int cursor_row = E.cy - E.rowoff + 1;
    int cursor_col = gutter + E.rx - E.coloff + 1;
    if (cursor_row < 1) cursor_row = 1;
    if (cursor_row > E.screenrows) cursor_row = E.screenrows;
    if (cursor_col < 1) cursor_col = 1;
    if (cursor_col > E.screencols) cursor_col = E.screencols;

    char cursor[32];
    int cursor_len = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH\x1b[?25h",
                              cursor_row, cursor_col);
    if (cursor_len > 0) appendBytes(&ab, cursor, (size_t)cursor_len);

    writeAll(STDOUT_FILENO, ab.data, ab.len);
    freeAppendBuffer(&ab);
}

static void editorProcessKey(void)
{
    int key = editorReadKey();
    if (key == KEY_NULL) {
        return;
    }

    if (key != CTRL_Q) {
        E.quit_times = 1;
        E.statusmsg[0] = '\0';
    }

    switch (key) {
        case CTRL_Q:
            if (E.dirty && E.quit_times > 0) {
                editorSetStatusMessage("Unsaved changes! Press Ctrl+Q again to quit without saving.");
                E.quit_times--;
                return;
            }
            editorFree();
            exit(EXIT_SUCCESS);

        case CTRL_S:
            editorSave();
            return;

        case ENTER:
        case '\n':
            editorInsertNewline();
            return;

        case BACKSPACE:
        case CTRL_H:
            editorBackspace();
            return;

        case DELETE_KEY:
            editorDeleteAtCursor();
            return;

        case TAB:
            editorInsertChar('\t');
            return;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case HOME_KEY:
        case END_KEY:
        case PAGE_UP:
        case PAGE_DOWN:
            editorMoveCursor(key);
            return;

        case ESC:
            return;

        default:
            if (key >= 32 && key <= 255 && key != 127) {
                editorInsertChar(key);
            }
            return;
    }
}

static void editorInit(void)
{
    memset(&E, 0, sizeof(E));
    E.screenrows = 22;
    E.screencols = 80;
    E.end_with_newline = 1;
    E.quit_times = 1;
    E.syntax = SYNTAX_NONE;
    editorUpdateWindowSize();
}

int main(int argc, char **argv)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "This editor needs an interactive terminal.\n");
        return EXIT_FAILURE;
    }

    editorInit();

    if (argc > 1 && editorOpen(argv[1]) == -1) {
        fprintf(stderr, "Cannot open '%s': %s\n", argv[1], strerror(errno));
        editorFree();
        return EXIT_FAILURE;
    }

    if (editorEnableRawMode() == -1) {
        fprintf(stderr, "Cannot enable terminal raw mode: %s\n", strerror(errno));
        editorFree();
        return EXIT_FAILURE;
    }

    editorEnterAlternateScreen();
    editorSetStatusMessage("Ctrl+S save | Ctrl+Q quit | Arrows move | Enter newline | Tab indent");

    for (;;) {
        editorRefreshScreen();
        editorProcessKey();
    }
}
