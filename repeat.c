#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   // for getopt()

int main(int argc, char *argv[]) {
    int opt;
    int repeats = 1;        // default
    int no_newline = 0;

    // Parse options: -r <count> and -n
    while ((opt = getopt(argc, argv, "r:n")) != -1) {
        switch (opt) {
            case 'r':
                repeats = atoi(optarg);   // convert string to int
                break;
            case 'n':
                no_newline = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-r count] [-n] msg\n", argv[0]);
                return 1;
        }
    }

    // The message is the first non-option argument
    if (optind >= argc) {
        fprintf(stderr, "No message given!\n");
        return 1;
    }
    char *msg = argv[optind];

    // Print the message
    for (int i = 0; i < repeats; i++) {
        for (char *p = msg; *p; p++) {
            putchar(*p);   // print one char at a time
        }
        if (!no_newline) {
            putchar('\n');
        }
    }

    return 0;
}

