#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "job.h"

/* ---------- growable array of strings ---------- */

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

/* Append s (takes ownership); always keeps room for a NULL terminator */
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

/* ---------- tokenizer ---------- */

/* Split line into whitespace-separated tokens, stopping at '#' (comment). */
static void tokenize(const char *line, struct strlist *tokens)
{
    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;                      /* end of line or start of comment */
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

/* ---------- wildcards ---------- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char * const *)a, *(char * const *)b);
}

/* Expand a token containing '*' and append the results to args.
   The '*' may only appear in the last path component (e.g. "baz/foo*bar").
   Matching names are added in sorted order; if nothing matches, the token
   itself is added unchanged. */
static void expand_wildcard(const char *tok, struct strlist *args)
{
    const char *slash = strrchr(tok, '/');
    const char *pattern = (slash != NULL) ? slash + 1 : tok;
    const char *star = strchr(pattern, '*');

    if (star == NULL) {                 /* '*' only in a directory part: not supported */
        sl_push(args, strdup(tok));
        return;
    }

    /* directory to search, and the text to put in front of each match */
    size_t dir_len = (slash != NULL) ? (size_t)(slash - tok) : 0;
    char *dir = (slash == NULL) ? strdup(".")
              : (dir_len == 0) ? strdup("/")          /* pattern like "/foo*" */
              : dup_range(tok, dir_len);
    char *front = dup_range(tok, (slash != NULL) ? dir_len + 1 : 0);

    size_t pre_len = (size_t)(star - pattern);
    const char *suffix = star + 1;
    size_t suf_len = strlen(suffix);

    struct strlist matches;
    sl_init(&matches);

    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            size_t name_len = strlen(name);

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            if (pre_len == 0 && name[0] == '.') {
                continue;               /* "*..." never matches hidden files */
            }
            if (name_len < pre_len + suf_len) {
                continue;
            }
            if (strncmp(name, pattern, pre_len) != 0) {
                continue;               /* wrong beginning */
            }
            if (strcmp(name + name_len - suf_len, suffix) != 0) {
                continue;               /* wrong ending */
            }

            /* match: build front + name, e.g. "baz/" + "foo1bar" */
            char *full = malloc(strlen(front) + name_len + 1);
            if (full == NULL) {
                perror("malloc");
                exit(EXIT_FAILURE);
            }
            strcpy(full, front);
            strcat(full, name);
            sl_push(&matches, full);
        }
        closedir(d);
    }

    if (matches.len == 0) {
        sl_push(args, strdup(tok));     /* no match: pass the token unchanged */
    } else {
        qsort(matches.items, matches.len, sizeof(char *), cmp_str);
        for (int i = 0; i < matches.len; i++) {
            sl_push(args, matches.items[i]);   /* ownership moves to args */
        }
        free(matches.items);
    }

    free(dir);
    free(front);
}

/* ---------- parser ---------- */

/* Finish the command being built and add it to the job */
static void add_command(struct job *job, struct strlist *args,
                        char *infile, char *outfile, int append)
{
    job->cmds = realloc(job->cmds, (job->ncmds + 1) * sizeof(struct command));
    if (job->cmds == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    struct command *c = &job->cmds[job->ncmds++];
    c->argv = args->items;      /* already NULL-terminated by sl_push */
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

    if (tokens.len == 0) {              /* empty line or comment only */
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
                sl_init(&args);         /* ownership moved into the job */
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
            /* ordinary argument; a token with '*' is a wildcard */
            if (strchr(t, '*') != NULL) {
                expand_wildcard(t, &args);
            } else {
                sl_push(&args, strdup(t));
            }
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
