/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <libgen.h>
#include "yyjson.h"

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_EXTENSIONS   8
#define MAX_LSP_ARGS     8
#define MAX_LSP_SERVERS 16

enum editorKey {
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
  MOUSE_PRESS,
  MOUSE_RELEASE,
  MOUSE_SCROLL_UP,
  MOUSE_SCROLL_DOWN,
  MOUSE_DRAG
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

typedef struct erow {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
  unsigned char *diag_hl;
  unsigned char diag_level;
  int hl_open_comment;
} erow;

struct theme {
  char *name;
  int comment;
  int mlcomment;
  int keyword1;
  int keyword2;
  int string;
  int number;
  int match;
  int default_fg;
  int diag_error;
  int diag_warning;
  int diag_info;
  int diag_hint;
};

struct lsp_server {
  char *name;                            /* "c", "cpp", "rust" */
  char *command;                         /* full path to binary */
  char *args[MAX_LSP_ARGS];              /* argv after command */
  int   nargs;
  char *languageId;                      /* sent in didOpen */
  char *extensions[MAX_EXTENSIONS];      /* ".c", ".h", ... */
  int   nextensions;
};

struct lsp_config {
  struct lsp_server servers[MAX_LSP_SERVERS];
  int nservers;
  int active;                            /* index of matched server, or -1 */
};

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  struct termios orig_termios;
  int mouse_x, mouse_y;
  int scroll_lock;
  int sel_start_x, sel_start_y;
  int sel_end_x, sel_end_y;
  int selecting;
  int dragging;
  FILE *lsp_in;
  FILE *lsp_out;
  int   lsp_fd;
  int   lsp_next_id;
  struct theme theme;
  struct lsp_config lsp;
  int tab_stop;
};


struct editorConfig E;

/* Built-in fallback palette (VS Code Dark+).
   Used if config.json or the .tcode file is missing or malformed.
   Values are 24-bit RGB: 0xRRGGBB. */
static const struct theme default_theme = {
  "builtin",
  0x6A9955,  /* comment      */
  0x6A9955,  /* mlcomment    */
  0x569CD6,  /* keyword1     */
  0x4EC9B0,  /* keyword2     */
  0xCE9178,  /* string       */
  0xB5CEA8,  /* number       */
  0x264F78,  /* match        */
  0xD4D4D4,  /* default_fg   */
  0xF44747,  /* diag_error   */
  0xCCA700,  /* diag_warning */
  0x3794FF,  /* diag_info    */
  0x6A9955,  /* diag_hint    */
};

/* Hardcoded default LSP server table.
   Used until config.json parsing is wired in (Commit 3),
   and as a fallback if config.json has no "lsp" section. */
static void setup_default_lsp_config(void) {
  struct lsp_server *c = &E.lsp.servers[0];
  memset(c, 0, sizeof(*c));
  c->name = strdup("c");
  c->command = strdup("/clang64/bin/clangd");
  c->args[0] = strdup("--log=error");
  c->nargs = 1;
  c->languageId = strdup("c");
  c->extensions[0] = strdup(".c");
  c->extensions[1] = strdup(".h");
  c->nextensions = 2;

  struct lsp_server *cpp = &E.lsp.servers[1];
  memset(cpp, 0, sizeof(*cpp));
  cpp->name = strdup("cpp");
  cpp->command = strdup("/clang64/bin/clangd");
  cpp->args[0] = strdup("--log=error");
  cpp->nargs = 1;
  cpp->languageId = strdup("cpp");
  cpp->extensions[0] = strdup(".cpp");
  cpp->extensions[1] = strdup(".cc");
  cpp->extensions[2] = strdup(".cxx");
  cpp->extensions[3] = strdup(".hpp");
  cpp->nextensions = 4;

  E.lsp.nservers = 2;
  E.lsp.active = -1;
}


/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",

  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

struct editorSyntax HLDB[] = {
  {
    "c",
    C_HL_extensions,
    C_HL_keywords,
    "//", "/*", "*/",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorInsertChar(int c);
void editorInsertNewline();
void editorDelChar();
void editorDeleteSelection();
void lsp_did_open();
void lsp_drain();
int  kilo_binary_dir(char *out, size_t outsz);
int  parse_hex_color(const char *s, int *out);
int  load_theme_file(const char *path, struct theme *out);
void load_config(void);
void lsp_handle_diagnostics(const char *json, size_t len);
int  lsp_match_extension(const char *filename);
void setup_default_lsp_config(void);

/*** terminal ***/
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}
void disableRawMode() {
  write(STDOUT_FILENO, "\x1b[?1006l", 8);
  write(STDOUT_FILENO, "\x1b[?1002l", 8);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");

  /* button-event tracking + SGR extended */
  write(STDOUT_FILENO, "\x1b[?1002h", 8);
  write(STDOUT_FILENO, "\x1b[?1006h", 8);
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      /* --- mouse: ESC [ < btn ; x ; y (M|m) --- */
      if (seq[1] == '<') {
        char buf[32];
        int i = 0;
        char ch;
        while (i < (int)sizeof(buf) - 1 &&
               read(STDIN_FILENO, &ch, 1) == 1) {
          if (ch == 'M' || ch == 'm') break;
          buf[i++] = ch;
        }
        buf[i] = '\0';

        int btn = 0, x = 0, y = 0;
        if (sscanf(buf, "%d;%d;%d", &btn, &x, &y) == 3) {
          E.mouse_x = x;
          E.mouse_y = y;
          if (ch == 'm') return MOUSE_RELEASE;
          if ((btn & 64) == 64)
            return (btn & 1) ? MOUSE_SCROLL_DOWN : MOUSE_SCROLL_UP;
          if (btn & 32) return MOUSE_DRAG;   /* motion while button held */
          /* ignore right-click (bit 1) and middle-click (bit 0) */
          if ((btn & 3) != 0) return '\x1b';
          return MOUSE_PRESS;
        }
        return '\x1b';
      }

      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if (E.syntax == NULL) return;

  char **keywords = E.syntax->keywords;

  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    if (scs_len && !in_string && !in_comment) {
      if (!strncmp(&row->render[i], scs, scs_len)) {
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        if (!strncmp(&row->render[i], mce, mce_len)) {
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
        i += mcs_len;
        in_comment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = HL_STRING;
        if (c == '\\' && i + 1 < row->rsize) {
          row->hl[i + 1] = HL_STRING;
          i += 2;
          continue;
        }
        if (c == in_string) in_string = 0;
        i++;
        prev_sep = 1;
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

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen - 1] == '|';
        if (kw2) klen--;

        if (!strncmp(&row->render[i], keywords[j], klen) &&
            is_separator(row->render[i + klen])) {
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }

  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->idx + 1 < E.numrows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;

  /* allocate diag_hl parallel to render/rsize.
     wiped on every row update — stale diagnostics shouldn't linger. */
  free(row->diag_hl);
  row->diag_hl = malloc(row->rsize);
  memset(row->diag_hl, 0, row->rsize);
  row->diag_level = 0;

  editorUpdateSyntax(row);
}


int editorSyntaxToColor(int hl) {
  switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return E.theme.comment;
    case HL_KEYWORD1:  return E.theme.keyword1;
    case HL_KEYWORD2:  return E.theme.keyword2;
    case HL_STRING:    return E.theme.string;
    case HL_NUMBER:    return E.theme.number;
    case HL_MATCH:     return E.theme.match;
    default:           return E.theme.default_fg;
  }
}

void editorSelectSyntaxHighlight() {
  E.syntax = NULL;
  if (E.filename == NULL) return;

  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = &HLDB[j];
    unsigned int i = 0;
    while (s->filematch[i]) {
      char *p = strstr(E.filename, s->filematch[i]);
      if (p != NULL) {
        int patlen = strlen(s->filematch[i]);
        if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
          E.syntax = s;

          int filerow;
          for (filerow = 0; filerow < E.numrows; filerow++) {
            editorUpdateSyntax(&E.row[filerow]);
          }

          return;
        }
      }
      i++;
    }
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    cur_rx++;

    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  E.row[at].idx = at;

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].diag_hl = NULL;
  E.row[at].diag_level = 0;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
  free(row->diag_hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

/* return the matching closer for an opener, or 0 if not an opener */
static char editorPairFor(char open) {
  switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default:  return 0;
  }
}

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }

  /* 1. Skip-over: if cursor is on the matching closer, just move past it. */
  char ch = (char)c;
  if (ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'') {
    if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
      if (E.row[E.cy].chars[E.cx] == ch) {
        E.cx++;
        return;
      }
    }
  }

  /* 2. Auto-pair: opener inserts opener + closer, cursor in between. */
  char closer = editorPairFor(ch);
  if (closer != 0) {
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    editorRowInsertChar(&E.row[E.cy], E.cx + 1, closer);
    E.cx++;
    return;
  }

  /* 3. Default: insert one char, advance. */
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

/* Returns the number of leading spaces/tabs on a row. */
static int editorRowIndent(erow *row) {
  int indent = 0;
  while (indent < row->size &&
         (row->chars[indent] == ' ' || row->chars[indent] == '\t')) {
    indent++;
  }
  return indent;
}


void editorInsertNewline() {
  /* Case A: cursor at column 0 — just insert a blank row. */
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
    E.cy++;
    E.cx = 0;
    return;
  }

  erow *row = &E.row[E.cy];

  /* Feature 4: expand "{}" (or "()", "[]") into three lines. */
  if (E.cx < row->size &&
      ((row->chars[E.cx - 1] == '{' && row->chars[E.cx] == '}') ||
       (row->chars[E.cx - 1] == '(' && row->chars[E.cx] == ')') ||
       (row->chars[E.cx - 1] == '[' && row->chars[E.cx] == ']'))) {

    int indent = editorRowIndent(row);

    /* Save the closer for later. */
    char closer = row->chars[E.cx];

    /* Line 1: everything up to and including the opener. */
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);

    /* Line 2: empty, indented one level deeper. */
    editorInsertRow(E.cy + 1, "", 0);
    for (int i = 0; i < indent + KILO_TAB_STOP; i++) {
      editorRowInsertChar(&E.row[E.cy + 1], i, ' ');
    }

    /* Line 3: just the closer, at the original indent. */
    char closerbuf[2] = { closer, '\0' };
    editorInsertRow(E.cy + 2, closerbuf, 1);
    for (int i = 0; i < indent; i++) {
      editorRowInsertChar(&E.row[E.cy + 2], i, ' ');
    }

    /* Cursor lands on line 2, after its indent. */
    E.cy += 1;
    E.cx = E.row[E.cy].size;
    return;
  }

  /* Feature 1 & 3: split the line, carry indent forward, adjust for braces. */
  int indent = editorRowIndent(row);

  /* Peek at the char just before the cursor. */
  char prev = (E.cx > 0) ? row->chars[E.cx - 1] : '\0';

  /* Peek at the char at the cursor (the one that will start the new line). */
  char next = (E.cx < row->size) ? row->chars[E.cx] : '\0';

  /* If the next line begins with a closer, dedent by one level. */
  if (next == '}' || next == ')' || next == ']') {
    indent -= KILO_TAB_STOP;
    if (indent < 0) indent = 0;
  }

  /* Split. */
  editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
  row = &E.row[E.cy];
  row->size = E.cx;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);

  /* Apply the carried indent to the new line. */
  for (int i = 0; i < indent; i++) {
    editorRowInsertChar(&E.row[E.cy + 1], i, ' ');
  }

  /* If the previous char was an opener, add one extra indent level. */
  if (prev == '{' || prev == '(' || prev == '[') {
    int base = E.row[E.cy + 1].size;
    for (int i = 0; i < KILO_TAB_STOP; i++) {
      editorRowInsertChar(&E.row[E.cy + 1], base + i, ' ');
    }
  }

  E.cy += 1;
  E.cx = editorRowIndent(&E.row[E.cy]);
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

void editorDeleteSelection() {
  if (!E.selecting) return;

  /* normalize */
  int sy = E.sel_start_y, sx = E.sel_start_x;
  int ey = E.sel_end_y,   ex = E.sel_end_x;
  if (sy > ey || (sy == ey && sx > ex)) {
    int t;
    t = sy; sy = ey; ey = t;
    t = sx; sx = ex; ex = t;
  }

  if (sy >= E.numrows) { E.selecting = 0; return; }

  /* Case 1: single-line selection */
  if (sy == ey) {
    erow *row = &E.row[sy];
    int from = sx;
    int to = ex;
    if (from < 0) from = 0;
    if (to > row->size) to = row->size;
    if (from > to) from = to;

    int count = to - from;
    if (count > 0) {
      memmove(&row->chars[from], &row->chars[to], row->size - to + 1);
      row->size -= count;
      editorUpdateRow(row);
      E.dirty++;
    }
    E.cy = sy;
    E.cx = from;
  } else {
    /* Case 2: multi-line selection */
    erow *first = &E.row[sy];
    erow *last  = (ey < E.numrows) ? &E.row[ey] : NULL;

    /* tail of the last line (from ex onward) */
    char *tail = NULL;
    int tail_len = 0;
    if (last) {
      tail = &last->chars[ex];
      tail_len = last->size - ex;
    }

    /* truncate first line at sx */
    first->size = sx;
    first->chars[first->size] = '\0';
    editorUpdateRow(first);

    /* append the tail of the last line onto the first */
    if (tail_len > 0) {
      editorRowAppendString(first, tail, tail_len);
    }

    /* delete all rows sy+1 through ey (inclusive) */
    int to_delete = ey - sy;
    for (int k = 0; k < to_delete; k++) {
      if (sy + 1 < E.numrows) editorDelRow(sy + 1);
    }

    E.cy = sy;
    E.cx = sx;
  }

  E.selecting = 0;
  E.dragging = 0;
  E.sel_start_x = E.sel_start_y = E.sel_end_x = E.sel_end_y = 0;
}

char *editorGetSelection(int *out_len) {
  if (!E.selecting) return NULL;

  /* normalize start/end */
  int start_y = E.sel_start_y, start_x = E.sel_start_x;
  int end_y = E.sel_end_y, end_x = E.sel_end_x;

  if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
    int tmp;
    tmp = start_y; start_y = end_y; end_y = tmp;
    tmp = start_x; start_x = end_x; end_x = tmp;
  }

  int cap = 128;
  char *buf = malloc(cap);
  int len = 0;

  for (int y = start_y; y <= end_y && y < E.numrows; y++) {
    erow *row = &E.row[y];
    int from = (y == start_y) ? start_x : 0;
    int to   = (y == end_y)   ? end_x   : row->size;

    if (from < 0) from = 0;
    if (to > row->size) to = row->size;
    if (from > to) from = to;

    int chunk = to - from;
    if (len + chunk + 2 > cap) {
      cap = (len + chunk + 2) * 2;
      buf = realloc(buf, cap);
    }
    memcpy(buf + len, &row->chars[from], chunk);
    len += chunk;
    if (y < end_y) buf[len++] = '\n';
  }

  buf[len] = '\0';
  *out_len = len;
  return buf;
}

void editorCopyToClipboard() {
  if (!E.selecting) {
    editorSetStatusMessage("Nothing selected");
    return;
  }

  int len = 0;
  char *text = editorGetSelection(&len);
  if (!text || len == 0) {
    free(text);
    editorSetStatusMessage("Nothing selected");
    return;
  }

  FILE *fp = fopen("/dev/clipboard", "w");
  if (!fp) {
    free(text);
    editorSetStatusMessage("Can't open /dev/clipboard for writing");
    return;
  }

  fwrite(text, 1, len, fp);
  fclose(fp);
  free(text);

  E.selecting = 0;
  editorSetStatusMessage("Copied %d bytes to clipboard", len);
}

void editorPasteFromClipboard() {
  FILE *fp = fopen("/dev/clipboard", "r");
  if (!fp) {
    editorSetStatusMessage("Can't open /dev/clipboard for reading");
    return;
  }

  int cap = 4096;
  char *buf = malloc(cap);
  int len = 0;

  /* read in one go (quirk of /dev/clipboard) */
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
    len += n;
    if (len >= cap - 1) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }
  fclose(fp);
  buf[len] = '\0';

  if (len == 0) {
    free(buf);
    editorSetStatusMessage("Clipboard is empty");
    return;
  }

  int inserted = 0;
  for (int i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)buf[i];
    if (ch == '\r') continue;
    if (ch == '\n') {
      editorInsertNewline();
    } else {
      editorInsertChar(ch);
    }
    inserted++;
  }

  free(buf);
  editorSetStatusMessage("Pasted %d bytes", inserted);
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;

  if (E.lsp_in) lsp_did_open();
}


void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** config ***/

/* Locate the directory the running binary lives in.
   Uses /proc/self/exe (supported by MSYS and Linux).
   Returns 0 on success, -1 on failure. */
int kilo_binary_dir(char *out, size_t outsz) {
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n < 0) return -1;
  exe[n] = '\0';

  char *slash = strrchr(exe, '/');
  if (!slash) return -1;
  *slash = '\0';

  snprintf(out, outsz, "%s", exe);
  return 0;
}

/* Parse "#RRGGBB" (or "RRGGBB") into a 24-bit int. Returns 0 on success. */
int parse_hex_color(const char *s, int *out) {
  if (!s) return -1;
  if (*s == '#') s++;
  if (strlen(s) != 6) return -1;

  char *end = NULL;
  long v = strtol(s, &end, 16);
  if (!end || *end != '\0') return -1;
  if (v < 0 || v > 0xFFFFFF) return -1;

  *out = (int)v;
  return 0;
}

/* Read a .tcode file (JSON), fill the theme struct.
   Missing fields keep their previous values (so partial themes work).
   Returns 0 on success, -1 on failure. */
int load_theme_file(const char *path, struct theme *out) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return -1;

  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) { fclose(fp); return -1; }

  char *buf = malloc(sz + 1);
  if (!buf) { fclose(fp); return -1; }
  size_t got = fread(buf, 1, sz, fp);
  buf[got] = '\0';
  fclose(fp);

  yyjson_doc *doc = yyjson_read(buf, got, 0);
  free(buf);
  if (!doc) return -1;

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) { yyjson_doc_free(doc); return -1; }

  /* helper: read a string field, parse as hex, store if valid */
  #define THEME_STR(key, field) do { \
    yyjson_val *_v = yyjson_obj_get(root, key); \
    if (_v && yyjson_is_str(_v)) { \
      int _c; \
      if (parse_hex_color(yyjson_get_str(_v), &_c) == 0) out->field = _c; \
    } \
  } while (0)

  #define THEME_NAME(key, field) do { \
    yyjson_val *_v = yyjson_obj_get(root, key); \
    if (_v && yyjson_is_str(_v)) { \
      if (out->field) free(out->field); \
      out->field = strdup(yyjson_get_str(_v)); \
    } \
  } while (0)
  

  THEME_NAME("name", name);
  THEME_STR("comment", comment);
  THEME_STR("mlcomment", mlcomment);
  THEME_STR("keyword1", keyword1);
  THEME_STR("keyword2", keyword2);
  THEME_STR("string", string);
  THEME_STR("number", number);
  THEME_STR("match", match);
  THEME_STR("default_fg", default_fg);
  THEME_STR("diag_error", diag_error);
  THEME_STR("diag_warning", diag_warning);
  THEME_STR("diag_info", diag_info);
  THEME_STR("diag_hint", diag_hint);

  #undef THEME_STR
  #undef THEME_NAME

  yyjson_doc_free(doc);
  return 0;
}

/* Parse a JSON array of strings into a char** array.
   Returns the number of items parsed (up to max). Strings are strdup'd. */
static int parse_string_array(yyjson_val *arr, char **out, int max) {
  if (!arr || !yyjson_is_arr(arr)) return 0;
  int n = 0;
  size_t idx, m;
  yyjson_val *v;
  yyjson_arr_foreach(arr, idx, m, v) {
    if (n >= max) break;
    if (yyjson_is_str(v)) {
      out[n++] = strdup(yyjson_get_str(v));
    }
  }
  return n;
}

/* Populate E.lsp.servers[] from the "lsp" object in config.json.
   Returns the number of servers loaded, or 0 on failure.
   Each server must have "command" and "extensions"; "args" and
   "languageId" are optional (languageId defaults to the section name). */
static int parse_lsp_section(yyjson_val *lsp) {
  if (!lsp || !yyjson_is_obj(lsp)) return 0;

  int n = 0;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(lsp, &iter);
  yyjson_val *key, *val;
  while ((key = yyjson_obj_iter_next(&iter)) && n < MAX_LSP_SERVERS) {
    val = yyjson_obj_iter_get_val(key);
    if (!yyjson_is_obj(val)) continue;

    struct lsp_server *s = &E.lsp.servers[n];
    memset(s, 0, sizeof(*s));

    s->name = strdup(yyjson_get_str(key));

    yyjson_val *cmd = yyjson_obj_get(val, "command");
    if (!cmd || !yyjson_is_str(cmd)) {
      free(s->name);
      s->name = NULL;
      continue;
    }
    s->command = strdup(yyjson_get_str(cmd));

    yyjson_val *lang = yyjson_obj_get(val, "languageId");
    if (lang && yyjson_is_str(lang)) {
      s->languageId = strdup(yyjson_get_str(lang));
    } else {
      s->languageId = strdup(s->name);
    }

    s->nargs = parse_string_array(yyjson_obj_get(val, "args"),
                                  s->args, MAX_LSP_ARGS);
    s->nextensions = parse_string_array(yyjson_obj_get(val, "extensions"),
                                        s->extensions, MAX_EXTENSIONS);

    if (s->nextensions == 0) {
      free(s->name);
      free(s->command);
      free(s->languageId);
      s->name = NULL;
      s->command = NULL;
      s->languageId = NULL;
      continue;
    }

    n++;
  }
  return n;
}

/* Read config.json: theme, tab_stop, and the lsp server table.
   Falls back gracefully if anything is missing or malformed. */
void load_config(void) {
  char bin_dir[1024];
  if (kilo_binary_dir(bin_dir, sizeof(bin_dir)) != 0) {
    setup_default_lsp_config();
    return;
  }

  char cfg_path[1200];
  snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", bin_dir);

  FILE *fp = fopen(cfg_path, "rb");
  if (!fp) {
    setup_default_lsp_config();
    return;
  }

  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) { fclose(fp); setup_default_lsp_config(); return; }

  char *buf = malloc(sz + 1);
  if (!buf) { fclose(fp); setup_default_lsp_config(); return; }
  size_t got = fread(buf, 1, sz, fp);
  buf[got] = '\0';
  fclose(fp);

  yyjson_doc *doc = yyjson_read(buf, got, 0);
  free(buf);
  if (!doc) { setup_default_lsp_config(); return; }

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    setup_default_lsp_config();
    return;
  }

  /* theme */
  yyjson_val *t = yyjson_obj_get(root, "theme");
  if (t && yyjson_is_str(t)) {
    char theme_name[256];
    snprintf(theme_name, sizeof(theme_name), "%s", yyjson_get_str(t));

    char theme_path[1400];
    snprintf(theme_path, sizeof(theme_path), "%s/themes/%s.tcode",
             bin_dir, theme_name);
    load_theme_file(theme_path, &E.theme);
  }

  /* tab_stop */
  yyjson_val *ts = yyjson_obj_get(root, "tab_stop");
  if (ts && yyjson_is_int(ts)) {
    int v = (int)yyjson_get_int(ts);
    if (v > 0 && v <= 16) E.tab_stop = v;
  }

  /* lsp servers */
  yyjson_val *lsp = yyjson_obj_get(root, "lsp");
  int n = parse_lsp_section(lsp);
  if (n == 0) {
    setup_default_lsp_config();
  } else {
    E.lsp.nservers = n;
    E.lsp.active = -1;
  }

  yyjson_doc_free(doc);
}


/*** find ***/

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;
  static char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;
  int i;
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numrows;

      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editorFind() {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
                             editorFindCallback);

  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** 




 ***/

int editorLineNumberWidth() {
  int digits = 1;
  int n = E.numrows;
  while (n >= 10) { digits++; n /= 10; }
  return digits + 2;
}

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  int gutter = editorLineNumberWidth();
  int textcols = E.screencols - gutter;
  if (textcols < 1) textcols = 1;

  if (E.scroll_lock) {
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + textcols)
      E.coloff = E.rx - textcols + 1;
    return;
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + textcols) {
    E.coloff = E.rx - textcols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      /* empty row: spaces for the gutter, then tilde or welcome */
      int gutter = editorLineNumberWidth();
      for (int i = 0; i < gutter; i++) abAppend(ab, " ", 1);

      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        int avail = E.screencols - gutter;
        if (avail < 0) avail = 0;
        if (welcomelen > avail) welcomelen = avail;
        int padding = (avail - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int gutter = editorLineNumberWidth();

      /* column 0: diagnostic marker */
      unsigned char lvl = E.row[filerow].diag_level;
      if (lvl != 0)
        fprintf(stderr, "[draw] filerow=%d lvl=%d\n", filerow, lvl);
      if (lvl != 0) {
        int mcol;
        switch (lvl) {
          case 1:  mcol = E.theme.diag_error;   break;
          case 2:  mcol = E.theme.diag_warning; break;
          case 3:  mcol = E.theme.diag_info;    break;
          case 4:  mcol = E.theme.diag_hint;    break;
          default: mcol = E.theme.diag_error;   break;
        }
        char mbuf[32];
        int mlen = snprintf(mbuf, sizeof(mbuf),
                            "\x1b[38;2;%d;%d;%dm>\x1b[39m",
                            (mcol >> 16) & 0xFF,
                            (mcol >> 8)  & 0xFF,
                             mcol        & 0xFF);
        abAppend(ab, mbuf, mlen);
      } else {
        abAppend(ab, " ", 1);
      }

      char lineno[16];
      int numlen = snprintf(lineno, sizeof(lineno), "%*d ",
                            gutter - 2, filerow + 1);
      abAppend(ab, lineno, numlen);

      int textcols = E.screencols - gutter;
      if (textcols < 0) textcols = 0;

      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > textcols) len = textcols;

      char *c = &E.row[filerow].render[E.coloff];
      unsigned char *hl = &E.row[filerow].hl[E.coloff];
      unsigned char *diag = &E.row[filerow].diag_hl[E.coloff];
      int current_color = -1;
      int j;

      int sx = E.sel_start_x, sy = E.sel_start_y;
      int ex = E.sel_end_x,   ey = E.sel_end_y;
      if (sy > ey || (sy == ey && sx > ex)) {
        int t;
        t = sy; sy = ey; ey = t;
        t = sx; sx = ex; ex = t;
      }

      for (j = 0; j < len; j++) {
        int filex = E.coloff + j;

        int in_sel = 0;
        if (E.selecting) {
          if (filerow > sy && filerow < ey) in_sel = 1;
          else if (filerow == sy && filerow == ey)
            in_sel = (filex >= sx && filex < ex);
          else if (filerow == sy)
            in_sel = (filex >= sx);
          else if (filerow == ey)
            in_sel = (filex < ex);
        }

        if (in_sel) abAppend(ab, "\x1b[7m", 4);

        /* STEP 2e: undercurl on diagnostic range */
        int in_diag = (diag[j] != 0);
        if (in_diag) {
          int dcol;
          switch (diag[j]) {
            case 1:  dcol = E.theme.diag_error;   break;
            case 2:  dcol = E.theme.diag_warning; break;
            case 3:  dcol = E.theme.diag_info;    break;
            case 4:  dcol = E.theme.diag_hint;    break;
            default: dcol = E.theme.diag_error;   break;
          }
          char dbuf[48];
          int dlen = snprintf(dbuf, sizeof(dbuf),
                              "\x1b[4:3m\x1b[38;2;%d;%d;%dm",
                              (dcol >> 16) & 0xFF,
                              (dcol >> 8)  & 0xFF,
                               dcol        & 0xFF);
          abAppend(ab, dbuf, dlen);
        }

        if (iscntrl(c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3);
          if (current_color != -1) {
            /* STEP 9 spot 1: truecolor re-emit after control-char symbol */
            char buf[32];
            int clen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm",
                                (current_color >> 16) & 0xFF,
                                (current_color >> 8)  & 0xFF,
                                 current_color        & 0xFF);
            abAppend(ab, buf, clen);
          }
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            current_color = color;
            /* STEP 9 spot 2: truecolor emit on color change */
            char buf[32];
            int clen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm",
                                (color >> 16) & 0xFF,
                                (color >> 8)  & 0xFF,
                                 color        & 0xFF);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }

        if (in_sel) {
          abAppend(ab, "\x1b[27m", 5);
          if (current_color != -1) {
            /* STEP 9 spot 3: truecolor restore after selection ends */
            char buf[32];
            int clen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm",
                                (current_color >> 16) & 0xFF,
                                (current_color >> 8)  & 0xFF,
                                 current_color        & 0xFF);
            abAppend(ab, buf, clen);
          }
        }

        /* STEP 2e: stop undercurl, restore syntax color */
        if (in_diag) {
          abAppend(ab, "\x1b[24m", 5);
          if (current_color != -1) {
            char buf[32];
            int clen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm",
                                (current_color >> 16) & 0xFF,
                                (current_color >> 8)  & 0xFF,
                                 current_color        & 0xFF);
            abAppend(ab, buf, clen);
          } else {
            abAppend(ab, "\x1b[39m", 5);
          }
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
    E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  int gutter = editorLineNumberWidth();
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1 + gutter);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input  ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorHandleMouse(int key) {
  int mx = E.mouse_x - 1;   /* screen coords are 1-based */
  int my = E.mouse_y - 1;

  int gutter = editorLineNumberWidth();
  int text_mx = mx - gutter;
  if (text_mx < 0) text_mx = 0;

  switch (key) {
    case MOUSE_SCROLL_UP:
      if (E.rowoff > 0) { E.rowoff--; E.scroll_lock = 1; }
      break;

    case MOUSE_SCROLL_DOWN:
      if (E.rowoff + E.screenrows < E.numrows) { E.rowoff++; E.scroll_lock = 1; }
      break;

    case MOUSE_PRESS: {
      if (my < 0 || my >= E.screenrows) return;
      if (mx < 0 || mx >= E.screencols) return;

      int filerow = my + E.rowoff;
      if (filerow > E.numrows) filerow = E.numrows;
      E.cy = filerow;
      if (filerow < E.numrows) {
        erow *row = &E.row[filerow];
        E.cx = editorRowRxToCx(row, text_mx + E.coloff);
      } else {
        E.cx = 0;
      }

      E.sel_start_x = E.cx;
      E.sel_start_y = E.cy;
      E.sel_end_x   = E.cx;
      E.sel_end_y   = E.cy;
      E.selecting   = 1;
      E.dragging    = 1;
      E.scroll_lock = 0;
      break;
    }

    case MOUSE_DRAG: {
      if (!E.dragging) return;
      if (my < 0 || my >= E.screenrows) return;
      if (mx < 0 || mx >= E.screencols) return;

      int filerow = my + E.rowoff;
      if (filerow > E.numrows) filerow = E.numrows;
      E.cy = filerow;
      if (filerow < E.numrows) {
        erow *row = &E.row[filerow];
        E.cx = editorRowRxToCx(row, text_mx + E.coloff);
      } else {
        E.cx = 0;
      }

      E.sel_end_x = E.cx;
      E.sel_end_y = E.cy;
      break;
    }

    case MOUSE_RELEASE:
      E.dragging = 0;
      break;
  }
}

void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();

  if (c == MOUSE_PRESS || c == MOUSE_RELEASE || c == MOUSE_DRAG ||
      c == MOUSE_SCROLL_UP || c == MOUSE_SCROLL_DOWN) {
    editorHandleMouse(c);
    return;
  }

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case '\t': {
        if (E.selecting) editorDeleteSelection();
        if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
        erow *row = &E.row[E.cy];
        int col = editorRowCxToRx(row, E.cx);
        int spaces = KILO_TAB_STOP - (col % KILO_TAB_STOP);
        for (int i = 0; i < spaces; i++) {
          editorRowInsertChar(row, E.cx, ' ');
          E.cx++;
        }
        break;
      }  

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case CTRL_KEY('c'):
      editorCopyToClipboard();
      break;
    case CTRL_KEY('a'):
        E.sel_start_x = 0;
        E.sel_start_y = 0;
        E.sel_end_x   = (E.numrows > 0) ? E.row[E.numrows - 1].size : 0;
        E.sel_end_y   = (E.numrows > 0) ? E.numrows - 1 : 0;
        E.selecting   = 1;
        E.dragging    = 0;
        E.cx = 0;
        E.cy = 0;
        E.scroll_lock = 0;
        break;  

    case CTRL_KEY('v'):
      editorPasteFromClipboard();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

   case BACKSPACE:
   case CTRL_KEY('h'):
   case DEL_KEY:
     editorSetStatusMessage("BS: sel=%d (%d,%d)-(%d,%d)",
       E.selecting,
       E.sel_start_x, E.sel_start_y,
       E.sel_end_x,   E.sel_end_y);
     if (E.selecting) {
       editorDeleteSelection();
     } else {
       if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
       editorDelChar();
     }
     break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

   case CTRL_KEY('l'):
   case '\x1b':
     break;

   default:
     if (E.selecting) editorDeleteSelection();
     editorInsertChar(c);
     break;
  }

  quit_times = KILO_QUIT_TIMES;
}

/*** LSP ***/

void lsp_send(const char *json) {
  if (!E.lsp_in) return;
  fprintf(E.lsp_in, "Content-Length: %zu\r\n\r\n%s", strlen(json), json);
  fflush(E.lsp_in);

  /* DEBUG: log what we send */
  static FILE *slog = NULL;
  if (!slog) slog = fopen("/tmp/lsp_send.log", "a");
  if (slog) { fprintf(slog, "=== SEND ===\n%s\n", json); fflush(slog); }
}

/* Blocking read of one full LSP message. Returns malloc'd JSON or NULL. */
char *lsp_recv() {
  if (!E.lsp_out) return NULL;

  int content_length = -1;
  char line[256];
  while (fgets(line, sizeof(line), E.lsp_out)) {
    if (strncmp(line, "Content-Length:", 15) == 0) {
      content_length = atoi(line + 15);
    }
    if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
  }
  if (content_length < 0) return NULL;

  char *buf = malloc(content_length + 1);
  size_t got = fread(buf, 1, content_length, E.lsp_out);
  buf[got] = '\0';
  return buf;
}


/* Return the index of the LSP server whose extension list matches
   the file's extension (case-insensitive), or -1 if none match. */
int lsp_match_extension(const char *filename) {
  if (!filename) return -1;

  const char *dot = strrchr(filename, '.');
  if (!dot) return -1;

  for (int i = 0; i < E.lsp.nservers; i++) {
    struct lsp_server *s = &E.lsp.servers[i];
    for (int j = 0; j < s->nextensions; j++) {
      if (strcasecmp(dot, s->extensions[j]) == 0) {
        return i;
      }
    }
  }
  return -1;
}

void lsp_start() {
  if (E.lsp.active < 0 || E.lsp.active >= E.lsp.nservers) {
    editorSetStatusMessage("LSP: no server for this file");
    return;
  }

  struct lsp_server *srv = &E.lsp.servers[E.lsp.active];

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    editorSetStatusMessage("LSP: pipe() failed");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    editorSetStatusMessage("LSP: fork() failed");
    return;
  }

  if (pid == 0) {
    dup2(in_pipe[0],  STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull != -1) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    close(in_pipe[0]);  close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);

    /* Build argv: [program_name, args..., NULL] */
    char *argv[MAX_LSP_ARGS + 2];
    char *cmd_copy = strdup(srv->command);
    argv[0] = basename(cmd_copy);
    for (int i = 0; i < srv->nargs; i++) {
      argv[i + 1] = srv->args[i];
    }
    argv[srv->nargs + 1] = NULL;

    execv(srv->command, argv);
    _exit(127);
  }

  close(in_pipe[0]);
  close(out_pipe[1]);
  E.lsp_in  = fdopen(in_pipe[1], "w");
  E.lsp_out = fdopen(out_pipe[0], "r");
  E.lsp_fd  = out_pipe[0];

  setvbuf(E.lsp_out, NULL, _IONBF, 0);
  setvbuf(E.lsp_in,  NULL, _IONBF, 0);
}

/* Forward declaration so lsp_initialize can call the URI helper. */
static void lsp_uri_from_filename(const char *filename, char *out, size_t outsz);

void lsp_initialize(const char *root_path) {
  char uri[1200];
  lsp_uri_from_filename(root_path, uri, sizeof(uri));

  char req[2048];
  snprintf(req, sizeof(req),
    "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\","
    "\"params\":{\"processId\":null,\"rootUri\":\"%s\","
    "\"capabilities\":{\"textDocument\":{\"publishDiagnostics\":{}}}}}",
    E.lsp_next_id++, uri);
  lsp_send(req);

  char *resp = lsp_recv();
  if (resp) {
    editorSetStatusMessage("clangd: initialized");
    free(resp);
  } else {
    editorSetStatusMessage("clangd: no response");
  }

  lsp_send("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

/* Build a file:// URI from a path.
   MSYS paths look like /c/Users/hrish/... but LSP wants file:///c:/Users/...
   so we fix up the drive letter. Backslashes become forward slashes. */
static void lsp_uri_from_filename(const char *filename, char *out, size_t outsz) {
  if (!filename) { out[0] = '\0'; return; }

  /* make absolute if it isn't already */
  char abs[1024];
  if (filename[0] == '/') {
    snprintf(abs, sizeof(abs), "%s", filename);
  } else {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)))
      snprintf(abs, sizeof(abs), "%s/%s", cwd, filename);
    else
      snprintf(abs, sizeof(abs), "%s", filename);
  }

  /* copy, converting \ to / */
  char fixed[1024];
  size_t j = 0;
  for (size_t i = 0; abs[i] && j < sizeof(fixed) - 1; i++) {
    fixed[j++] = (abs[i] == '\\') ? '/' : abs[i];
  }
  fixed[j] = '\0';

  /* MSYS: "/c/Users/..." -> "file:///c:/Users/..." */
  if (fixed[0] == '/' && fixed[1] && fixed[2] == '/') {
    snprintf(out, outsz, "file:///%c:%s", fixed[1], fixed + 2);
  } else {
    snprintf(out, outsz, "file://%s", fixed);
  }
}

void lsp_did_open() {
  if (!E.lsp_in || !E.filename) return;

  char uri[1200];
  lsp_uri_from_filename(E.filename, uri, sizeof(uri));

  int len = 0;
  char *text = editorRowsToString(&len);

  /* escape the file text for JSON: " \ \n \r \t and control chars */
  char *esc = malloc(len * 6 + 1);
  size_t ei = 0;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
      case '"':  esc[ei++] = '\\'; esc[ei++] = '"';  break;
      case '\\': esc[ei++] = '\\'; esc[ei++] = '\\'; break;
      case '\n': esc[ei++] = '\\'; esc[ei++] = 'n';  break;
      case '\r': esc[ei++] = '\\'; esc[ei++] = 'r';  break;
      case '\t': esc[ei++] = '\\'; esc[ei++] = 't';  break;
      default:
        if (c < 0x20) {
          ei += snprintf(esc + ei, 7, "\\u%04x", c);
        } else {
          esc[ei++] = c;
        }
    }
  }
  esc[ei] = '\0';
  free(text);

  /* pick the languageId from the active LSP server (fallback to "c") */
  const char *lang = "c";
  if (E.lsp.active >= 0 && E.lsp.active < E.lsp.nservers) {
    lang = E.lsp.servers[E.lsp.active].languageId;
  }

  /* notification — no "id" field */
  size_t reqsz = ei + strlen(uri) + strlen(lang) + 256;
  char *req = malloc(reqsz);
  snprintf(req, reqsz,
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
    "\"params\":{\"textDocument\":{"
      "\"uri\":\"%s\","
      "\"languageId\":\"%s\","
      "\"version\":1,"
      "\"text\":\"%s\""
    "}}}",
    uri, lang, esc);

  lsp_send(req);
  free(req);
  free(esc);

  editorSetStatusMessage("LSP: didOpen sent (%zu bytes text)", ei);
}

/* Parse a textDocument/publishDiagnostics notification and stamp
   E.row[].diag_hl[] with per-character severity.
   severity: 1=error, 2=warning, 3=info, 4=hint. 0 = no diagnostic. */
void lsp_handle_diagnostics(const char *json, size_t len) {
  yyjson_doc *doc = yyjson_read(json, len, 0);
  if (!doc) {
    fprintf(stderr, "[diag] yyjson_read failed\n");
    return;
  }

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *params = yyjson_obj_get(root, "params");
  if (!params) { yyjson_doc_free(doc); return; }

  yyjson_val *uri_v = yyjson_obj_get(params, "uri");
  if (!uri_v || !yyjson_is_str(uri_v)) { yyjson_doc_free(doc); return; }

  /* Only act on diagnostics for the currently open file. */
  char current_uri[1200];
  lsp_uri_from_filename(E.filename, current_uri, sizeof(current_uri));
  if (strcmp(current_uri, yyjson_get_str(uri_v)) != 0) {
    yyjson_doc_free(doc);
    return;
  }

  /* Clear all existing markers before stamping the new set. */
  for (int i = 0; i < E.numrows; i++) {
    memset(E.row[i].diag_hl, 0, E.row[i].rsize);
    E.row[i].diag_level = 0;
  }

  yyjson_val *diags = yyjson_obj_get(params, "diagnostics");
  if (!diags || !yyjson_is_arr(diags)) { yyjson_doc_free(doc); return; }

  int count = 0;
  size_t idx, max;
  yyjson_val *d;
  yyjson_arr_foreach(diags, idx, max, d) {
    yyjson_val *sev_v = yyjson_obj_get(d, "severity");
    int severity = sev_v && yyjson_is_int(sev_v) ? (int)yyjson_get_int(sev_v) : 1;

    yyjson_val *range = yyjson_obj_get(d, "range");
    if (!range) continue;
    yyjson_val *start = yyjson_obj_get(range, "start");
    yyjson_val *end   = yyjson_obj_get(range, "end");
    if (!start || !end) continue;

    yyjson_val *sl_v = yyjson_obj_get(start, "line");
    yyjson_val *sc_v = yyjson_obj_get(start, "character");
    yyjson_val *el_v = yyjson_obj_get(end, "line");
    yyjson_val *ec_v = yyjson_obj_get(end, "character");
    if (!sl_v || !sc_v || !el_v || !ec_v) continue;

    int start_line = (int)yyjson_get_int(sl_v);
    int start_col  = (int)yyjson_get_int(sc_v);
    int end_line   = (int)yyjson_get_int(el_v);
    int end_col    = (int)yyjson_get_int(ec_v);

    if (start_line < 0 || start_line >= E.numrows) continue;

    erow *row = &E.row[start_line];
    int line_size = row->size;

    if (start_col < 0) start_col = 0;
    if (end_col > line_size) end_col = line_size;
    if (end_line != start_line) end_col = line_size;  /* simple: only first line */

    for (int c = start_col; c < end_col; c++) {
      row->diag_hl[c] = (unsigned char)severity;
    }

    /* line-level marker: lowest severity number wins on a line */
    unsigned char newsev = (unsigned char)severity;
    if (row->diag_level == 0 || newsev < row->diag_level) {
      row->diag_level = newsev;
    }

    count++;
  }

  yyjson_doc_free(doc);
  editorSetStatusMessage("LSP: %d diagnostics", count);
}


/* Non-blocking drain: pull every complete LSP message currently available.
   Logs raw JSON to /tmp/lsp.log. Safe to call every frame. */

void lsp_drain() {
  if (!E.lsp_out || E.lsp_fd < 0) return;

  static FILE *log = NULL;
  if (!log) {
    log = fopen("/tmp/lsp.log", "a");
    if (log) {
      fprintf(log, "\n=== kilo started %ld ===\n", (long)time(NULL));
      fflush(log);
    }
  }

  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(E.lsp_fd, &rfds);
    struct timeval tv = {0, 0};

    int r = select(E.lsp_fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) break;
    if (!FD_ISSET(E.lsp_fd, &rfds)) break;

    char *msg = lsp_recv();
    if (!msg) break;

    if (log) {
      fprintf(log, "--- LSP MSG ---\n%s\n", msg);
      fflush(log);
    }

    if (strstr(msg, "\"method\":\"textDocument/publishDiagnostics\"")) {
      lsp_handle_diagnostics(msg, strlen(msg));
    }

    free(msg);
  }
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;
  E.mouse_x = 0;
  E.mouse_y = 0;
  E.scroll_lock = 0;
  E.sel_start_x = 0;
  E.sel_start_y = 0;
  E.sel_end_x = 0;
  E.sel_end_y = 0;
  E.selecting = 0;
  E.dragging = 0;
  E.lsp_in = NULL;
  E.lsp_out = NULL;
  E.lsp_fd = -1;
  E.lsp_next_id = 1;
  E.tab_stop = 4;
  E.lsp.nservers = 0;
  E.lsp.active = -1;
  setup_default_lsp_config();

  memcpy(&E.theme, &default_theme, sizeof(struct theme));
  E.theme.name = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;

      

  
}  


int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  load_config();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage(
    "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  /* Match the file's extension against the configured LSP servers.
     Only start a server if we found a match. */
  if (E.filename) {
    E.lsp.active = lsp_match_extension(E.filename);
  }

  if (E.lsp.active >= 0) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
      lsp_start();
      lsp_initialize(cwd);
    }
    if (E.filename) lsp_did_open();
  }

  while (1) {
    lsp_drain();
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
