#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <ctype.h>

#define MAX_LINE     80
#define MAX_ARGS     40
#define HISTORY_SIZE 36

/* ── Raw mode ──────────────────────────────────────────────────── */

static struct termios orig_termios;

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ── History ───────────────────────────────────────────────────── */

static char hist[HISTORY_SIZE][MAX_LINE];
static int  hist_count = 0;
static int  hist_next  = 0;

static void history_add(const char *cmd) {
    strncpy(hist[hist_next], cmd, MAX_LINE - 1);
    hist[hist_next][MAX_LINE - 1] = 0;
    hist_next = (hist_next + 1) % HISTORY_SIZE;
    if (hist_count < HISTORY_SIZE) hist_count++;
}

/* nav=1 → most recent, nav=hist_count → oldest */
static const char *history_get(int nav) {
    int idx = ((hist_next - nav) % HISTORY_SIZE + HISTORY_SIZE) % HISTORY_SIZE;
    return hist[idx];
}

/* ── Line editor ───────────────────────────────────────────────── */

static void redraw(const char *prompt, const char *buf, int len, int pos) {
    printf("\r\x1b[K%s%s", prompt, buf);
    if (len - pos > 0)
        printf("\x1b[%dD", len - pos);
    fflush(stdout);
}

/* Returns length >= 0, or -1 on EOF (Ctrl-D on empty line). */
static int read_line(char *buf, int maxlen, const char *prompt) {
    int len = 0, pos = 0, nav = 0;
    char saved[MAX_LINE] = "";

    buf[0] = 0;
    enable_raw_mode();
    printf("%s", prompt); fflush(stdout);

    while (1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) { disable_raw_mode(); return -1; }

        if (c == '\r' || c == '\n') {           /* Enter */
            buf[len] = 0;
            printf("\r\n"); fflush(stdout);
            disable_raw_mode();
            return len;

        } else if (c == 3) {                    /* Ctrl-C: clear line */
            printf("^C\r\n%s", prompt); fflush(stdout);
            buf[0] = 0; len = pos = nav = 0; saved[0] = 0;

        } else if (c == 4 && len == 0) {        /* Ctrl-D on empty: EOF */
            printf("\r\n"); fflush(stdout);
            disable_raw_mode(); return -1;

        } else if (c == 127 || c == 8) {        /* Backspace */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                len--; pos--; buf[len] = 0;
                redraw(prompt, buf, len, pos);
            }

        } else if (c == 1) {                    /* Ctrl-A: start of line */
            pos = 0; redraw(prompt, buf, len, pos);

        } else if (c == 5) {                    /* Ctrl-E: end of line */
            pos = len; redraw(prompt, buf, len, pos);

        } else if (c == '\x1b') {               /* Escape sequence */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[1] == 'A') {                /* Up */
                if (nav < hist_count) {
                    if (nav == 0) strncpy(saved, buf, MAX_LINE - 1);
                    nav++;
                    strncpy(buf, history_get(nav), maxlen - 1);
                    len = pos = strlen(buf);
                    redraw(prompt, buf, len, pos);
                }
            } else if (seq[1] == 'B') {         /* Down */
                if (nav > 0) {
                    nav--;
                    strncpy(buf, nav == 0 ? saved : history_get(nav), maxlen - 1);
                    len = pos = strlen(buf);
                    redraw(prompt, buf, len, pos);
                }
            } else if (seq[1] == 'C') {         /* Right */
                if (pos < len) { pos++; redraw(prompt, buf, len, pos); }
            } else if (seq[1] == 'D') {         /* Left */
                if (pos > 0) { pos--; redraw(prompt, buf, len, pos); }
            } else if (seq[1] == '3') {         /* Delete key (ESC[3~) */
                unsigned char tilde;
                if (read(STDIN_FILENO, &tilde, 1) > 0 && tilde == '~' && pos < len) {
                    memmove(buf + pos, buf + pos + 1, len - pos - 1);
                    len--; buf[len] = 0;
                    redraw(prompt, buf, len, pos);
                }
            }

        } else if (c >= 32 && len < maxlen - 1) { /* Printable */
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = c;
            len++; pos++; buf[len] = 0;
            redraw(prompt, buf, len, pos);
        }
    }
}

/* ── Parser ────────────────────────────────────────────────────── */

static char *expand_env_vars(const char *tok, char *out, int outlen) {
    int i = 0, j = 0;
    while (tok[i] && j < outlen - 1) {
        if (tok[i] == '$') {
            i++;
            char varname[256];
            int k = 0;
            if (tok[i] == '{') {
                i++;
                while (tok[i] && tok[i] != '}' && k < (int)sizeof(varname) - 1)
                    varname[k++] = tok[i++];
                if (tok[i] == '}') i++;
            } else {
                while ((isalnum((unsigned char)tok[i]) || tok[i] == '_') && k < (int)sizeof(varname) - 1)
                    varname[k++] = tok[i++];
            }
            varname[k] = 0;
            if (k > 0) {
                const char *val = getenv(varname);
                if (val)
                    while (*val && j < outlen - 1)
                        out[j++] = *val++;
            } else {
                out[j++] = '$';
            }
        } else {
            out[j++] = tok[i++];
        }
    }
    out[j] = 0;
    return out;
}

static int parse_input(char *input, char *args[], int *background,
                       char **in_file, char **out_file, int *pipe_pos)
{
    static char exp_bufs[MAX_ARGS][MAX_LINE];
    static char exp_in[MAX_LINE], exp_out[MAX_LINE];
    *background = 0; *in_file = NULL; *out_file = NULL; *pipe_pos = -1;
    int argc = 0;
    char *tok = strtok(input, " \t\n");
    while (tok && argc < MAX_ARGS - 1) {
        if      (!strcmp(tok, "&")) { *background = 1; }
        else if (!strcmp(tok, "<")) {
            if ((tok = strtok(NULL, " \t\n")))
                *in_file = expand_env_vars(tok, exp_in, MAX_LINE);
        }
        else if (!strcmp(tok, ">")) {
            if ((tok = strtok(NULL, " \t\n")))
                *out_file = expand_env_vars(tok, exp_out, MAX_LINE);
        }
        else if (!strcmp(tok, "|")) { *pipe_pos = argc; args[argc++] = NULL; }
        else {
            expand_env_vars(tok, exp_bufs[argc], MAX_LINE);
            args[argc] = exp_bufs[argc];
            argc++;
        }
        tok = strtok(NULL, " \t\n");
    }
    args[argc] = NULL;
    return argc;
}

/* ── Executor ──────────────────────────────────────────────────── */

static void run_command(char *args[], int background, char *in_file, char *out_file)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        if (in_file) {
            int fd = open(in_file, O_RDONLY);
            if (fd < 0) { perror(in_file); exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (out_file) {
            int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror(out_file); exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }
        execvp(args[0], args); perror(args[0]); exit(1);
    }
    if (!background) waitpid(pid, NULL, 0);
}

static void run_pipe(char *args[], int pipe_pos)
{
    char **cmd1 = args, **cmd2 = args + pipe_pos + 1;
    int fd[2];
    if (pipe(fd) < 0) { perror("pipe"); return; }

    pid_t p1 = fork();
    if (p1 == 0) {
        close(fd[0]); dup2(fd[1], STDOUT_FILENO); close(fd[1]);
        execvp(cmd1[0], cmd1); perror(cmd1[0]); exit(1);
    }
    pid_t p2 = fork();
    if (p2 == 0) {
        close(fd[1]); dup2(fd[0], STDIN_FILENO); close(fd[0]);
        execvp(cmd2[0], cmd2); perror(cmd2[0]); exit(1);
    }
    close(fd[0]); close(fd[1]);
    waitpid(p1, NULL, 0); waitpid(p2, NULL, 0);
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    char input[MAX_LINE];
    char *args[MAX_ARGS];
    int background, pipe_pos;
    char *in_file, *out_file;

    atexit(disable_raw_mode);

    while (1) {
        /* Build prompt */
        char cwd[1024], hostname[256], prompt[512];
        const char *home = getenv("HOME");
        const char *user = getenv("USER");
        if (!user) user = getenv("USERNAME");
        getcwd(cwd, sizeof(cwd));
        gethostname(hostname, sizeof(hostname));
        char *dot = strchr(hostname, '.'); if (dot) *dot = 0;
        const char *display = cwd;
        if (home && !strncmp(cwd, home, strlen(home)))
            display = cwd + strlen(home) - 1, cwd[strlen(home) - 1] = '~';
        snprintf(prompt, sizeof(prompt), "[slopsh] %s@%s %s> ",
                 user ? user : "?", hostname, display);

        if (read_line(input, MAX_LINE, prompt) < 0) break;
        if (!strlen(input)) continue;

        /* !! — repeat last command */
        if (!strcmp(input, "!!")) {
            if (!hist_count) { puts("No commands in history."); continue; }
            puts(history_get(1));
            strncpy(input, history_get(1), MAX_LINE - 1);
        } else {
            history_add(input);
        }

        if (!strcmp(input, "exit")) break;

        int argc = parse_input(input, args, &background, &in_file, &out_file, &pipe_pos);
        if (argc == 0) continue;

        /* Built-ins */
        if (!strcmp(args[0], "cd")) {
            const char *h = getenv("HOME");
            const char *arg = args[1];
            char expanded[1024];
            if (!arg || !strcmp(arg, "~")) {
                arg = h;
            } else if (arg[0] == '~' && arg[1] == '/') {
                snprintf(expanded, sizeof(expanded), "%s/%s", h, arg + 2);
                arg = expanded;
            }
            if (chdir(arg) < 0) perror(arg);
            continue;
        }
        if (!strcmp(args[0], "pwd"))    { char p[1024]; if (getcwd(p, sizeof(p))) puts(p); else perror("pwd"); continue; }
        if (!strcmp(args[0], "export")) { if (args[1] && putenv(args[1]) < 0) perror("export"); continue; }
        if (!strcmp(args[0], "unset"))  { if (args[1]) unsetenv(args[1]); continue; }

        if (pipe_pos >= 0) run_pipe(args, pipe_pos);
        else               run_command(args, background, in_file, out_file);
    }

    return 0;
}
