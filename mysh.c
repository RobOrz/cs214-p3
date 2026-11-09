#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define BUFSIZE 1024

static int interactive = 0;     
static int exit_requested = 0;  


static void say(const char *s)
{
    (void)write(STDOUT_FILENO, s, strlen(s));
}


static void print_prompt(void)
{
    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        say("$ ");
        return;
    }

    const char *home = getenv("HOME");
    size_t home_len = (home != NULL) ? strlen(home) : 0;


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


static void run_line(char *line)
{
    say("[command] ");
    say(line);
    say("\n");
    if (strcmp(line, "exit") == 0) {
        exit_requested = 1;
    }
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
        interactive = 0;               
    } else {
        interactive = isatty(STDIN_FILENO);
    }

    char buf[BUFSIZE];
    char *line = NULL;                 
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
        if (n == 0) {                 
            if (len > 0) {             
                line[len] = '\0';
                run_line(line);
                len = 0;
            }
            break;
        }

        for (ssize_t i = 0; i < n && !exit_requested; i++) {
           
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
