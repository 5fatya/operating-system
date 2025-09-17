#define _POSIX_C_SOURCE 200809L
/*
  bench.c — measure command execution time by repeatedly running a command
  - Parses two options:
      -w N   : number of warmup runs (default 0, ignored in stats)
      -d SEC : measure for SEC seconds (default 5)
    After options, everything is the command to execute, e.g.:
      ./bench -w 2 -d 4 -- sleep 1
  - Warmups: run the command N times to “warm the cache”, but do not include
    these runs in min/avg/max/total statistics.
  - Timed loop: for about -d seconds, run the command in a loop:
      parent:  fork() a child
      child:   execvp() to replace itself with the requested command
      parent:  waitpid() for the child to finish and record the runtime
  - Reports: Min / Avg / Max runtime (seconds), total elapsed seconds, number
    of runs completed, number of run failures, and the warmup count.
  - Return code: returns non-zero if any measured run failed.
*/

#include <errno.h>        // errno, EINTR
#include <getopt.h>       // getopt()
#include <stdio.h>        // printf(), fprintf(), perror()
#include <stdlib.h>       // atoi(), atof(), exit()
#include <sys/types.h>    // pid_t
#include <sys/wait.h>     // waitpid(), WIFEXITED, WEXITSTATUS
#include <time.h>         // clock_gettime(), struct timespec
#include <unistd.h>       // fork(), execvp()

/* Global pointer to the command + its arguments (set in main after parsing).
   We keep it global so the helper function run_once() can access it easily. */
static char **cmd_argv = NULL;

/* now_monotonic()
   - Returns current time from a monotonic clock as a double in seconds.
   - "Monotonic" means it only moves forward (unaffected by wall-clock changes),
     which is what we want for measuring durations. */
static double now_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* If this fails, something is wrong with the system call;
           print why and abort the whole program. */
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* run_once(secs_out)
   - Runs the command exactly once and measures how long it took.
   - Returns:
       0  => child exited with status 0 (success)
       1  => child finished but exit status was non-zero (failure)
      -1  => system error in fork/wait (also treated as a failure by caller)
   - On success or failure, *secs_out is set to the run's wall time.
*/
static int run_once(double *secs_out) {
    double t0 = now_monotonic();       // Start timestamp before creating the child

    pid_t pid = fork();                // Create a new process by cloning
    if (pid < 0) {
        /* fork() failed: typically due to resource limits */
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child process branch:
           - Replace the child with the target program.
           - On success, execvp() does not return.
           - On failure (e.g., command not found), it returns -1 and we print why,
             then exit with 127 (a common convention for "command not found"). */
        execvp(cmd_argv[0], cmd_argv);
        perror("execvp");   // Only reached if exec failed
        _exit(127);
    }

    /* Parent process branch:
       - Wait for the child to finish.
       - waitpid() can be interrupted by signals (errno==EINTR), in which case
         we simply retry. */
    int status;
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal: try again
            perror("waitpid");             // Other error: report and mark failure
            return -1;
        }
        break; // Successfully reaped the child
    }

    double t1 = now_monotonic();       // End timestamp after child finished
    if (secs_out) *secs_out = t1 - t0; // Report run duration to caller

    /* Determine success/failure of the child:
       - Success means the child exited "normally" (not killed by a signal)
         AND returned exit status 0.
       - Any non-zero exit status or abnormal termination => failure. */
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;   // success
    } else {
        return 1;   // failure (non-zero exit or signal)
    }
}

int main(int argc, char *argv[]) {
    /* Defaults required by the assignment */
    int    warmups  = 0;     // number of warmup runs (excluded from stats)
    double duration = 5.0;   // measure for this many seconds

    /* Command-line option parsing: only -w and -d are supported.
       Example: ./bench -w 2 -d 4 -- sleep 1
       getopt() moves 'optind' to the first non-option argument (the command). */
    int opt;
    while ((opt = getopt(argc, argv, "w:d:")) != -1) {
        switch (opt) {
        case 'w':
            /* atoi is simple; if you want stricter validation, use strtol */
            warmups = atoi(optarg);
            if (warmups < 0) {
                fprintf(stderr, "Invalid warmup count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'd':
            /* atof is simple; if you want stricter validation, use strtod */
            duration = atof(optarg);
            if (duration <= 0.0) {
                fprintf(stderr, "Invalid duration (seconds): %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        default:
            fprintf(stderr,
                "Usage: %s [-w warmups] [-d seconds] -- command [args...]\n",
                argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* After options, we expect at least one argument: the command to execute. */
    if (optind >= argc) {
        fprintf(stderr,
            "Missing command. Example:\n  %s -w 2 -d 4 -- sleep 1\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Point cmd_argv to the command and its arguments (e.g., {"sleep","1",NULL}) */
    cmd_argv = &argv[optind];

    /* Warmup phase (not included in stats): run the command 'warmups' times.
       We ignore success/failure and timing here, on purpose. */
    for (int i = 0; i < warmups; i++) {
        double ignore_time;
        (void)run_once(&ignore_time);
    }

    /* Measurement phase: run for about 'duration' seconds, collecting stats. */
    double total_start = now_monotonic();  // Start of timed window (excludes warmups)

    /* Stats computed over measured runs only */
    double min_s = 0.0, max_s = 0.0, sum_s = 0.0;
    long   runs  = 0;   // number of runs (successful or failed) completed
    long   fails = 0;   // number of runs whose child exit status was non-zero or errored

    while (1) {
        /* Before starting another run, check if the duration budget has been used.
           This policy matches “run as many times as possible within the window”. */
        double elapsed = now_monotonic() - total_start;
        if (elapsed >= duration) break;

        double secs = 0.0;
        int rc = run_once(&secs);   // 0=success, 1=failure, -1=system error

        /* Update statistics */
        if (runs == 0) {            // initialize min/max on first measured run
            min_s = max_s = secs;
        } else {
            if (secs < min_s) min_s = secs;
            if (secs > max_s) max_s = secs;
        }
        sum_s += secs;
        runs++;

        if (rc != 0) {              // count both 1 and -1 as a failed run
            fails++;
        }
    }

    double total_elapsed = now_monotonic() - total_start;   // window length actually used
    double avg_s = (runs > 0) ? (sum_s / runs) : 0.0;       // avoid div-by-zero

   
    printf("Min: %.6f seconds  Warmups: %d\n", (runs ? min_s : 0.0), warmups);
    printf("Avg: %.6f seconds  Runs: %ld\n", (runs ? avg_s : 0.0), runs);
    printf("Max: %.6f seconds  Fails: %ld\n", (runs ? max_s : 0.0), fails);
    printf("Total: %.6f seconds\n", total_elapsed);

    /* Exit non-zero if any measured run failed, as required. */
    return (fails > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
