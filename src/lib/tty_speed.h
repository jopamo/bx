#ifndef BX_COMMON_TTY_SPEED_H
#define BX_COMMON_TTY_SPEED_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

struct bx_tty_speed_entry {
    const char* name;
    speed_t speed;
    unsigned int baud;
};

size_t bx_tty_speed_entry_count(void);
const struct bx_tty_speed_entry* bx_tty_speed_entry_at(size_t index);
const struct bx_tty_speed_entry* bx_tty_speed_lookup(const char* name);
bool bx_tty_speed_parse(const char* name, speed_t* speed_out);
unsigned int bx_tty_speed_to_baud(speed_t speed);

#endif /* BX_COMMON_TTY_SPEED_H */
