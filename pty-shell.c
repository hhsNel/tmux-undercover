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

#define MAX_PARAMS		16
#define BUFFER_SIZE		1024
#define CSI_BUFFER_SIZE		256
#define DEF_MARGIN_H 4
#define DEF_MARGIN_V 8
#define DEF_CHILD "/bin/sh"
#define CHILD_LEN 256

#define ATTR_BOLD		1
#define ATTR_FAINT		2
#define ATTR_ITALIC		4
#define ATTR_UNDERLINE		8
#define ATTR_BLINK		16
#define ATTR_REVERSE		32
#define ATTR_CONCEAL		64
#define ATTR_STRIKE		128

#define STR(S) #S

#define ANSIESC "\033["
#define ANSIGOTO(X,Y) ANSIESC X ";" Y "H"
#define ANSISCROLL(B,E) ANSIESC B ";" E "r"
#define ANSIMARGIN(B,E) ANSIESC B ";" E "s"
#define ANSIRESETATTR ANSIESC "0m"

typedef struct {
	int fg;
	int bg;
	int attr;
} Attr;

typedef struct {
	char ch;
	Attr attr;
} Cell;

typedef enum {
	NORMAL,
	ESC,
	CSI
} ParserState;

typedef struct {
	int x, y;
	int w, h;
	int vrow, vcol;
	int wrap_pending;		/* Flag for pending line wrap. */
	int saved_vrow, saved_vcol;
	int scroll_top, scroll_bottom;
	Cell **buffer;
	Attr current_attr;
	char child[CHILD_LEN];
} PTYState;

static void move_to_real(int offset_row, int offset_col, int vrow, int vcol);

static void cleanup(void);

static int set_raw_mode(int fd);

static int parse_arguments(int argc, char *argv[], struct winsize *ws, PTYState *state);

static int initialize_pty(PTYState *state, int *master, pid_t *pid, struct winsize *ws);

static void apply_attributes(Attr attr);

static void redraw_line(PTYState *state, int row, int start_col, int end_col);

static void scroll_up_pty(PTYState *state, int n);

static void scroll_down_pty(PTYState *state, int n);

static void reset_cell(Cell *cell);

static void clear_line_to_start(PTYState *state);

static void handle_normal_state(char ch, PTYState *state);

static int handle_csi_sequence(PTYState *state, int *params, int param_count, char final_char);

static int process_input(int master, PTYState *state);

static struct termios orig_termios;

static void
move_to_real(int offset_row, int offset_col, int vrow, int vcol)
{
	printf(ANSIGOTO("%d","%d"), offset_row + vrow + 1, offset_col + vcol + 1);
}

static void
cleanup(void)
{
	struct winsize ws;

	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) >= 0) {
		printf(ANSIESC "?69l" ANSISCROLL("1", "%d") ANSIMARGIN("1", "%d"), ws.ws_row, ws.ws_col);
		fflush(stdout);
	}
}

static int
set_raw_mode(int fd)
{
	struct termios term;

	if(tcgetattr(fd, &term) < 0) {
		perror("tcgetattr " STR(__LINE__));
		return 1;
	}
	cfmakeraw(&term);
	if(tcsetattr(fd, TCSANOW, &term) < 0) {
		perror("tcsetattr" STR(__LINE__));
		return 1;
	}
	return 0;
}

static int
parse_arguments(int argc, char *argv[], struct winsize *ws, PTYState *state)
{
	int opt;
	int x, y, w, h;
	char c[CHILD_LEN];
	
	x = DEF_MARGIN_H;
	y = DEF_MARGIN_V;
	w = ws->ws_col - 2*DEF_MARGIN_H;
	h = ws->ws_row - 2*DEF_MARGIN_V;
	strncpy(c, DEF_CHILD, CHILD_LEN-1);

	while(-1 != (opt = getopt(argc, argv, "x:y:w:h:c:"))) {
		switch(opt) {
		case 'x':
			x = atoi(optarg);
			break;
		case 'y':
			y = atoi(optarg);
			break;
		case 'w':
			w = atoi(optarg);
			break;
		case 'h':
			h = atoi(optarg);
			break;
		case 'c':
			strncpy(c, optarg, CHILD_LEN-1);
		default:
			fprintf(stderr, "Usage: %s [-x xpos] [-y ypos] [-w width] [-h height]\nIf xpos/ypos negative, add the width/height of the terminal.\nIf width/height nonpositive, add the width/height of the terminal.\n", *argv);
			return 1;
		}
	}

	if(x < 0) {
		x += ws->ws_col;
	}
	if(y < 0) {
		y += ws->ws_row;
	}
	if(w <= 0) {
		w += ws->ws_col;
	}
	if(h <= 0) {
		h += ws->ws_row;
	}

	if(x < 0 || y < 0 || w < 0 || h < 0 || x + w >= ws->ws_col || y + h >= ws->ws_row) {
		fprintf(stderr, "Invalid position/size.\n");
		return 2;
	}

	ws->ws_row = h;
	ws->ws_col = w;
	state->x = x;
	state->y = y;
	state->w = w;
	state->h = h;
	state->scroll_top = 0;
	state->scroll_bottom = h - 1;
	state->current_attr.fg = state->current_attr.bg = -1;
	state->current_attr.attr = 0;
	strcpy(state->child, c);
	return 0;
}

static int
initialize_pty(PTYState *state, int *master, pid_t *pid, struct winsize *ws)
{
	*pid = forkpty(master, NULL, NULL, ws);
	if(*pid < 0) {
		perror("forkpty " STR(__LINE__));
		return 1;
	}
	if(*pid == 0) {
		execl(state->child, state->child, NULL);
		perror("execl " STR(__LINE__));
		return 2;
	}

	return 0;
}

static void
apply_attributes(Attr attr)
{
	printf(ANSIRESETATTR);
	if(attr.attr & ATTR_BOLD) printf(ANSIESC "1m");
	if(attr.attr & ATTR_FAINT) printf(ANSIESC "2m");
	if(attr.attr & ATTR_ITALIC) printf(ANSIESC "3m");
	if(attr.attr & ATTR_UNDERLINE) printf(ANSIESC "4m");
	if(attr.attr & ATTR_BLINK) printf(ANSIESC "5m");
	if(attr.attr & ATTR_REVERSE) printf(ANSIESC "7m");
	if(attr.attr & ATTR_CONCEAL) printf(ANSIESC "8m");
	if(attr.attr & ATTR_STRIKE) printf(ANSIESC "9m");
	if(attr.fg != -1) {
		if(attr.fg < 8) {
			printf(ANSIESC "3%dm", attr.fg);
		} else if (attr.fg < 16) {
			printf(ANSIESC "9%dm", attr.fg - 8);
		} else {
			printf(ANSIESC "38;5;%dm", attr.fg);
		}
	}
	if(attr.bg != -1) {
		if(attr.bg < 8) {
			printf(ANSIESC "4%dm", attr.bg);
		} else if (attr.bg < 16) {
			printf(ANSIESC "10%dm", attr.bg - 8);
		} else {
			printf(ANSIESC "48;5;%dm", attr.bg);
		}
	}
}

static void
redraw_line(PTYState *state, int row, int start_col, int end_col)
{
	Attr last_attr;
	unsigned int i;
	Cell *cell;

	if(start_col < 0) {
		start_col = 0;
	}
	if(end_col >= state->w) {
		end_col = state->w - 1;
	}
	if(start_col > end_col) return;

	move_to_real(state->x, state->y, row, start_col);
	last_attr.fg = last_attr.bg = -2;
	last_attr.attr = -1;
	for(i = start_col; i <= end_col; ++i) {
		cell = state->buffer[row] + i;
		
		if(memcmp(&last_attr, &cell->attr, sizeof(Attr))) {
			last_attr = cell->attr;
			apply_attributes(last_attr);
		}
		putchar(cell->ch);
	}
	printf(ANSIRESETATTR);
	move_to_real(state->x, state->y, state->vrow, state->vcol);
	fflush(stdout);
}

static void
scroll_up_pty(PTYState *state, int n)
{
	unsigned int i, j;
	Cell *row;

	for(i = 0; i < n; ++i) {
		row = state->buffer[state->scroll_top];
		for(j = state->scroll_top; j < state->scroll_bottom; ++j) {
			state->buffer[j] = state->buffer[j + 1];
		}
		state->buffer[j] = row;

		for(j = 0; j < state->w; ++j) {
			state->buffer[state->scroll_bottom][j].ch = ' ';
			state->buffer[state->scroll_bottom][j].attr.fg = -1;
			state->buffer[state->scroll_bottom][j].attr.bg = -1;
			state->buffer[state->scroll_bottom][j].attr.attr = 0;
		}
	}

	for(i = state->scroll_top; i <= state->scroll_bottom; ++i) {
		redraw_line(state, i, 0, state->w - 1);
	}
}

static void
scroll_down_pty(PTYState *state, int n)
{
	unsigned int i, j;
	Cell *row;

	for(i = 0; i < n; ++i) {
		row = state->buffer[state->scroll_bottom];
		for(j = state->scroll_bottom; j > state->scroll_top; --j) {
			state->buffer[j] = state->buffer[j - 1];
		}
		state->buffer[j] = row;

		for(j = 0; j < state->w; ++j) {
			state->buffer[state->scroll_bottom][j].ch = ' ';
			state->buffer[state->scroll_bottom][j].attr.fg = -1;
			state->buffer[state->scroll_bottom][j].attr.bg = -1;
			state->buffer[state->scroll_bottom][j].attr.attr = 0;
		}
	}

	for(i = state->scroll_top; i <= state->scroll_bottom; ++i) {
		redraw_line(state, i, 0, state->w - 1);
	}
}

static void
reset_cell(Cell *cell)
{
	cell->ch = ' ';
	cell->attr.fg = cell->attr.bg = -1;
	cell->attr.attr = 0;
}

static void
clear_line_to_start(PTYState *state)
{
	unsigned int i;

	for(i = 0; i <= state->vcol; ++i) {
		reset_cell(state->buffer[state->vrow] + i);
	}
	move_to_real(state->x, state->y, state->vrow, 0);
	apply_attributes(state->current_attr);
	for(i = 0; i <= state->vcol; ++i) {
		putchar(' ');
	}
	state->vcol = 0;
	move_to_real(state->x, state->y, state->vrow, state->vcol);
	fflush(stdout);
}

static void
handle_normal_state(char ch, PTYState *state)
{
	switch(ch) {
	case '\n':
		if(state->vrow < state->scroll_bottom) {
			++state->vrow;
		} else {
			scroll_up_pty(state, 1);
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case '\r':
		state->vcol = 0;
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case '\b':
		if(state->vcol > 0) {
			--state->vcol;
			state->wrap_pending = 0;
			move_to_real(state->x, state->y, state->vrow, state->vcol);
			apply_attributes(state->current_attr);
			putchar(' ');
			state->buffer[state->vrow][state->vcol].ch = ' ';
			state->buffer[state->vrow][state->vcol].attr = state->current_attr;
			move_to_real(state->x, state->y, state->vrow, state->vcol);
			fflush(stdout);
		}
		break;
	case '\025': /* C-U */
		clear_line_to_start(state);
		state->wrap_pending = 0;
		break;
	default:
		if(isprint(ch)) {
			if(state->wrap_pending) {
				if(state->vrow < state->scroll_bottom) {
					++state->vrow;
				} else {
					scroll_up_pty(state, 1);
				}
				state->vcol = 0;
				state->wrap_pending = 0;
			}
			move_to_real(state->x, state->y, state->vrow, state->vcol);
			if(state->vcol == state->w - 1) {
				printf(ANSIESC "?7l");
			}
			apply_attributes(state->current_attr);
			putchar(ch);
			state->buffer[state->vrow][state->vcol].ch = ch;
			state->buffer[state->vrow][state->vcol].attr = state->current_attr;
			if(state->vcol == state->w - 1) {
				printf(ANSIESC "?7h");
				state->wrap_pending = 1;
			} else {
				++state->vcol;
			}
			fflush(stdout);
		}
	}
}

static int
handle_csi_sequence(PTYState *state, int *params, int param_count, char final_char)
{
	int n, m, handled;
	int svrow, svcol, swrap;

#define IF_UNDEF_1(P) (param_count > P && params[P] > 0) ? params[P] : 1;
	n = IF_UNDEF_1(0);
	m = IF_UNDEF_1(1);
#undef IF_UNDEF_1

	handled = 1;
	
	switch(final_char) {
	case 'A': /* cursor up */
		state->vrow -= n;
		if(state->vrow < state->scroll_top) {
			state->vrow = state->scroll_top;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'B': /* cursor down */
		state->vrow += n;
		if(state->vrow > state->scroll_bottom) {
			state->vrow = state->scroll_bottom;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'C': /* cursor right */
		state->vcol += n;
		if(state->vcol > state->w - 1) {
			state->vcol = state->w - 1;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'D': /* cursor left */
		state->vcol -= n;
		if(state->vcol < 0) {
			state->vcol = 0;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'H': /* FALLTHROUGH */
	case 'f': /* jump cursor */
		--n;
		--m;
		state->vrow = n;
		if(state->vrow > state->h - 1) {
			state->vrow = state->h - 1;
		}
		if(state->vrow < 0) {
			state->vrow = 0;
		}
		state->vcol = m;
		if(state->vcol > state->w - 1) {
			state->vcol = state->w - 1;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'J': /* erase */
		svrow = state->vrow;
		svcol = state->vcol;
		swrap = state->wrap_pending;
		switch((param_count > 0) ? params[0] : 0) {
		case 0: /* till end of screen */
			for(n = state->vcol; n < state->w; ++n) {
				reset_cell(state->buffer[state->vrow] + n);
			}
			redraw_line(state, state->vrow, state->vcol, state->w - 1);
			for(n = state->vrow + 1; n < state->h; ++n) {
				for(m = 0; m < state->w; ++m) {
					reset_cell(state->buffer[n] + m);
				}
				redraw_line(state, n, 0, state->w - 1);
			}
			break;
		case 1: /* till beginning of screen */
			for(n = 0; n < state->vrow; ++n) {
				for(m = 0; m < state->w; ++m) {
					reset_cell(state->buffer[n] + m);
				}
				redraw_line(state, n, 0, state->w - 1);
			}
			for(n = 0; n <= state->vcol; ++n) {
				reset_cell(state->buffer[state->vrow] + n);
			}
			redraw_line(state, state->vrow, 0, state->vcol);
			break;
		case 2: /* FALLTHROUGH, not supported */
		case 3: /* entire screen */
			for(n = 0; n < state->h; ++n) {
				for(m = 0; m < state->w; ++m) {
					reset_cell(state->buffer[n] + m);
				}
				redraw_line(state, n, 0, state->w - 1);
			}
			state->current_attr.fg = state->current_attr.bg = -1;
			state->current_attr.attr = 0;
			break;
		}
		state->vrow = svrow;
		state->vcol = svcol;
		state->wrap_pending = swrap;
        move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'K': /* erase line */
		svcol = state->vcol;
		swrap = state->wrap_pending;
		switch((param_count > 0) ? params[0] : 0) {
		case 0: /* till end of line */
			for(n = state->vcol; n < state->w; ++n) {
				reset_cell(state->buffer[state->vrow] + n);
			}
			move_to_real(state->x, state->y, state->vrow, state->vcol);
			apply_attributes(state->current_attr);
			for(n = state->vcol; n < state->w; ++n) {
				putchar(' ');
			}
			move_to_real(state->x, state->y, state->vrow, svcol);
			break;
		case 1: /* till start of line */
			for(n = 0; n < state->vcol; ++n) {
				reset_cell(state->buffer[state->vrow] + n);
			}
			move_to_real(state->x, state->y, state->vrow, 0);
			apply_attributes(state->current_attr);
			for(n = 0; n < state->vcol; ++n) {
				putchar(' ');
			}
			move_to_real(state->x, state->y, state->vrow, svcol);
			break;
		case 2: /* entire line */
			for(n = 0; n < state->w; ++n) {
				reset_cell(state->buffer[state->vrow] + n);
			}
			move_to_real(state->x, state->y, state->vrow, 0);
			apply_attributes(state->current_attr);
			for(n = 0; n < state->w; ++n) {
				putchar(' ');
			}
			move_to_real(state->x, state->y, state->vrow, svcol);
			break;
		}
		state->wrap_pending = swrap;
		fflush(stdout);
		break;
	case 'r': /* scrolling region */
		--n;
		m = (param_count > 1 && params[1] > 0) ? params[1] - 1 : state->h - 1;
		if(n >= 0 && m < state->h && n <= m) {
			state->scroll_top = n;
			state->scroll_bottom = m;
			state->vrow = state->scroll_top;
			state->vcol = 0;
			move_to_real(state->x, state->y, state->vrow, state->vcol);
		}
		break;
	case 's': /* save cursor */
		state->saved_vrow = state->vrow;
		state->saved_vcol = state->vcol;
		break;
	case 'u': /* restore cursor */
		state->vrow = state->saved_vrow;
		state->vcol = state->saved_vcol;
		if(state->vrow  < state->scroll_top) {
			state->vrow = state->scroll_top;
		}
		if(state->vrow > state->scroll_bottom) {
			state->scroll_bottom = state->scroll_bottom;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'G': /* cursor absolute column */
		state->vcol = n - 1;
		if(state->vcol >= state->w) {
			state->vcol = state->w - 1;
		}
		state->wrap_pending = 0;
		move_to_real(state->x, state->y, state->vrow, state->vcol);
		break;
	case 'L': /* insert line */
		if(state->vrow >= state->scroll_top && state->vrow <= state->scroll_bottom) {
			scroll_down_pty(state, n);
		}
		break;
	case 'M': /* delete line */
		if(state->vrow >= state->scroll_top && state->vrow <= state->scroll_bottom) {
			scroll_up_pty(state, n);
		}
		break;
	case '@': /* insert char */
		if(state->vcol + n > state->w) {
			n = state->w - state->vcol;
		}
		memmove(state->buffer[state->vrow] + state->vcol + n, state->buffer[state->vrow] + state->vcol, (state->w - state->vcol - n) * sizeof(Cell));
		for(m = 0; m < n; ++m) {
			reset_cell(state->buffer[state->vrow] + state->vcol + m);
		}
		redraw_line(state, state->vrow, state->vcol, state->w - 1);
		break;
	case 'P': /* delete char */
		if(state->vcol + n > state->w) {
			n = state->w - state->vcol;
		}
		memmove(state->buffer[state->vrow] + state->vcol, state->buffer[state->vrow] + state->vcol + n, (state->w - state->vcol - n) * sizeof(Cell));
		for(m = state->w - n; m < state->w; ++m) {
			reset_cell(state->buffer[state->vrow] + m);
		}
		redraw_line(state, state->vrow, state->vcol, state->w - 1);
		break;
	case 'X': /* erase char */
		if(state->vcol + n > state->w) {
			n = state->w - state->vcol;
		}
		for(m = 0; m < n; ++m) {
			reset_cell(state->buffer[state->vrow] + state->vcol + m);
		}
		redraw_line(state, state->vrow, state->vcol, state->vcol + n - 1);
		break;
	case 'm': /* graphics */
		n = 0;
		while(n < param_count) {
			m = params[n];
			switch(m) {
			case 0:
				state->current_attr.fg = state->current_attr.bg = -1;
				state->current_attr.attr = 0;
				break;
			case 1:
				state->current_attr.attr |= ATTR_BOLD;
				break;
			case 2:
				state->current_attr.attr |= ATTR_FAINT;
				break;
			case 3:
				state->current_attr.attr |= ATTR_ITALIC;
				break;
			case 4:
				state->current_attr.attr |= ATTR_UNDERLINE;
				break;
			case 5:
				state->current_attr.attr |= ATTR_BLINK;
				break;
			case 7:
				state->current_attr.attr |= ATTR_REVERSE;
				break;
			case 8:
				state->current_attr.attr |= ATTR_CONCEAL;
				break;
			case 9:
				state->current_attr.attr |= ATTR_STRIKE;
				break;
			case 22:
				state->current_attr.attr &= ~(ATTR_BOLD | ATTR_FAINT);
				break;
			case 23:
				state->current_attr.attr &= ~ATTR_ITALIC;
				break;
			case 24:
				state->current_attr.attr &= ~ATTR_UNDERLINE;
				break;
			case 25:
				state->current_attr.attr &= ~ATTR_BLINK;
				break;
			case 27:
				state->current_attr.attr &= ~ATTR_REVERSE;
				break;
			case 28:
				state->current_attr.attr &= ~ATTR_CONCEAL;
				break;
			case 29:
				state->current_attr.attr &= ~ATTR_STRIKE;
				break;
			case 30: /* FALLTHROUGH */
			case 31: /* FALLTHROUGH */
			case 32: /* FALLTHROUGH */
			case 33: /* FALLTHROUGH */
			case 34: /* FALLTHROUGH */
			case 35: /* FALLTHROUGH */
			case 36: /* FALLTHROUGH */
			case 37:
				state->current_attr.fg = m - 30;
				break;
			case 38:
				if(n + 2 < param_count && params[n+1] == 5) {
					state->current_attr.fg = params[n+2];
					n += 2;
				}
				break;
			case 39:
				state->current_attr.fg = -1;
				break;
			case 40: /* FALLTHROUGH */
			case 41: /* FALLTHROUGH */
			case 42: /* FALLTHROUGH */
			case 43: /* FALLTHROUGH */
			case 44: /* FALLTHROUGH */
			case 45: /* FALLTHROUGH */
			case 46: /* FALLTHROUGH */
			case 47:
				state->current_attr.bg = m - 40;
				break;
			case 48:
				if(n + 2 < param_count && params[n+1] == 5) {
					state->current_attr.bg = params[n+2];
					n += 2;
				}
				break;
			case 49:
				state->current_attr.bg = -1;
				break;
			case 90: /* FALLTHROUGH */
			case 91: /* FALLTHROUGH */
			case 92: /* FALLTHROUGH */
			case 93: /* FALLTHROUGH */
			case 94: /* FALLTHROUGH */
			case 95: /* FALLTHROUGH */
			case 96: /* FALLTHROUGH */
			case 97:
				state->current_attr.fg = m - 82;
				break;
			case 100: /* FALLTHROUGH */
			case 101: /* FALLTHROUGH */
			case 102: /* FALLTHROUGH */
			case 103: /* FALLTHROUGH */
			case 104: /* FALLTHROUGH */
			case 105: /* FALLTHROUGH */
			case 106: /* FALLTHROUGH */
			case 107:
				state->current_attr.bg = m - 92;
				break;
			}
			++n;
		}
		break;
	default:
		handled = 0;
	}

	return handled;
}

static int
process_input(int master, PTYState *state)
{
	fd_set fd_in;
	char buff[BUFFER_SIZE];
	ParserState parser_state = NORMAL;
	char csi_buff[CSI_BUFFER_SIZE];
	int csi_index;
	int params[MAX_PARAMS];
	int param_count;
	int param_val;
	char private_param;
	char intermediate;
	char final_char;
	int r;
	unsigned int i, j;
	char ch;
	int handled;
	int p;

	csi_index = 0;
	param_count = 0;
	param_val = 0;
	private_param = 0;
	intermediate = 0;
	final_char = 0;

	while(1) {
		FD_ZERO(&fd_in);
		FD_SET(STDIN_FILENO, &fd_in);
		FD_SET(master, &fd_in);
		if(select(master + 1, &fd_in, NULL, NULL, NULL) < 0) {
			perror("select " STR(__LINE__));
			return 1;
		}

		if(FD_ISSET(STDIN_FILENO, &fd_in)) {
			r = read(STDIN_FILENO, buff, sizeof(buff));
			if(r > 0) {
				write(master, buff, r);
			}
		}

		if(FD_ISSET(master, &fd_in)) {
			r = read(master, buff, sizeof(buff));
			if(r <= 0) break;

			for(i = 0; i < r; ++i) {
				ch = buff[i];
				switch(parser_state) {
				case NORMAL:
					if(ch == 27) {
						parser_state = ESC;
					} else {
						handle_normal_state(ch, state);
					}
					break;
				case ESC:
					switch(ch) {
					case '[':
						parser_state = CSI;
						csi_index = 0;
						memset(csi_buff, 0, sizeof(csi_buff));
						param_count = 0;
						param_val = 0;
						private_param = 0;
						intermediate = 0;
						final_char = 0;
						break;
					case '7':
						state->saved_vrow = state->vrow;
						state->saved_vcol = state->vcol;
						break;
					case '8':
						state->vrow = state->saved_vrow;
						state->vcol = state->saved_vcol;
						if(state->vrow < state->scroll_top) {
							state->vrow = state->scroll_top;
						}
						if(state->vrow > state->scroll_bottom) {
							state->vrow = state->scroll_bottom;
						}

						move_to_real(state->x, state->y, state->vrow, state->vcol);

						break;
					case 'D':
						if(state->vrow < state->scroll_bottom) {
							++state->vrow;
						} else {
							scroll_up_pty(state, 1);
						}
						
						move_to_real(state->x, state->y, state->vrow, state->vcol);

						break;
					case 'M':
						if(state->vrow > state->scroll_top) {
							--state->vrow;
						} else {
							scroll_down_pty(state, 1);
						}
						
						move_to_real(state->x, state->y, state->vrow, state->vcol);

						break;
					default:
						printf("\033%c", ch);
						fflush(stdout);
					}

					if(ch != '[') {
						parser_state = NORMAL;
					}

					break;
				case CSI:
					csi_buff[csi_index++] = ch;
					if(ch >= '0' && ch <= '9') {
						param_val = param_val * 10 + ch - '0';
					} else if(ch >= 0x30 && ch <= 0x3f) {
						/* ignore */
					} else if(ch >= 0x20 && ch <= 0x2f) {
						intermediate = ch;
					} else if(ch >= 0x40 && ch <= 0x7e) {
						final_char = ch;
						if(param_val || param_count == 0) {
							params[param_count++] = param_val;
						}
						parser_state = NORMAL;

						handled = 0;
						if(private_param == 0 && intermediate == 0) {
							handled = handle_csi_sequence(state, params, param_count, final_char);
						}

						if(private_param) {
							p = (param_count > 0) ? params[0] : 0;
							if(p == 47   || p == 1047 || p == 1048 || p == 1049 ||
							   p == 1000 || p == 1001 || p == 1002 || p == 1003 ||
							   p == 1004 || p == 1005 || p == 1006 || p == 1015 ||
							   p == 1016 || p == 2004) {
								/* ignore */
							} else {
								printf(ANSIESC "?");
								if(param_count > 0) {
									printf("%d", params[0]);
									for(j = 1; j < param_count; ++j) {
										printf(";%d", params[j]);
									}
								}
								putchar(final_char);
								fflush(stdout);
							}
						} else if(!handled) {
							printf(ANSIESC);
							if(param_count > 0) {
								printf("%d", params[0]);
								for(j = 1; j < param_count; ++j) {
									printf(";%d", params[j]);
								}
							}
							putchar(final_char);
							fflush(stdout);
						}
					} else {
						switch(ch) {
						case ';':
							if(param_count < MAX_PARAMS) {
								params[param_count++] = param_val;
							}
							param_val = 0;
							break;
						case '?':
							private_param = '?';
							csi_buff[--csi_index] = 0;
							break;
						}
					}
				}
			}
		}
	}
}

int
main(int argc, char *argv[])
{
	struct winsize ws;
	PTYState state;
	unsigned int i, j;
	int master;
	pid_t pid;

	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
		perror("ioctl " STR(__LINE__));
		exit(1);
	}

	state.vrow = state.vcol = state.saved_vrow = state.saved_vcol = 0;
	state.wrap_pending = 0;
	if(parse_arguments(argc, argv, &ws, &state)) {
		exit(1);
	}

	state.buffer = (Cell **)malloc(state.h * sizeof(Cell *));
	for(i = 0; i < state.h; ++i) {
		state.buffer[i] = (Cell *)malloc(state.w * sizeof(Cell));
		for(j = 0; j < state.w; ++j) {
			reset_cell(state.buffer[i] + j);
		}
	}

	if(initialize_pty(&state, &master, &pid, &ws)) {
		exit(1);
	}

	if(tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
		perror("tcgetattr " STR(__LINE__));
		exit(1);
	}

	atexit(cleanup);
	if(set_raw_mode(STDIN_FILENO)) {
		exit(1);
	}

	printf(ANSIESC "?69h" ANSIESC "%d;%ds" ANSIESC "%d;%dr", state.x + 1, state.x + state.w, state.y + 1, state.y + state.h);
	for(i = 0; i < state.h; ++i) {
		redraw_line(&state, i, 0, state.w - 1);
	}
	move_to_real(state.x, state.y, state.vrow, state.vcol);
	fflush(stdout);

	process_input(master, &state);

	for(i = 0; i < state.h; ++i) {
		free(state.buffer[i]);
	}
	free(state.buffer);

	return 0;
}
