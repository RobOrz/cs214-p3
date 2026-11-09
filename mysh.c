#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "job.h"

#define BUFSIZE 1024

static int interactive = 0;     /* 1 if reading commands from a terminal */
static int exit_requested = 0;  /* set by the exit built-in */

/* Write a string to standard output */
static void say(const char *s)
{
    (void)write(STDOUT_FILENO, s, strlen(s));
}

/* Print the prompt: working directory with $HOME replaced by ~, then "$ " */
static void print_prompt(void)
{
    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        say("$ ");
        return;
    }

    const char *home = getenv("HOME");
    size_t home_len = (home != NULL) ? strlen(home) : 0;

    /* Only replace HOME if it is a whole path prefix: /home/user or /home/user/... */
    if (home_len > 0 && strncmp(cwd, home, home_len) == 0 &&
        (cwd[home_len] == '\0' || cwd[home_len] == '/')) {
        say("~");
        say(cwd + home_len);
    } else {
        say(cwd);
    }
    say("$ ");
    free(cwd);
}

/* Parse one command line and run it */
static void run_line(char *line)
{
    struct job job;
    if (parse_line(line, &job) != 0) {
        return;                         /* syntax error: the command fails */
    }
    if (job.ncmds == 0) {
        return;                         /* empty line or comment only */
    }

    /* TEMPORARY: show the parsed job until run_job() is ready */
    for (int i = 0; i < job.ncmds; i++) {
        struct command *c = &job.cmds[i];
        say("[cmd]");
        for (int j = 0; j < c->argc; j++) {
            say(" ");
            say(c->argv[j]);
        }
        if (c->infile != NULL) {
            say("  < ");
            say(c->infile);
        }
        if (c->outfile != NULL) {
            say(c->append ? "  >> " : "  > ");
            say(c->outfile);
        }
        say("\n");
    }

    /* exit anywhere in the command ends the shell once the command is done */
    for (int i = 0; i < job.ncmds; i++) {
        if (strcmp(job.cmds[i].argv[0], "exit") == 0) {
            exit_requested = 1;
        }
    }

    free_job(&job);
}

int main(int argc, char *argv[])
{
    int fd = STDIN_FILENO;

    if (argc > 2) {
        (void)write(STDERR_FILENO, "Usage: mysh [script]\n", 21);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
        interactive = 0;               /* a script file is always batch mode */
    } else {
        interactive = isatty(STDIN_FILENO);
    }

    char buf[BUFSIZE];
    char *line = NULL;                 /* the command being assembled */
    size_t len = 0, cap = 0;

    if (interactive) {
        say("Welcome to my shell!\n");
        print_prompt();
    }

    while (!exit_requested) {
        ssize_t n = read(fd, buf, BUFSIZE);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            break;
        }
        if (n == 0) {                  /* end of input */
            if (len > 0) {             /* last line had no trailing newline */
                line[len] = '\0';
                run_line(line);
                len = 0;
            }
            break;
        }

        for (ssize_t i = 0; i < n && !exit_requested; i++) {
            /* keep room for this character plus the terminating '\0' */
            if (len + 2 > cap) {
                cap = (cap == 0) ? 128 : cap * 2;
                char *bigger = realloc(line, cap);
                if (bigger == NULL) {
                    perror("realloc");
                    free(line);
                    return EXIT_FAILURE;
                }
                line = bigger;
            }

            if (buf[i] == '\n') {
                /* full command received: run it before reading any further */
                line[len] = '\0';
                run_line(line);
                len = 0;
                if (interactive && !exit_requested) {
                    print_prompt();
                }
            } else {
                line[len++] = buf[i];
            }
        }
    }

    if (interactive) {
        say("Exiting my shell.\n");
    }

    free(line);
    if (fd != STDIN_FILENO) {
        close(fd);
    }
    return EXIT_SUCCESS;
}
