#include "walk_internal.h"

bool bx_walk_ancestor_contains(const struct bx_walk_ancestor *anc, dev_t dev, ino_t ino) {
    for (const struct bx_walk_ancestor *it = anc; it; it = it->parent) {
        if (it->dev == dev && it->ino == ino)
            return true;
    }
    return false;
}
