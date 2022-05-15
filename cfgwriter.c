#include <stdio.h>
#include <string.h>
#include <simple_protobuf.h>
#include "config.h"

config_t cfg;

int main() {
    printf("Enter a password: ");
    scanf("%s", cfg.pwd);
    printf("Enter a set password: ");
    scanf("%s", cfg.sps);
    uint32_t* types_len = align_struct(sizeof(config_t), 2, &cfg.pwd, &cfg.sps);
    FILE* fp = fopen("cfg.sp", "wb");
    if(fp) {
        set_pb(fp, types_len, sizeof(config_t), &cfg);
        fclose(fp);
        puts("Config is saved to cfg.sp.");
        fp = NULL;
        puts("Check config...");
        fp = fopen("cfg.sp", "rb");
        if(fp) {
            SIMPLE_PB* spb = get_pb(fp);
            memset(&cfg, 0, sizeof(config_t));
            memcpy(&cfg, spb->target, sizeof(config_t));
            printf("set pwd: %s, sps: %s\n", cfg.pwd, cfg.sps);
        } else perror("[SPB]");
    } else perror("[SPB]");
}
