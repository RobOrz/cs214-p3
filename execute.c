#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "execute.h"

/*
 * This file contains the part of the shell that runs a parsed job.
 * mysh.c reads input, parse.c builds the job, and this file executes it.
 */

/* Make a normal result. Most shell errors use exit code 1. */
static struct job_result normal_result(int exit_code)
{
    struct job_result result;
    result.exit_code = exit_code;
    result.signal = 0;
    return result;
}

/* Convert the value from waitpid() into a simpler result for mysh.c. */
static struct job_result wait_result(int status)
{
    struct job_result result = normal_result(1);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }

    return result;
}

/* Return a newly allocated path for a program, or NULL if it is not found. */
static char *find_program(const char *name)
{
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) == 0) {
            return strdup(name);
        }
        return NULL;
    }

    const char *directories[] = { "/usr/local/bin", "/usr/bin", "/bin" };
    int count = sizeof(directories) / sizeof(directories[0]);

    for (int i = 0; i < count; i++) {
        size_t size = strlen(directories[i]) + 1 + strlen(name) + 1;
        char *path = malloc(size);
        if (path == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }

        snprintf(path, size, "%s/%s", directories[i], name);
        if (access(path, X_OK) == 0) {
            return path;
        }
        free(path);
    }

    return NULL;
}

/* Return 1 when the command is implemented by mysh itself. */
static int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
           strcmp(name, "which") == 0 || strcmp(name, "exit") == 0;
}

/* Keep writing until the full string is written or an error occurs. */
static int write_all(int fd, const char *text, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        text += written;
        length -= written;
    }
    return 0;
}

/* Write one line to standard output without using buffered stdio. */
static int print_line(const char *text)
{
    if (write_all(STDOUT_FILENO, text, strlen(text)) < 0 ||
        write_all(STDOUT_FILENO, "\n", 1) < 0) {
        perror("write");
        return -1;
    }
    return 0;
}

/* Change the shell's working directory. */
static int builtin_cd(struct command *cmd)
{
    if (cmd->argc > 2) {
        (void)write(STDERR_FILENO, "cd: too many arguments\n", 23);
        return 1;
    }

    const char *directory = (cmd->argc == 1) ? getenv("HOME") : cmd->argv[1];
    if (directory == NULL) {
        (void)write(STDERR_FILENO, "cd: HOME is not set\n", 20);
        return 1;
    }
    if (chdir(directory) < 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

/* Print the current working directory. */
static int builtin_pwd(struct command *cmd)
{
    if (cmd->argc != 1) {
        (void)write(STDERR_FILENO, "pwd: too many arguments\n", 24);
        return 1;
    }

    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        perror("pwd");
        return 1;
    }
    int printed = print_line(cwd);
    free(cwd);
    return (printed == 0) ? 0 : 1;
}

/* Print the executable path for every argument. */
static int builtin_which(struct command *cmd)
{
    if (cmd->argc == 1) {
        (void)write(STDERR_FILENO, "which: missing argument\n", 24);
        return 1;
    }

    int success = 1;
    for (int i = 1; i < cmd->argc; i++) {
        char *path = find_program(cmd->argv[i]);
        if (path == NULL) {
            success = 0;
        } else {
            if (print_line(path) < 0) {
                success = 0;
            }
            free(path);
        }
    }
    return success ? 0 : 1;
}

/* Run a built-in command. exit intentionally ignores all arguments. */
static int run_builtin(struct command *cmd)
{
    if (strcmp(cmd->argv[0], "cd") == 0) {
        return builtin_cd(cmd);
    }
    if (strcmp(cmd->argv[0], "pwd") == 0) {
        return builtin_pwd(cmd);
    }
    if (strcmp(cmd->argv[0], "which") == 0) {
        return builtin_which(cmd);
    }
    return 0;                           /* exit */
}

/* Close every pipe. Each child only keeps the copies made with dup2(). */
static void close_pipes(int (*pipes)[2], int npipes)
{
    for (int i = 0; i < npipes; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Set up standard input and output for one command in a job. */
static int set_up_io(struct command *cmd, int position, int ncmds,
                     int (*pipes)[2], int npipes, int interactive)
{
    int input_fd = -1;
    int output_fd = -1;
    int input_is_file = 0;
    int output_is_file = 0;

    if (cmd->infile != NULL) {
        input_fd = open(cmd->infile, O_RDONLY);
        if (input_fd < 0) {
            perror(cmd->infile);
            return -1;
        }
        input_is_file = 1;
    } else if (position > 0) {
        input_fd = pipes[position - 1][0];
    } else if (!interactive) {
        input_fd = open("/dev/null", O_RDONLY);
        if (input_fd < 0) {
            perror("/dev/null");
            return -1;
        }
        input_is_file = 1;
    }

    if (cmd->outfile != NULL) {
        int flags = O_WRONLY | O_CREAT;
        flags |= cmd->append ? O_APPEND : O_TRUNC;
        output_fd = open(cmd->outfile, flags, 0640);
        if (output_fd < 0) {
            perror(cmd->outfile);
            goto io_error;
        }
        output_is_file = 1;
    } else if (position < ncmds - 1) {
        output_fd = pipes[position][1];
    }

    if (input_fd >= 0 && dup2(input_fd, STDIN_FILENO) < 0) {
        perror("dup2");
        goto io_error;
    }
    if (output_fd >= 0 && dup2(output_fd, STDOUT_FILENO) < 0) {
        perror("dup2");
        goto io_error;
    }

    close_pipes(pipes, npipes);

    if (input_is_file) {
        close(input_fd);
    }
    if (output_is_file) {
        close(output_fd);
    }

    return 0;

io_error:
    close_pipes(pipes, npipes);
    if (input_is_file) {
        close(input_fd);
    }
    if (output_is_file) {
        close(output_fd);
    }
    return -1;
}

/*
 * A built-in by itself runs in the shell process. This is what lets cd
 * change the shell's directory. Save and restore standard input/output so
 * redirection only lasts for this one command.
 */
static struct job_result run_builtin_in_shell(struct command *cmd,
                                              int interactive)
{
    int saved_input = dup(STDIN_FILENO);
    int saved_output = dup(STDOUT_FILENO);
    int exit_code = 1;

    if (saved_input < 0 || saved_output < 0) {
        perror("dup");
    } else if (set_up_io(cmd, 0, 1, NULL, 0, interactive) == 0) {
        exit_code = run_builtin(cmd);
    }

    if (saved_input >= 0) {
        if (dup2(saved_input, STDIN_FILENO) < 0) {
            perror("dup2");
            exit_code = 1;
        }
        close(saved_input);
    }
    if (saved_output >= 0) {
        if (dup2(saved_output, STDOUT_FILENO) < 0) {
            perror("dup2");
            exit_code = 1;
        }
        close(saved_output);
    }

    return normal_result(exit_code);
}

/* Execute all commands in a job and wait for every child process. */
struct job_result run_job(struct job *job, int interactive)
{
    int ncmds = job->ncmds;
    int npipes = ncmds - 1;
    int (*pipes)[2] = NULL;
    pid_t *pids = NULL;
    char **paths = NULL;
    int started = 0;
    int fork_failed = 0;
    struct job_result result = normal_result(1);

    /* Standalone built-ins must run in the shell instead of a child. */
    if (ncmds == 1 && is_builtin(job->cmds[0].argv[0])) {
        return run_builtin_in_shell(&job->cmds[0], interactive);
    }

    pids = calloc(ncmds, sizeof(pid_t));
    paths = calloc(ncmds, sizeof(char *));
    if (pids == NULL || paths == NULL) {
        perror("calloc");
        goto cleanup;
    }

    /* Resolve all external program names before starting the pipeline. */
    for (int i = 0; i < ncmds; i++) {
        if (!is_builtin(job->cmds[i].argv[0])) {
            paths[i] = find_program(job->cmds[i].argv[0]);
            if (paths[i] == NULL) {
                (void)write(STDERR_FILENO, job->cmds[i].argv[0],
                            strlen(job->cmds[i].argv[0]));
                (void)write(STDERR_FILENO, ": command not found\n", 20);
                goto cleanup;
            }
        }
    }

    if (npipes > 0) {
        pipes = calloc(npipes, sizeof(int[2]));
        if (pipes == NULL) {
            perror("calloc");
            goto cleanup;
        }
        for (int i = 0; i < npipes; i++) {
            if (pipe(pipes[i]) < 0) {
                perror("pipe");
                close_pipes(pipes, i);
                free(pipes);
                pipes = NULL;
                goto cleanup;
            }
        }
    }

    for (int i = 0; i < ncmds; i++) {
        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            fork_failed = 1;
            break;
        }

        if (child == 0) {
            if (set_up_io(&job->cmds[i], i, ncmds, pipes, npipes,
                          interactive) != 0) {
                _exit(1);
            }

            /* Built-ins in a pipeline run in a child like other commands. */
            if (is_builtin(job->cmds[i].argv[0])) {
                _exit(run_builtin(&job->cmds[i]));
            }

            execv(paths[i], job->cmds[i].argv);
            perror(paths[i]);
            _exit(1);
        }

        pids[i] = child;
        started++;
    }

    close_pipes(pipes, npipes);

    for (int i = 0; i < started; i++) {
        int status;
        pid_t waited;

        do {
            waited = waitpid(pids[i], &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0) {
            perror("waitpid");
            fork_failed = 1;
        } else if (i == ncmds - 1) {
            result = wait_result(status);
        }
    }

    /* A partial pipeline is always a failed command. */
    if (fork_failed || started != ncmds) {
        result = normal_result(1);
    }

cleanup:
    if (paths != NULL) {
        for (int i = 0; i < ncmds; i++) {
            free(paths[i]);
        }
    }
    free(paths);
    free(pids);
    free(pipes);
    return result;
}
