#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <pty.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <getopt.h>

#define MAX_PARAMS 16
#define BUFFER_SIZE 1024
#define CSI_BUFFER_SIZE 256

#define ATTR_BOLD 1
#define ATTR_FAINT 2
#define ATTR_ITALIC 4
#define ATTR_UNDERLINE 8
#define ATTR_BLINK 16
#define ATTR_REVERSE 32
#define ATTR_CONCEAL 64
#define ATTR_STRIKE 128

typedef struct {
    char ch;
    int fg;
    int bg;
    int attr;
} Cell;

typedef enum { NORMAL, ESC, CSI } ParserState;

typedef struct {
    int xpos, ypos;
    int width, height;
    int offset_row, offset_col;
    int max_vrow, max_vcol;
    int vrow, vcol;
    int wrap_pending;
    int saved_vrow, saved_vcol;
    int scroll_top, scroll_bottom;
    Cell **buffer;
    struct {
        int fg;
        int bg;
        int attr;
    } current_attr;
} PTYState;

struct termios orig_termios;

void move_to_real(int offset_row, int offset_col, int vrow, int vcol) {
    printf("\033[%d;%dH", offset_row + vrow, offset_col + vcol);
}

void cleanup(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) >= 0) {
        printf("\033[?69l\033[1;%dr\033[1;%ds", ws.ws_row, ws.ws_col);
        fflush(stdout);
    }
}

void set_raw_mode(int fd) {
    struct termios term;
    if (tcgetattr(fd, &term) < 0) {
        perror("tcgetattr");
        exit(1);
    }
    cfmakeraw(&term);
    if (tcsetattr(fd, TCSANOW, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
}

void parse_arguments(int argc, char *argv[], struct winsize *ws, PTYState *state) {
    int opt;
    state->xpos = 8 + 1;
    state->ypos = 8 + 1;
    state->width = ws->ws_col - 16;
    state->height = ws->ws_row - 16;

    while ((opt = getopt(argc, argv, "x:y:w:h:")) != -1) {
        switch (opt) {
            case 'x': state->xpos = atoi(optarg); break;
            case 'y': state->ypos = atoi(optarg); break;
            case 'w': state->width = atoi(optarg); break;
            case 'h': state->height = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage: %s [-x xpos] [-y ypos] [-w width] [-h height]\n", argv[0]);
                exit(1);
        }
    }

    if (state->xpos < 0) state->xpos += ws->ws_col + 1;
    if (state->ypos < 0) state->ypos += ws->ws_row + 1;
    if (state->width <= 0) state->width += ws->ws_col;
    if (state->height <= 0) state->height += ws->ws_row;

    if (state->xpos < 1 || state->ypos < 1 || state->width < 1 || state->height < 1 ||
        state->xpos + state->width - 1 > ws->ws_col || state->ypos + state->height - 1 > ws->ws_row) {
        fprintf(stderr, "Invalid position or size\n");
        exit(1);
    }

    ws->ws_row = state->height;
    ws->ws_col = state->width;
    state->offset_row = state->ypos;
    state->offset_col = state->xpos;
    state->max_vrow = state->height - 1;
    state->max_vcol = state->width - 1;
    state->scroll_top = 0;
    state->scroll_bottom = state->max_vrow;
    state->current_attr.fg = -1;
    state->current_attr.bg = -1;
    state->current_attr.attr = 0;
}

void initialize_pty(int *master, pid_t *pid, struct winsize *ws) {
    *pid = forkpty(master, NULL, NULL, ws);
    if (*pid == 0) {
        execl("/bin/sh", "sh", NULL);
        perror("execl");
        exit(1);
    } else if (*pid < 0) {
        perror("forkpty");
        exit(1);
    }
}

void apply_attributes(PTYState *state) {
    printf("\033[0m"); // Reset first
    if (state->current_attr.attr & ATTR_BOLD) printf("\033[1m");
    if (state->current_attr.attr & ATTR_FAINT) printf("\033[2m");
    if (state->current_attr.attr & ATTR_ITALIC) printf("\033[3m");
    if (state->current_attr.attr & ATTR_UNDERLINE) printf("\033[4m");
    if (state->current_attr.attr & ATTR_BLINK) printf("\033[5m");
    if (state->current_attr.attr & ATTR_REVERSE) printf("\033[7m");
    if (state->current_attr.attr & ATTR_CONCEAL) printf("\033[8m");
    if (state->current_attr.attr & ATTR_STRIKE) printf("\033[9m");
    if (state->current_attr.fg == -1) {} else if (state->current_attr.fg < 8) printf("\033[%dm", 30 + state->current_attr.fg);
    else if (state->current_attr.fg < 16) printf("\033[%dm", 90 + state->current_attr.fg - 8);
    else printf("\033[38;5;%dm", state->current_attr.fg);
    if (state->current_attr.bg == -1) {} else if (state->current_attr.bg < 8) printf("\033[%dm", 40 + state->current_attr.bg);
    else if (state->current_attr.bg < 16) printf("\033[%dm", 100 + state->current_attr.bg - 8);
    else printf("\033[48;5;%dm", state->current_attr.bg);
}

void redraw_line(PTYState *state, int row, int start_col, int end_col) {
    // Constrain redraw to specified column range
    if (start_col < 0) start_col = 0;
    if (end_col > state->max_vcol) end_col = state->max_vcol;
    if (start_col > end_col) return;

    // Move to start of the range
    move_to_real(state->offset_row, state->offset_col, row, start_col);
    int last_fg = -2, last_bg = -2, last_attr = -1;
    for (int c = start_col; c <= end_col; c++) {
        Cell *cell = &state->buffer[row][c];
        if (cell->fg != last_fg || cell->bg != last_bg || cell->attr != last_attr) {
            printf("\033[0m");
            if (cell->attr & ATTR_BOLD) printf("\033[1m");
            if (cell->attr & ATTR_FAINT) printf("\033[2m");
            if (cell->attr & ATTR_ITALIC) printf("\033[3m");
            if (cell->attr & ATTR_UNDERLINE) printf("\033[4m");
            if (cell->attr & ATTR_BLINK) printf("\033[5m");
            if (cell->attr & ATTR_REVERSE) printf("\033[7m");
            if (cell->attr & ATTR_CONCEAL) printf("\033[8m");
            if (cell->attr & ATTR_STRIKE) printf("\033[9m");
            if (cell->fg == -1) {} else if (cell->fg < 8) printf("\033[%dm", 30 + cell->fg);
            else if (cell->fg < 16) printf("\033[%dm", 90 + cell->fg - 8);
            else printf("\033[38;5;%dm", cell->fg);
            if (cell->bg == -1) {} else if (cell->bg < 8) printf("\033[%dm", 40 + cell->bg);
            else if (cell->bg < 16) printf("\033[%dm", 100 + cell->bg - 8);
            else printf("\033[48;5;%dm", cell->bg);
            last_fg = cell->fg;
            last_bg = cell->bg;
            last_attr = cell->attr;
        }
        putchar(cell->ch);
    }
    printf("\033[0m"); // Reset attributes
    // Move cursor back to original position
    move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    fflush(stdout);
}

void scroll_up_pty(PTYState *state, int n) {
    for (int k = 0; k < n; k++) {
        Cell *temp = state->buffer[state->scroll_top];
        for (int i = state->scroll_top; i < state->scroll_bottom; i++) {
            state->buffer[i] = state->buffer[i + 1];
        }
        state->buffer[state->scroll_bottom] = temp;
        for (int c = 0; c < state->width; c++) {
            state->buffer[state->scroll_bottom][c].ch = ' ';
            state->buffer[state->scroll_bottom][c].fg = -1;
            state->buffer[state->scroll_bottom][c].bg = -1;
            state->buffer[state->scroll_bottom][c].attr = 0;
        }
    }
    for (int i = state->scroll_top; i <= state->scroll_bottom; i++) {
        redraw_line(state, i, 0, state->max_vcol);
    }
}

void scroll_down_pty(PTYState *state, int n) {
    for (int k = 0; k < n; k++) {
        Cell *temp = state->buffer[state->scroll_bottom];
        for (int i = state->scroll_bottom; i > state->scroll_top; i--) {
            state->buffer[i] = state->buffer[i - 1];
        }
        state->buffer[state->scroll_top] = temp;
        for (int c = 0; c < state->width; c++) {
            state->buffer[state->scroll_top][c].ch = ' ';
            state->buffer[state->scroll_top][c].fg = -1;
            state->buffer[state->scroll_top][c].bg = -1;
            state->buffer[state->scroll_top][c].attr = 0;
        }
    }
    for (int i = state->scroll_top; i <= state->scroll_bottom; i++) {
        redraw_line(state, i, 0, state->max_vcol);
    }
}

void reset_cell(Cell *cell) {
    cell->ch = ' ';
    cell->fg = -1;
    cell->bg = -1;
    cell->attr = 0;
}

void clear_line_to_start(PTYState *state) {
    for (int col = 0; col <= state->vcol; col++) {
        reset_cell(&state->buffer[state->vrow][col]);
    }
    move_to_real(state->offset_row, state->offset_col, state->vrow, 0);
    apply_attributes(state);
    for (int col = 0; col <= state->vcol; col++) {
        putchar(' ');
    }
    state->vcol = 0;
    move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    fflush(stdout);
}

void handle_normal_state(char ch, PTYState *state) {
    if (ch == '\n') {
        if (state->vrow < state->scroll_bottom) {
            state->vrow++;
        } else {
            scroll_up_pty(state, 1);
        }
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (ch == '\r') {
        state->vcol = 0;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (ch == '\b') {
        if (state->vcol > 0) {
            state->vcol--;
            state->wrap_pending = 0;
            move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
            apply_attributes(state);
            putchar(' ');
            state->buffer[state->vrow][state->vcol].ch = ' ';
            state->buffer[state->vrow][state->vcol].fg = state->current_attr.fg;
            state->buffer[state->vrow][state->vcol].bg = state->current_attr.bg;
            state->buffer[state->vrow][state->vcol].attr = state->current_attr.attr;
            move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
            fflush(stdout);
        }
    } else if (ch == '\025') { // Ctrl-U
        clear_line_to_start(state);
        state->wrap_pending = 0;
    } else if (isprint(ch)) {
        if (state->wrap_pending) {
            if (state->vrow < state->scroll_bottom) {
                state->vrow++;
            } else {
                scroll_up_pty(state, 1);
            }
            state->vcol = 0;
            state->wrap_pending = 0;
        }
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
        if (state->vcol == state->max_vcol) {
            printf("\033[?7l");
        }
        apply_attributes(state);
        putchar(ch);
        state->buffer[state->vrow][state->vcol].ch = ch;
        state->buffer[state->vrow][state->vcol].fg = state->current_attr.fg;
        state->buffer[state->vrow][state->vcol].bg = state->current_attr.bg;
        state->buffer[state->vrow][state->vcol].attr = state->current_attr.attr;
        if (state->vcol == state->max_vcol) {
            printf("\033[?7h");
            state->wrap_pending = 1;
        } else {
            state->vcol++;
        }
        fflush(stdout);
    }
}

int handle_csi_sequence(PTYState *state, int *params, int param_count, char final_char) {
    int handled = 1;

    if (final_char == 'A') { // Cursor up
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        state->vrow -= n;
        if (state->vrow < state->scroll_top) state->vrow = state->scroll_top;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'B') { // Cursor down
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        state->vrow += n;
        if (state->vrow > state->scroll_bottom) state->vrow = state->scroll_bottom;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'C') { // Cursor right
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        state->vcol += n;
        if (state->vcol > state->max_vcol) state->vcol = state->max_vcol;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'D') { // Cursor left
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        state->vcol -= n;
        if (state->vcol < 0) state->vcol = 0;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'H' || final_char == 'f') { // Cursor position
        int r = (param_count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int c = (param_count > 1 && params[1] > 0) ? params[1] - 1 : 0;
        state->vrow = r;
        if (state->vrow > state->max_vrow) state->vrow = state->max_vrow;
        if (state->vrow < 0) state->vrow = 0;
        state->vcol = c;
        if (state->vcol > state->max_vcol) state->vcol = state->max_vcol;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'J') { // Erase display
        int mode = (param_count > 0) ? params[0] : 0;
        int save_vrow = state->vrow;
        int save_vcol = state->vcol;
        int save_wrap_pending = state->wrap_pending;
        if (mode == 2 || mode == 3) {
            for (int row = 0; row < state->height; row++) {
                for (int col = 0; col < state->width; col++) {
                    reset_cell(&state->buffer[row][col]);
                }
                redraw_line(state, row, 0, state->max_vcol);
            }
            state->current_attr.fg = -1;
            state->current_attr.bg = -1;
            state->current_attr.attr = 0;
        } else if (mode == 0) {
            for (int col = state->vcol; col < state->width; col++) {
                reset_cell(&state->buffer[state->vrow][col]);
            }
            redraw_line(state, state->vrow, state->vcol, state->max_vcol);
            for (int row = state->vrow + 1; row < state->height; row++) {
                for (int col = 0; col < state->width; col++) {
                    reset_cell(&state->buffer[row][col]);
                }
                redraw_line(state, row, 0, state->max_vcol);
            }
        } else if (mode == 1) {
            for (int row = 0; row < state->vrow; row++) {
                for (int col = 0; col < state->width; col++) {
                    reset_cell(&state->buffer[row][col]);
                }
                redraw_line(state, row, 0, state->max_vcol);
            }
            for (int col = 0; col <= state->vcol; col++) {
                reset_cell(&state->buffer[state->vrow][col]);
            }
            redraw_line(state, state->vrow, 0, state->vcol);
        }
        state->vrow = save_vrow;
        state->vcol = save_vcol;
        state->wrap_pending = save_wrap_pending;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'K') { // Erase line
        int mode = (param_count > 0) ? params[0] : 0;
        int save_vcol = state->vcol;
        int save_wrap_pending = state->wrap_pending;
        if (mode == 0) {
            // Erase from cursor to end of line
            for (int col = state->vcol; col < state->width; col++) {
                reset_cell(&state->buffer[state->vrow][col]);
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
            apply_attributes(state);
            for (int col = state->vcol; col <= state->max_vcol; col++) {
                putchar(' ');
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, save_vcol);
        } else if (mode == 1) {
            // Erase from start of line to cursor
            for (int col = 0; col <= state->vcol; col++) {
                reset_cell(&state->buffer[state->vrow][col]);
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, 0);
            apply_attributes(state);
            for (int col = 0; col <= state->vcol; col++) {
                putchar(' ');
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, save_vcol);
        } else if (mode == 2) {
            // Erase entire line
            for (int col = 0; col < state->width; col++) {
                reset_cell(&state->buffer[state->vrow][col]);
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, 0);
            apply_attributes(state);
            for (int col = 0; col <= state->max_vcol; col++) {
                putchar(' ');
            }
            move_to_real(state->offset_row, state->offset_col, state->vrow, save_vcol);
        }
        state->wrap_pending = save_wrap_pending;
        move_to_real(state->offset_row, state->offset_col, state->vrow, save_vcol);
        fflush(stdout);
    } else if (final_char == 'r') { // Set scrolling region
        int top = (param_count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int bottom = (param_count > 1 && params[1] > 0) ? params[1] - 1 : state->max_vrow;
        if (top >= 0 && bottom <= state->max_vrow && top <= bottom) {
            state->scroll_top = top;
            state->scroll_bottom = bottom;
            state->vrow = state->scroll_top;
            state->vcol = 0;
            move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
        }
    } else if (final_char == 's') { // Save cursor position
        state->saved_vrow = state->vrow;
        state->saved_vcol = state->vcol;
    } else if (final_char == 'u') { // Restore cursor position
        state->vrow = state->saved_vrow;
        state->vcol = state->saved_vcol;
        if (state->vrow < state->scroll_top) state->vrow = state->scroll_top;
        if (state->vrow > state->scroll_bottom) state->vrow = state->scroll_bottom;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'G') { // Cursor absolute column
        int col = (param_count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        state->vcol = col;
        if (state->vcol > state->max_vcol) state->vcol = state->max_vcol;
        state->wrap_pending = 0;
        move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
    } else if (final_char == 'L') { // Insert line
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        if (state->vrow >= state->scroll_top && state->vrow <= state->scroll_bottom) {
            scroll_down_pty(state, n);
        }
    } else if (final_char == 'M') { // Delete line
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        if (state->vrow >= state->scroll_top && state->vrow <= state->scroll_bottom) {
            scroll_up_pty(state, n);
        }
    } else if (final_char == '@') { // ICH insert char
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        if (state->vcol + n > state->max_vcol + 1) n = state->max_vcol - state->vcol + 1;
        memmove(&state->buffer[state->vrow][state->vcol + n], &state->buffer[state->vrow][state->vcol], (state->max_vcol - state->vcol - n + 1) * sizeof(Cell));
        for (int i = 0; i < n; i++) {
            reset_cell(&state->buffer[state->vrow][state->vcol + i]);
        }
        redraw_line(state, state->vrow, state->vcol, state->max_vcol);
    } else if (final_char == 'P') { // DCH delete char
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        if (state->vcol + n > state->max_vcol + 1) n = state->max_vcol - state->vcol + 1;
        memmove(&state->buffer[state->vrow][state->vcol], &state->buffer[state->vrow][state->vcol + n], (state->max_vcol - state->vcol - n + 1) * sizeof(Cell));
        for (int i = state->max_vcol - n + 1; i <= state->max_vcol; i++) {
            reset_cell(&state->buffer[state->vrow][i]);
        }
        redraw_line(state, state->vrow, state->vcol, state->max_vcol);
    } else if (final_char == 'X') { // ECH erase char
        int n = (param_count > 0 && params[0] > 0) ? params[0] : 1;
        if (state->vcol + n > state->max_vcol + 1) n = state->max_vcol - state->vcol + 1;
        for (int i = 0; i < n; i++) {
            reset_cell(&state->buffer[state->vrow][state->vcol + i]);
        }
        redraw_line(state, state->vrow, state->vcol, state->vcol + n - 1);
    } else if (final_char == 'm') { // SGR
        int p = 0;
        while (p < param_count) {
            int val = params[p];
            if (val == 0) {
                state->current_attr.fg = -1;
                state->current_attr.bg = -1;
                state->current_attr.attr = 0;
            } else if (val == 1) state->current_attr.attr |= ATTR_BOLD;
            else if (val == 2) state->current_attr.attr |= ATTR_FAINT;
            else if (val == 3) state->current_attr.attr |= ATTR_ITALIC;
            else if (val == 4) state->current_attr.attr |= ATTR_UNDERLINE;
            else if (val == 5) state->current_attr.attr |= ATTR_BLINK;
            else if (val == 7) state->current_attr.attr |= ATTR_REVERSE;
            else if (val == 8) state->current_attr.attr |= ATTR_CONCEAL;
            else if (val == 9) state->current_attr.attr |= ATTR_STRIKE;
            else if (val == 22) state->current_attr.attr &= ~(ATTR_BOLD | ATTR_FAINT);
            else if (val == 23) state->current_attr.attr &= ~ATTR_ITALIC;
            else if (val == 24) state->current_attr.attr &= ~ATTR_UNDERLINE;
            else if (val == 25) state->current_attr.attr &= ~ATTR_BLINK;
            else if (val == 27) state->current_attr.attr &= ~ATTR_REVERSE;
            else if (val == 28) state->current_attr.attr &= ~ATTR_CONCEAL;
            else if (val == 29) state->current_attr.attr &= ~ATTR_STRIKE;
            else if (val >= 30 && val <= 37) state->current_attr.fg = val - 30;
            else if (val == 38) {
                if (p + 2 < param_count && params[p+1] == 5) {
                    state->current_attr.fg = params[p+2];
                    p += 2;
                }
            } else if (val == 39) state->current_attr.fg = -1;
            else if (val >= 40 && val <= 47) state->current_attr.bg = val - 40;
            else if (val == 48) {
                if (p + 2 < param_count && params[p+1] == 5) {
                    state->current_attr.bg = params[p+2];
                    p += 2;
                }
            } else if (val == 49) state->current_attr.bg = -1;
            else if (val >= 90 && val <= 97) state->current_attr.fg = val - 90 + 8;
            else if (val >= 100 && val <= 107) state->current_attr.bg = val - 100 + 8;
            p++;
        }
    } else {
        handled = 0;
    }

    return handled;
}

void process_input(int master, PTYState *state) {
    fd_set fd_in;
    char buf[BUFFER_SIZE];
    ParserState parser_state = NORMAL;
    char csi_buf[CSI_BUFFER_SIZE];
    int csi_idx = 0;
    int params[MAX_PARAMS];
    int param_count = 0;
    int param_val = 0;
    char private_param = 0;
    char intermediate = 0;
    char final_char = 0;

    while (1) {
        FD_ZERO(&fd_in);
        FD_SET(STDIN_FILENO, &fd_in);
        FD_SET(master, &fd_in);
        if (select(master + 1, &fd_in, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &fd_in)) {
            int r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) write(master, buf, r);
        }

        if (FD_ISSET(master, &fd_in)) {
            int r = read(master, buf, sizeof(buf));
            if (r <= 0) break;

            for (int i = 0; i < r; i++) {
                char ch = buf[i];
                switch (parser_state) {
                    case NORMAL:
                        if (ch == 27) {
                            parser_state = ESC;
                        } else {
                            handle_normal_state(ch, state);
                        }
                        break;
                    case ESC:
                        if (ch == '[') {
                            parser_state = CSI;
                            csi_idx = 0;
                            memset(csi_buf, 0, sizeof(csi_buf));
                            param_count = 0;
                            param_val = 0;
                            private_param = 0;
                            intermediate = 0;
                            final_char = 0;
                        } else {
                            switch (ch) {
                                case '7':
                                    state->saved_vrow = state->vrow;
                                    state->saved_vcol = state->vcol;
                                    break;
                                case '8':
                                    state->vrow = state->saved_vrow;
                                    state->vcol = state->saved_vcol;
                                    if (state->vrow < state->scroll_top) state->vrow = state->scroll_top;
                                    if (state->vrow > state->scroll_bottom) state->vrow = state->scroll_bottom;
                                    move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
                                    break;
                                case 'D':
                                    if (state->vrow < state->scroll_bottom) {
                                        state->vrow++;
                                    } else {
                                        scroll_up_pty(state, 1);
                                    }
                                    move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
                                    break;
                                case 'M':
                                    if (state->vrow > state->scroll_top) {
                                        state->vrow--;
                                    } else {
                                        scroll_down_pty(state, 1);
                                    }
                                    move_to_real(state->offset_row, state->offset_col, state->vrow, state->vcol);
                                    break;
                                default:
                                    printf("\033%c", ch);
                                    fflush(stdout);
                                    break;
                            }
                            parser_state = NORMAL;
                        }
                        break;
                    case CSI:
                        csi_buf[csi_idx++] = ch;
                        if (ch >= '0' && ch <= '9') {
                            param_val = param_val * 10 + (ch - '0');
                        } else if (ch == ';') {
                            if (param_count < MAX_PARAMS) {
                                params[param_count++] = param_val ? param_val : 0;
                            }
                            param_val = 0;
                        } else if (ch == '?') {
                            private_param = '?';
                            csi_buf[--csi_idx] = 0;
                        } else if (ch >= 0x30 && ch <= 0x3F) {
                            // Ignore other params
                        } else if (ch >= 0x20 && ch <= 0x2F) {
                            intermediate = ch;
                        } else if (ch >= 0x40 && ch <= 0x7E) {
                            final_char = ch;
                            if (param_val || param_count == 0) {
                                params[param_count++] = param_val ? param_val : 0;
                            }
                            parser_state = NORMAL;

                            int handled = 0;
                            if (private_param == 0 && intermediate == 0) {
                                handled = handle_csi_sequence(state, params, param_count, final_char);
                            }

                            if (private_param) {
                                int p = (param_count > 0) ? params[0] : 0;
                                if (p == 47 || p == 1047 || p == 1048 || p == 1049 ||
                                    p == 1000 || p == 1001 || p == 1002 || p == 1003 ||
                                    p == 1004 || p == 1005 || p == 1006 || p == 1015 ||
                                    p == 1016 || p == 2004) {
                                    // Ignore
                                } else {
                                    printf("\033[?");
                                    if (param_count > 0) {
                                        printf("%d", params[0]);
                                        for (int k = 1; k < param_count; k++) {
                                            printf(";%d", params[k]);
                                        }
                                    }
                                    putchar(final_char);
                                    fflush(stdout);
                                }
                            } else if (!handled) {
                                printf("\033[");
                                if (param_count > 0) {
                                    printf("%d", params[0]);
                                    for (int k = 1; k < param_count; k++) {
                                        printf(";%d", params[k]);
                                    }
                                }
                                if (intermediate) putchar(intermediate);
                                putchar(final_char);
                                fflush(stdout);
                            }
                        }
                        break;
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
        perror("ioctl");
        exit(1);
    }

    PTYState state = { .vrow = 0, .vcol = 0, .wrap_pending = 0, .saved_vrow = 0, .saved_vcol = 0 };
    parse_arguments(argc, argv, &ws, &state);

    state.buffer = malloc(state.height * sizeof(Cell *));
    for (int i = 0; i < state.height; i++) {
        state.buffer[i] = malloc(state.width * sizeof(Cell));
        for (int j = 0; j < state.width; j++) {
            reset_cell(&state.buffer[i][j]);
        }
    }

    int master;
    pid_t pid;
    initialize_pty(&master, &pid, &ws);

    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
        perror("tcgetattr orig");
        exit(1);
    }
    atexit(cleanup);
    set_raw_mode(STDIN_FILENO);

    printf("\033[?69h\033[%d;%ds\033[%d;%dr", state.offset_col, state.offset_col + state.width - 1,
           state.offset_row, state.offset_row + state.height - 1);
    for (int i = 0; i < state.height; i++) {
        redraw_line(&state, i, 0, state.max_vcol);
    }
    move_to_real(state.offset_row, state.offset_col, state.vrow, state.vcol);
    fflush(stdout);

    process_input(master, &state);

    for (int i = 0; i < state.height; i++) {
        free(state.buffer[i]);
    }
    free(state.buffer);

    return 0;
}
