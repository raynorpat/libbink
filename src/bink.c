#include "bink.h"

#include <stdio.h>

bink bink_open_file(const char *filename, unsigned char mode)
{
    (void)filename;
    (void)mode;
    return NULL;
}

bink bink_open_filepointer(FILE *file, unsigned char mode)
{
    (void)file;
    (void)mode;
    return NULL;
}

bink bink_open_memory(const unsigned char *buffer, unsigned long size)
{
    (void)buffer;
    (void)size;
    return NULL;
}

void bink_close(bink object)
{
    (void)object;
}
