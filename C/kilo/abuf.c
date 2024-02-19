// #include <unistd.h>
#include "abuf.h"
#include <stdlib.h>
#include <string.h>

void abuf_append(struct abuf *ab, const char *s, int len) {
    if (len <= 0) {
        return;
    }
    char *new = realloc(ab->s, ab->len + len);
    if (new == NULL) {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->s = new;
    ab->len += len;
}

void abuf_free(struct abuf *ab) {
    free(ab->s);
    ab->len = 0;
}
