#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GB(x) ((size_t)(x) << 30)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <size-in-GB>\n", argv[0]);
        return 1;
    }

    size_t size = GB(atol(argv[1]));
    char *buffer = malloc(size);
    if (!buffer) {
        perror("malloc failed");
        return 1;
    }

    // Write one byte per page to force physical allocation
    size_t pagesize = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < size; i += pagesize) {
        buffer[i] = 1;
    }

    printf("Allocated and touched %lu GB of memory.\n", size >> 30);
    pause(); // Keep process alive to inspect with tools like `top`, `smem`, etc.

    free(buffer);
    return 0;
}