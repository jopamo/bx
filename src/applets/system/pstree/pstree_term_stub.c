#include <curses.h>
#include <term.h>

int bx_pstree_no_termcap_setupterm(const char* term, int fd, int* errret) {
    (void)term;
    (void)fd;
    if (errret != NULL) {
        *errret = 0;
    }
    return ERR;
}

char* bx_pstree_no_termcap_tigetstr(const char* capname) {
    (void)capname;
    return (char*)-1;
}

int bx_pstree_no_termcap_tgetent(char* buffer, const char* termtype) {
    (void)buffer;
    (void)termtype;
    return 0;
}

char* bx_pstree_no_termcap_tgetstr(const char* id, char** area) {
    (void)id;
    (void)area;
    return NULL;
}

int bx_pstree_no_termcap_tputs(const char* str, int affcnt, int (*putc_fn)(int)) {
    (void)str;
    (void)affcnt;
    (void)putc_fn;
    return ERR;
}
