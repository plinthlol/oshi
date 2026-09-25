/*** includes ***/

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define ALT_KEY(k) (2000 + (k)) /* an Alt-key reads back as this */
#ifndef OSHI_VERSION
#define OSHI_VERSION "1.0.0"
#endif
#define OSHI_TAB_STOP 8
#define OSHI_CLICK_MS 400   /* a second click lands within this to count */
#define OSHI_CLICK_SLOP 2   /* cells the clicks may drift and still count */
#define OSHI_UNDO_MAX 1000  /* undo steps kept (each one is a small diff) */
#define OSHI_OSC52_MAX 74994 /* biggest copy we hand the terminal clipboard */
#define OSHI_STATUS_MS 1500 /* how long a bar message lives, in milliseconds */
#define OSHI_PASTE_MAX (64 * 1024 * 1024)
#define OSHI_PROMPT_MAX 200

enum editorKey {
  ESC_AGAIN = -1,
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
  CTRL_LEFT,
  CTRL_RIGHT,
  SHIFT_UP,
  SHIFT_DOWN,
  SHIFT_LEFT,
  SHIFT_RIGHT,
  CSHIFT_LEFT,  /* ctrl+shift: the far end jumps whole words */
  CSHIFT_RIGHT,
  MOUSE_KEY,
  UTF8_KEY,   /* a whole multi-byte character, bytes in E.utf8 */
  PASTE_KEY,  /* the terminal is about to send a bracketed paste */
  RESIZE_KEY, /* the window changed size */
  NOKEY       /* something we don't understand: swallowed, does nothing */
};

/* The commands the config may move to a different key. */
enum editorCommand {
  CMD_NONE = -1,
  CMD_SAVE,
  CMD_QUIT,
  CMD_FIND,
  CMD_SELECT,
  CMD_COPY,
  CMD_CUT,
  CMD_PASTE,
  CMD_KILL,
  CMD_UNDO,
  CMD_REDO,
  CMD_REPLACE,
  CMD_FIND_NEXT,
  CMD_FIND_PREV,
  CMD_GOTO,
  CMD_HELP,
  CMD_COUNT
};

/*** colors ***/

/* Configurable color roles. Each resolves to one ANSI escape sequence: the
 * foregrounds paint text color, the backgrounds paint cell color. The config
 * can rebind any of them; "colorscheme default" resets the whole set. */
enum editorColor {
  COL_STATUS_BG,       /* status bar background                  */
  COL_STATUS_FG,       /* status bar foreground                */
  COL_GUTTER_FG,       /* line-number gutter, non-current line */
  COL_GUTTER_CURSOR_FG,/* line-number gutter, current line     */
  COL_GUTTER_BG,        /* background behind the gutter, non-current line */
  COL_GUTTER_CURSOR_BG, /* background behind the gutter, current line     */
  COL_TILDE_FG,        /* the ~ fringe below the buffer        */
  COL_TEXT_FG,         /* buffer text foreground             */
  COL_SCROLLMARK_BG,   /* the < / > "more this way" marks    */
  COL_SCROLLMARK_FG,
  COL_COUNT
};

static int colIsFg[COL_COUNT] = { 0, 1, 1, 1, 0, 0, 1, 1, 0, 1 };

/*** data ***/

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;      /* cursor: byte offset in the row, and the row */
  int rx;          /* cursor: display column in the row */
  int goal_rx;     /* the column up/down try to keep */
  int rowoff;
  int coloff;
  int sel; /* a live selection: anchor -> cursor */
  int sel_cx;
  int sel_cy;
  int mouse_button;  /* SGR button code of the last mouse event */
  int mouse_release; /* that event was a release */
  int mouse_x;
  int mouse_y;
  int mouse_click_n;   /* clicks in a row: 1, 2 for a double, 3 for a triple */
  int mouse_click_x;
  int mouse_click_y;
  long mouse_click_ms; /* when the last one landed */
  int screenrows;
  int screencols;
  int numrows;
  int rowcap;
  erow *row;
  int dirty;
  int crlf;         /* the file used CRLF line endings: save them back */
  int redraw;       /* clear the whole screen on the next refresh */
  int ruler;
  int ruler_width;
  int gutter;
  int cursor_style; /* DECSCUSR code (2 = block, 6 = beam) */
  int binds[CMD_COUNT]; /* which key runs each command */
  char col_str[COL_COUNT][16]; /* cached ANSI escapes for each color role */
  char *filename;
  char statusmsg[256];
  long statusmsg_time; /* ms on the monotonic clock, when the message was set */
  int notice_pending;  /* 1 while the bar shows a notice that lapses */
  long notice_deadline; /* ms: when that notice comes off the bar */
  char config_err[160]; /* shown on the bar for a moment, and until the first key */
  long config_err_time; /* ms: when the config error was set */
  int prompt_col;   /* where the caret sits on the bar while prompting */
  int in_prompt; /* 1 while editorPrompt is open: draw the caret in the bar */
  int quit_confirm; /* 1 after the first quit key on a dirty buffer */
  long quit_time;   /* ms: when that quit warning went up */
  char utf8[8];     /* the bytes behind the last UTF8_KEY */
  int utf8len;
  char *clip;       /* oshi's own copy of the last copy/cut */
  int clip_len;
  int clip_line;    /* it was a whole line (no selection): paste puts it above */
  struct termios orig_termios;
};

struct editorConfig E;

static volatile sig_atomic_t got_winch;
static volatile sig_atomic_t got_term;
static int raw_enabled;

/* The commands, with the config name for each and how help words it. */
struct editorCmd { const char *name; const char *desc; };
struct editorCmd editorCmds[CMD_COUNT] = {
  { "save", "save the file (root via sudo/doas if refused)" },
  { "quit", "quit (one warning on the bar if modified, then quits)" },
  { "find", "find in the file" },
  { "select", "select everything" },
  { "copy", "copy selection, else the line" },
  { "cut", "cut selection, else the line" },
  { "paste", "paste what oshi last copied" },
  { "delete-line", "delete the line" },
  { "undo", "undo a change" },
  { "redo", "redo it" },
  { "replace", "find and replace" },
  { "find-next", "jump to the next match" },
  { "find-prev", "jump to the previous match" },
  { "goto", "go to a line (N, +N, -N or N:col)" },
  { "help", "show this help screen" }
};

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int),
                   void (*decorate)(char *));
int editorCommandForKey(int c);
void editorKeyName(int key, char *buf, size_t len);
char *editorIstrstr(const char *hay, const char *needle);
int editorFindCount(int *cur);
int editorFindNextMatch(int dir);
void editorHelp(void);
int editorSaveElevated(const char *buf, int len);
void editorColorSet(int role, int code);
void editorApplyDefaultColors(void);
int editorParseColorRole(const char *name);
void editorSyncGoal(void);
void editorScroll(void);
long editorNowMs(void);
char *editorRowsToString(int *buflen);

/*** terminal ***/

/* Write a string in one go. Only write() and strlen(), so signal handlers
 * may use it too. */
static void editorWrite(const char *s) {
  size_t len = strlen(s);
  ssize_t r = write(STDOUT_FILENO, s, len);
  (void)r;
}

/* Draw on a screen of our own rather than over the top of whatever the shell
 * had printed, so none of our frames end up in the scrollback. */
void editorUseAltScreen(void) {
  editorWrite("\x1b[?1049h");
}

/* Everything we ask the terminal for while we run:
 *   mouse reports (so the line numbers stay out of selections),
 *   the text (I-beam) pointer, bracketed paste, and no auto-wrap - a line
 *   that is too long for the row is clipped instead of spilling onto the
 *   next one and scrolling the screen. */
void editorTermSetup(void) {
  editorWrite("\x1b[?1002h\x1b[?1006h"
              "\x1b]22;text\x1b\\"
              "\x1b[?2004h"
              "\x1b[?7l");
}

/* Hand the terminal back: the shell's own screen, its cursor shape, its
 * pointer, wrapping, and a visible cursor. Safe from a signal handler. */
void editorTermReset(void) {
  editorWrite("\x1b[?2004l\x1b[?7h"
              "\x1b]22;default\x1b\\"
              "\x1b[?1006l\x1b[?1002l"
              "\x1b[?1049l\x1b[?25h\x1b[0 q");
}

/* Tell the terminal which cursor to draw: 2 is a steady block, 6 a steady bar. */
void editorSetCursorStyle(void) {
  char seq[16];
  int len = snprintf(seq, sizeof(seq), "\x1b[%d q", E.cursor_style);
  ssize_t r = write(STDOUT_FILENO, seq, len);
  (void)r;
}

void disableRawMode(void) {
  if (raw_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    raw_enabled = 0;
  }
}

void die(const char *s) {
  int e = errno;
  editorTermReset();
  disableRawMode(); /* so perror's newline gets its carriage return back */
  errno = e;
  perror(s);
  exit(1);
}

void editorQuit(void) {
  editorTermReset();
  exit(0);
}

void enableRawMode(void) {
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
  raw_enabled = 1;
}

/*** signals ***/

static void editorOnWinch(int sig) { (void)sig; got_winch = 1; }
static void editorOnTerm(int sig) { got_term = sig; }

/* A crash: put the terminal right before the default action takes over, so a
 * bug doesn't leave the shell in raw mode on an alt screen with the mouse
 * grabbed. Only async-signal-safe calls in here. */
static void editorOnCrash(int sig) {
  editorTermReset();
  if (raw_enabled) tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
  signal(sig, SIG_DFL);
  raise(sig);
}

/* No SA_RESTART on purpose: a blocked read() must be interrupted so the main
 * loop notices a resize or a request to quit. */
static void editorInstallSignals(void) {
  struct sigaction sa;
  int i;
  static const int term_sigs[] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT };
  static const int crash_sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);

  sa.sa_handler = editorOnWinch;
  sigaction(SIGWINCH, &sa, NULL);

  /* popen() runs through /bin/sh, and tools we try may not be installed: a
   * closed pipe must make the write fail, not raise SIGPIPE and take the
   * editor down with it. */
  signal(SIGPIPE, SIG_IGN);

  sa.sa_handler = editorOnTerm;
  for (i = 0; i < (int)(sizeof(term_sigs) / sizeof(term_sigs[0])); i++)
    sigaction(term_sigs[i], &sa, NULL);

  sa.sa_handler = editorOnCrash;
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  for (i = 0; i < (int)(sizeof(crash_sigs) / sizeof(crash_sigs[0])); i++)
    sigaction(crash_sigs[i], &sa, NULL);
}

/* The window changed: read the new size. */
static int editorResize(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 ||
      ws.ws_row == 0)
    return 0;
  E.screencols = ws.ws_col;
  E.screenrows = ws.ws_row - 1; /* the status bar takes one */
  if (E.screenrows < 1) E.screenrows = 1;
  E.redraw = 1;
  return 1;
}

/*** input: keys ***/

/* Consumes the rest of an escape sequence and returns the key it stands for.
 * A sequence we don't recognize is swallowed - its tail would otherwise end
 * up in the buffer as typed text - and reported as NOKEY, which does
 * nothing (and, unlike a bare ESC, doesn't cancel a prompt). */
int editorReadEscape(void) {
  char seq[32];
  int len = 1;

  if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b'; /* lone ESC */
  if (seq[0] == '\x1b') return ESC_AGAIN; /* ESC glued to the next key's ESC */
  if (seq[0] != '[' && seq[0] != 'O')
    return ALT_KEY((unsigned char)seq[0]); /* ESC + Alt-key */
  while (len < (int)sizeof(seq) - 1) {
    if (read(STDIN_FILENO, &seq[len], 1) != 1) break;
    len++;
    if (seq[len - 1] >= 0x40 && seq[len - 1] <= 0x7e) break; /* final byte */
  }
  if (len == 1) return ALT_KEY((unsigned char)seq[0]); /* Alt-[ or Alt-O */
  seq[len] = '\0';

  char final = seq[len - 1];
  if (seq[0] == '[' && final == 'M' && len == 2) { /* X10 mouse report */
    /* Three bytes follow - button, column, row, each biased by 32. They can
     * arrive split across reads, so take all three and drop them together: a
     * partial report left in the stream would be parsed as the next keypress.
     * VTIME makes an idle read return 0 after 100ms, so a few of those are
     * waited out - a whole report is one write from the terminal, and after
     * a second of silence it was truncated and there is nothing more to take.
     * The legacy report itself is not decoded - it can't tell a drag from a
     * wheel notch, and every terminal oshi talks to has SGR (1006) by now -
     * so it reads as NOKEY, as before. */
    unsigned char m[3];
    int got = 0, idle = 0;
    while (got < 3) {
      ssize_t r = read(STDIN_FILENO, m + got, sizeof(m) - (size_t)got);
      if (r > 0) {
        got += (int)r;
        idle = 0;
        continue;
      }
      if (r == 0 && ++idle <= 10) continue; /* a beat between the bytes */
      if (r == -1 && errno == EINTR && !got_term) continue;
      break;
    }
    return NOKEY;
  }
  if (len > 2 && seq[0] == '[' && seq[1] == '<' &&
      sscanf(&seq[2], "%d;%d;%d", &E.mouse_button, &E.mouse_x, &E.mouse_y) == 3) {
    E.mouse_release = final == 'm';
    return MOUSE_KEY;
  }
  switch (final) {
  case 'A': case 'B': case 'C': case 'D': {
    /* xterm "[1;N": N-1 is the modifier mask - bit0 shift, bit2 ctrl */
    int m = (len > 4 && seq[1] == '1' && seq[2] == ';') ? atoi(&seq[3]) - 1 : 0;
    int shift = m & 1, ctrl = m & 4;
    if (final == 'A') return shift ? SHIFT_UP : ARROW_UP;
    if (final == 'B') return shift ? SHIFT_DOWN : ARROW_DOWN;
    if (ctrl) return final == 'C'
                    ? (shift ? CSHIFT_RIGHT : CTRL_RIGHT)
                    : (shift ? CSHIFT_LEFT : CTRL_LEFT);
    if (shift) return final == 'C' ? SHIFT_RIGHT : SHIFT_LEFT;
    return final == 'C' ? ARROW_RIGHT : ARROW_LEFT;
  }
  case 'H': return HOME_KEY;
  case 'F': return END_KEY;
  case '~':
    switch (atoi(&seq[1])) { /* the number in front of the '~' */
    case 1: case 7: return HOME_KEY;
    case 3: return DEL_KEY;
    case 4: case 8: return END_KEY;
    case 5: return PAGE_UP;
    case 6: return PAGE_DOWN;
    case 200: return PASTE_KEY;
    }
    break;
  }
  return NOKEY;
}

int editorReadKey(void) {
  int nread;
  char c;
  static int instant_reads;
  for (;;) {
    if (got_term) { /* asked to die politely: leave the terminal as we found it */
      editorTermReset();
      exit(128 + (int)got_term);
    }
    if (got_winch) {
      got_winch = 0;
      if (editorResize()) return RESIZE_KEY;
    }
    long t0 = editorNowMs();
    nread = read(STDIN_FILENO, &c, 1);
    if (nread == 1) {
      instant_reads = 0;
      break;
    }
    if (nread == 0 && editorNowMs() - t0 < 20) {
      /* A quiet read takes VTIME (100ms). One that comes back at once, again
       * and again, means the terminal is gone and nobody told us: quit
       * instead of spinning forever. */
      if (++instant_reads > 200) {
        editorTermReset();
        exit(1);
      }
    } else {
      instant_reads = 0;
    }
    if (nread == -1 && errno != EAGAIN && errno != EINTR) die("read");
    /* The VTIME idle read is the only timer we have: use it to take a notice
     * off the bar once its time is up, rather than leaving it there until the
     * next keypress happens to repaint. */
    if (E.notice_pending && editorNowMs() >= E.notice_deadline) {
      E.notice_pending = 0; /* repaint once, then it is gone */
      return NOKEY; /* nothing to act on: just a redraw */
    }
  }
  if (c == '\x1b') {
    int key = editorReadEscape();
    while (key == ESC_AGAIN) key = editorReadEscape();
    return key;
  }
  unsigned char u = (unsigned char)c;
  if (u < 0x80) return u;
  if (u >= 0xC2 && u <= 0xF4) {
    /* the lead byte of a multi-byte character: take the rest with it, so the
     * cursor never rests between the halves of one character */
    int need = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
    int total = need + 1;
    E.utf8[0] = c;
    E.utf8len = 1;
    while (need-- > 0) {
      char cc;
      if (read(STDIN_FILENO, &cc, 1) != 1) break;
      if (((unsigned char)cc & 0xC0) != 0x80) return NOKEY;
      E.utf8[E.utf8len++] = cc;
    }
    return E.utf8len == total ? UTF8_KEY : NOKEY;
  }
  return NOKEY; /* a stray continuation byte, or not UTF-8 at all */
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

/*** memory ***/

/* Out of memory is fatal here, not a NULL to chase at every call site. */
void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (p == NULL) die("malloc");
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *np = realloc(p, n ? n : 1);
  if (np == NULL) die("realloc");
  return np;
}

/* Read the text of a bracketed paste, up to the terminator. Never NULL. */
static char *editorReadPasteBuf(int *len);
static void editorSkipPaste(void) {
  int n;
  free(editorReadPasteBuf(&n));
}

/* Find endseq in buf[0..n), starting the search at `from`. Returns the byte
 * offset of the match, or n if it isn't there yet. A plain loop rather than
 * memmem(): the needle is 6 fixed bytes, and this keeps the function off of
 * a glibc/BSD-specific call some libc's don't ship. */
static size_t editorFindPasteEnd(const char *buf, size_t n, size_t from) {
  static const char endseq[] = "\x1b[201~";
  const size_t elen = sizeof(endseq) - 1;
  size_t i;
  if (n < elen) return n;
  for (i = from; i + elen <= n; i++)
    if (memcmp(buf + i, endseq, elen) == 0) return i;
  return n;
}

static char *editorReadPasteBuf(int *len) {
  static const char endseq[] = "\x1b[201~";
  const size_t elen = sizeof(endseq) - 1;
  const size_t step = 8192; /* read this much at a time, not one byte at a time */
  size_t cap = step, n = 0;
  char *buf = xmalloc(cap);
  int idle = 0;

  for (;;) {
    if (n + step + 1 > cap) {
      if (cap >= OSHI_PASTE_MAX) break;
      size_t newcap = cap * 2;
      if (newcap > OSHI_PASTE_MAX) newcap = OSHI_PASTE_MAX;
      if (newcap <= cap) break; /* already at the cap */
      buf = xrealloc(buf, newcap);
      cap = newcap;
    }
    size_t want = cap - n;
    if (want > step) want = step;
    if (want == 0) break;
    ssize_t r = read(STDIN_FILENO, buf + n, want);
    if (r == 0) { /* a pause in the stream: wait a while for the rest */
      if (++idle > 100) break;
      continue;
    }
    if (r == -1) {
      if (errno == EINTR && !got_term) continue;
      break;
    }
    idle = 0;
    /* the terminator (6 bytes) may straddle this read and the last one, so
     * start scanning a little before the new data rather than exactly at it */
    size_t scan_from = n >= elen - 1 ? n - (elen - 1) : 0;
    n += (size_t)r;
    size_t hit = editorFindPasteEnd(buf, n, scan_from);
    if (hit < n) {
      n = hit;
      break;
    }
  }
  *len = (int)n;
  return buf;
}

/*** utf-8 ***/

/* Decode the code point at s (n bytes to work with). *len is how many bytes it
 * takes. A byte that isn't valid UTF-8 comes back as U+FFFD, one byte long, so
 * a file in some other encoding still opens (and is saved back untouched). */
static int utf8Decode(const unsigned char *s, int n, int *len) {
  unsigned char c = s[0];
  int need, cp, i;
  if (c < 0x80) { *len = 1; return c; }
  if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
  else { *len = 1; return 0xFFFD; }
  if (n < need + 1) { *len = 1; return 0xFFFD; }
  for (i = 1; i <= need; i++) {
    if ((s[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
      cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *len = 1;
    return 0xFFFD;
  }
  *len = need + 1;
  return cp;
}

/* How many terminal cells a code point takes: 0 for combining marks and
 * joiners, 2 for CJK and emoji, 1 for the rest. */
static int cpWidth(int cp) {
  static const struct { int lo, hi; } zero[] = {
    {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},
    {0x05C1,0x05C2},{0x05C4,0x05C5},{0x05C7,0x05C7},{0x0610,0x061A},
    {0x064B,0x065F},{0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},
    {0x06E7,0x06E8},{0x06EA,0x06ED},{0x0E31,0x0E31},{0x0E34,0x0E3A},
    {0x0E47,0x0E4E},{0x1AB0,0x1AFF},{0x1DC0,0x1DFF},{0x200B,0x200F},
    {0x202A,0x202E},{0x2060,0x2064},{0x20D0,0x20FF},{0xFE00,0xFE0F},
    {0xFE20,0xFE2F},{0xFEFF,0xFEFF},{0x1F3FB,0x1F3FF},{0xE0100,0xE01EF}
  };
  static const struct { int lo, hi; } wide[] = {
    {0x1100,0x115F},{0x2E80,0x303E},{0x3041,0x33FF},{0x3400,0x4DBF},
    {0x4E00,0x9FFF},{0xA000,0xA4CF},{0xA960,0xA97F},{0xAC00,0xD7A3},
    {0xF900,0xFAFF},{0xFE10,0xFE19},{0xFE30,0xFE6F},{0xFF00,0xFF60},
    {0xFFE0,0xFFE6},{0x1F300,0x1F64F},{0x1F680,0x1F6FF},{0x1F900,0x1F9FF},
    {0x1FA70,0x1FAFF},{0x20000,0x3FFFD}
  };
  size_t i;
  if (cp < 0x300) return 1; /* includes control bytes: drawn as one '?' */
  for (i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
    if (cp >= zero[i].lo && cp <= zero[i].hi) return 0;
  for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
    if (cp >= wide[i].lo && cp <= wide[i].hi) return 2;
  return 1;
}

/* One user-perceived character is a base plus any marks that ride on it (and
 * whatever follows a zero-width joiner, so emoji sequences stay whole). The
 * cursor, Backspace and Delete all move by these. */
static int editorCharEnd(const erow *row, int b) {
  int len, cp, zwj;
  if (b >= row->size) return row->size;
  cp = utf8Decode((const unsigned char *)row->chars + b, row->size - b, &len);
  b += len;
  zwj = cp == 0x200D;
  while (b < row->size) {
    int l2, c2 = utf8Decode((const unsigned char *)row->chars + b,
                            row->size - b, &l2);
    if (!zwj && cpWidth(c2) != 0) break;
    zwj = c2 == 0x200D;
    b += l2;
  }
  return b;
}

/* Where the character before byte b starts. */
static int editorPrevChar(const erow *row, int b) {
  int s = 0, e;
  if (b > row->size) b = row->size;
  while (s < b) {
    e = editorCharEnd(row, s);
    if (e >= b) return s;
    s = e;
  }
  return b > 0 ? b - 1 : 0;
}

/* Cells the character at byte b takes, given the column it starts in (a tab
 * runs to the next tab stop). */
static int editorCellWidth(const erow *row, int b, int col) {
  int len, cp, w;
  if (row->chars[b] == '\t') return OSHI_TAB_STOP - (col % OSHI_TAB_STOP);
  cp = utf8Decode((const unsigned char *)row->chars + b, row->size - b, &len);
  w = cpWidth(cp);
  return w > 0 ? w : 1;
}

/* The same for plain strings (the status bar). */
static int editorStrWidth(const char *s, int n) {
  int w = 0, i = 0;
  while (i < n) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    w += cpWidth(cp);
    i += l;
  }
  return w;
}

/* Bytes of the longest prefix of s that fits in maxcols cells. */
static int editorFitHead(const char *s, int n, int maxcols) {
  int w = 0, i = 0;
  while (i < n) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    int cw = cpWidth(cp);
    if (w + cw > maxcols) break;
    w += cw;
    i += l;
  }
  return i;
}

/* Where the longest suffix of s that fits in maxcols cells starts. */
static int editorFitTail(const char *s, int n, int maxcols) {
  int total = editorStrWidth(s, n), i = 0;
  while (i < n && total > maxcols) {
    int l, cp = utf8Decode((const unsigned char *)s + i, n - i, &l);
    total -= cpWidth(cp);
    i += l;
  }
  return i;
}

/* Count user-perceived characters (UTF-8 code points) in s[0..len): bytes that
 * are a continuation (10xxxxxx) of a multi-byte sequence are skipped, so one
 * glyph -- even one spanning bytes -- counts as one character. */
int editorCharCount(const char *s, int len) {
  int n = 0, i;
  for (i = 0; i < len; i++)
    if ((unsigned char)(s[i] & 0xC0) != 0x80) n++;
  return n;
}

/* How many lines s[0..len) spans: one per newline, plus a last line that
 * doesn't end in one. Empty text is zero lines. */
int editorLineCount(const char *s, int len) {
  int n = 0, i;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  if (len > 0 && s[len - 1] != '\n') n++;
  return n;
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0, b = 0;
  if (cx > row->size) cx = row->size;
  while (b < cx) {
    rx += editorCellWidth(row, b, rx);
    b = editorCharEnd(row, b);
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur = 0, b = 0;
  while (b < row->size) {
    int w = editorCellWidth(row, b, cur);
    if (cur + w > rx) return b;
    cur += w;
    b = editorCharEnd(row, b);
  }
  return row->size;
}

/* The column the cursor is in: what up and down try to hold on to. */
void editorSyncGoal(void) {
  E.goal_rx = E.cy < E.numrows ? editorRowCxToRx(&E.row[E.cy], E.cx) : 0;
}

/* Keep the cursor on a row that exists, at a spot that exists. The one time
 * there is no row is an empty buffer, where it sits at 0,0. */
static void editorClampCursor(void) {
  if (E.numrows == 0) { E.cx = 0; E.cy = 0; return; }
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  if (E.cy < 0) E.cy = 0;
  if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
  if (E.cx < 0) E.cx = 0;
}

static void editorRowsReserve(int n) {
  int cap;
  if (n <= E.rowcap) return;
  cap = E.rowcap ? E.rowcap : 64;
  while (cap < n) cap *= 2;
  E.row = xrealloc(E.row, sizeof(erow) * cap);
  E.rowcap = cap;
}

void editorInsertRow(int at, const char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  editorRowsReserve(E.numrows + 1);
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = (int)len;
  E.row[at].chars = xmalloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
}

/* Swap ndel rows starting at `at` for nins new ones, in one move (undo and redo
 * use this so big steps don't shuffle the array row by row). */
static void editorReplaceRows(int at, int ndel, char **s, int *sz, int nins) {
  int i, tail = E.numrows - at - ndel;
  for (i = 0; i < ndel; i++) editorFreeRow(&E.row[at + i]);
  editorRowsReserve(E.numrows - ndel + nins);
  if (tail > 0)
    memmove(&E.row[at + nins], &E.row[at + ndel], sizeof(erow) * tail);
  for (i = 0; i < nins; i++) {
    E.row[at + i].size = sz[i];
    E.row[at + i].chars = xmalloc(sz[i] + 1);
    memcpy(E.row[at + i].chars, s[i], sz[i]);
    E.row[at + i].chars[sz[i]] = '\0';
  }
  E.numrows += nins - ndel;
}

void editorRowInsertBytes(erow *row, int at, const char *s, int n) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, row->size + n + 1);
  memmove(&row->chars[at + n], &row->chars[at], row->size - at + 1);
  memcpy(&row->chars[at], s, n);
  row->size += n;
}

void editorRowAppendString(erow *row, const char *s, size_t len) {
  row->chars = xrealloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += (int)len;
  row->chars[row->size] = '\0';
}

/* Delete the bytes [from,to) in a row. */
void editorRowDeleteRange(erow *row, int from, int to) {
  if (from < 0) from = 0;
  if (to > row->size) to = row->size;
  if (from >= to) return;
  memmove(&row->chars[from], &row->chars[to], row->size - to + 1);
  row->size -= (to - from);
}

/*** undo ***/

/* Every change is remembered as a small diff: the rows a change replaced (a
 * handful, not the whole file) and how many rows took their place. Undo swaps
 * the two, and the very same swap is redo. Plain typing runs together into
 * one step. */
typedef struct editorRec {
  int at;        /* first row the change touched */
  char **old;    /* the rows that used to be there, each on its own */
  int *osize;
  int nold;      /* how many of them */
  int nnew;      /* how many rows sit there now */
  int cx0, cy0;  /* where the cursor goes when this record is applied */
  int cx1, cy1;  /* and where it goes when it is applied back again */
} editorRec;

static editorRec *undo_stack;
static int undo_count, undo_cap;
static long undo_dropped;   /* steps that fell off the old end */
static editorRec *redo_stack;
static int redo_count, redo_cap;
static editorRec undo_pend;  /* the change being made right now */
static int undo_pending;
static int undo_rows_before;
static int undo_run;         /* plain typing keeps running: one undo step */
static int undo_run_cx, undo_run_cy; /* where the next key of that run lands */
static char *saved_text;     /* the buffer bytes the file on disk holds */
static int saved_len;        /* how long that is */

static void editorFreeRec(editorRec *r) {
  int i;
  for (i = 0; i < r->nold; i++) free(r->old[i]);
  free(r->old);
  free(r->osize);
  r->old = NULL;
  r->osize = NULL;
  r->nold = 0;
}

static void editorCaptureRows(editorRec *r, int at, int n) {
  int i;
  r->at = at;
  r->nold = n;
  r->old = xmalloc(sizeof(char *) * (n > 0 ? n : 1));
  r->osize = xmalloc(sizeof(int) * (n > 0 ? n : 1));
  for (i = 0; i < n; i++) {
    r->osize[i] = E.row[at + i].size;
    r->old[i] = xmalloc(E.row[at + i].size + 1);
    memcpy(r->old[i], E.row[at + i].chars, E.row[at + i].size);
    r->old[i][E.row[at + i].size] = '\0';
  }
}

/* The buffer is "modified" exactly when its bytes are no longer the ones the
 * file on disk holds - compared as content, not by undo-step arithmetic, so
 * any route back to the saved text (undo, redo, or edits that happen to
 * recreate it) clears the flag. The total-length precheck is O(rows) and
 * rejects the usual case without materialising the buffer; only a length
 * match pays for the full compare. */
static void editorSnapshot(void) {
  free(saved_text);
  saved_text = editorRowsToString(&saved_len);
}

static void editorUpdateDirty(void) {
  long total = 0;
  int j, len;
  char *cur;
  for (j = 0; j < E.numrows; j++)
    total += E.row[j].size + (E.crlf ? 2 : 1);
  if (total != saved_len) {
    E.dirty = 1;
    return;
  }
  cur = editorRowsToString(&len);
  E.dirty = saved_text == NULL || len != saved_len ||
            memcmp(cur, saved_text, len) != 0;
  free(cur);
}

static void editorPushUndo(editorRec r) {
  if (undo_count == OSHI_UNDO_MAX) { /* the oldest falls off the end */
    editorFreeRec(&undo_stack[0]);
    memmove(undo_stack, undo_stack + 1, sizeof(editorRec) * (undo_count - 1));
    undo_count--;
    undo_dropped++;
  }
  if (undo_count == undo_cap) {
    undo_cap = undo_cap ? undo_cap * 2 : 64;
    undo_stack = xrealloc(undo_stack, sizeof(editorRec) * undo_cap);
  }
  undo_stack[undo_count++] = r;
}

static void editorPushRedo(editorRec r) {
  if (redo_count == redo_cap) {
    redo_cap = redo_cap ? redo_cap * 2 : 64;
    redo_stack = xrealloc(redo_stack, sizeof(editorRec) * redo_cap);
  }
  redo_stack[redo_count++] = r;
}

/* Call before a change: say which rows it may touch. Every row the change
 * alters must lie within rows [at, at+nold). */
void editorUndoBegin(int at, int nold) {
  if (at < 0) at = 0;
  if (at > E.numrows) at = E.numrows;
  if (nold < 0) nold = 0;
  if (at + nold > E.numrows) nold = E.numrows - at;
  editorCaptureRows(&undo_pend, at, nold);
  undo_pend.cx0 = E.cx;
  undo_pend.cy0 = E.cy;
  undo_rows_before = E.numrows;
  undo_pending = 1;
}

/* Call after it: files the step. A new change gives up on whatever was waiting
 * to be redone. */
void editorUndoCommit(void) {
  if (!undo_pending) return;
  undo_pending = 0;
  undo_pend.nnew = undo_pend.nold + (E.numrows - undo_rows_before);
  undo_pend.cx1 = E.cx;
  undo_pend.cy1 = E.cy;
  if (redo_count > 0) {
    while (redo_count > 0) editorFreeRec(&redo_stack[--redo_count]);
  }
  editorPushUndo(undo_pend);
  undo_run = 0;
  editorUpdateDirty();
}

/* Swap what the record holds with what is in the buffer now. */
static void editorRecApply(editorRec *r) {
  editorRec cur;
  int oldn = r->nold, t;
  if (r->at > E.numrows) r->at = E.numrows;
  if (r->at + r->nnew > E.numrows) r->nnew = E.numrows - r->at;
  editorCaptureRows(&cur, r->at, r->nnew);
  editorReplaceRows(r->at, r->nnew, r->old, r->osize, r->nold);
  editorFreeRec(r);
  r->old = cur.old;
  r->osize = cur.osize;
  r->nold = cur.nold;
  r->nnew = oldn;
  E.cx = r->cx0;
  E.cy = r->cy0;
  t = r->cx0; r->cx0 = r->cx1; r->cx1 = t;
  t = r->cy0; r->cy0 = r->cy1; r->cy1 = t;
  editorClampCursor();
}

void editorUndo(void) {
  if (undo_count == 0) {
    editorSetStatusMessage("Nothing to undo");
    return;
  }
  editorRec r = undo_stack[--undo_count];
  editorRecApply(&r);
  editorPushRedo(r);
  undo_run = 0;
  editorUpdateDirty();
}

void editorRedo(void) {
  if (redo_count == 0) {
    editorSetStatusMessage("Nothing to redo");
    return;
  }
  editorRec r = redo_stack[--redo_count];
  editorRecApply(&r);
  editorPushUndo(r);
  undo_run = 0;
  editorUpdateDirty();
}

/*** selection ***/

static void editorClampPos(int *y, int *x) {
  if (E.numrows == 0) { *y = 0; *x = 0; return; }
  if (*y < 0) *y = 0;
  if (*y >= E.numrows) { *y = E.numrows - 1; *x = E.row[*y].size; }
  if (*x > E.row[*y].size) *x = E.row[*y].size;
  if (*x < 0) *x = 0;
}

/* The selection runs from the anchor to the cursor, whichever way round the two
 * of them happen to be. */
void editorSelectionRange(int *sy, int *sx, int *ey, int *ex) {
  int forward = E.sel_cy < E.cy || (E.sel_cy == E.cy && E.sel_cx <= E.cx);
  *sy = forward ? E.sel_cy : E.cy;
  *sx = forward ? E.sel_cx : E.cx;
  *ey = forward ? E.cy : E.sel_cy;
  *ex = forward ? E.cx : E.sel_cx;
  editorClampPos(sy, sx);
  editorClampPos(ey, ex);
}

/* Which part of a row the selection covers, in bytes. */
int editorRowSelection(erow *row, int filerow, int *from, int *to) {
  int sy, sx, ey, ex;
  if (!E.sel) return 0;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  if (filerow < sy || filerow > ey) return 0;
  *from = filerow == sy ? sx : 0;
  *to = filerow == ey ? ex : row->size;
  return 1;
}

/* Put the cursor at the very start or the very end of the file. */
void editorMoveToEnd(int dir) {
  if (dir > 0) {
    E.cy = E.numrows > 0 ? E.numrows - 1 : 0;
    E.cx = E.numrows > 0 ? E.row[E.cy].size : 0;
  } else {
    E.cy = 0;
    E.cx = 0;
  }
}

/* True when the selection covers the whole buffer - the one case where a word
 * motion goes the whole way instead. */
int editorAllSelected(void) {
  int sy, sx, ey, ex;
  if (!E.sel) return 0;
  if (E.numrows == 0) return 1;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  return sy == 0 && sx == 0 && ey == E.numrows - 1 && ex == E.row[ey].size;
}

void editorSelectAll(void) {
  E.sel_cy = 0;
  E.sel_cx = 0;
  E.sel = 1;
  editorMoveToEnd(1);
}

/* The selected text exactly as it sits on disk: rows joined by newlines, and
 * nothing added at either end. */
char *editorSelectionToString(int *len) {
  int sy, sx, ey, ex, y, total = 0, at = 0;
  char *buf;
  if (E.numrows == 0) { *len = 0; return xmalloc(1); }
  editorSelectionRange(&sy, &sx, &ey, &ex);

  for (y = sy; y <= ey; y++) {
    erow *row = &E.row[y];
    int from = y == sy ? sx : 0;
    int to = y == ey ? ex : row->size;
    if (to > from) total += to - from;
    if (y < ey) total++;
  }

  buf = xmalloc(total + 1);
  for (y = sy; y <= ey; y++) {
    erow *row = &E.row[y];
    int from = y == sy ? sx : 0;
    int to = y == ey ? ex : row->size;
    if (to > from) {
      memcpy(buf + at, row->chars + from, to - from);
      at += to - from;
    }
    if (y < ey) buf[at++] = '\n';
  }

  *len = at;
  return buf;
}

/*** editor operations ***/

/* The raw mutators below change the buffer and nothing else. The public ones
 * wrap them in an undo step. */

/* Wipe out the selected text and leave the cursor at its start. */
static void editorRawDeleteSelection(void) {
  int sy, sx, ey, ex;
  editorSelectionRange(&sy, &sx, &ey, &ex);
  E.sel = 0;
  if (E.numrows == 0) return;

  erow *first = &E.row[sy];
  if (sy == ey) {
    if (ex > sx) editorRowDeleteRange(first, sx, ex);
  } else {
    /* the tail of the last row carries the first one, the rest goes away */
    erow *last = &E.row[ey];
    int tail = last->size - ex, y;
    first->chars = xrealloc(first->chars, sx + tail + 1);
    memcpy(&first->chars[sx], &last->chars[ex], tail);
    first->size = sx + tail;
    first->chars[first->size] = '\0';
    for (y = ey; y > sy; y--) editorDelRow(y);
  }
  E.cy = sy;
  E.cx = sx;
}

static void editorRawInsertBytes(const char *s, int n) {
  while (E.numrows <= E.cy) editorInsertRow(E.numrows, "", 0);
  if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
  editorRowInsertBytes(&E.row[E.cy], E.cx, s, n);
  E.cx += n;
}

static void editorRawNewline(void) {
  while (E.numrows <= E.cy) editorInsertRow(E.numrows, "", 0);
  erow *row = &E.row[E.cy];
  if (E.cx > row->size) E.cx = row->size;
  editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
  row = &E.row[E.cy];
  row->size = E.cx;
  row->chars[row->size] = '\0';
  E.cy++;
  E.cx = 0;
}

void editorDeleteSelection(void) {
  int sy, sx, ey, ex;
  if (E.numrows == 0) { E.sel = 0; return; }
  editorSelectionRange(&sy, &sx, &ey, &ex);
  editorUndoBegin(sy, ey - sy + 1);
  editorRawDeleteSelection();
  editorUndoCommit();
}

/* Type some bytes (one character, usually). Typing over a selection replaces
 * it in one step; plain typing sticks together as one undo step, not one per
 * keypress. */
void editorTypeBytes(const char *s, int n) {
  int sy, sx, ey, ex;
  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
    editorRawInsertBytes(s, n);
    editorUndoCommit();
    return;
  }
  E.sel = 0;
  if (undo_run && undo_count > 0 && E.cy == undo_run_cy && E.cx == undo_run_cx) {
    editorRawInsertBytes(s, n);
    undo_stack[undo_count - 1].cx1 = E.cx;
    undo_stack[undo_count - 1].cy1 = E.cy;
  } else {
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
    editorRawInsertBytes(s, n);
    editorUndoCommit();
    undo_run = 1;
  }
  undo_run_cy = E.cy;
  undo_run_cx = E.cx;
}

void editorInsertNewline(void) {
  int sy, sx, ey, ex;
  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
  } else {
    E.sel = 0;
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
  }
  editorRawNewline();
  editorUndoCommit();
}

/* Backspace (forward = 0) or Delete (forward = 1): one whole character, so a
 * multi-byte one is never left in halves. At a line edge it joins two lines. */
void editorDeleteChar(int forward) {
  if (E.numrows == 0) return;
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  erow *row = &E.row[E.cy];
  if (E.cx > row->size) E.cx = row->size;
  if (!forward) {
    if (E.cx > 0) {
      int p = editorPrevChar(row, E.cx);
      editorUndoBegin(E.cy, 1);
      editorRowDeleteRange(row, p, E.cx);
      E.cx = p;
      editorUndoCommit();
    } else if (E.cy > 0) {
      editorUndoBegin(E.cy - 1, 2);
      E.cx = E.row[E.cy - 1].size;
      editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
      editorDelRow(E.cy);
      E.cy--;
      editorUndoCommit();
    }
  } else {
    if (E.cx < row->size) {
      int e = editorCharEnd(row, E.cx);
      editorUndoBegin(E.cy, 1);
      editorRowDeleteRange(row, E.cx, e);
      editorUndoCommit();
    } else if (E.cy + 1 < E.numrows) {
      editorUndoBegin(E.cy, 2);
      editorRowAppendString(row, E.row[E.cy + 1].chars, E.row[E.cy + 1].size);
      editorDelRow(E.cy + 1);
      editorUndoCommit();
    }
  }
}

/* Delete the whole line the cursor sits on. */
void editorDeleteLine(void) {
  if (E.numrows == 0) return; /* nothing on the page yet */
  if (E.cy >= E.numrows) E.cy = E.numrows - 1;
  editorUndoBegin(E.cy, 1);
  editorDelRow(E.cy);
  if (E.numrows == 0) {
    E.cx = E.cy = 0;
  } else {
    if (E.cy >= E.numrows) E.cy = E.numrows - 1;
    E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
  }
  editorUndoCommit();
}

/* Put text in, as one undo step: the way a paste lands. Line endings are
 * normalised to \n and stray control characters dropped. */
static void editorInsertText(const char *s, int n) {
  int i, m = 0, start = 0, sy, sx, ey, ex;
  char *clean;
  if (n <= 0) return;
  clean = xmalloc(n + 1);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\r') {
      clean[m++] = '\n';
      if (i + 1 < n && s[i + 1] == '\n') i++;
    } else if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) {
      clean[m++] = (char)c;
    }
  }
  if (m == 0) { free(clean); return; }

  if (E.sel && E.numrows > 0) {
    editorSelectionRange(&sy, &sx, &ey, &ex);
    editorUndoBegin(sy, ey - sy + 1);
    editorRawDeleteSelection();
  } else {
    E.sel = 0;
    editorUndoBegin(E.cy, E.cy < E.numrows ? 1 : 0);
  }
  for (i = 0; i <= m; i++) {
    if (i == m || clean[i] == '\n') {
      if (i > start) editorRawInsertBytes(clean + start, i - start);
      if (i < m) editorRawNewline();
      start = i + 1;
    }
  }
  editorUndoCommit();
  free(clean);
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + (E.crlf ? 2 : 1);
  *buflen = totlen;
  char *buf = xmalloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    if (E.crlf) *p++ = '\r';
    *p++ = '\n';
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  if (E.filename == NULL) die("strdup");

  struct stat sb;
  if (stat(filename, &sb) == 0 && S_ISDIR(sb.st_mode)) {
    errno = EISDIR;
    die(filename);
  }

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    if (errno == ENOENT) return; /* a new file: start empty, keep the name */
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int crlf_lines = 0, lf_lines = 0;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen > 0 && line[linelen - 1] == '\n') {
      if (linelen > 1 && line[linelen - 2] == '\r') crlf_lines++;
      else lf_lines++;
    }
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.crlf = crlf_lines > lf_lines; /* whichever the file mostly uses */
  editorSnapshot(); /* this content is what the file holds */
  E.dirty = 0;
}

/* write() can stop short or be interrupted by a signal. */
int editorWriteAll(int fd, const char *buf, int len) {
  int written = 0;
  while (written < len) {
    ssize_t n = write(fd, buf + written, len - written);
    if (n == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    written += (int)n;
  }
  return 0;
}

/* Save without ever leaving a half-written file: write a temp file next to
 * the real one, then rename it over the top, so a full disk or a crash leaves
 * the old file whole. A symlink stays a symlink and the mode and owner carry
 * over. Where a temp file can't be made (a read-only directory, a device),
 * fall back to writing in place - the data first, the truncate after. */
static int editorWriteFile(const char *name, const char *buf, int len) {
  char *real = realpath(name, NULL);
  const char *path = real ? real : name;
  struct stat st;
  int have = stat(path, &st) == 0;
  int rc = -1, err = 0, fd;

  if (!have || S_ISREG(st.st_mode)) {
    size_t n = strlen(path) + 32;
    char *tmp = xmalloc(n);
    snprintf(tmp, n, "%s.oshi-tmp-%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd != -1) {
      if (have) {
        int r = fchmod(fd, st.st_mode & 07777);
        (void)r;
        r = fchown(fd, st.st_uid, st.st_gid);
        (void)r;
      }
      if (editorWriteAll(fd, buf, len) == 0 && fsync(fd) == 0) rc = 0;
      else err = errno;
      if (close(fd) == -1 && rc == 0) { rc = -1; err = errno; }
      if (rc == 0 && rename(tmp, path) == -1) { rc = -1; err = errno; }
      if (rc == -1) unlink(tmp);
      free(tmp);
      free(real);
      errno = err;
      return rc;
    }
    free(tmp);
  }

  fd = open(path, O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    err = errno;
    free(real);
    errno = err;
    return -1;
  }
  if (editorWriteAll(fd, buf, len) == 0 && ftruncate(fd, len) == 0) rc = 0;
  else err = errno;
  if (close(fd) == -1 && rc == 0) { rc = -1; err = errno; }
  free(real);
  errno = err;
  return rc;
}

/* The buffer now matches the file on disk: remember its bytes as what's on
 * disk, and let the next typing start a fresh step. */
static void editorMarkSaved(void) {
  editorSnapshot();
  undo_run = 0; /* typing after a save starts a new step */
  editorUpdateDirty();
}

void editorSave(void) {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s ", NULL, NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);
  int rc = editorWriteFile(E.filename, buf, len);
  int err = errno;
  if (rc == 0) {
    free(buf);
    editorMarkSaved();
    editorSetStatusMessage("%d bytes written to disk", len);
    return;
  }
  /* A refused write is the one failure we can do something about: offer to
   * redo it as root. Everything else (a full disk, a bad path) is just an
   * error. From here the box owns the messaging, saved or not. */
  if (err == EACCES || err == EPERM) {
    if (editorSaveElevated(buf, len) == 0) editorMarkSaved();
    free(buf);
    return;
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(err));
}

/*** clipboard ***/

/* Put text in the terminal's clipboard, the way every terminal app does it:
 * OSC 52. One sequence, not pieces: rio and foot both decode each sequence on
 * its own, so a split payload would leave only the last chunk behind. */
void editorClipboardPut(const char *buf, int len) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  char *b64 = xmalloc(4 * ((len + 2) / 3) + 1);

  int i, out = 0;
  for (i = 0; i + 2 < len; i += 3) {
    unsigned int n = ((unsigned char)buf[i] << 16) |
                     ((unsigned char)buf[i + 1] << 8) |
                      (unsigned char)buf[i + 2];
    b64[out++] = table[(n >> 18) & 63];
    b64[out++] = table[(n >> 12) & 63];
    b64[out++] = table[(n >> 6) & 63];
    b64[out++] = table[n & 63];
  }
  if (i < len) { /* one or two bytes left, padded with '=' */
    unsigned int n = (unsigned char)buf[i] << 16;
    b64[out++] = table[(n >> 18) & 63];
    if (i + 1 < len) {
      n |= (unsigned char)buf[i + 1] << 8;
      b64[out++] = table[(n >> 12) & 63];
      b64[out++] = table[(n >> 6) & 63];
      b64[out++] = '=';
    } else {
      b64[out++] = table[(n >> 12) & 63];
      b64[out++] = '=';
      b64[out++] = '=';
    }
  }

  editorWriteAll(STDOUT_FILENO, "\x1b]52;c;", 7);
  editorWriteAll(STDOUT_FILENO, b64, out);
  editorWriteAll(STDOUT_FILENO, "\x07", 1);

  free(b64);
}

/* Try an external clipboard tool when OSC 52 would be too big to trust:
 * wl-copy (Wayland), xclip/xsel (X11), pbcopy (macOS), clip.exe (WSL).
 * Their output is thrown away: this runs on the alt screen, so even sh's
 * "command not found" must not paint over the frame.
 * Returns 0 on success, -1 if none worked. */
static int editorClipboardExternal(const char *text, int len) {
  static const char *cmds[] = {
    "wl-copy",
    "xclip -selection clipboard",
    "xsel --clipboard --input",
    "pbcopy",
    "clip.exe",
    NULL
  };
  int i;
  for (i = 0; cmds[i]; i++) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>/dev/null", cmds[i]);
    FILE *fp = popen(cmd, "w");
    if (!fp) continue;
    size_t written = fwrite(text, 1, len, fp);
    int rc = pclose(fp);
    if (written == (size_t)len && rc == 0) return 0;
  }
  return -1;
}

/* What a copy grabs: the selection, else the line under the cursor (with its
 * newline). Returns malloc'd text, or NULL when there is nothing. */
static char *editorCopyGrab(int *len, int *isline) {
  if (E.sel) {
    *isline = 0;
    return editorSelectionToString(len);
  }
  if (E.numrows == 0) return NULL;
  erow *row = &E.row[E.cy < E.numrows ? E.cy : E.numrows - 1];
  char *text = xmalloc(row->size + 2);
  memcpy(text, row->chars, row->size);
  text[row->size] = '\n';
  *len = row->size + 1;
  *isline = 1;
  return text;
}

/* Keep our own copy (so ctrl-v works even where the terminal ignores OSC 52,
 * or the text is too big for it) and hand the terminal clipboard the same. */
static void editorClipSet(char *text, int len, int isline, const char *verb) {
  int n = editorLineCount(text, len);
  free(E.clip);
  E.clip = text;
  E.clip_len = len;
  E.clip_line = isline;
  if (len <= OSHI_OSC52_MAX) {
    editorClipboardPut(text, len);
    editorSetStatusMessage("%s %d line%s", verb, n, n == 1 ? "" : "s");
  } else if (editorClipboardExternal(text, len) == 0) {
    editorSetStatusMessage("%s %d lines", verb, n);
  } else {
    /* worded to fit: the bar only leaves ~50 columns for this on an 80-col
     * terminal (the original wording was 79 and was always cut off) */
    editorSetStatusMessage("%s %d lines: clipboard failed, ctrl-v ok", verb, n);
  }
}

void editorCopySelection(void) {
  int len = 0, isline = 0;
  char *text = editorCopyGrab(&len, &isline);
  if (text == NULL || len == 0) {
    free(text);
    editorSetStatusMessage("Nothing to copy");
  } else {
    editorClipSet(text, len, isline, "Copied");
  }
  /* the selection stays up: the next ordinary key gives it up, the way the
   * key dispatcher's comment promises (cut still clears it - the text is
   * gone, so there is nothing left to highlight) */
}

void editorCut(void) {
  int len = 0, isline = 0;
  char *text = editorCopyGrab(&len, &isline);
  if (text == NULL || len == 0) {
    free(text);
    editorSetStatusMessage("Nothing to cut");
    E.sel = 0;
    return;
  }
  editorClipSet(text, len, isline, "Cut");
  if (isline) editorDeleteLine();
  else editorDeleteSelection();
}

/* ctrl-v: what oshi last copied or cut. (Text copied elsewhere goes in with
 * the terminal's own paste, which arrives as a bracketed paste.) */
void editorPasteInternal(void) {
  if (E.clip == NULL || E.clip_len == 0) {
    editorSetStatusMessage("Nothing to paste");
    return;
  }
  /* Same wording and line count as copy/cut use: the whole clip, newline and all */
  int n = editorLineCount(E.clip, E.clip_len);
  if (E.clip_line && !E.sel) { /* a whole line goes in above this one */
    int at = E.cy < E.numrows ? E.cy : 0;
    int had_rows = E.numrows > 0;
    editorUndoBegin(at, 0);
    editorInsertRow(at, E.clip, E.clip_len - 1);
    if (had_rows) E.cy++; /* the cursor stays on the line it was on */
    editorUndoCommit();
  } else {
    editorInsertText(E.clip, E.clip_len);
  }
  editorSetStatusMessage("Pasted %d line%s", n, n == 1 ? "" : "s");
}

/*** find ***/

/* Case-insensitive prefix compare of n bytes (so Replace matches Find's
 * case-insensitivity without pulling in strcasecmp on a partial). */
int editorIcmp(const char *s1, const char *s2, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    if (tolower((unsigned char)s1[i]) != tolower((unsigned char)s2[i])) return 0;
  return 1;
}

/* Case-insensitive strstr (strcasestr is a GNU symbol and -pedantic grumbles at
 * it). Returns a pointer into `hay` where `needle` first matches ignoring case,
 * or NULL. */
char *editorIstrstr(const char *hay, const char *needle) {
  size_t hl = strlen(hay), nl = strlen(needle);
  size_t i;
  for (i = 0; i + nl <= hl; i++) {
    size_t j;
    for (j = 0; j < nl; j++)
      if (tolower((unsigned char)hay[i + j]) !=
          tolower((unsigned char)needle[j])) break;
    if (j == nl) return (char *)hay + i;
  }
  return NULL;
}

/* What the prompt is hunting for, so the rows can light the matches up.
 * Matches are byte offsets into a row's text. */
static char find_q[256];
static int find_on;
static int find_row = -1;
static int find_col;
static long find_time; /* ms: when the match counter last changed */

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  snprintf(find_q, sizeof(find_q), "%s", query);
  find_on = query[0] != '\0';

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    find_on = 0;
    /* keep find_row/find_col: the Enter/Esc that ends the prompt still leaves the
     * caret on the last hit so editorFind can keep the search live + show its
     * place (cur/total). The live highlight only survives if editorFind re-enables
     * find_on after the prompt. */
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
  if (!query[0]) { /* nothing typed yet: sit still instead of leaping to the top */
    last_match = -1;
    direction = 1;
    return;
  }
  find_row = -1; /* (re)scan: drop any stale position before re-searching */
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;
    erow *row = &E.row[current];
    char *match = editorIstrstr(row->chars, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = (int)(match - row->chars);
      find_row = current;
      find_col = E.cx;
      break;
    }
  }
}

/* During the prompt the bar stays clean -- "Find: <query>" with no counter
 * cluttering the typing area. Once a search lands, the counter ("<cur>/<total>")
 * lives on the right of the bar (see editorDrawStatusBar) and Ctrl-N/Ctrl-P
 * step through the hits. */
void editorFindStatus(char *query) {
  (void)query;
}

/* Count matches of find_q. *cur gets the 1-based index of the hit sitting on
 * (find_row, find_col) -- i.e. "where I am" in the x/y -- and the total is
 * returned. */
int editorFindCount(int *cur) {
  int total = 0, before = 0, y;
  *cur = 0;
  if (!find_on || !find_q[0] || E.numrows == 0) return 0;
  size_t qlen = strlen(find_q);
  for (y = 0; y < E.numrows; y++) {
    erow *row = &E.row[y];
    char *p = row->chars;
    char *m;
    while ((m = editorIstrstr(p, find_q)) != NULL) {
      total++;
      if (y < find_row || (y == find_row && (m - row->chars) < find_col)) before++;
      p = m + qlen;
    }
  }
  if (find_row >= 0) *cur = before + 1;
  return total;
}

void editorFind(void) {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;
  char *query = editorPrompt("Find: %s", editorFindCallback, editorFindStatus);
  if (query) {
    /* Enter: leave the search live. Matches stay underlined, the caret rests on
     * the current hit, and the bar reads "<file>  <cur>/<total>" -- Ctrl-N/Ctrl-P
     * (or the prompt's own left/right) walk the rest. */
    find_on = 1;
    find_time = editorNowMs(); /* the counter is a toast: it starts now */
    free(query);
  } else {
    /* Esc/cancel: clear the search and its highlights. */
    find_on = 0;
    find_q[0] = '\0';
    find_row = -1;
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/* Step to the next (dir > 0) or previous (dir < 0) match of find_q, wrapping
 * around the buffer. Updates the caret, find_row/find_col, and scrolls to keep
 * the hit visible. The bar's "cur/total" counter follows automatically. */
int editorFindNextMatch(int dir) {
  if (!find_on || !find_q[0] || E.numrows == 0) return 0;
  find_time = editorNowMs(); /* stepping through hits re-ups the counter */
  size_t qlen = strlen(find_q);
  int fy = -1, fcol = -1;   /* first match (wrap target) */
  int ly = -1, lcol = -1;   /* last match  (wrap target) */
  int ay = -1, acol = -1;   /* first hit strictly after the current one */
  int by = -1, bcol = -1;   /* last hit strictly before the current one */
  int y;
  for (y = 0; y < E.numrows; y++) {
    erow *row = &E.row[y];
    char *p = row->chars;
    char *m;
    while ((m = editorIstrstr(p, find_q)) != NULL) {
      int col = (int)(m - row->chars);
      if (fy < 0) { fy = y; fcol = col; }
      ly = y; lcol = col;
      if (y > find_row || (y == find_row && col > find_col)) {
        if (ay < 0) { ay = y; acol = col; }
      }
      if (y < find_row || (y == find_row && col < find_col)) {
        by = y; bcol = col;
      }
      p = m + qlen;
    }
  }
  int ny, nx;
  if (dir > 0) {
    if (ay >= 0) { ny = ay; nx = acol; }
    else if (fy >= 0) { ny = fy; nx = fcol; }   /* wrapped */
    else return 0;
  } else {
    if (by >= 0) { ny = by; nx = bcol; }
    else if (ly >= 0) { ny = ly; nx = lcol; }   /* wrapped */
    else return 0;
  }
  find_row = ny;
  find_col = nx;
  E.cy = ny;
  E.cx = nx;
  editorScroll();
  editorRefreshScreen();
  return 1;
}

/*** replace ***/

/* The next occurrence of `query` at or after (srow,scol), searching to the end
 * of the buffer and stopping there (no wrapping: a wrap would revisit text
 * already replaced, and could go on forever). 1 on match, else 0. */
int editorFindNextReplace(char *query, int srow, int scol, int *fr, int *fc) {
  int qlen = (int)strlen(query);
  int y, c = scol;
  if (qlen == 0 || srow < 0) return 0;
  for (y = srow; y < E.numrows; y++, c = 0) {
    erow *row = &E.row[y];
    int cc;
    for (cc = c; cc + qlen <= row->size; cc++) {
      if (editorIcmp(row->chars + cc, query, qlen)) {
        *fr = y;
        *fc = cc;
        return 1;
      }
    }
  }
  return 0;
}

/* Prompt for "find" and "replace", then walk every match offering to swap it.
 * The whole session is one undo step. */
void editorReplace(void) {
  int scx = E.cx, scy = E.cy;
  char *find = editorPrompt("Replace find: %s", editorFindCallback, editorFindStatus);
  find_on = 0;
  if (!find) { E.cx = scx; E.cy = scy; return; } /* esc */
  if (!find[0]) { free(find); editorSetStatusMessage(""); return; }

  char *repl = editorPrompt("Replace: %s", NULL, NULL);
  if (!repl) {
    E.cx = scx; E.cy = scy;
    editorSetStatusMessage("Replace aborted");
    free(find);
    return;
  }

  int qlen = (int)strlen(find);
  int rlen = (int)strlen(repl);
  int replace_all = 0;
  int replaced = 0;
  int begun = 0;
  int row = E.cy, col = E.cx;

  for (;;) {
    int fr, fc;
    if (!editorFindNextReplace(find, row, col, &fr, &fc)) {
      if (replaced == 0)
        editorSetStatusMessage("Replace: no matches found");
      else
        editorSetStatusMessage("Replace: %d occurrence(s) swapped", replaced);
      break;
    }
    E.cy = fr;
    E.cx = fc;
    if (!replace_all) {
      int k;
      editorSetStatusMessage("Replace? (y)es (n)o (a)ll (r)est (esc): ");
      do {
        editorRefreshScreen();
        k = editorReadKey();
        if (k == PASTE_KEY) editorSkipPaste();
      } while (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY);
      if (k == 'a' || k == 'A' || k == 'r' || k == 'R') {
        replace_all = 1; /* fall through to the replacement below */
      } else if (k == 'n' || k == 'N') {
        row = fr;
        col = fc + qlen; /* skip past this occurrence */
        continue;
      } else if (k != 'y' && k != 'Y') {
        editorSetStatusMessage("Replace canceled");
        break;
      }
    }

    /* swap the match for the replacement */
    if (!begun) {
      editorUndoBegin(0, E.numrows);
      begun = 1;
    }
    editorRowDeleteRange(&E.row[fr], fc, fc + qlen);
    editorRowInsertBytes(&E.row[fr], fc, repl, rlen);
    /* keep searching just past the text we just wrote */
    row = fr;
    col = fc + rlen;
    E.cx = col;
    replaced++;
  }

  if (begun) editorUndoCommit();
  free(find);
  free(repl);
  editorClampCursor();
  editorRefreshScreen();
}

/*** goto ***/

/* ctrl-g: N goes to line N, +N / -N move by that many, N:C also picks a column. */
void editorGoto(void) {
  char *q = editorPrompt("Goto: %s", NULL, NULL);
  if (!q) return;
  char *end;
  long n = strtol(q, &end, 10);
  if (end == q) {
    editorSetStatusMessage("Not a line number");
    free(q);
    return;
  }
  int col = 0;
  if (*end == ':') col = atoi(end + 1);
  if (q[0] == '+' || q[0] == '-') n = E.cy + 1 + n; /* relative to here */
  free(q);
  if (E.numrows == 0) return;
  if (n < 1) n = 1;
  if (n > E.numrows) n = E.numrows;
  E.cy = (int)n - 1;
  E.cx = col > 1 ? editorRowRxToCx(&E.row[E.cy], col - 1) : 0;
  E.rowoff = E.cy - E.screenrows / 2; /* put the line mid-screen... */
  if (E.rowoff > E.numrows + 1 - E.screenrows) /* ...but not past the end */
    E.rowoff = E.numrows + 1 - E.screenrows;
  if (E.rowoff < 0) E.rowoff = 0;
  editorSetStatusMessage("Line %ld of %d", n, E.numrows);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  if (ab->len + len > ab->cap) {
    int cap = ab->cap ? ab->cap * 2 : 4096;
    while (cap < ab->len + len) cap *= 2;
    ab->b = xrealloc(ab->b, cap);
    ab->cap = cap;
  }
  if (len > 0) memcpy(&ab->b[ab->len], s, len);
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

int editorGutterWidth(void) {
  if (!E.ruler) return 0;
  int digits = 1; /* how many digits the line count needs */
  int n = E.numrows;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  int w = E.ruler_width > 0 ? E.ruler_width : digits;
  if (w < digits) w = digits; /* a fixed width must still fit the numbers */
  if (w > 64) w = 64;
  if (w + 2 > E.screencols) w = E.screencols - 2;
  if (w < 1) return 0; /* no room for a gutter at all */
  return w + 1; /* number field + one separator space */
}

void editorScroll(void) {
  int width = E.screencols - E.gutter;
  if (width < 1) width = 1; /* keep at least one text column */

  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  /* editorDrawRowText reserves a column on whichever side(s) need a '<' or
   * '>' scroll mark, so the cursor's own column must stay clear of both
   * edges - one column in from each - or it can land in a reserved column
   * and never get drawn. Reserving unconditionally (rather than only when a
   * mark actually shows) costs at most 2 of the available columns but
   * keeps this independent of editorDrawRowText's own per-row decision. */
  int inner = width > 2 ? width - 2 : 1;
  if (E.rx < E.coloff + 1) {
    E.coloff = E.rx > 0 ? E.rx - 1 : 0;
  }
  if (E.rx > E.coloff + inner) {
    E.coloff = E.rx - inner;
  }
  if (E.coloff < 0) E.coloff = 0;
}

/* Paint one cell with the block caret: a reverse-video cell. (The beam cursor
 * is the terminal's own thin bar and is positioned separately, so this only
 * runs for block.) `ch` is ' ' when the caret hangs in whitespace. */
void editorDrawCursorCell(struct abuf *ab, char ch) {
  abAppend(ab, "\x1b[7m", 4);
  abAppend(ab, &ch, 1);
  abAppend(ab, "\x1b[27m", 5);
}

/* < and > at the edges of the text area: this line keeps going off-screen in
 * that direction. Each one, when shown, claims one column of `avail` for
 * itself, so the actual text is drawn one narrower on that side. Drawn as a
 * highlighted chip (its own background color) so it reads as a UI mark
 * rather than as more buffer text. */
static void editorDrawScrollMark(struct abuf *ab, char mark) {
  abAppend(ab, E.col_str[COL_SCROLLMARK_BG],
           (int)strlen(E.col_str[COL_SCROLLMARK_BG]));
  abAppend(ab, E.col_str[COL_SCROLLMARK_FG],
           (int)strlen(E.col_str[COL_SCROLLMARK_FG]));
  abAppend(ab, &mark, 1);
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, E.col_str[COL_TEXT_FG], (int)strlen(E.col_str[COL_TEXT_FG]));
}

static void editorDrawRowText(struct abuf *ab, erow *row, int filerow, int avail) {
  int sel_from = 0, sel_to = 0, sel_eol = 0;
  int sel_on = editorRowSelection(row, filerow, &sel_from, &sel_to);
  if (sel_on) {
    int sy, sx, ey, ex;
    editorSelectionRange(&sy, &sx, &ey, &ex);
    /* the selection keeps going below this row, so this row's newline is in
     * it too - the byte the per-cell loop above can never reach */
    sel_eol = filerow < ey;
  }
  int block = filerow == E.cy && !E.in_prompt && E.cursor_style == 2;
  int qlen = (find_on && find_q[0]) ? (int)strlen(find_q) : 0;
  char *m = qlen ? editorIstrstr(row->chars, find_q) : NULL;

  int roww = row->size > 0 ? editorRowCxToRx(row, row->size) : 0;
  int has_right = avail > 0 && roww > E.coloff + avail;
  int has_left = E.coloff > 0 && roww > 0;
  int left = E.coloff + (has_left ? 1 : 0);   /* first column of real text */
  int right = E.coloff + avail - (has_right ? 1 : 0); /* one past the last */
  if (right < left) right = left;
  int col = 0, b = 0;

  if (has_left) editorDrawScrollMark(ab, '<');

  while (b < row->size) {
    int e = editorCharEnd(row, b);
    int w = editorCellWidth(row, b, col);
    if (col >= right) break;
    if (col + w > left) { /* at least part of it is on screen */
      int in_sel, on_cur, in_match, rev;
      while (m && (m - row->chars) + qlen <= b)
        m = editorIstrstr(m + qlen, find_q);
      in_sel = sel_on && b >= sel_from && b < sel_to;
      on_cur = block && b == E.cx;
      in_match = qlen && m && b >= (m - row->chars) &&
                 b < (m - row->chars) + qlen;
      rev = in_sel || on_cur;

      if (rev) abAppend(ab, "\x1b[7m", 4);
      else if (in_match) abAppend(ab, "\x1b[4m", 4);

      if (row->chars[b] == '\t' || col < left || col + w > right) {
        /* a tab, or a wide character cut by the edge: paint just the cells
         * that are showing, as spaces */
        int k;
        for (k = 0; k < w; k++)
          if (col + k >= left && col + k < right) abAppend(ab, " ", 1);
      } else if ((unsigned char)row->chars[b] < 32 ||
                 (unsigned char)row->chars[b] == 127) {
        abAppend(ab, "?", 1); /* a control byte must not reach the terminal */
      } else {
        int len, cp = utf8Decode((const unsigned char *)row->chars + b,
                                 row->size - b, &len);
        if (cp >= 0x80 && cp < 0xA0)
          abAppend(ab, "?", 1); /* C1 controls: some terminals obey them */
        else if (cp == 0xFFFD && len == 1)
          abAppend(ab, "\xEF\xBF\xBD", 3); /* not valid UTF-8: show the replacement mark */
        else
          abAppend(ab, row->chars + b, e - b);
      }

      if (rev) abAppend(ab, "\x1b[27m", 5);
      else if (in_match) abAppend(ab, "\x1b[24m", 5);
    }
    col += w;
    b = e;
  }
  /* the cell past the last character: the block caret at the end of the line
   * or on an empty row - and, whenever the selection swallows this row's
   * newline, its own stand-in there, so a selected empty line is visible */
  if ((sel_eol || (block && E.cx >= row->size)) && col >= left && col < right)
    editorDrawCursorCell(ab, ' ');

  if (has_right) editorDrawScrollMark(ab, '>');
}

void editorDrawRows(struct abuf *ab) {
  static erow blank = { 0, "" };
  int y;
  int avail = E.screencols - E.gutter;
  if (avail < 0) avail = 0;
  int nvis = E.numrows > 0 ? E.numrows : 1; /* an empty buffer still shows line 1 */
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow > nvis) {
      abAppend(ab, "\x1b[K", 3);
      abAppend(ab, "\r\n", 2);
      continue;
    }
    if (filerow == nvis) { /* the ~ fringe: the one row past the end */
      abAppend(ab, E.col_str[COL_TILDE_FG],
               (int)strlen(E.col_str[COL_TILDE_FG]));
      abAppend(ab, "~", 1);
      abAppend(ab, "\x1b[0m", 4);
      abAppend(ab, "\x1b[K", 3);
      abAppend(ab, "\r\n", 2);
      continue;
    }
    if (E.gutter > 0) {
      char num[80];
      int n = snprintf(num, sizeof(num), "%*d", E.gutter - 1, filerow + 1);
      int cur = filerow == E.cy;
      const char *bg = cur ? E.col_str[COL_GUTTER_CURSOR_BG] : E.col_str[COL_GUTTER_BG];
      const char *fg = cur ? E.col_str[COL_GUTTER_CURSOR_FG] : E.col_str[COL_GUTTER_FG];
      if (bg[0]) abAppend(ab, bg, (int)strlen(bg)); /* unset by default: no panel, just the terminal's own background */
      abAppend(ab, fg, (int)strlen(fg));
      abAppend(ab, num, n);
      abAppend(ab, " ", 1); /* the separator column too, so a set background reads as one panel */
      abAppend(ab, "\x1b[0m", 4);
    }
    /* text color is set once for the whole line; the plain cells inherit it,
     * the reverse-video cells (cursor + selection) invert it, and find-hits
     * underline it. */
    abAppend(ab, E.col_str[COL_TEXT_FG], (int)strlen(E.col_str[COL_TEXT_FG]));
    editorDrawRowText(ab, filerow < E.numrows ? &E.row[filerow] : &blank,
                      filerow, avail);
    abAppend(ab, "\x1b[0m", 4);
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, E.col_str[COL_STATUS_BG], (int)strlen(E.col_str[COL_STATUS_BG]));
  abAppend(ab, E.col_str[COL_STATUS_FG], (int)strlen(E.col_str[COL_STATUS_FG]));
  char name[512], left[1024], right[96];
  char suffix[300]; /* what trails the name: dirty flag and/or a notice */
  suffix[0] = '\0';
  const char *fn = E.filename ? E.filename : "[No Name]";
  int fnlen = (int)strlen(fn);

  int total = E.numrows > 0 ? E.numrows : 1;
  int cur = E.cy + 1 > total ? total : E.cy + 1;
  /* One unified bar that's never "removed", with a few faces:
   *  - in a prompt (Find/Replace/Save/Goto): the prompt text on the left (its
   *    end, if it is longer than the bar) and the file name on the right.
   *  - a live search after Enter: the file name on the left and the match index
   *    on the right, e.g. "oshi_test.txt  1/5".
   *  - otherwise: the file name on the left, line/column on the right, and any
   *    transient message ("saved", "Copied 7 lines") floats in between.
   *  - a pending quit confirm: "Makefile  Modified press ctrl-q again  1/1".
   *
   * Every one of those notices is a toast: it holds the bar for OSHI_STATUS_MS
   * and then the bar falls back to the file name and the line/column, whether
   * or not a key comes in (see the idle timer in editorReadKey). A prompt is
   * the one thing that stays, since it is input you are still in the middle
   * of rather than a message about something that already happened. */
  int prompt = E.in_prompt;
  long now = editorNowMs();
  int quit_shown = E.quit_confirm && now - E.quit_time < OSHI_STATUS_MS;
  int find_shown = find_on && find_q[0] && now - find_time < OSHI_STATUS_MS;
  int cfg_shown = E.config_err[0] && now - E.config_err_time < OSHI_STATUS_MS;
  int hasmsg = !quit_shown && !find_shown && !cfg_shown && E.statusmsg[0] &&
               (now - E.statusmsg_time < OSHI_STATUS_MS);
  long notice_t = -1; /* which notice this is, so editorReadKey can lapse it */
  int len = 0, rlen = 0, slen = 0;
  if (prompt) {
    /* The name rides on the right while you type: keep its tail -- the
     * basename -- and leave a margin on the bar for the prompt text. */
    int cap = E.screencols - 24; /* margin left for the prompt text */
    if (cap > 127) cap = 127;    /* widest UTF-8 spelling of name[], "<" included */
    if (cap < 9) cap = 9;        /* "<" plus a toehold */
    int skip = editorFitTail(fn, fnlen, cap - 1); /* -1: "<" costs a cell too */
    snprintf(name, sizeof(name), "%s%s", skip > 0 ? "<" : "", fn + skip);
    rlen = snprintf(right, sizeof(right), "%s", name);
    len = snprintf(left, sizeof(left), "%s", E.statusmsg);
  } else if (quit_shown) {
    char keyname[16];
    editorKeyName(E.binds[CMD_QUIT], keyname, sizeof(keyname));
    slen = snprintf(suffix, sizeof(suffix), "  Modified press %s again", keyname);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.quit_time;
  } else if (find_shown) {
    int current, found = editorFindCount(&current);
    slen = snprintf(suffix, sizeof(suffix), "%s", E.dirty ? " (modified)" : "");
    rlen = snprintf(right, sizeof(right), "%d/%d", current, found);
    notice_t = find_time;
  } else if (cfg_shown) {
    slen = snprintf(suffix, sizeof(suffix), "  %s", E.config_err);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.config_err_time;
  } else if (hasmsg) {
    slen = snprintf(suffix, sizeof(suffix), "%s  %s", E.dirty ? " (modified)" : "",
                    E.statusmsg);
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
    notice_t = E.statusmsg_time;
  } else {
    slen = snprintf(suffix, sizeof(suffix), "%s", E.dirty ? " (modified)" : "");
    rlen = snprintf(right, sizeof(right), "%d/%d", cur, E.rx + 1);
  }
  if (notice_t >= 0) {
    E.notice_pending = 1;
    E.notice_deadline = notice_t + OSHI_STATUS_MS;
  } else {
    E.notice_pending = 0; /* a prompt, or a plain bar: nothing to lapse */
  }
  /* snprintf reports what it wanted to write, not what fit */
  if (slen >= (int)sizeof(suffix)) slen = (int)sizeof(suffix) - 1;
  if (len >= (int)sizeof(left)) len = (int)sizeof(left) - 1;
  if (rlen >= (int)sizeof(right)) rlen = (int)sizeof(right) - 1;

  int rw = editorStrWidth(right, rlen);
  if (rw > E.screencols) {
    rlen = editorFitHead(right, rlen, E.screencols);
    rw = editorStrWidth(right, rlen);
  }
  int block = prompt && E.cursor_style == 2;
  int avail = E.screencols - rw - (block ? 1 : 0);
  if (avail < 0) avail = 0;
  if (!prompt) {
    /* The name gets everything the notice and the right side leave over, so a
     * long path can use a wide bar instead of stopping at 30 columns. What it
     * must give up it loses from the front ("<"), never the basename -- and
     * that "<" gets a cell out of the budget, so the finished name (marker
     * included) never overshoots. 16 is the floor under a long notice; 127 is
     * the widest UTF-8 spelling that fits name[]. */
    int budget = avail - editorStrWidth(suffix, slen);
    if (budget < 16) budget = 16;
    if (budget > 127) budget = 127;
    int skip = editorFitTail(fn, fnlen, budget - 1); /* -1: "<" costs a cell too */
    snprintf(name, sizeof(name), "%s%s", skip > 0 ? "<" : "", fn + skip);
    len = snprintf(left, sizeof(left), "%s%s", name, suffix);
    if (len >= (int)sizeof(left)) len = (int)sizeof(left) - 1;
  }
  const char *lp = left;
  if (editorStrWidth(left, len) > avail) {
    if (prompt) { /* keep the end: that's where you are typing */
      int s = editorFitTail(left, len, avail);
      lp = left + s;
      len -= s;
    } else {
      len = editorFitHead(left, len, avail);
    }
  }
  int lw = editorStrWidth(lp, len);
  abAppend(ab, lp, len);
  int used = lw;
  if (block) { /* block caret at end of the prompt text */
    editorDrawCursorCell(ab, ' ');
    used++;
  }
  E.prompt_col = lw + 1;
  for (; used < E.screencols - rw; used++)
    abAppend(ab, " ", 1);
  abAppend(ab, right, rlen);
  abAppend(ab, "\x1b[0m", 4);
}

void editorRefreshScreen(void) {
  E.gutter = editorGutterWidth();
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  if (E.redraw) { /* the window changed shape: start from a clean slate */
    abAppend(&ab, "\x1b[2J", 4);
    E.redraw = 0;
  }
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  /* One unified bar: file name (left), a transient message (middle), row/col
   * position (right) -- so it's never "removed". */
  editorDrawStatusBar(&ab);

  /* Block is painted by editorDrawRows (terminal cursor hidden). The beam
   * cursor is the terminal's own thin bar, so hand it a position and show it. */
  if (E.cursor_style == 6) {
    char buf[32];
    int row, col;
    if (E.in_prompt) {
      row = E.screenrows + 1;
      col = E.prompt_col;
      if (col > E.screencols) col = E.screencols;
    } else {
      row = E.cy - E.rowoff + 1;
      /* a '<' scroll mark, when shown, pushes the row's own text (and so
       * the cursor) one column to the right; it shows exactly when the
       * cursor's own row is scrolled - editorScroll() always keeps E.rx
       * within one line of E.coloff, so E.coloff > 0 here means this row
       * has one. */
      col = E.rx - E.coloff + E.gutter + 1;
      if (col < 1) col = 1;
    }
    snprintf(buf, sizeof(buf), "\x1b[?25h\x1b[%d;%dH", row, col);
    abAppend(&ab, buf, (int)strlen(buf));
  }

  ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
  (void)r;
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = editorNowMs();
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int),
                   void (*decorate)(char *)) {
  size_t bufsize = OSHI_PROMPT_MAX + 8;
  char *buf = xmalloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  E.in_prompt = 1;
  while (1) {
    editorSetStatusMessage(prompt, buf);
    if (decorate) decorate(buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == RESIZE_KEY || c == MOUSE_KEY || c == NOKEY) continue;
    if (c == DEL_KEY || c == BACKSPACE || c == 8) {
      if (buflen != 0) { /* a whole character, not one byte of it */
        do {
          buflen--;
        } while (buflen > 0 && ((unsigned char)buf[buflen] & 0xC0) == 0x80);
        buf[buflen] = '\0';
      }
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      E.in_prompt = 0;
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        E.in_prompt = 0;
        return buf;
      }
    } else if (c == UTF8_KEY) {
      if (buflen + E.utf8len <= OSHI_PROMPT_MAX) {
        memcpy(buf + buflen, E.utf8, E.utf8len);
        buflen += E.utf8len;
        buf[buflen] = '\0';
      }
    } else if (c == PASTE_KEY) { /* the first line of it, without control bytes */
      int n, i;
      char *t = editorReadPasteBuf(&n);
      for (i = 0; i < n && t[i] != '\n' && t[i] != '\r'; i++) {
        unsigned char u = (unsigned char)t[i];
        if (u < 32 || u == 127) continue;
        if (buflen + 1 > OSHI_PROMPT_MAX) break;
        buf[buflen++] = t[i];
      }
      buf[buflen] = '\0';
      free(t);
    } else if (c >= 32 && c < 127) {
      if (buflen + 1 <= OSHI_PROMPT_MAX) {
        buf[buflen++] = c;
        buf[buflen] = '\0';
      }
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (row && E.cx > 0) {
        E.cx = editorPrevChar(row, E.cx);
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx = editorCharEnd(row, E.cx);
      } else if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy > 0) {
        E.cy--;
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
      }
      break;
    case ARROW_DOWN:
      /* the last line is the last stop: the ~ below it is not a place */
      if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
      }
      break;
  }
}

/* Ctrl-Left and Ctrl-Right: back to the start of the previous word, or on to
 * the end of the next one. */
void editorMoveWord(int dir) {
  erow *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;
  int size = row ? row->size : 0;
  int i = E.cx;

  if (dir > 0) {
    if (i >= size) { /* at the end of the line: carry on below */
      if (E.cy + 1 < E.numrows) {
        E.cy++;
        E.cx = 0;
      }
      return;
    }
    while (i < size && !isspace((unsigned char)row->chars[i])) i++;
    while (i < size && isspace((unsigned char)row->chars[i])) i++;
  } else {
    if (i == 0) { /* at the start of the line: back up to the one above */
      if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      return;
    }
    while (i > 0 && isspace((unsigned char)row->chars[i - 1])) i--;
    while (i > 0 && !isspace((unsigned char)row->chars[i - 1])) i--;
  }
  E.cx = i;
}

/* Milliseconds from a clock that never jumps. */
long editorNowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The whitespace-delimited word under the cursor: where it starts and ends. */
void editorWordAt(int *from, int *to) {
  erow *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;
  int i, sp, a, b;
  if (!row || row->size == 0) {
    *from = *to = 0;
    return;
  }
  i = E.cx;
  if (i >= row->size) i = row->size - 1; /* clicked past the text: last word */
  sp = isspace((unsigned char)row->chars[i]) ? 1 : 0;
  a = i;
  b = i + 1;
  while (a > 0 && (isspace((unsigned char)row->chars[a - 1]) ? 1 : 0) == sp) a--;
  while (b < row->size && (isspace((unsigned char)row->chars[b]) ? 1 : 0) == sp) b++;
  *from = a;
  *to = b;
}

/* The mouse, now that it reports here. A click puts the cursor down and a drag
 * selects; cells out in the gutter are pulled to the first text column, so the
 * line numbers can never be part of a selection. Two clicks in a row pick out
 * the word, three pick out the whole line. */
void editorMouse(int button, int release, int x, int y) {
  if (y > E.screenrows) y = E.screenrows; /* the status bar is not text */
  if (y < 1) y = 1;
  int filerow = y - 1 + E.rowoff;

  if (button & 64) { /* wheel: up/down, or left/right (touchpad horizontal) */
    int dir = button & 3; /* 0=up 1=down 2=left 3=right */
    int i;
    if (dir >= 2) {
      for (i = 0; i < 3; i++)
        editorMoveCursor(dir == 3 ? ARROW_RIGHT : ARROW_LEFT);
      editorSyncGoal();
    } else {
      for (i = 0; i < 3; i++)
        editorMoveCursor(dir == 1 ? ARROW_DOWN : ARROW_UP);
    }
    return;
  }
  if ((button & 3) != 0) return; /* only the left button selects */

  int last = E.numrows > 0 ? E.numrows - 1 : 0;
  if (filerow < 0) filerow = 0;
  if (filerow > last) filerow = last;

  int rx = x - 1 - E.gutter + E.coloff; /* screen column -> display column */
  if (rx < 0) rx = 0;

  /* A release that lands where it pressed leaves a word- or line-selection
   * alone; anything else is a drag finishing, so put the cursor down there. */
  if (!(release && E.sel && x == E.mouse_click_x && y == E.mouse_click_y)) {
    E.cy = filerow;
    E.cx = filerow < E.numrows ? editorRowRxToCx(&E.row[filerow], rx) : 0;
    editorSyncGoal();
  }

  if (release) return; /* the selection stays put, ready for Ctrl-C */
  if (button & 32) {   /* dragging: stretch it from the anchor */
    E.sel = 1;
    return;
  }

  /* A press: count it against the last one. */
  long now = editorNowMs();
  int near = abs(x - E.mouse_click_x) <= OSHI_CLICK_SLOP &&
             abs(y - E.mouse_click_y) <= OSHI_CLICK_SLOP;
  E.mouse_click_n = (now - E.mouse_click_ms <= OSHI_CLICK_MS && near)
                      ? E.mouse_click_n + 1 : 1;
  E.mouse_click_ms = now;
  E.mouse_click_x = x;
  E.mouse_click_y = y;

  E.sel_cy = E.cy;
  E.sel_cx = E.cx;
  E.sel = 0;
  if (E.numrows == 0) return;

  if (E.mouse_click_n >= 3) { /* the whole line */
    E.sel_cx = 0;
    E.cx = E.row[E.cy].size;
    E.sel = 1;
    E.mouse_click_n = 0; /* the next click starts over */
  } else if (E.mouse_click_n == 2) { /* the word */
    int from, to;
    editorWordAt(&from, &to);
    E.sel_cx = from;
    E.cx = to;
    E.sel = 1;
  }
}

/* A centered, single-line-bordered box titled `title` with `nbody` rows of
 * `body` (each no wider than `width - 1` display cols, already laid out). Drawn
 * into `ab`; the caller flushes and handles dismissal. Gives the help screen
 * a real overlay instead of inline status text. */
void editorDrawBox(struct abuf *ab, const char *title,
                   const char **body, int nbody, int width) {
  char tmp[256];
  int height = nbody + 2;
  int top = (E.screenrows - height) / 2;
  if (top < 0) top = 0;
  int left = (E.screencols - (width + 2)) / 2;   /* +2 for the | borders */
  if (left < 0) left = 0;

  int tlen = (int)strlen(title);
  int pre = (width - tlen) > 0 ? (width - tlen) / 2 : 0;
  int post = width - tlen - pre;                 /* pre+title+post == width */
  if (post < 0) post = 0;
  int i;

  abAppend(ab, "\x1b[?25l", 6);              /* the box has no text field to point a cursor at */
  abAppend(ab, "\x1b[2J\x1b[H", 7);          /* clear + home the alt screen */

  /* top border: +-<pre>title<post>-+ */
  int n = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH+", top + 1, left + 1);
  abAppend(ab, tmp, n);
  for (i = 0; i < pre; i++) abAppend(ab, "-", 1);
  abAppend(ab, title, tlen);
  for (i = 0; i < post; i++) abAppend(ab, "-", 1);
  abAppend(ab, "+", 1);

  for (i = 0; i < nbody; i++) {
    int blen = (int)strlen(body[i]);
    int n2 = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH", top + 2 + i, left + 1);
    abAppend(ab, tmp, n2);
    abAppend(ab, "| ", 2);
    abAppend(ab, body[i], blen);
    int pad = width - 1 - blen;
    for (; pad > 0; pad--) abAppend(ab, " ", 1);
    abAppend(ab, "|", 1);
  }

  /* bottom border */
  int n3 = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH+", top + 2 + nbody, left + 1);
  abAppend(ab, tmp, n3);
  for (i = 0; i < width; i++) abAppend(ab, "-", 1);
  abAppend(ab, "+", 1);
  abAppend(ab, "\x1b[0m", 4);
}

/* Ctrl-H: a quick reference of every bindable command and the key it's on, in a
 * centered box. Reads E.binds live, so it matches whatever the config moved
 * around. Any key drops back to the editor (a resize just redraws the box). */
void editorHelp(void) {
  char lines[CMD_COUNT][128];
  int n = CMD_COUNT, width = 0, i;
  for (i = 0; i < n; i++) {
    char key[16];
    editorKeyName(E.binds[i], key, sizeof(key));
    int len = snprintf(lines[i], sizeof(lines[i]), "%-10s %-14s %s",
                       key, editorCmds[i].name, editorCmds[i].desc);
    if (len > width) width = len;
  }
  width += 1; /* the cell after the longest line, before the right border */
  const char *body[CMD_COUNT];
  for (i = 0; i < n; i++) body[i] = lines[i];
  for (;;) {
    struct abuf ab = ABUF_INIT;
    editorDrawBox(&ab, "oshi key bindings", body, n, width);
    ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
    (void)r;
    abFree(&ab);
    int k = editorReadKey();
    if (k == PASTE_KEY) editorSkipPaste();
    if (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY)
      continue;
    break;
  }
  E.redraw = 1;
}

/* Is doas or sudo around to redo a refused save? doas is looked for first:
 * where both are installed, it is the one you put there on purpose. */
static const char *editorElevTool(void) {
  static const char *names[] = { "doas", "sudo" };
  const char *path = getenv("PATH");
  size_t i;
  if (!path) return NULL;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char *p = path;
    for (;;) {
      const char *end = strchr(p, ':');
      size_t dirlen = end ? (size_t)(end - p) : strlen(p);
      char full[4096];
      if (dirlen == 0) { /* an empty PATH entry means the current directory */
        snprintf(full, sizeof(full), "./%s", names[i]);
      } else {
        snprintf(full, sizeof(full), "%.*s/%s", (int)dirlen, p, names[i]);
      }
      if (access(full, X_OK) == 0) return names[i];
      if (!end) break;
      p = end + 1;
    }
  }
  return NULL;
}

/* Turn the tool's stderr into one short line fit for the bar: control bytes
 * and newlines become spaces, and it is cut off before it can get silly. */
static void editorElevReason(char *out, size_t outsz, const char *raw, int rawlen) {
  size_t o = 0;
  int i;
  for (i = 0; i < rawlen && o + 1 < outsz; i++) {
    unsigned char c = (unsigned char)raw[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    else if (c < 32 || c == 127) continue;
    out[o++] = (char)c;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = '\0';
}

/* A tool that exits 0 is not proof it wrote what we meant to write, and the
 * one outcome worth shouting about is the file quietly not matching the
 * buffer. Returns 0 only when the file on disk can be read back and differs
 * from it; anything unverifiable (not a regular file, not readable by us, no
 * longer there) is left to the exit status. *got is the size seen. */
static int editorVerifySaved(const char *buf, int len, long *got) {
  struct stat st;
  char chunk[8192];
  int fd, off = 0;

  *got = -1;
  if (stat(E.filename, &st) == -1 || !S_ISREG(st.st_mode)) return 1;
  *got = (long)st.st_size;
  if (st.st_size != (off_t)len) return 0;
  fd = open(E.filename, O_RDONLY);
  if (fd == -1) return 1;
  for (;;) {
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return 1; /* unreadable now: trust the exit status */
    }
    if (off + (int)n > len || memcmp(chunk, buf + off, (size_t)n) != 0) {
      close(fd);
      return 0;
    }
    off += (int)n;
  }
  close(fd);
  return off == len;
}

/* Redo the save through doas or sudo: stream the buffer straight into a
 * privileged `dd of=<file>` over a pipe, rather than staging it in a file
 * first. That staging file is what the old version of this function used to
 * do - write the whole buffer to a mkstemp() file, then loosen it to 0666 so
 * a freshly-created destination would inherit a sane mode from `cp`. That
 * loosening is exactly backwards for a save that exists to protect a file
 * an ordinary user can't read: for as long as the password prompt is up
 * (which is not under our control - however long the person takes to type
 * it), the full plaintext sat world-readable in /tmp. Piping it in means the
 * content is never written anywhere but the final destination; nothing sits
 * on disk to be read out from under us, and there's no leftover temp file to
 * worry about cleaning up if oshi is killed mid-save.
 *
 * `dd` is run rather than fed directly, because sudo runs commands in a pty
 * (use_pty, on by default since 1.9.14) which passes stdin through the line
 * discipline - \r collapsed, DEL/ctrl-u/ctrl-d swallowed, lines past
 * MAX_CANON cut - and the tool still exits 0. That's true whether the data
 * comes from a file or a pipe, so dd's block-mode read (bs=4k) rather than
 * line-mode is what actually avoids it, same as before. `conv=fsync` asks
 * dd to sync the write before it exits, matching the fsync() the plain
 * (non-elevated) save path already does; drop it if you're pointing this at
 * a dd that doesn't understand the option (some minimal/embedded ones
 * don't). Both tools still read the password from /dev/tty themselves, so
 * the terminal drops back to cooked mode for the run - the password is
 * never ours to see - and stderr is captured so a refusal can be quoted on
 * the bar. Returns 0 when the file was written. Nothing here goes through a
 * shell: the target path is one argv, built as "of=<path>" so dd's
 * key=value parsing can't mistake it for a flag. */
static int editorRunElevated(const char *tool, const char *buf, int len) {
  char etmpl[] = "/tmp/oshi-err-XXXXXX";
  char arg_of[4096];
  int pfd[2];
  int efd, status = 0, ok = 0;
  pid_t pid;

  if (pipe(pfd) == -1) {
    editorSetStatusMessage("Can't save! pipe: %s", strerror(errno));
    return -1;
  }
  efd = mkstemp(etmpl);
  if (efd == -1) {
    editorSetStatusMessage("Can't save! Temp file: %s", strerror(errno));
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  /* dd's of= is one argv, never a shell string, so no quoting is needed
   * here - just make sure the path fits. A path this long is vanishingly
   * unlikely, but truncating silently would target the wrong file, so bail
   * instead. */
  if (strlen(E.filename) + 3 >= sizeof(arg_of)) {
    editorSetStatusMessage("Can't save! Path too long");
    close(pfd[0]);
    close(pfd[1]);
    close(efd);
    unlink(etmpl);
    return -1;
  }
  snprintf(arg_of, sizeof(arg_of), "of=%s", E.filename);

  /* park the caret on the bar: that is where the password prompt lands */
  {
    char at[32];
    int n = snprintf(at, sizeof(at), "\x1b[%d;1H", E.screenrows);
    editorWriteAll(STDOUT_FILENO, at, n);
  }

  disableRawMode(); /* the password is typed at the terminal, not read by us */
  editorWrite("\x1b[?1002l\x1b[?1006l\x1b[?2004l"); /* no mouse or paste markers while it is typed */
  /* Ctrl-C at a password prompt signals the whole foreground group, us
   * included, and our handler would walk off with the buffer. Ignore it here
   * and give the tool the default back, so Ctrl-C aborts the tool alone. */
  struct sigaction ign, old_int, old_quit;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  sigaction(SIGINT, &ign, &old_int);
  sigaction(SIGQUIT, &ign, &old_quit);
  /* Start the child before writing anything: a pipe's kernel buffer is far
   * smaller than most files (commonly 64KB on Linux), so if dd weren't
   * already running and draining its end, a buffer bigger than that would
   * block the write below forever. */
  pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    dup2(pfd[0], STDIN_FILENO);        /* the buffer arrives here */
    if (devnull != -1) dup2(devnull, STDOUT_FILENO); /* dd's summary line, discarded */
    dup2(efd, STDERR_FILENO);          /* complaints, for the bar */
    close(pfd[0]);
    close(pfd[1]);
    close(efd);
    if (devnull > 2) close(devnull);
    execlp(tool, tool, "dd", "bs=4k", arg_of, "conv=fsync", (char *)NULL);
    _exit(127); /* not found after all */
  }
  close(pfd[0]); /* only the child reads */
  if (pid > 0) {
    /* SIGPIPE is ignored globally (editorInstallSignals): if dd exits early
     * (wrong password, refused), this write fails with EPIPE instead of
     * taking oshi down with it. */
    int werr = editorWriteAll(pfd[1], buf, len) == -1 ? errno : 0;
    close(pfd[1]); /* EOF: tells dd it has the whole file */
    while (waitpid(pid, &status, 0) == -1)
      if (errno != EINTR) break;
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && werr == 0;
  } else {
    close(pfd[1]);
  }
  sigaction(SIGINT, &old_int, NULL);
  sigaction(SIGQUIT, &old_quit, NULL);
  enableRawMode();
  editorTermSetup(); /* mouse, paste and the rest, the way the editor had them */
  E.redraw = 1;      /* wipe the prompt and whatever the tool printed */

  if (pid == -1) {
    editorSetStatusMessage("Can't save! fork: %s", strerror(errno));
  } else if (ok) {
    long got = -1;
    if (!editorVerifySaved(buf, len, &got)) {
      editorSetStatusMessage("Save wrong: %ld of %d bytes - check the file", got, len);
      ok = 0;
    } else {
      editorSetStatusMessage("%d bytes written to disk (via %s)", len, tool);
    }
  } else {
    char raw[160] = "", why[160];
    ssize_t got;
    lseek(efd, 0, SEEK_SET); /* the child wrote through the same description */
    got = read(efd, raw, sizeof(raw) - 1);
    if (got < 0) got = 0;
    editorElevReason(why, sizeof(why), raw, (int)got);
    const char *reason = why; /* sudo and doas already name themselves first */
    if (strncmp(reason, tool, strlen(tool)) == 0 && reason[strlen(tool)] == ':' &&
        reason[strlen(tool) + 1] == ' ')
      reason += strlen(tool) + 2;
    if (reason[0])
      editorSetStatusMessage("%s: %s", tool, reason);
    else if (WIFSIGNALED(status))
      editorSetStatusMessage("%s was interrupted - nothing written", tool);
    else
      editorSetStatusMessage("%s failed (exit %d) - nothing written", tool,
                             WEXITSTATUS(status));
  }
  close(efd);
  unlink(etmpl);
  return ok ? 0 : -1;
}

/* The save was refused for lack of permission: the same centered box as the
 * help screen, offering to redo it as root. Enter runs the tool, which then
 * asks for its password on the terminal; esc, ctrl-q or any other key goes
 * back to editing with the buffer and the file untouched. */
int editorSaveElevated(const char *buf, int len) {
  const char *tool = editorElevTool();
  char lines[4][96];
  const char *body[4];
  int n = 0, width = 0, i;

  snprintf(lines[n++], sizeof(lines[0]), "%s", "Save refused: permission denied.");
  if (tool) {
    snprintf(lines[n++], sizeof(lines[0]), "Redo the save as root with %s?", tool);
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "enter", "save as root");
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "esc/q/ctrl-q", "keep editing");
  } else {
    snprintf(lines[n++], sizeof(lines[0]), "%s", "No doas or sudo found in PATH.");
    snprintf(lines[n++], sizeof(lines[0]), "%-14s%s", "esc/q/ctrl-q", "keep editing");
  }
  for (i = 0; i < n; i++) {
    int l = (int)strlen(lines[i]);
    if (l > width) width = l;
    body[i] = lines[i];
  }
  width += 1; /* the cell after the longest line, before the right border */

  for (;;) {
    struct abuf ab = ABUF_INIT;
    editorDrawBox(&ab, "privileges required", body, n, width);
    ssize_t r = write(STDOUT_FILENO, ab.b, ab.len);
    (void)r;
    abFree(&ab);
    int k = editorReadKey();
    if (k == PASTE_KEY) editorSkipPaste();
    if (k == RESIZE_KEY || k == MOUSE_KEY || k == NOKEY || k == PASTE_KEY)
      continue;
    if (tool && (k == '\r' || k == '\n')) {
      E.redraw = 1;
      return editorRunElevated(tool, buf, len);
    }
    break;
  }
  E.redraw = 1;
  editorSetStatusMessage(tool ? "Save cancelled" : "No sudo or doas in PATH");
  return -1;
}

void editorProcessKeypress(void) {
  int c = editorReadKey();
  int cmd = editorCommandForKey(c);
  /* up/down keep the column they were aiming for; anything else re-aims it */
  int vertical = (c == ARROW_UP || c == ARROW_DOWN || c == PAGE_UP ||
                  c == PAGE_DOWN || c == MOUSE_KEY);

  if (c == RESIZE_KEY || c == NOKEY) return; /* nothing for us to do */
  E.config_err[0] = '\0'; /* the config message is seen: done */

  /* The selection only outlives keys that do something with it: copy and cut
   * use it, the mouse drags it, the shift-arrows grow it, the word motions
   * collapse a select-all, and the keys that write over it. Every other key
   * gives it up. */
  if (cmd != CMD_COPY && cmd != CMD_CUT && cmd != CMD_PASTE &&
      c != MOUSE_KEY && c != NOKEY && c != CTRL_LEFT && c != CTRL_RIGHT &&
      (c < SHIFT_UP || c > CSHIFT_RIGHT) &&
      c != '\r' && c != '\t' && c != DEL_KEY && c != 8 && c != UTF8_KEY &&
      c != PASTE_KEY && !(c >= 32 && c < ARROW_LEFT)) E.sel = 0;

  /* The commands the config can move around run first, whatever key they're on. */
  if (cmd != CMD_NONE) {
    switch (cmd) {
    case CMD_QUIT: {
      long now = editorNowMs();
      if (E.dirty && !(E.quit_confirm && now - E.quit_time < OSHI_STATUS_MS)) {
        /* first quit key on a dirty buffer: warn. The warning is a toast too,
         * so a second key only quits while it is still up. */
        E.quit_confirm = 1;
        E.quit_time = now;
        return;
      }
      editorQuit();
      break;
    }
    case CMD_SAVE: editorSave(); break;
    case CMD_FIND: editorFind(); break;
    case CMD_SELECT: editorSelectAll(); break;
    case CMD_COPY: editorCopySelection(); break;
    case CMD_CUT: editorCut(); break;
    case CMD_PASTE: editorPasteInternal(); break;
    case CMD_KILL: editorDeleteLine(); break;
    case CMD_UNDO: editorUndo(); break;
    case CMD_REDO: editorRedo(); break;
    case CMD_REPLACE: editorReplace(); break;
    case CMD_FIND_NEXT:
      if (find_on) editorFindNextMatch(1);
      break;
    case CMD_FIND_PREV:
      if (find_on) editorFindNextMatch(-1);
      break;
    case CMD_GOTO: editorGoto(); break;
    case CMD_HELP: editorHelp(); break;
    }
    E.quit_confirm = 0;
    editorSyncGoal();
    return;
  }

  /* Any non-command key means you're done browsing the results and back to
   * editing: drop the live search so its "cur/total" counter and underlines
   * don't linger over your typing. n/N/help/save stay in search mode because
   * they're commands above. */
  if (find_on && !(c == MOUSE_KEY && (E.mouse_button & 64))) { /* (the wheel just scrolls) */
    find_on = 0;
    find_q[0] = '\0';
    find_row = -1;
  }

  switch (c) {
  case '\r':
    editorInsertNewline();
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
    break;

  case CTRL_LEFT:
  case CTRL_RIGHT:
    /* a word at a time - or the whole way, right after a select-all */
    if (editorAllSelected()) {
      editorMoveToEnd(c == CTRL_RIGHT ? 1 : -1);
      E.sel = 0;
    } else {
      editorMoveWord(c == CTRL_RIGHT ? 1 : -1);
    }
    break;

  case SHIFT_UP:
  case SHIFT_DOWN:
  case SHIFT_LEFT:
  case SHIFT_RIGHT:
  case CSHIFT_LEFT:
  case CSHIFT_RIGHT:
    /* shift+arrows drag the far end of the selection; the end that was first
     * stays where it is, so going back shortens it again. ctrl+shift makes
     * that far end jump whole words. */
    if (!E.sel) { E.sel_cy = E.cy; E.sel_cx = E.cx; E.sel = 1; }
    if (c == CSHIFT_LEFT || c == CSHIFT_RIGHT)
      editorMoveWord(c == CSHIFT_RIGHT ? 1 : -1);
    else
      editorMoveCursor(c == SHIFT_UP ? ARROW_UP : c == SHIFT_DOWN ? ARROW_DOWN
                     : c == SHIFT_LEFT ? ARROW_LEFT : ARROW_RIGHT);
    if (E.sel_cy == E.cy && E.sel_cx == E.cx) E.sel = 0; /* shrunk to nothing */
    break;

  case MOUSE_KEY:
    editorMouse(E.mouse_button, E.mouse_release, E.mouse_x, E.mouse_y);
    break;

  case BACKSPACE:
  case 8: /* ^H, when it isn't bound to a command, is how some terminals say Backspace */
  case DEL_KEY:
    if (E.sel) {
      editorDeleteSelection();
      break;
    }
    editorDeleteChar(c == DEL_KEY);
    break;

  case PAGE_UP:
  case PAGE_DOWN:
    {
      int last = E.numrows > 0 ? E.numrows - 1 : 0;
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      } else {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > last) E.cy = last;
      }
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      if (E.cy < E.numrows) /* landed on a row that may be shorter */
        E.cx = editorRowRxToCx(&E.row[E.cy], E.goal_rx);
    }
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;

  case UTF8_KEY:
    editorTypeBytes(E.utf8, E.utf8len);
    break;

  case PASTE_KEY: {
    int n;
    char *t = editorReadPasteBuf(&n);
    editorInsertText(t, n);
    free(t);
    break;
  }

  case '\x1b':
    /* Esc: a live search is already cleared above; otherwise no-op (it also
     * cancels any prompt, since editorPrompt reads Esc itself). */
  case CTRL_KEY('l'):
  case NOKEY:
    break;

  default:
    /* don't let unbound control bytes - or Alt-keys - end up in the file */
    if ((c >= 32 && c < 127) || c == '\t') {
      char ch = (char)c;
      editorTypeBytes(&ch, 1);
    }
    break;
  }
  E.quit_confirm = 0;
  editorClampCursor();
  if (!vertical) editorSyncGoal();
}

/*** config ***/

static int cfg_errs;
static char cfg_first[128];

/* Remember the first thing wrong with the config (and that there were more),
 * to put on the bar at startup instead of quietly ignoring it. */
static void cfgError(int lineno, const char *fmt, ...) {
  char msg[96];
  va_list ap;
  if (cfg_errs++ > 0) return;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(cfg_first, sizeof(cfg_first), "config line %d: %s", lineno, msg);
}

/* Make every part of dir, the way mkdir -p does; anything already there or in
 * the way is fine, we just keep going. */
void editorMkdirs(char *dir) {
  char *p;
  for (p = dir + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(dir, 0700);
      *p = '/';
    }
  }
  mkdir(dir, 0700);
}

/* No config on file: make the directory and drop in the defaults so there's
 * something to edit. Failing quietly is fine — oshi runs either way. */
void editorMakeConfig(const char *path) {
  char dir[512];
  FILE *fp;
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  *slash = '\0';
  editorMkdirs(dir);

  fp = fopen(path, "w");
  if (!fp) return;
  fputs(
    "# oshi config - ctrl-h in the editor lists every command and its key.\n"
    "# A mistake here shows up on the status bar at startup.\n"
    "\n"
    "ruler true            # line numbers in the gutter\n"
    "ruler_width default   # or a fixed number of columns\n"
    "cursor block           # block | beam\n"
    "\n"
    "# color <role> <code>   256-color palette code, 0-255\n"
    "# roles: status_bg status_fg gutter_fg gutter_cursor_fg gutter_bg\n"
    "#        gutter_cursor_bg tilde_fg text scrollmark_bg scrollmark_fg\n"
    "# text, gutter_bg and gutter_cursor_bg are unset here - they follow the\n"
    "# terminal's own colors until you set them.\n"
    "color status_bg 245\n"
    "color status_fg 235\n"
    "color gutter_fg 245\n"
    "color gutter_cursor_fg 15\n"
    "color tilde_fg 245\n"
    "color scrollmark_bg 244\n"
    "color scrollmark_fg 232\n"
    "# colorscheme default    resets every color above\n"
    "\n"
    "# bind <key> <command>   move a command to a different key, e.g:\n"
    "# bind ctrl-w save\n", fp);
  fclose(fp);
}

/* The key names the config speaks: ctrl-x, alt-x, none (0). -1 for something
 * we can't bind, -2 for a control key that already means something to the
 * terminal (Tab and Enter). */
int editorParseKey(const char *spec) {
  if (strcmp(spec, "none") == 0) return 0;
  if (strncmp(spec, "ctrl-", 5) == 0 && spec[5] && !spec[6] &&
      isalpha((unsigned char)spec[5])) {
    int k = CTRL_KEY(tolower((unsigned char)spec[5]));
    if (k == '\t' || k == '\r') return -2;
    return k;
  }
  if (strncmp(spec, "alt-", 4) == 0 && spec[4] && !spec[5])
    return ALT_KEY(tolower((unsigned char)spec[4]));
  return -1;
}

/* bind <key> <command>: move a command to another key. A later line wins, so
 * binding a key takes it away from whoever had it. Returns 0, or a negative
 * code for what was wrong: -1 bad key, -2 reserved key, -3 unknown command. */
int editorBind(const char *spec, const char *name) {
  int key = editorParseKey(spec);
  int cmd = -1, i;
  for (i = 0; i < CMD_COUNT; i++)
    if (strcmp(name, editorCmds[i].name) == 0) cmd = i;
  if (cmd < 0) return -3;
  if (key < 0) return key;
  for (i = 0; i < CMD_COUNT; i++)
    if (i != cmd && E.binds[i] == key) E.binds[i] = 0;
  E.binds[cmd] = key;
  return 0;
}

/* Which command a key runs; CMD_NONE when it runs none. */
int editorCommandForKey(int c) {
  int i;
  if (c == 0) return CMD_NONE;
  for (i = 0; i < CMD_COUNT; i++)
    if (E.binds[i] == c) return i;
  return CMD_NONE;
}

/* How a key reads back: ctrl-q, alt-x, off. */
void editorKeyName(int key, char *buf, size_t len) {
  if (key == 0)
    snprintf(buf, len, "off");
  else if (key >= ALT_KEY(0) && key <= ALT_KEY(255))
    snprintf(buf, len, "alt-%c", key - ALT_KEY(0));
  else if (key >= 1 && key <= 26)
    snprintf(buf, len, "ctrl-%c", key - 1 + 'a');
  else
    snprintf(buf, len, "?");
}

static int cfgBool(const char *val) {
  if (!strcmp(val, "true") || !strcmp(val, "on") ||
      !strcmp(val, "1") || !strcmp(val, "yes")) return 1;
  if (!strcmp(val, "false") || !strcmp(val, "off") ||
      !strcmp(val, "0") || !strcmp(val, "no")) return 0;
  return -1;
}

void editorLoadConfig(void) {
  char path[512];
  const char *dir = getenv("XDG_CONFIG_HOME");
  if (dir && *dir) {
    snprintf(path, sizeof(path), "%s/oshi/config", dir);
  } else {
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.config/oshi/config", home);
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    if (errno == ENOENT) editorMakeConfig(path);
    return;
  }

  char line[256];
  int lineno = 0;
  while (fgets(line, sizeof(line), fp)) {
    lineno++;
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] != '\n' && !feof(fp)) {
      /* longer than the buffer: swallow the rest instead of reading the tail
       * of it as a line of its own */
      int ch;
      while ((ch = fgetc(fp)) != EOF && ch != '\n') ;
      cfgError(lineno, "line too long");
      continue;
    }
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';

    char *key = strtok(line, " \t=\r\n");
    if (!key) continue; /* blank, or all comment */
    char *val = strtok(NULL, " \t=\r\n");
    if (!val) {
      cfgError(lineno, "'%s' needs a value", key);
      continue;
    }

    if (strcmp(key, "bind") == 0) {
      char *cmdname = strtok(NULL, " \t=\r\n");
      if (!cmdname) {
        cfgError(lineno, "bind wants a key and a command");
        continue;
      }
      switch (editorBind(val, cmdname)) {
      case -1: cfgError(lineno, "'%s' isn't a key (ctrl-x, alt-x, none)", val); break;
      case -2: cfgError(lineno, "%s can't be rebound (Tab/Enter)", val); break;
      case -3: cfgError(lineno, "no command called '%s'", cmdname); break;
      }
      continue;
    }

    if (strcmp(key, "colorscheme") == 0) {
      if (strcmp(val, "default") == 0) editorApplyDefaultColors();
      else cfgError(lineno, "colorscheme knows only 'default'");
      continue;
    }

    if (strcmp(key, "color") == 0) {
      char *code = strtok(NULL, " \t=\r\n"); /* val is the role name */
      int r = editorParseColorRole(val);
      if (r < 0) {
        cfgError(lineno, "no color role '%s'", val);
      } else if (!code) {
        cfgError(lineno, "color %s needs a code 0-255", val);
      } else {
        char *end;
        long c = strtol(code, &end, 10);
        if (*code && !*end && c >= 0 && c < 256) editorColorSet(r, (int)c);
        else cfgError(lineno, "'%s' isn't a color code 0-255", code);
      }
      continue;
    }

    if (strcmp(key, "ruler") == 0) {
      int b = cfgBool(val);
      if (b < 0) cfgError(lineno, "ruler wants true or false");
      else E.ruler = b;
    } else if (strcmp(key, "ruler_width") == 0) {
      if (strcmp(val, "default") == 0) {
        E.ruler_width = 0; /* auto-fit to line count */
      } else {
        char *end;
        long w = strtol(val, &end, 10);
        if (*val && !*end && w > 0 && w < 100000)
          E.ruler_width = (int)w;
        else
          cfgError(lineno, "ruler_width wants default or a number");
      }
    } else if (strcmp(key, "cursor") == 0) {
      if (strcmp(val, "block") == 0) E.cursor_style = 2;
      else if (strcmp(val, "beam") == 0) E.cursor_style = 6;
      else cfgError(lineno, "cursor wants block or beam");
    } else {
      cfgError(lineno, "unknown setting '%s'", key);
    }
  }
  fclose(fp);
  if (cfg_errs > 0) {
    snprintf(E.config_err, sizeof(E.config_err), "%s%s", cfg_first,
             cfg_errs > 1 ? " (and more)" : "");
    E.config_err_time = editorNowMs(); /* a toast, like the other notices */
  }
}

/*** colors ***/

/* Build the ANSI escape for a color role from a 256-palette code. Foreground
 * roles emit \x1b[38;5;Nm, background roles emit \x1b[48;5;Nm. */
void editorColorSet(int role, int code) {
  snprintf(E.col_str[role], sizeof(E.col_str[role]),
           colIsFg[role] ? "\x1b[38;5;%dm" : "\x1b[48;5;%dm", code);
}

/* The stock palette. "colorscheme default" calls this to wipe the board. */
void editorApplyDefaultColors(void) {
  editorColorSet(COL_STATUS_BG, 245); /* dimmer gray than 250 was: same fill
                                         style, less glare */
  editorColorSet(COL_STATUS_FG, 235);
  editorColorSet(COL_GUTTER_FG, 245);
  editorColorSet(COL_GUTTER_CURSOR_FG, 15); /* bright white */
  editorColorSet(COL_TILDE_FG, 245);
  editorColorSet(COL_SCROLLMARK_BG, 15);  /* bright white */
  editorColorSet(COL_SCROLLMARK_FG, 0);   /* black */
  /* text foreground and the gutter backgrounds are empty by default so they
   * follow the terminal's own theme (no panel behind the numbers unless you
   * ask for one); "color text/gutter_bg/gutter_cursor_bg <code>" opts in. */
  E.col_str[COL_TEXT_FG][0] = '\0';
  E.col_str[COL_GUTTER_BG][0] = '\0';
  E.col_str[COL_GUTTER_CURSOR_BG][0] = '\0';
}

int editorParseColorRole(const char *name) {
  if (!strcmp(name, "status_bg")) return COL_STATUS_BG;
  if (!strcmp(name, "status_fg")) return COL_STATUS_FG;
  if (!strcmp(name, "gutter_fg")) return COL_GUTTER_FG;
  if (!strcmp(name, "gutter_cursor_fg")) return COL_GUTTER_CURSOR_FG;
  if (!strcmp(name, "gutter_bg")) return COL_GUTTER_BG;
  if (!strcmp(name, "gutter_cursor_bg")) return COL_GUTTER_CURSOR_BG;
  if (!strcmp(name, "tilde_fg")) return COL_TILDE_FG;
  if (!strcmp(name, "text")) return COL_TEXT_FG;
  if (!strcmp(name, "scrollmark_bg")) return COL_SCROLLMARK_BG;
  if (!strcmp(name, "scrollmark_fg")) return COL_SCROLLMARK_FG;
  return -1;
}

/*** init ***/

void editorFreeAll(void) {
  int i;
  for (i = 0; i < E.numrows; i++)
    editorFreeRow(&E.row[i]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  free(E.filename);
  E.filename = NULL;
  free(E.clip);
  E.clip = NULL;
  while (undo_count > 0) editorFreeRec(&undo_stack[--undo_count]);
  free(undo_stack);
  undo_stack = NULL;
  while (redo_count > 0) editorFreeRec(&redo_stack[--redo_count]);
  free(redo_stack);
  redo_stack = NULL;
  free(saved_text);
  saved_text = NULL;
  saved_len = 0;
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.goal_rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.rowcap = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.notice_pending = 0;
  E.notice_deadline = 0;
  E.config_err[0] = '\0';
  E.config_err_time = 0;
  E.quit_time = 0;
  E.ruler = 1;
  E.ruler_width = 0;
  E.gutter = 0;
  E.cursor_style = 2; /* steady block */
  E.clip = NULL;
  editorSnapshot(); /* an empty buffer counts as what's on disk */
  E.dirty = 0;
  E.binds[CMD_SAVE] = CTRL_KEY('s');
  E.binds[CMD_QUIT] = CTRL_KEY('q');
  E.binds[CMD_FIND] = CTRL_KEY('f');
  E.binds[CMD_SELECT] = CTRL_KEY('a');
  E.binds[CMD_COPY] = CTRL_KEY('c');
  E.binds[CMD_CUT] = CTRL_KEY('x');
  E.binds[CMD_PASTE] = CTRL_KEY('v');
  E.binds[CMD_KILL] = CTRL_KEY('d');
  E.binds[CMD_UNDO] = CTRL_KEY('z');
  E.binds[CMD_REDO] = CTRL_KEY('r');
  E.binds[CMD_REPLACE] = ALT_KEY('r');
  E.binds[CMD_FIND_NEXT] = CTRL_KEY('n');
  E.binds[CMD_FIND_PREV] = CTRL_KEY('p');
  E.binds[CMD_GOTO] = CTRL_KEY('g');
  E.binds[CMD_HELP] = CTRL_KEY('h');
  editorApplyDefaultColors();
  editorLoadConfig();
  atexit(editorFreeAll);
  editorSetCursorStyle();
  editorUseAltScreen();
  editorTermSetup();

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 1;
  if (E.screenrows < 1) E.screenrows = 1;
}

int main(int argc, char *argv[]) {
  if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                    strcmp(argv[1], "-v") == 0)) {
    printf("oshi %s\n", OSHI_VERSION);
    return 0;
  }
  if (argc > 2) {
    fprintf(stderr, "usage: oshi [-v|--version] [file]\n");
    return 1;
  }

  enableRawMode();
  editorInstallSignals(); /* from here on, whatever kills us puts the terminal back */
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
