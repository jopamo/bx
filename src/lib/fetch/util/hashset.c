#define _GNU_SOURCE
#include "lib/fetch/hashset.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct HashSetNode {
    char* key;
    struct HashSetNode* next;
} HashSetNode;

struct HashSet {
    HashSetNode** buckets;
    size_t size;
    size_t count;
};

static uint32_t hash_string(const char* s) {
    uint32_t hash = 5381;
    int c;
    while ((c = *s++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

HashSet* hashset_new(size_t size) {
    if (size == 0)
        return NULL;

    HashSet* hs = calloc(1, sizeof(HashSet));
    if (!hs)
        return NULL;

    hs->size = size;
    hs->buckets = calloc(size, sizeof(HashSetNode*));
    if (!hs->buckets) {
        free(hs);
        return NULL;
    }
    return hs;
}

void hashset_free(HashSet* hs) {
    if (!hs)
        return;
    for (size_t i = 0; i < hs->size; i++) {
        HashSetNode* n = hs->buckets[i];
        while (n) {
            HashSetNode* next = n->next;
            free(n->key);
            free(n);
            n = next;
        }
    }
    free(hs->buckets);
    free(hs);
}

bool hashset_contains(HashSet* hs, const char* key) {
    if (!hs || !key || hs->size == 0)
        return false;

    uint32_t h = hash_string(key) % hs->size;
    HashSetNode* n = hs->buckets[h];
    while (n) {
        if (strcmp(n->key, key) == 0)
            return true;
        n = n->next;
    }
    return false;
}

bool hashset_add(HashSet* hs, const char* key) {
    if (!hs || !key || hs->size == 0)
        return false;

    uint32_t h = hash_string(key) % hs->size;
    HashSetNode* n = hs->buckets[h];
    while (n) {
        if (strcmp(n->key, key) == 0)
            return false;  // Already exists
        n = n->next;
    }

    n = malloc(sizeof(HashSetNode));
    if (!n)
        return false;

    n->key = strdup(key);
    if (!n->key) {
        free(n);
        return false;
    }
    n->next = hs->buckets[h];
    hs->buckets[h] = n;
    hs->count++;
    return true;
}

bool hashset_remove(HashSet* hs, const char* key) {
    if (!hs || !key || hs->size == 0)
        return false;

    uint32_t h = hash_string(key) % hs->size;
    HashSetNode** link = &hs->buckets[h];
    while (*link) {
        HashSetNode* node = *link;
        if (strcmp(node->key, key) == 0) {
            *link = node->next;
            free(node->key);
            free(node);
            hs->count--;
            return true;
        }
        link = &node->next;
    }
    return false;
}
