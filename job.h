#ifndef JOB_H
#define JOB_H

/* One program in a pipeline */
struct command {
    char **argv;     /* NULL-terminated argument list (wildcards expanded) */
    int argc;
    char *infile;    /* file after <, or NULL */
    char *outfile;   /* file after > or >>, or NULL */
    int append;      /* 1 if >>, 0 if > */
};

struct job {
    struct command *cmds;
    int ncmds;
};

int parse_line(const char *line, struct job *job);
void free_job(struct job *job);

#endif
