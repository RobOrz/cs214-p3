#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "job.h"



struct strlist {
    char **items;
    int len;
    int cap;
};

static void sl_init(struct strlist *l)
{
    l->items = NULL;
    l->len = 0;
    l->cap = 0;
}


static void sl_push(struct strlist *l, char *s)
{
    if (l->len + 2 > l->cap) {
        l->cap = (l->cap == 0) ? 8 : l->cap * 2;
        l->items = realloc(l->items, l->cap * sizeof(char *));
        if (l->items == NULL) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
    }
    l->items[l->len++] = s;
    l->items[l->len] = NULL;
}

static void sl_free(struct strlist *l)
{
    for (int i = 0; i < l->len; i++) {
        free(l->items[i]);
    }
    free(l->items);
    sl_init(l);
}

static char *dup_range(const char *start, size_t n)
{
    char *s = malloc(n + 1);
    if (s == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}


static void tokenize(const char *line, struct strlist *tokens)
{
    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;                      
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '#') {
            p++;
        }
        sl_push(tokens, dup_range(start, p - start));
    }
}

static int is_redirect(const char *t)
{
    return strcmp(t, "<") == 0 || strcmp(t, ">") == 0 || strcmp(t, ">>") == 0;
}

static int is_special(const char *t)
{
    return is_redirect(t) || strcmp(t, "|") == 0;
}

static void syntax_error(const char *msg)
{
    (void)write(STDERR_FILENO, "mysh: syntax error: ", 20);
    (void)write(STDERR_FILENO, msg, strlen(msg));
    (void)write(STDERR_FILENO, "\n", 1);
}


static void add_command(struct job *job, struct strlist *args,
                        char *infile, char *outfile, int append)
{
    job->cmds = realloc(job->cmds, (job->ncmds + 1) * sizeof(struct command));
    if (job->cmds == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    struct command *c = &job->cmds[job->ncmds++];
    c->argv = args->items;      
    c->argc = args->len;
    c->infile = infile;
    c->outfile = outfile;
    c->append = append;
}

int parse_line(const char *line, struct job *job)
{
    struct strlist tokens;
    sl_init(&tokens);
    tokenize(line, &tokens);

    job->cmds = NULL;
    job->ncmds = 0;

    if (tokens.len == 0) {              
        sl_free(&tokens);
        return 0;
    }

    struct strlist args;
    sl_init(&args);
    char *infile = NULL, *outfile = NULL;
    int append = 0;
    int error = 0;

    for (int i = 0; i < tokens.len && !error; i++) {
        char *t = tokens.items[i];

        if (strcmp(t, "|") == 0) {
            if (args.len == 0) {
                syntax_error("missing command before |");
                error = 1;
            } else if (i == tokens.len - 1) {
                syntax_error("missing command after |");
                error = 1;
            } else {
                add_command(job, &args, infile, outfile, append);
                sl_init(&args);         
                infile = outfile = NULL;
                append = 0;
            }
        } else if (is_redirect(t)) {
            if (i + 1 >= tokens.len || is_special(tokens.items[i + 1])) {
                syntax_error("missing file name after redirection");
                error = 1;
            } else {
                char *file = strdup(tokens.items[++i]);
                if (strcmp(t, "<") == 0) {
                    free(infile);
                    infile = file;
                } else {
                    free(outfile);
                    outfile = file;
                    append = (strcmp(t, ">>") == 0);
                }
            }
        } else {
            
            sl_push(&args, strdup(t));
        }
    }

    if (!error) {
        if (args.len == 0) {
            syntax_error("missing command");
            error = 1;
        } else {
            add_command(job, &args, infile, outfile, append);
            sl_init(&args);
            infile = outfile = NULL;
        }
    }

    sl_free(&tokens);
    if (error) {
        sl_free(&args);
        free(infile);
        free(outfile);
        free_job(job);
        return 1;
    }
    return 0;
}

void free_job(struct job *job)
{
    for (int i = 0; i < job->ncmds; i++) {
        struct command *c = &job->cmds[i];
        for (int j = 0; j < c->argc; j++) {
            free(c->argv[j]);
        }
        free(c->argv);
        free(c->infile);
        free(c->outfile);
    }
    free(job->cmds);
    job->cmds = NULL;
    job->ncmds = 0;
}
