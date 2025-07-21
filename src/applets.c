#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "applets.h"

int bx_true_main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: true [ignored command line arguments]");
        puts("  or:  true OPTION");
        puts("Exit with a status code indicating success.");
        puts("");
        puts("      --help");
        puts("         display this help and exit");
        puts("      --version");
        puts("         output version information and exit");
        puts("");
        puts("Your shell may have its own version of true, which usually supersedes");
        puts("the version described here.  Please refer to your shell's documentation");
        puts("for details about the options it supports.");
        puts("");
        puts("Report bugs to: bug-coreutils@gnu.org");
        puts("GNU coreutils home page: <https://www.gnu.org/software/coreutils/>");
        puts("General help using GNU software: <https://www.gnu.org/gethelp/>");
        puts("Report any translation bugs to <https://translationproject.org/team/>");
        puts("Full documentation <https://www.gnu.org/software/coreutils/true>");
        puts("or available locally via: info '(coreutils) true invocation'");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("true (GNU coreutils) 9.11");
        puts("Copyright (C) 2026 Free Software Foundation, Inc.");
        puts("License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.");
        puts("This is free software: you are free to change and redistribute it.");
        puts("There is NO WARRANTY, to the extent permitted by law.");
        puts("");
        puts("Written by Jim Meyering.");
        return 0;
    }

    (void)argc;
    (void)argv;
    return 0;
}

int bx_false_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return 1;
}
