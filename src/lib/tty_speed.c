#include <string.h>
#include <termios.h>

#include "lib/tty_speed.h"

static const struct bx_tty_speed_entry bx_tty_speed_table[] = {
#ifdef B0
    {"0", B0, 0},
#endif
#ifdef B50
    {"50", B50, 50},
#endif
#ifdef B75
    {"75", B75, 75},
#endif
#ifdef B110
    {"110", B110, 110},
#endif
#ifdef B134
    {"134", B134, 134},
#endif
#ifdef B150
    {"150", B150, 150},
#endif
#ifdef B200
    {"200", B200, 200},
#endif
#ifdef B300
    {"300", B300, 300},
#endif
#ifdef B600
    {"600", B600, 600},
#endif
#ifdef B1200
    {"1200", B1200, 1200},
#endif
#ifdef B1800
    {"1800", B1800, 1800},
#endif
#ifdef B2400
    {"2400", B2400, 2400},
#endif
#ifdef B4800
    {"4800", B4800, 4800},
#endif
#ifdef B9600
    {"9600", B9600, 9600},
#endif
#ifdef B19200
    {"19200", B19200, 19200},
#endif
#ifdef B38400
    {"38400", B38400, 38400},
#endif
#ifdef EXTA
    {"exta", EXTA, 19200},
#endif
#ifdef EXTB
    {"extb", EXTB, 38400},
#endif
#ifdef B57600
    {"57600", B57600, 57600},
#endif
#ifdef B115200
    {"115200", B115200, 115200},
#endif
#ifdef B230400
    {"230400", B230400, 230400},
#endif
#ifdef B460800
    {"460800", B460800, 460800},
#endif
#ifdef B500000
    {"500000", B500000, 500000},
#endif
#ifdef B576000
    {"576000", B576000, 576000},
#endif
#ifdef B921600
    {"921600", B921600, 921600},
#endif
#ifdef B1000000
    {"1000000", B1000000, 1000000},
#endif
#ifdef B1152000
    {"1152000", B1152000, 1152000},
#endif
#ifdef B1500000
    {"1500000", B1500000, 1500000},
#endif
#ifdef B2000000
    {"2000000", B2000000, 2000000},
#endif
#ifdef B2500000
    {"2500000", B2500000, 2500000},
#endif
#ifdef B3000000
    {"3000000", B3000000, 3000000},
#endif
#ifdef B3500000
    {"3500000", B3500000, 3500000},
#endif
#ifdef B4000000
    {"4000000", B4000000, 4000000},
#endif
};

size_t bx_tty_speed_entry_count(void) {
    return sizeof(bx_tty_speed_table) / sizeof(bx_tty_speed_table[0]);
}

const struct bx_tty_speed_entry* bx_tty_speed_entry_at(size_t index) {
    if (index >= bx_tty_speed_entry_count()) {
        return NULL;
    }

    return &bx_tty_speed_table[index];
}

const struct bx_tty_speed_entry* bx_tty_speed_lookup(const char* name) {
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < bx_tty_speed_entry_count(); i++) {
        if (strcmp(name, bx_tty_speed_table[i].name) == 0) {
            return &bx_tty_speed_table[i];
        }
    }

    return NULL;
}

bool bx_tty_speed_parse(const char* name, speed_t* speed_out) {
    if (speed_out == NULL) {
        return false;
    }

    const struct bx_tty_speed_entry* entry = bx_tty_speed_lookup(name);
    if (entry == NULL) {
        return false;
    }

    *speed_out = entry->speed;
    return true;
}

unsigned int bx_tty_speed_to_baud(speed_t speed) {
    for (size_t i = 0; i < bx_tty_speed_entry_count(); i++) {
        if (bx_tty_speed_table[i].speed == speed) {
            return bx_tty_speed_table[i].baud;
        }
    }

    return (unsigned int)speed;
}
