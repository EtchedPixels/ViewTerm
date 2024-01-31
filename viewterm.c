#define _XOPEN_SOURCE_EXTENDED

/*
 *   viewterm - a rather simplistic Viewdata terminal implementation for
 *		Linux.
 *
 *   Copyright (C) 2014, Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *	cc viewterm.c -o viewterm -lncursesw
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncursesw/curses.h>
#include <sys/select.h>
#include <netdb.h>
#include <locale.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* If you want a job doing properly, do it yourself */

#define UC_STERLING     0x00A3
#define UC_QUARTER	0x00BC
#define UC_HALF		0x00BD
#define UC_3QUARTER	0x00BE
#define UC_LARROW	0x2190
#define UC_UARROW       0x2191
#define UC_RARROW	0x2192
#define UC_HLINE        0x2501
#define UC_BLOCK	0x25ae

static int nethook(char *hostname) {
  struct hostent *hp;
  struct in_addr addr;
  struct sockaddr_in sin;
  int fd;
  int port = 23;
  char *rp;
  rp = strrchr(hostname, ':');
  if (rp) {
    *rp ++ = 0;
    if (sscanf(rp, "%d", &port) != 1) {
      fprintf(stderr, "Invalid port number '%s'.\n", rp);
      exit(1);
    }
  }
  
  hp = gethostbyname(hostname);
  if (hp == NULL) {
    fprintf(stderr, "Unable to resolve '%s'.\n", hostname);
    exit(1);
  }
  sin.sin_family = AF_INET;
  memcpy(&sin.sin_addr, hp->h_addr, hp->h_length);
  sin.sin_port = htons(port);
  
  fd = socket(hp->h_addrtype, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    exit(1);
  }
  printf("Connecting...\n");
  if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    perror("connect");
    exit(1);
  }
  if (fcntl(fd, F_SETFL, FNDELAY) < 0) {
    perror("fcntl");
    exit(1);
  }
  printf("OK\n");
}

static WINDOW *base;
static WINDOW *debug;
static char *display;
static int scrolling = 0;
static int colour;
static int lines = 24;
static int cols = 40;
static int lowbitsheresy = 0;
static int unicode;

static wchar_t xlate_text_uc(unsigned char ci, int *inv)
{
  wchar_t cx;
  
  *inv = 0;
  switch(ci) {
  case '#':
    cx = UC_STERLING;
    break;
  case '[':
    cx = UC_LARROW;
    break;
  case ']':
    cx = UC_RARROW;
    break;
  case '^':
    cx = UC_UARROW;
    break;
  case '`':
    cx = UC_HLINE;
    break;
  case '|':
    cx = '|'; /* should really be a double bar */
    break;
  case '~':
    cx = '/'; /* really should be a divide symbol */
    break;
  case '{':
    /* should be 1/2 */
    cx = UC_HALF;
    break;
  case '}':
    cx = UC_3QUARTER;
    break;
    /* should be 3/4 */
  case '\\': /* should be 1/4 */
    cx = UC_QUARTER;
    break;
  case '_':
    cx = '#';
    break;
  case 127:
    if (colour) {
      *inv = 1;
      cx = ' ';
    }
    else 
      cx = UC_BLOCK;
    break;
  default:
    cx = ci;
  }
  return cx;
}

static wchar_t xlate_text_a(unsigned char ci, int *inv)
{
  wchar_t cx;
  
  *inv = 0;
  switch(ci) {
  case '#':
    cx = 'L';
    break;
  case '[':
    cx = '<';
    break;
  case ']':
    cx = '>';
    break;
  case '^':
    cx = '^';
    break;
  case '`':
    cx = '-';
    break;
  case '|':
    cx = '|'; /* should really be a double bar */
    break;
  case '~':
    cx = '/'; /* really should be a divide symbol */
    break;
  case '{':
    /* should be 1/2 */
    cx = '{';
    break;
  case '}':
    cx = '}';
    break;
    /* should be 3/4 */
  case '\\': /* should be 1/4 */
    cx = '\\';
    break;
  case '_':
    cx = '#';
    break;
  case 127:
    if (colour) {
      *inv = 1;
      cx = ' ';
    }
    else 
      cx = '#';
    break;
  default:
    cx = ci;
  }
  return cx;
}

static wchar_t gmap4[] = {
  /* top only */
  ' ',
  0x2598,
  0x259D,
  0x2580,
  /* with bl */
  0x2596,
  0x258C,
  0x259E,
  0x259B,
  /* with br */
  0x2597,
  0x259A,
  0x2590,
  0x259C,
  /* with br/bl */
  0x2584,
  0x2599,
  0x259F,
  0x25A0
};

static wchar_t xlate_gfx_uc(unsigned char c, int sep, int *inv)
{
  int tl = c & 1;
  int tr = c & 2;
  int ml = c & 4;
  int mr = c & 8;
  int bl = c & 16;
  int br = c & 64;
  int lu;
  wchar_t r;
  
  *inv = 0;

  /* Fold six into four */
  tl |= ml;
  tr |= mr;
  
  /* Work from the block elements */
  lu = c & 3;
  if (bl)
    lu |= 4;
  if (br)
    lu |= 8;
  r = gmap4[lu];
  if (r == 0x25A0 && colour) {
    r = ' ';
    *inv = 1;
  }
}

static wchar_t xlate_gfx_a(unsigned char c, int sep, int *inv)
{
  int tl = c & 1;
  int tr = c & 2;
  int ml = c & 4;
  int mr = c & 8;
  int bl = c & 16;
  int br = c & 64;
  int lu;
  wchar_t r;
  
  *inv = 0;
  
  if (tl + tr + ml + mr + bl + br < 4)
    return ' ';
  if (colour) {
    *inv = 1;
    return ' ';
  }
  return '#';
}

static wchar_t xlate_text(unsigned char ci, int *inv)
{
  if (unicode)
    return xlate_text_uc(ci, inv);
  return xlate_text_a(ci, inv);
}

static wchar_t xlate_gfx(unsigned char c, int sep, int *inv)
{
  if (unicode)
    return xlate_gfx_uc(c, sep, inv);
  return xlate_gfx_a(c, sep, inv);
}

static int redraw_line(int l)
{
  unsigned char held = ' ';
  int hold = 0;
  int sep = 0;
  int nextbg = COLOR_BLACK;
  int nextfg = COLOR_WHITE;
  int fg, bg;
  int gfx = 0;
  int dbl = 0;
  int flash = 0;
  unsigned char c;
  int inv;
  static wchar_t out[2];
  
  int i;
  
  for (i = 0; i < cols; i++) {
    fg = nextfg;
    bg = nextbg;
    c = display[l * cols + i];
    switch (c) {
      case 0x90:
      case 0x80:
        if (lowbitsheresy)
          nextfg = COLOR_BLACK;
        break;
      case 0x91:
      case 0x81:
        nextfg = COLOR_RED;
        break;
      case 0x92:
      case 0x82:
        nextfg = COLOR_GREEN;
        break;
      case 0x93:
      case 0x83:
        nextfg = COLOR_YELLOW;
        break;
      case 0x94:
      case 0x84:
        nextfg = COLOR_BLUE;
        break;
      case 0x95:
      case 0x85:
        nextfg = COLOR_MAGENTA;
        break;
      case 0x96:
      case 0x86:
        nextfg = COLOR_CYAN;
        break;
      case 0x97:
      case 0x87:
        nextfg = COLOR_WHITE;
        break;
      case 0x88:
        flash = 0;
        break;
      case 0x89:
        flash = 1;
        break;
      case 0x8A:
        /* box off */
        break;
      case 0x8B:
        /* box on */
        break;
      case 0x8C:
        /* normal height */
        break;
      case 0x8D:
        /* double height */
        break;
      case 0x8E:
        /* shift */
      case 0x8F:
        /* shift out */
        break;
      case 0x98:
        /* conceal */
      case 0x99:
        sep = 0;
        break;
      case 0x9A:
        sep = 1;
        break;
      case 0x9B:
        /* Unused (esc) */
        break;
      case 0x9C:
        bg = COLOR_BLACK;
        nextbg = bg;
        break;
      case 0x9D:
        nextbg = fg;
        break;
      case 0x9E:
        hold = 1;
        break;
      case 0x9F:
        hold = 0;
        break;
    }
    if (c >= 0x80 && c <= 0x87)
      gfx = 0;
    else if (c >= 0x90 && c <= 0x97)
      gfx = 1;
    
    if (c >= 0x80 && c < 0xA0) {
      if (hold == 1)
        c = held;
      else 
        c = ' ';
    }
    
    if (gfx && (c <= 0x40 || c >= 0x5F)) {
      if (hold)
        held = c;
      out[0] = xlate_gfx(c, sep, &inv);
    } else {
      out[0] = xlate_text(c, &inv);
    }
    /* Inverse is useful for block graphic */
    if (inv == 0)
      wattron(base, COLOR_PAIR(1 + 8 * fg + bg));
    else
      wattron(base, COLOR_PAIR(1 + 8 * bg + fg));
      mvwaddwstr(base, l, i, out);
  }
}
    
/*
 *	Do something useful with the basic set of symbols and controls
 *
 *	We do not support T.100 (supplementary control functions) or
 *	the various shifted controls.
 *
 *	If anyone ever finds a user of them then they are akin to the low
 *	control codes but
 *	0x90-0x97 set the background directly not graphics + fg
 *	0x8E sets double width
 *	0x8F sets double size
 *	0x9C/9D turn invert off/on
 *	0x9E turns the background transparent
 *	0x9F turns off conceal
 *
 *	That would need a whole pile of logic adding in both control
 *	processing and the like as well as the line render.
 */
static int process(unsigned char c)
{
  static int xp, yp, escape;
  unsigned char cx;
  int i;
  int found = 1;

  if (escape) {
    escape = 0;
    c += 64;
  }

  switch(c) {
    case 0: /* NUL */
      break;
    case 5: /* ENQ */
      break;
    case 8:
      xp --;
      if (xp < 0)
        xp = 79;
      break;
    case 9:
      xp++;
      if (xp == 79)
        xp = 0;
      break;
    case 10:
      /* Scrolling modes ? */
      yp++;
      if (yp == 23)
        yp = 0;
      break;
    case 11:
      if (yp)
        yp --;
      else
        yp = 23;
      break;
    case 12:
      werase(base);
      if (debug)
        werase(debug);
      memset(display, ' ', cols * lines);
      for (i = 0; i < lines; i++)
        redraw_line(i);
      xp = 0;
      yp = 0;
      break;
    case 13:
      xp = 0;
      break;
    case 14:
      /* Shift character set except ' ' and 127 */
      break;
    case 15:
      /* End shift */
    case 17:
      /* cursor on */
      break;
    case 20:
      /* cursor off */
      break;
    case 24:
      /* Cancel: clear rest of row to spaces */
      break;
    case 25:
      /* Shift to G2 character set for one symbol */
      break;
    case 26:
    case 27:
      escape = 1;
      break;
    case 29:
      /* Shift to G3 character set for one symbol */
    case 30:
      xp = 0;
      yp = 0;
      break;
    case 31:
      xp = 0;
      yp = 23;
      break;
    default:
      cx = c & 0x7F;
  
      if (cx > 31) {
        if (debug)
          mvwprintw(debug, yp, xp, "%c", cx);
        display[cols * yp + xp] = c;
        redraw_line(yp);
        xp++;
        if (xp == cols) {
          xp = 0;
          yp++;
          if (yp == lines) {
            if (scrolling) {
              if (debug)
                wscrl(debug, 1);
              memmove(display, display + cols, cols * (lines - 1));
              memset(display + cols * (lines - 1), ' ', cols);
              for (i = 0; i < lines; i++)
                redraw_line(i);
            }
            else
            yp = 0;
          }
        }
        break;
      }
      if (c & 0x80) {
        display[cols * yp + xp] = c;
        if (debug)
          mvwprintw(debug, yp, xp, "%c", cx);
        redraw_line(yp);
        xp++;
        if (xp == cols) {
          xp = 0;
          yp++;
          if (yp == lines) {
            if (scrolling) {
              if (debug)
                wscrl(debug, 1);
              memmove(display, display + cols, cols * (lines - 1));
              memset(display + 40 * (lines - 1), ' ', 40);
              for (i = 0; i < 23; i++)
                redraw_line(i);
            }
            else yp = 0;
          }
        }
      }
      break;
  }
  /* So the cursor looks right */
  wmove(base, yp, xp);
}

/* Ought to handle pound/#/_ etc ? */
/* type will be OK for a character, KEY_CODE_YES for a code */
static void sendcode(int s, int type, wchar_t code)
{
  unsigned char c = 0;

  /* Direct map normal codes but cover exceptions below */  
  if (type == OK && (code >= ' ' && code <= 127))
    c = code;

  /* Key codes */
  if (type == KEY_CODE_YES) {
    switch(code) {
    case KEY_ENTER:
      c = 13;
      break;
    case KEY_UP:
      c = 11;
      break;
    case KEY_DOWN:
      c = 10;
      break;
    case KEY_BACKSPACE:
    case KEY_LEFT:
      c = 8;
      break;
    case KEY_RIGHT:
      c = 9;
      break;
    case KEY_PPAGE:
      c = 17;
      break;
    case KEY_NPAGE:
      c = 20;
      break;
    case KEY_IC:
      c = 12;
      break;
    case 27: /* Escape */
      c = 27;
      break;
    case KEY_HOME:
      c = 30;
      break;
    case KEY_END:
      c = 31;
      break;
    }
  } else {
    /* Text codes */
    switch(code) {
    case 10:
    case 13:
      c = 13;
    break;
    case 127:
      c = 8;
      break;
    case 9: /* Tab */
      c = 9;
      break;
    case '#':
      c = '_';  /* Send a teletext hash */
      break;
    case 0x00A3:
      c = '#';  /* Pound is hash */
      break;
    case 0x00BD:
      c = '{';
      break;
    case 0x00F7:
      c = '~';
      break;
    }
  }
  if (c != 0)
    write(s, &c, 1);
}

int main(int argc, char *argv[])
{
  int xp = 0, yp = 0;
  int s;
  wchar_t ch;
  int err;
  fd_set in;
  unsigned char c;
  int cn;
  int do_debug = 0;
  int opt;

  setlocale(LC_ALL, "");
  
  while((opt = getopt(argc, argv, "dHhur:c:")) != -1) {
    switch(opt) {
      case 'd':
        do_debug = 1;
        break;
      case 'H':
        lowbitsheresy = 1;
        break;
      case 'h':
      default:
        printf("viewterm is copyright (C) 2014 Alan Cox\n");
        printf("This program comes with ABSOLUTELY NO WARRANTY. This is free software, and\n");
        printf("you are welcome to redistribute it under certain conditions. See LICENSE.txt\n");
        printf("for more information.\n\n");
        printf("%s hostname{:port}\n", argv[0]);
        printf("-d: enable debug window.\n");
        printf("-H: allow black colour setting.\n");
        printf("-h: this help text\n");
        printf("-u: enable unicode graphics mode.\n");
        printf("-c cols: set the columns (usually 40)\n");
        printf("-r rows: set the rows (usually 24, but 25 for some)\n");
        exit(0);
      case 'u':
        unicode = 1;
        break;
      case 'r':
        if (sscanf(optarg, "%d", &lines) == 0 || lines == 0) {
          fprintf(stderr, "%s: '%s' is not a valid number of rows.\n",
            argv[0], optarg);
          if (opt == 'r')
            exit(EXIT_SUCCESS);
          exit(EXIT_FAILURE);
        }
        break;
      case 'c':
        if (sscanf(optarg, "%d", &cols) == 0 || cols == 0) {
          fprintf(stderr, "%s: '%s' is not a valid number of columns.\n",
            argv[0], optarg);
          exit(EXIT_FAILURE);
        }
        break;
    }
  }
      
  if (optind >= argc) {
    fprintf(stderr, "%s: hostname required.\n", argv[0]);
    exit(1);
  }
  
  display = malloc(cols * lines);
  if (display == NULL) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(display, ' ', cols * lines);  

  s = nethook(argv[optind]);
  
  initscr();
  start_color();
  cbreak();
  halfdelay(1);
  noecho();
  intrflush(stdscr, FALSE);
  keypad(stdscr, TRUE);

  if (has_colors())
    colour = 1;
  
  if (COLOR_PAIRS < 64) {
    fprintf(stderr, "not enough colour pairs.\n");
    colour = 0;
  }
  if (has_colors()) {
    for (cn = 0; cn < 63; cn++) {
      if (init_pair(cn + 1, cn >> 3, cn & 7)) {
        endwin();
        fprintf(stderr, "init_pair failed\n");
        exit(1);
      }
    }
  }
  
  base = newwin(lines,cols,0,0);
  if (base == NULL) {
    endwin();
    perror("newwin");
    exit(1);
  }
  
  if (do_debug) {
    debug = newwin(lines,cols,0,cols);
    if (debug == NULL) {
      endwin();
      perror("newwin");
      exit(1);
    }
  }
  
  werase(base);
  wrefresh(base);
  leaveok(base, FALSE);
  if (debug) {
    werase(debug);
    wrefresh(debug);
    leaveok(debug, TRUE);
  }

  while(1) {
    FD_ZERO(&in);
    FD_SET(0, &in);
    FD_SET(s, &in);
    /* Should deal with overruns to socket ? */
    if (select(s+1, &in, NULL, NULL, NULL) < 0) {
      endwin();
      perror("select");
      exit(1);
    }
    if (FD_ISSET(0, &in)) {
      err = wget_wch(base, &ch);
      if (ch != ERR)
        sendcode(s, err, ch);
    }
    if (FD_ISSET(s, &in)) {
      unsigned char buf[256];
      int len =  read(s,buf,256);
      int i;
      if (len == 0)
        break;
      if (len == -1) {
        perror("read");
        exit(1);
      }
      for (i = 0; i < len; i++)
        process(buf[i]);
      if (debug)
        wrefresh(debug);
      wrefresh(base);
    }
  }
  close(s);
  endwin();
}   