#ifndef BX_SEARCH_SEARCH_H
#define BX_SEARCH_SEARCH_H

enum bx_search_personality {
    BX_SEARCH_GREP,
    BX_SEARCH_EGREP,
    BX_SEARCH_FGREP,
    BX_SEARCH_RG,
};

int bx_search_main(int argc, char **argv, enum bx_search_personality personality);

#endif
