#include "sovd/adapter.h"

#include <stdlib.h>

void sovd_default_free_faults(sovd_fault_t *faults, size_t count) {
    (void)count;
    free(faults);
}

void sovd_default_free_buffer(sovd_buffer_t *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

void sovd_default_free_string(char *str) {
    free(str);
}
