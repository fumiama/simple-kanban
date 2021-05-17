#include <stdio.h>
#include <string.h>
#include "simple-protobuf/simple_protobuf.h"

struct CONFIG {
    char pwd[64];  //password
    char sps[64];  //set password
};
typedef struct CONFIG CONFIG;

CONFIG cfg;

uint64_t items_len[2] = {sizeof(cfg.pwd), sizeof(cfg.sps)};
uint8_t types_len[2];

int main() {
    printf("Enter a password: ");
    scanf("%s", cfg.pwd);
    printf("Enter a set password: ");
    scanf("%s", cfg.sps);
    for(int i = 0; i < 2; i++) {
        types_len[i] = first_set(items_len[i]);
        printf("Item %d has type %d with size %llu\n", i, types_len[i], items_len[i]);
    }
    /*align_struct(types_len, 2, sizeof(CONFIG));
    for(int i = 0; i < 5; i++) {
        printf("Item %d's type after align: %u\n", i, types_len[i]);
    }*/
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
