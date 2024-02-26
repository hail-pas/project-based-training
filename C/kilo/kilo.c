#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "abuf.h"

#define CTRL_KEY(k)                                                            \
    ((k)&0x1f) // ascii 前 32 个字符为控制值，即将前三位设为0的所有 ascii 码
#define KILO_WELCOME_MSG "Phoenio editor -- version %s"
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define debug(...) write_log("DEBUG", NULL, __VA_ARGS__, NULL)
#define info(...) write("INFO", NULL, __VA_ARGS__, NULL)

char *str_to_lower(const char *orig_str) {
    if (orig_str == NULL) {
        return NULL;
    }
    char *lower_str = malloc(strlen(orig_str) + 1);
    if (lower_str == NULL) {
        return NULL;
    }
    for (int i = 0; orig_str[i] != '\0'; i++) {
        lower_str[i] = tolower((unsigned char)orig_str[i]);
    }

    lower_str[strlen(orig_str)] = '\0';
    return lower_str;
}

char *get_time(void) {
    time_t raw_time;
    struct tm *timeinfo;
    char *buf = malloc(80 * sizeof(char));
    time(&raw_time);
    timeinfo = localtime(&raw_time);
    strftime(buf, 80, "%Y-%m-%d %H:%M:%S", timeinfo);
    return buf;
}

void write_log_impl(const char *level, const char *filename, const char *format,
                    va_list args) {
    FILE *fp = fopen(filename, "a"); // 打开文件用于追加
    if (fp == NULL) {
        perror("Error opening file");
        return;
    }

    fprintf(fp, "%s-[%s]:", level, get_time());

    // va_list args;
    // va_start(args, format);
    vfprintf(fp, format, args);
    // va_end(args);
    fprintf(fp, "\n");

    fclose(fp); // 关闭文件
}

void write_log(const char *level, char *filename, const char *format, ...) {
    if (level == NULL) {
        perror("log level missing");
        return;
    }
    if (filename == NULL) {
        char *filename_prefix = str_to_lower(level);
        filename = malloc(sizeof(char) * (strlen(filename_prefix) + 7));
        sprintf(filename, "./%s.log", filename_prefix); // 默认文件名
        free(filename_prefix);
    }
    va_list args;
    va_start(args, format);
    write_log_impl(level, filename, format, args);
    va_end(args);
    free(filename);
}

enum EditorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

enum EditorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH,
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

typedef struct _editorSyntax
{
    char * filetype;
    char **file_match;
    int flags;
} EditorSyntax;

typedef struct _erow {
    int size;
    char *chars;
    int rsize;
    char *render;
    unsigned char * hl;
} Erow;

struct EditorConfig {
    int screen_rows, screen_cols;
    int cursor_x, cursor_y, render_cursor_x;
    int rowoff;
    int coloff;
    int row_num;
    Erow *erow;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    EditorSyntax *syntax;
    struct termios orig_termios;
};

struct EditorConfig E;

char *C_HL_EXTENSIONS[] = {
    ".c",
    ".h",
    ".cpp",
    NULL
};

EditorSyntax HLDB[] = {
    "c",
    C_HL_EXTENSIONS,
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

void editor_set_status_message(const char *fmt, ...);
char * editor_prompt(char *prompt, void (*callbak)(char *, int));

void die(const char *msg) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(msg);
    exit(EXIT_FAILURE);
}

void disable_raw_mode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode); // 注册推出时执行

    struct termios raw_termios = E.orig_termios;
    raw_termios.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw_termios.c_oflag &= ~(OPOST);
    raw_termios.c_cflag |= (CS8);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw_termios.c_cc[VMIN] = 0;
    raw_termios.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios) == -1)
        die("tcsetattr");
}

int editor_read_key(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1) != 1)) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    // char buf[2];
    // buf[1] = '\0';
    // buf[0] = c;
    // debug(buf);

    if (iscntrl(c)) {
        debug("%d", c);
    } else {
        debug("%d ('%c')", c, c);
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    debug("key: %c%c%c", seq[0], seq[1], seq[2]);
                    switch (seq[1]) {
                    case '1':
                        return HOME_KEY;
                    case '7':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '8':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    }
                }
            } else {
                debug("key: %c%c", seq[0], seq[1]);
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            debug("key: %c%c", seq[0], seq[1]);
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }

    return c;
}

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }

    buf[i] = '\0';
    // printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/\\*=~%<>[];", c) != NULL;
}

void editor_update_syntax(Erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL)
    {
        return;
    }


    // int prev_sep = 1;
    int i = 0;
    int in_string = 0;
    // for ( i = 0; i < row->rsize; i++)
    // {
    //     if (isdigit(row->render[i]))
    //     {
    //         row->hl[i] = HL_NUMBER;
    //     }
    // }
    while (i < row->rsize)
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            if (isdigit(c) || (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                // prev_sep = 0;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
      }
        }

        // prev_sep = is_separator(c);
        i++;
    }
}

int editor_syntax_to_color(int hl) {
    switch (hl)
    {
    case HL_COMMENT: return 36;
    case HL_STRING:
        return 35;
    case HL_NUMBER:
        return 31;

    case HL_MATCH:
        return 34;

    default:
        return 37;
    }
}

void editor_select_syntax_highlight(void){
    E.syntax = NULL;
    if (E.filename == NULL)
    {
        return;
    }

    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        EditorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->file_match[i])
        {
            int is_ext = (s->file_match[i][0] == '.');
            if (
                (is_ext && ext && !strcmp(ext, s->file_match[i]))
                ||
                (!is_ext && strstr(E.filename, s->file_match[i]))
            )
            {
                E.syntax = s;
                int filerow;
                for ( filerow = 0; filerow < E.row_num; filerow++)
                {
                    editor_update_syntax(&E.erow[filerow]);
                }

                return;
            }
            i++;
        }

    }


}

int editor_row_cx_to_rx(Erow *row, int cursor_x) {
    int rx = 0;
    int j;

    for (j = 0; j < cursor_x; j++) {
        if (row->chars[j] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
            // debug("char: %c, rx: %d", row->chars[j], rx);
        }
        // debug("rx: %d", rx);
        rx++;
    }
    return rx;
}

int editor_row_rx_to_cx(Erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for ( cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;
        if (cur_rx > rx)
        {
            return cx;
        }

    }

    return cx;

}

void editor_update_row(Erow *row) {
    int tabs = 0;
    int j;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * 7 + 1);
    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }

        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > E.row_num)
    {
        return;
    }

    E.erow = realloc(E.erow, sizeof(Erow) * (E.row_num + 1));
    memmove(&E.erow[at+1], &E.erow[at], sizeof(Erow) * (E.row_num - at));

    // int at = E.row_num;
    E.erow[at].size = len;
    E.erow[at].chars = malloc(len + 1);
    memcpy(E.erow[at].chars, s, len);
    E.erow[at].chars[len] = '\0';

    E.erow[at].rsize = 0;
    E.erow[at].render = NULL;
    E.erow[at].hl = NULL;
    editor_update_row(&E.erow[at]);

    E.row_num++;
    E.dirty++;
}

void editor_free_row(Erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_del_row(int at){
    if (at < 0 || at >= E.row_num)
    {
        return;
    }
    editor_free_row(&E.erow[at]);
    memmove(&E.erow[at], &E.erow[at + 1], sizeof(Erow) * (E.row_num - at -1));
    E.row_num--;
    E.dirty++;

}

void editor_row_insert_char(Erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_appen_string(Erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_insert_char(int c) {
    if (E.cursor_y == E.row_num) {
        editor_insert_row(E.row_num, "", 0);
    }
    editor_row_insert_char(&E.erow[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void editor_insert_new_line(void) {
    if (E.cursor_x == 0)
    {
        editor_insert_row(E.cursor_y, "", 0);
    }else
    {
        Erow *row = &E.erow[E.cursor_y];
        if (row->size - 1 > E.cursor_x)
        {
            editor_insert_row(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
            row = &E.erow[E.cursor_y];
            row->size = E.cursor_x;
            row->chars[row->size] = '\0';
            editor_update_row(row);
        }else {
            E.cursor_x = 0;
            editor_insert_row(E.cursor_y + 1, "", 0);
        }
    }

    E.cursor_y++;
    E.dirty++;

}

void editor_row_del_char(Erow *row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

void editor_del_char(void) {
    if (E.cursor_y == E.row_num) {
        return;
    }
    if (E.cursor_x == 0 && E.cursor_y == 0)
    {
        return;
    }

    Erow *row = &E.erow[E.cursor_y];
    if (E.cursor_x > 0) {
        editor_row_del_char(row, E.cursor_x - 1);
        E.cursor_x--;
    } else
    {
        E.cursor_x = E.erow[E.cursor_y - 1].size;
        editor_row_appen_string(&E.erow[E.cursor_y-1], row->chars, row->size);
        editor_del_row(E.cursor_y);
        E.cursor_y--;
    }
}

char *editor_rows_to_string(int *buflen) {
    int tolen = 0;
    int j;
    for (j = 0; j < E.row_num; j++) {
        tolen += E.erow[j].size + 1;
    }
    *buflen = tolen;
    char *buf = malloc(tolen * sizeof(char));
    char *p = buf;
    for (j = 0; j < E.row_num; j++) {
        memcpy(p, E.erow[j].chars, E.erow[j].size);
        p += E.erow[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    free(E.filename);
    E.filename = strdup(filename);

    editor_select_syntax_highlight();

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 &&
               (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line_len--;
        }
        if (line_len > 0)
            editor_insert_row(E.row_num, line, line_len);
    }
    if (line != NULL) free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save(void) {
    if (E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s", NULL);
        if (E.filename == NULL)
        {
            editor_set_status_message("Save aborted");
            return;
        }
        editor_select_syntax_highlight();

    }

    int len;
    char *buf = editor_rows_to_string(&len);
    if (buf == NULL) {
        editor_set_status_message("Empty content");
        return;
    }
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) != -1) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

void editor_find_callback(char * query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line = -1;
    static char * saved_hl = NULL;
    if (saved_hl && saved_hl_line != -1)
    {
        memcpy(E.erow[saved_hl_line].hl, saved_hl, E.erow[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl_line = -1;
        saved_hl = NULL;
    }


    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1)
    {
        direction = 1;
    }

    int current = last_match;

    int i;
    for ( i = 0; i < E.row_num; i++)
    {
        current += direction;
        if (current == -1)
        {
             current = E.row_num - 1;
        } else if (current == E.row_num)
        {
            current = 0;
        }

        Erow *row = &E.erow[current];
        char *match = strstr(row->render, query);
        if (match)
        {
            last_match = current;
            E.cursor_y = current;
            E.cursor_x = editor_row_cx_to_rx(row, match - row->render);
            E.rowoff = E.row_num;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editor_find(void){
    int saved_cursor_x = E.cursor_x;
    int saved_cursor_y = E.cursor_y;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char * query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);
    if (query)
    {
        free(query);
    } else
    {
        E.cursor_x = saved_cursor_x;
        E.cursor_y = saved_cursor_y;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }

}

void editor_scroll(void) {
    E.render_cursor_x = 0;
    if (E.cursor_y < E.row_num) {
        E.render_cursor_x =
            editor_row_cx_to_rx(&E.erow[E.cursor_y], E.cursor_x);
    }

    if (E.cursor_y < E.rowoff) {
        E.rowoff = E.cursor_y;
    }
    if (E.cursor_y >= E.rowoff + E.screen_rows - 1) {
        E.rowoff = E.cursor_y - E.screen_rows + 2;
    }
    if (E.render_cursor_x < E.coloff) {
        E.coloff = E.render_cursor_x;
    }
    if (E.render_cursor_x >= E.coloff + E.screen_cols) {
        E.coloff = E.render_cursor_x - E.screen_cols + 1;
    }
}

void editor_draw_rows(struct abuf *ab) {
    for (int y = 0; y < E.screen_rows - 1; y++) {
        // write(STDOUT_FILENO, "~\r\n", 3);
        int filerow = y + E.rowoff;
        if (filerow >= E.row_num) {
            if (E.row_num == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          KILO_WELCOME_MSG, KILO_VERSION);
                if (welcomelen > E.screen_cols)
                    welcomelen = E.screen_cols;
                int padding = (E.screen_cols - welcomelen) / 2;
                if (padding) {
                    abuf_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abuf_append(ab, " ", 1);
                abuf_append(ab, welcome, welcomelen);
            } else {
                abuf_append(ab, "~", 1);
            }
        } else {
            int len = E.erow[filerow].rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }

            if (len > E.screen_cols)
                len = E.screen_cols;
            char *c = &E.erow[filerow].render[E.coloff];
            unsigned char *hl = &E.erow[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for ( j = 0; j < len; j++)
            {
                if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abuf_append(ab, "\x1b[39m", 5);
                    }
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != current_color)
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abuf_append(ab, buf, clen);
                    }
                }
                abuf_append(ab, &c[j], 1);
            }
            abuf_append(ab, "\x1b[39m", 5);

            // abuf_append(ab, &(E.erow[filerow].render[E.coloff]), len);
        }
        abuf_append(ab, "\x1b[K", 3);
        // if (y < E.screen_rows - 1) {
        abuf_append(ab, "\r\n", 2);
        // }
    }
}

void editor_draw_status_bar(struct abuf *ab) {
    abuf_append(ab, "\x1b[1;4;7m", 8);
    char status[E.screen_cols], rstatus[80];

    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.row_num,
                       E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype:"no ft", E.cursor_x + 1,
                        E.cursor_y + 1);

    if (len > E.screen_cols)
        len = E.screen_cols;
    abuf_append(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            abuf_append(ab, rstatus, rlen);
            break;
        }
        abuf_append(ab, " ", 1);
        len++;
    }
    abuf_append(ab, "\x1b[m", 3);
    abuf_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
    abuf_append(ab, "\x1b[K", 3);
    int msg_len = strlen(E.statusmsg);
    if (msg_len > E.screen_cols)
        msg_len = E.screen_cols;
    // debug("msg_len: %d, differ: %ld", msg_len, time(NULL) - E.statusmsg_time)
    if (msg_len && time(NULL) - E.statusmsg_time < 5) {
        abuf_append(ab, E.statusmsg, msg_len);
    }
}

void editor_refresh_screen(void) {
    // // <ESC>[2J  清除屏幕     VT100转义序列
    // // https://vt100.net/docs/vt100-ug/chapter3.html#ED
    // write(STDOUT_FILENO, "\x1b[2J", 4);
    // // <esc>[1;1H 默认行号、列号为1, 光标定位
    // write(STDOUT_FILENO, "\x1b[H", 3);

    // editor_draw_rows();
    // write(STDOUT_FILENO, "\x1b[H", 3);
    editor_scroll();

    struct abuf ab = ABUF_INIT;
    abuf_append(&ab, "\x1b[?25l", 6);
    // abuf_append(&ab, "\x1b[2J", 4);
    abuf_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    // abuf_append(&ab, "\x1b[H", 3);
    char buf[32];
    // snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursor_y + 1,
    // E.render_cursor_x + 1);
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoff) + 1,
             (E.render_cursor_x - E.coloff) + 1);
    abuf_append(&ab, buf, strlen(buf));
    abuf_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.s, ab.len);

    abuf_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, args);
    va_end(args);
    E.statusmsg_time = time(NULL);
}

void init_editor(void) {
    E.cursor_x = 0;
    E.cursor_y = 0;
    E.render_cursor_x = 0;
    E.row_num = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.erow = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        die("get_window_Size");
    }
    E.screen_rows -= 1;
}

char * editor_prompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
            {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b')
        {
            editor_set_status_message("");
            if (callback)
            {
                callback(buf, c);
            }

            free(buf);
            return NULL;
        } else if (c == '\r')
        {
            if (buflen != 0)
            {
                editor_set_status_message("");
                if (callback)
                {
                    callback(buf, c);
                }

                return buf;
            }

        } else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize -1)
            {
                bufsize *=2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
        {
            callback(buf, c);
        }
    }

}


void editor_move_cursor(int key) {
    Erow *row = (E.cursor_y >= E.row_num) ? NULL : &E.erow[E.cursor_y];
    switch (key) {
    case ARROW_UP:
        if (E.cursor_y != 0) {
            E.cursor_y--;
        }
        break;

    case ARROW_DOWN:
        if (E.cursor_y < E.row_num) {
            E.cursor_y++;
        }
        break;
    case ARROW_LEFT:
        if (E.cursor_x > 0) {
            E.cursor_x--;
        } else if (E.cursor_y > 0) {
            E.cursor_y--;
            E.cursor_x = E.erow[E.cursor_y].size;
        }

        break;
    case ARROW_RIGHT:
        if (row && E.cursor_x < row->size) {
            E.cursor_x++;
        } else if (row && E.cursor_y < E.row_num && E.cursor_x == row->size) {
            E.cursor_y++;
            E.cursor_x = 0;
        }

        break;
    }
    row = (E.cursor_y >= E.row_num) ? NULL : &E.erow[E.cursor_y];
    int rowlen = row ? row->size : 0;
    if (E.cursor_x > rowlen) {
        E.cursor_x = rowlen;
    }
    debug("E: row_num: %d, row_off: %d, colol_off: %d, screen_rows:%d, "
          "screen_cols: %d, render_cursor_x: %d, cursor_x: %d, cursor_y: %d",
          E.row_num, E.rowoff, E.coloff, E.screen_rows, E.screen_cols,
          E.render_cursor_x, E.cursor_x, E.cursor_y);
}

void editor_process_key_press(void) {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editor_read_key();

    switch (c) {
    case '\r':
        editor_insert_new_line();
        break;
    case CTRL_KEY('q'): {
        if (E.dirty && quit_times > 0) {
            editor_set_status_message("WARNING!!! File has unsaved changes. "
                                      "Press Ctrl-Q %d more times to quit.",
                                      quit_times);
            quit_times--;
            return;
        }

        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(EXIT_SUCCESS);
        break;
    }
    case CTRL_KEY('s'):
        editor_save();
        break;
    case CTRL_KEY('f'):
        editor_find();
        break;
    case ARROW_UP:
        editor_move_cursor(c);
        break;
    case ARROW_DOWN:
        editor_move_cursor(c);
        break;
    case ARROW_LEFT:
        editor_move_cursor(c);
        break;
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY: {
        if (c == DEL_KEY) {
            editor_move_cursor(ARROW_RIGHT);
        }
        editor_del_char();
    } break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cursor_y = E.rowoff;
        } else if (c == PAGE_DOWN) {
            E.cursor_y = E.rowoff + E.screen_rows - 1;
            if (E.cursor_y > E.row_num) {
                E.cursor_y = E.row_num;
            }
        }

        int times = E.screen_rows;
        while (times--) {
            editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    } break;
    case HOME_KEY:
        E.cursor_x = 0;
        break;
    case END_KEY:
        if (E.cursor_y < E.row_num) {
            E.cursor_x = E.erow[E.cursor_y].size;
        }
        break;

    default:
        editor_insert_char(c);
        break;
    }
    quit_times = KILO_QUIT_TIMES;
}

int main(int argc, char const *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editor_refresh_screen();
        editor_process_key_press();
    }

    return 0;
}
