/**
 * @file pty_fixture.c
 * @brief A small terminal program for the PTY tests to drive.
 *
 * Each mode exercises one thing a real TUI does to its terminal, and
 * reports what it saw in a form a test can check on the screen:
 *
 *   print TEXT        write TEXT, exit 0
 *   env NAME          print NAME=value, or "NAME unset"
 *   keys              raw mode; print every input chunk in hex; 'q' quits
 *   appkeys           as keys, after enabling application cursor mode
 *   dsr               ask the terminal for the cursor position, print reply
 *   winch             print the window size, again on every SIGWINCH
 *   frame             draw a coloured box on the alternate screen
 *   linger PATH MS    sleep MS, then write PATH, then exit 0
 *   flood N           print N numbered lines, exit 0
 *   exit N            exit with status N
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios g_saved;

static void raw_mode(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0) { perror("tcgetattr"); exit(2); }
    t = g_saved;
    cfmakeraw(&t);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0) { perror("tcsetattr"); exit(2); }
}

static void cooked_mode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

static void out(const char *s) {
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, s, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) exit(3);
        s += n;
        len -= (size_t)n;
    }
}

static int mode_keys(int app) {
    raw_mode();
    if (app) out("\033[?1h");
    out("READY\r\n");
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        char line[256] = "K:";
        for (ssize_t i = 0; i < n; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02x", buf[i]);
            strcat(line, hex);
        }
        strcat(line, "\r\n");
        out(line);
        if (n == 1 && buf[0] == 'q') break;
    }
    if (app) out("\033[?1l");
    cooked_mode();
    return 0;
}

static void on_alarm(int sig) { (void)sig; }

static int mode_dsr(void) {
    raw_mode();
    out("\033[5;10H\033[6n");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);   /* no SA_RESTART: read is interrupted */
    alarm(5);
    char reply[32];
    size_t len = 0;
    while (len < sizeof(reply) - 1) {
        ssize_t n = read(STDIN_FILENO, reply + len, 1);
        if (n <= 0) break;
        len++;
        if (reply[len - 1] == 'R') break;
    }
    alarm(0);
    reply[len] = '\0';
    char line[96] = "\r\nREPLY:";
    for (size_t i = 0; i < len; i++) {
        char c[4];
        if (reply[i] == '\033') strcat(line, "ESC");
        else { c[0] = reply[i]; c[1] = '\0'; strcat(line, c); }
    }
    strcat(line, "\r\n");
    out(line);
    cooked_mode();
    return len > 0 ? 0 : 1;
}

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int sig) { (void)sig; g_winch = 1; }

static void print_size(void) {
    struct winsize ws;
    char line[64];
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) { perror("TIOCGWINSZ"); exit(2); }
    snprintf(line, sizeof(line), "SIZE:%dx%d\r\n", ws.ws_col, ws.ws_row);
    out(line);
}

static int mode_winch(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    raw_mode();
    print_size();
    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (g_winch) { g_winch = 0; print_size(); }
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || c == 'q') break;
    }
    cooked_mode();
    return 0;
}

static int mode_frame(void) {
    raw_mode();
    out("\033[?1049h\033[?25l\033[2J");
    /* A 12x4 box at row 2, col 3 (1-based), title in bold, a selected line
     * in reverse video, a red status word, blue background in the box. */
    out("\033[2;3H\033(0lqqqqqqqqqqk\033(B");
    out("\033[3;3H\033(0x\033(B\033[44m\033[1m Title    \033[0m\033(0x\033(B");
    out("\033[4;3H\033(0x\033(B\033[7m> item 1  \033[0m\033(0x\033(B");
    out("\033[5;3H\033(0mqqqqqqqqqqj\033(B");
    out("\033[7;1H\033[31mstatus:\033[0m ok");
    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || c == 'q') break;
    }
    out("\033[?25h\033[?1049lBYE\r\n");
    cooked_mode();
    return 0;
}

static int mode_linger(const char *path, int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    FILE *fp = fopen(path, "w");
    if (!fp) { perror(path); return 1; }
    fputs("complete\n", fp);
    return fclose(fp) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s MODE [ARGS]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    if (strcmp(mode, "print") == 0 && argc > 2) { out(argv[2]); return 0; }
    if (strcmp(mode, "env") == 0 && argc > 2) {
        const char *v = getenv(argv[2]);
        printf(v ? "%s=%s\n" : "%s unset\n", argv[2], v);
        return 0;
    }
    if (strcmp(mode, "keys") == 0) return mode_keys(0);
    if (strcmp(mode, "appkeys") == 0) return mode_keys(1);
    if (strcmp(mode, "dsr") == 0) return mode_dsr();
    if (strcmp(mode, "winch") == 0) return mode_winch();
    if (strcmp(mode, "frame") == 0) return mode_frame();
    if (strcmp(mode, "linger") == 0 && argc > 3) return mode_linger(argv[2], atoi(argv[3]));
    if (strcmp(mode, "flood") == 0 && argc > 2) {
        int n = atoi(argv[2]);
        for (int i = 0; i < n; i++) printf("line-%05d-abcdefghijklmnopqrstuvwxyz\n", i);
        printf("FLOOD-END\n");
        return 0;
    }
    if (strcmp(mode, "exit") == 0 && argc > 2) return atoi(argv[2]);
    fprintf(stderr, "%s: unknown mode '%s'\n", argv[0], mode);
    return 2;
}
