#include <stdio.h>
#include <string.h>
#include "simple-protobuf/simple_protobuf.h"

struct CONFIG {
    char pwd[64];  //password
    char sps[64];  //set password
};
typedef struct CONFIG CONFIG;

CONFIG cfg;

int main() {
    printf("Enter a password: ");
    scanf("%s", cfg.pwd);
    printf("Enter a set password: ");
    scanf("%s", cfg.sps);
    uint64_t* types_len = align_struct(sizeof(CONFIG), 2, &cfg.pwd, &cfg.sps);
    FILE* fp = fopen("cfg.sp", "wb");
    if(fp) {
        set_pb(fp, types_len, sizeof(CONFIG), &cfg);
        memset(&cfg, 0, sizeof(CONFIG));
        fclose(fp);
        puts("Write file succeed.");
        fp = NULL;
        fp = fopen("cfg.sp", "rb");
        if(fp) {
            SIMPLE_PB* spb = get_pb(fp);
            memcpy(&cfg, spb->target, sizeof(CONFIG));
            printf("set pwd: %s, sps: %s\n", cfg.pwd, cfg.sps);
        } else perror("[SPB]");
    } else perror("[SPB]");
}
