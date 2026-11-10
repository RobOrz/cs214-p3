#ifndef EXECUTE_H
#define EXECUTE_H

#include "job.h"

/* A job either exits normally with a code, or is stopped by a signal. */
struct job_result {
    int exit_code;
    int signal;
};

/* Run one parsed job. interactive controls the default input. */
struct job_result run_job(struct job *job, int interactive);

#endif
