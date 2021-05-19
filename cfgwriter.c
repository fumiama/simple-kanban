#include <stdio.h>
#include <string.h>
#include <simple_protobuf.h>
#include "config.h"

CONFIG cfg;

int main() {
    printf("Enter a password: ");
    scanf("%s", cfg.pwd);
    printf("Enter a set password: ");
    scanf("%s", cfg.sps);
    uint32_t* types_len = align_struct(sizeof(CONFIG), 2, &cfg.pwd, &cfg.sps);
    FILE* fp = fopen("cfg.sp", "wb");
    if(fp) {
        set_pb(fp, types_len, sizeof(CONFIG), &cfg);
        fclose(fp);
        puts("Config is saved to cfg.sp.");
        fp = NULL;
        puts("Check config...");
        fp = fopen("cfg.sp", "rb");
        if(fp) {
            SIMPLE_PB* spb = get_pb(fp);
            memset(&cfg, 0, sizeof(CONFIG));
            memcpy(&cfg, spb->target, sizeof(CONFIG));
            printf("set pwd: %s, sps: %s\n", cfg.pwd, cfg.sps);
        } else perror("[SPB]");
    } else perror("[SPB]");
}
