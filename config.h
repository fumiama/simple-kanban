#ifndef _CONFIG_H_
#define _CONFIG_H_

struct config_t {
    char pwd[64];  //password
    char sps[64];  //set password
};
typedef struct config_t config_t;

struct const_config_t {
    const char pwd[64];  //password
    const char sps[64];  //set password
};
typedef struct const_config_t const_config_t;

#define TCPOOL_THREAD_TIMER_T_SZ 65536

#define TCPOOL_MAXWAITSEC 16

#endif
