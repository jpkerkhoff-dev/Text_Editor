#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <limits.h>


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define ABUF_INIT {NULL, 0}

#define TAB_STOP 8


/* ============================================================
 * TYPES
 * ============================================================ */

/*
 * Temporary buffer used when building one screen frame.
 */
typedef struct {
    char *b;
    int len;
} abuf;


/*
 * Represents one line of the document.
 */
typedef struct {
    int idx;

    int size;
    int rsz;

    char *chars;
    char *render;
} irow;


/*
 * Persistent state of the editor.
 */
typedef struct {
    /*
     * Cursor position in document coordinates.
     */
    int cx;
    int cy;

    /*
     * Terminal dimensions.
     */
    int screen_rows;
    int screen_cols;

    /*
     * Document.
     */
    int num_rows;
    irow *rows;

    /*
     * File information.
     */
    int not_saved;
    char *filename;
} edit_config;


/*
 * Special key values.
 */
enum key_binds {
    KEY_NULL = 0,

    CTRL_C = 3,
    CTRL_D = 4,
    CTRL_F = 6,
    CTRL_H = 8,

    TAB = 9,

    CTRL_L = 12,
    ENTER = 13,

    CTRL_Q = 17,
    CTRL_S = 19,
    CTRL_U = 21,

    ESC = 27,
    BKSPACE = 127,

    ARR_UP = 1000,
    ARR_DOWN,
    ARR_LEFT,
    ARR_RIGHT
};


/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

edit_config E;

struct termios original_termios;


/* ============================================================
 * FUNCTION PROTOTYPES
 * ============================================================ */

/* Terminal */
void Disable_Rawmode(void);
void Enable_Rawmode(void);
int Wdw_Size(int *rows, int *cols);

/* Append buffer */
void Ab_Append(abuf *ab, const char *s, int len);
void Ab_Free(abuf *ab);

/* Editor initialization / cleanup */
void Init_Editor(void);
void Free_Editor(void);

/* Input */
int Interpret_Key(void);
void Process_Key(void);
void Move_Cursor(int key);

/* Row / document */
void Edt_Update_Row(irow *row);
void Edt_Insert_Row(int at, const char *s, size_t len);
void Edt_Free_Rows(void);
int Row_Cx_To_Rx(irow *row, int cx);

/* Files */
int Edt_Open_File(const char *filename);

/* Rendering */
void Edt_Draw_Rows(abuf *ab);
void Edt_Refresh_Screen(void);


/* ============================================================
 * TERMINAL
 * ============================================================ */

void Disable_Rawmode(void)
{
    /*
     * Make sure the cursor is visible when we leave.
     */
    write(
        STDOUT_FILENO,
        "\x1b[?25h",
        sizeof("\x1b[?25h") - 1
    );

    tcsetattr(
        STDIN_FILENO,
        TCSAFLUSH,
        &original_termios
    );
}


void Enable_Rawmode(void)
{
    if (
        tcgetattr(
            STDIN_FILENO,
            &original_termios
        ) == -1
    ) {
        perror("tcgetattr");
        exit(1);
    }

    atexit(Disable_Rawmode);

    struct termios raw = original_termios;

    raw.c_lflag &= ~(
        ECHO |
        ICANON |
        ISIG |
        IEXTEN
    );

    raw.c_iflag &= ~(
        IXON |
        ICRNL |
        BRKINT |
        INPCK |
        ISTRIP
    );

    raw.c_oflag &= ~(OPOST);

    /*
     * Normally block until at least one byte arrives.
     */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (
        tcsetattr(
            STDIN_FILENO,
            TCSAFLUSH,
            &raw
        ) == -1
    ) {
        perror("tcsetattr");
        exit(1);
    }
}


int Wdw_Size(int *rows, int *cols)
{
    struct winsize ws;

    if (
        ioctl(
            STDOUT_FILENO,
            TIOCGWINSZ,
            &ws
        ) == -1
    ) {
        return -1;
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;

    return 0;
}


/* ============================================================
 * APPEND BUFFER
 * ============================================================ */

void Ab_Append(abuf *ab, const char *s, int len)
{
    if (len <= 0) {
        return;
    }

    char *new_buffer =
        realloc(
            ab->b,
            ab->len + len
        );

    if (new_buffer == NULL) {
        perror("realloc");
        exit(1);
    }

    memcpy(
        new_buffer + ab->len,
        s,
        len
    );

    ab->b = new_buffer;
    ab->len += len;
}


void Ab_Free(abuf *ab)
{
    free(ab->b);

    ab->b = NULL;
    ab->len = 0;
}


/* ============================================================
 * EDITOR INITIALIZATION / CLEANUP
 * ============================================================ */

void Init_Editor(void)
{
    /*
     * Cursor.
     */
    E.cx = 0;
    E.cy = 0;

    /*
     * Document starts empty.
     */
    E.num_rows = 0;
    E.rows = NULL;

    /*
     * No file yet.
     */
    E.filename = NULL;
    E.not_saved = 0;

    if (
        Wdw_Size(
            &E.screen_rows,
            &E.screen_cols
        ) == -1
    ) {
        perror("Wdw_Size");
        exit(1);
    }
}


void Free_Editor(void)
{
    Edt_Free_Rows();

    free(E.filename);

    E.filename = NULL;
}


/* ============================================================
 * ROW / DOCUMENT
 * ============================================================ */

/*
 * Convert a character position inside a row into the actual
 * rendered column.
 *
 * This matters because one TAB character may occupy several
 * columns on screen.
 */
int Row_Cx_To_Rx(irow *row, int cx)
{
    int rx = 0;

    for (int j = 0; j < cx && j < row->size; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_STOP - 1)
                - (rx % TAB_STOP);
        }

        rx++;
    }

    return rx;
}

void Edt_Update_Row(irow *row)
{
    int tabs = 0;

    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    size_t render_size =
        (size_t)row->size
        + (size_t)tabs * (TAB_STOP - 1)
        + 1;

    if (render_size > INT_MAX) {
        fprintf(
            stderr,
            "Row is too large to render\n"
        );

        exit(1);
    }

    char *new_render = malloc(render_size);

    if (new_render == NULL) {
        perror("malloc");
        exit(1);
    }

    free(row->render);

    row->render = new_render;

    int idx = 0;

    for (int j = 0; j < row->size; j++) {

        if (row->chars[j] == '\t') {

            /*
             * Always insert at least one space.
             */
            row->render[idx++] = ' ';

            /*
             * Continue until the next tab stop.
             */
            while (idx % TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }

        } else {

            row->render[idx++] =
                row->chars[j];
        }
    }

    row->render[idx] = '\0';

    row->rsz = idx;
}


void Edt_Insert_Row(
    int at,
    const char *s,
    size_t len
)
{
    if (at < 0 || at > E.num_rows) {
        return;
    }

    if (len > INT_MAX) {
        fprintf(
            stderr,
            "Row is too long\n"
        );

        exit(1);
    }

    /*
     * Increase the row array by one element.
     *
     * Keep the realloc result in a temporary pointer.
     */
    irow *new_rows =
        realloc(
            E.rows,
            sizeof(irow) * (E.num_rows + 1)
        );

    if (new_rows == NULL) {
        perror("realloc");
        exit(1);
    }

    E.rows = new_rows;

    /*
     * If this is not being inserted at the end,
     * move existing rows one place to the right.
     */
    if (at < E.num_rows) {

        memmove(
            &E.rows[at + 1],
            &E.rows[at],
            sizeof(irow) * (E.num_rows - at)
        );
    }

    /*
     * Fix row indexes after moving them.
     */
    for (int j = at + 1; j <= E.num_rows; j++) {
        E.rows[j].idx = j;
    }

    /*
     * Create the new row.
     */
    E.rows[at].idx = at;
    E.rows[at].size = (int)len;

    E.rows[at].chars =
        malloc(len + 1);

    if (E.rows[at].chars == NULL) {
        perror("malloc");
        exit(1);
    }

    memcpy(
        E.rows[at].chars,
        s,
        len
    );

    /*
     * Make chars a valid C string.
     */
    E.rows[at].chars[len] = '\0';

    E.rows[at].render = NULL;
    E.rows[at].rsz = 0;

    /*
     * Generate the screen representation.
     */
    Edt_Update_Row(
        &E.rows[at]
    );

    E.num_rows++;

    E.not_saved++;
}


/*
 * Free the complete document.
 */
void Edt_Free_Rows(void)
{
    for (int i = 0; i < E.num_rows; i++) {

        free(E.rows[i].chars);
        free(E.rows[i].render);

        E.rows[i].chars = NULL;
        E.rows[i].render = NULL;
    }

    free(E.rows);

    E.rows = NULL;
    E.num_rows = 0;
}


/* ============================================================
 * FILES
 * ============================================================ */

int Edt_Open_File(const char *filename)
{
    /*
     * Remove any document that was already open.
     */
    Edt_Free_Rows();

    E.cx = 0;
    E.cy = 0;

    /*
     * Store our own copy of the filename.
     */
    free(E.filename);

    size_t filename_len =
        strlen(filename);

    E.filename =
        malloc(filename_len + 1);

    if (E.filename == NULL) {
        perror("malloc");
        exit(1);
    }

    memcpy(
        E.filename,
        filename,
        filename_len + 1
    );

    FILE *fp =
        fopen(
            filename,
            "r"
        );

    /*
     * If the file does not exist, treat it as
     * a new empty document.
     */
    if (fp == NULL) {

        if (errno == ENOENT) {
            E.not_saved = 0;
            return 1;
        }

        perror("fopen");
        exit(1);
    }

    char *line = NULL;

    size_t line_capacity = 0;

    ssize_t line_length;

    while (
        (
            line_length = getline(
                &line,
                &line_capacity,
                fp
            )
        ) != -1
    ) {

        while (
            line_length > 0 &&
            (
                line[line_length - 1] == '\n' ||
                line[line_length - 1] == '\r'
            )
        ) {
            line_length--;
        }

        Edt_Insert_Row(
            E.num_rows,
            line,
            (size_t)line_length
        );
    }

    free(line);

    fclose(fp);

    /*
     * Edt_Insert_Row() marks rows as modified.
     *
     * Loading a file is not an edit, so reset the
     * not_saved flag after loading finishes.
     */
    E.not_saved = 0;

    return 0;
}


/* ============================================================
 * INPUT
 * ============================================================ */

int Interpret_Key(void)
{
    char c;

    while (
        read(
            STDIN_FILENO,
            &c,
            1
        ) != 1
    ) {
    }

    /*
     * Normal character.
     */
    if (c != '\x1b') {
        return (unsigned char)c;
    }

    /*
     * Possible escape sequence.
     */
    char seq[2];

    struct termios raw;

    if (
        tcgetattr(
            STDIN_FILENO,
            &raw
        ) == -1
    ) {
        return ESC;
    }

    /*
     * Temporarily allow reads to time out.
     *
     * Otherwise pressing ESC by itself would
     * block while waiting for another byte.
     */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (
        tcsetattr(
            STDIN_FILENO,
            TCSANOW,
            &raw
        ) == -1
    ) {
        return ESC;
    }

    if (
        read(
            STDIN_FILENO,
            &seq[0],
            1
        ) != 1
        ||
        read(
            STDIN_FILENO,
            &seq[1],
            1
        ) != 1
    ) {

        /*
         * Restore blocking raw reads.
         */
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        tcsetattr(
            STDIN_FILENO,
            TCSANOW,
            &raw
        );

        return ESC;
    }

    /*
     * Restore normal blocking raw reads.
     */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(
        STDIN_FILENO,
        TCSANOW,
        &raw
    );

    if (
        seq[0] == '[' ||
        seq[0] == 'O'
    ) {

        switch (seq[1]) {

            case 'A':
                return ARR_UP;

            case 'B':
                return ARR_DOWN;

            case 'C':
                return ARR_RIGHT;

            case 'D':
                return ARR_LEFT;
        }
    }

    return ESC;
}


/* ============================================================
 * CURSOR / EDITOR ACTIONS
 * ============================================================ */

void Move_Cursor(int key)
{
    /*
     * Empty document.
     */
    if (E.num_rows == 0) {
        E.cx = 0;
        E.cy = 0;

        return;
    }

    switch (key) {

        case ARR_UP:

            if (E.cy > 0) {
                E.cy--;
            }

            break;


        case ARR_DOWN:

            if (E.cy < E.num_rows - 1) {
                E.cy++;
            }

            break;


        case ARR_LEFT:

            if (E.cx > 0) {
                E.cx--;
            }

            break;


        case ARR_RIGHT:

            if (
                E.cx <
                E.rows[E.cy].size
            ) {
                E.cx++;
            }

            break;
    }

    /*
     * If we moved vertically onto a shorter row,
     * make sure cx is still valid.
     */
    if (
        E.cx >
        E.rows[E.cy].size
    ) {
        E.cx =
            E.rows[E.cy].size;
    }
}


void Process_Key(void)
{
    int key =
        Interpret_Key();

    switch (key) {

        case CTRL_Q:

            /*
             * Clear screen before leaving.
             */
            write(
                STDOUT_FILENO,
                "\x1b[2J",
                sizeof("\x1b[2J") - 1
            );

            write(
                STDOUT_FILENO,
                "\x1b[H",
                sizeof("\x1b[H") - 1
            );

            Free_Editor();

            exit(0);


        case ARR_UP:
        case ARR_DOWN:
        case ARR_LEFT:
        case ARR_RIGHT:

            Move_Cursor(key);

            break;


        default:

            /*
             * Character editing will be added later.
             */
            break;
    }
}


/* ============================================================
 * SCREEN RENDERING
 * ============================================================ */

void Edt_Draw_Rows(abuf *ab)
{
    for (
        int y = 0;
        y < E.screen_rows;
        y++
    ) {

        /*
         * No document row exists here.
         */
        if (y >= E.num_rows) {

            Ab_Append(
                ab,
                "~",
                1
            );

        } else {

            irow *row =
                &E.rows[y];

            /*
             * For now there is no horizontal scrolling.
             *
             * Only draw what fits on the terminal.
             */
            int len =
                row->rsz;

            if (
                len >
                E.screen_cols
            ) {
                len =
                    E.screen_cols;
            }

            Ab_Append(
                ab,
                row->render,
                len
            );
        }

        /*
         * Avoid newline after final screen row.
         */
        if (
            y <
            E.screen_rows - 1
        ) {
            Ab_Append(
                ab,
                "\r\n",
                2
            );
        }
    }
}


void Edt_Refresh_Screen(void)
{
    abuf ab = ABUF_INIT;

    /*
     * Hide cursor while drawing.
     */
    Ab_Append(
        &ab,
        "\x1b[?25l",
        sizeof("\x1b[?25l") - 1
    );

    /*
     * Clear screen.
     */
    Ab_Append(
        &ab,
        "\x1b[2J",
        sizeof("\x1b[2J") - 1
    );

    /*
     * Move drawing position to top-left.
     */
    Ab_Append(
        &ab,
        "\x1b[H",
        sizeof("\x1b[H") - 1
    );

    /*
     * Draw document.
     */
    Edt_Draw_Rows(&ab);

    /*
     * Translate document cx into rendered x.
     *
     * Usually:
     *
     *     rx == cx
     *
     * But tabs make them different.
     */
    int rx = E.cx;

    if (
        E.cy >= 0 &&
        E.cy < E.num_rows
    ) {
        rx =
            Row_Cx_To_Rx(
                &E.rows[E.cy],
                E.cx
            );
    }

    /*
     * Position terminal cursor.
     *
     * Editor coordinates start at 0.
     * ANSI terminal coordinates start at 1.
     */
    char buf[32];

    int len =
        snprintf(
            buf,
            sizeof(buf),
            "\x1b[%d;%dH",
            E.cy + 1,
            rx + 1
        );

    if (
        len > 0 &&
        len < (int)sizeof(buf)
    ) {
        Ab_Append(
            &ab,
            buf,
            len
        );
    }

    /*
     * Show cursor.
     */
    Ab_Append(
        &ab,
        "\x1b[?25h",
        sizeof("\x1b[?25h") - 1
    );

    /*
     * Draw the complete frame at once.
     */
    write(
        STDOUT_FILENO,
        ab.b,
        ab.len
    );

    Ab_Free(&ab);
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char *argv[])
{
    Enable_Rawmode();

    Init_Editor();

    /*
     * Usage:
     *
     *     ./app filename.txt
     */
    if (argc >= 2) {
        Edt_Open_File(
            argv[1]
        );
    }

    while (1) {

        Edt_Refresh_Screen();

        Process_Key();
    }

    return 0;
}
