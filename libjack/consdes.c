#include <config.h>

#include <stdlib.h>

int __attribute__((constructor)) some_constructor(void)
{
    printf("some_constructor was called\n");
}

int __attribute__((destructor)) some_destructor(void)
{
    printf("some_destructor was called\n");
}
