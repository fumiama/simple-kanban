#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <simple_protobuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

#if !__APPLE__
    #include <sys/sendfile.h>
    #include <endian.h>
#else
    #include <machine/endian.h>
#endif

#include "config.h"
#include "file.h"

static char *data_path;    // cat 命令读取的文件位置
static char *kanban_path;  // get 命令读取的文件位置

static uint8_t _cfg[sizeof(simple_pb_t)+sizeof(config_t)];
#define cfg ((const const_config_t*)(_cfg+sizeof(simple_pb_t)))        // 存储 pwd 和 sps

#define TCPOOL_THREAD_TIMER_T_SZ 65536
#define TCPOOL_MAXWAITSEC 16
#define SERVER_THREAD_BUFSZ ( \
    TCPOOL_THREAD_TIMER_T_SZ    \
    -TCPOOL_THREAD_TIMER_T_HEAD_SZ  \
    -sizeof(ssize_t)-2*sizeof(uint8_t)  \
)
#define TCPOOL_THREAD_CONTEXT   \
    ssize_t numbytes;   /* 本次接收的数据长度 */    \
    int8_t status;      /* 本会话所处的状态 */      \
    uint8_t isdata;     /* 是否为 datfile */       \
    char data[SERVER_THREAD_BUFSZ]
#define TCPOOL_TOUCH_TIMER_CONDITION (*(volatile uint32_t*)(is_file_opening))
#define TCPOOL_INIT_ACTION init_file((char*[]){kanban_path, data_path})
#define TCPOOL_PREHANDLE_ACCEPT_ACTION(timer)   \
    timer->status = -1; \
    timer->isdata = 0;
#define TCPOOL_CLEANUP_THREAD_ACTION(timer) \
    close_file(timer->index, timer->isdata);  \
    timer->status = -1;

#include "tcpool.h"


/*
 * check_buffer 检查接收到的数据，结合
 * 当前会话所处状态决定接下来的处理流程
*/
static int check_buffer(tcpool_thread_timer_t *timer);
static int send_all(tcpool_thread_timer_t *timer);
static int send_data(int accept_fd, char *data, size_t length);
static int sm1_pwd(tcpool_thread_timer_t *timer);
static int s0_init(tcpool_thread_timer_t *timer);
static int s1_get(tcpool_thread_timer_t *timer);
static int s2_set(tcpool_thread_timer_t *timer);
static int s3_set_data(tcpool_thread_timer_t *timer);

#define take_word(p, w, buff) if((p)->numbytes >= strlen(w) && !strncmp(buff, w, strlen(w))) {\
                        printf("<--- Taking: %s in %zd --->\n", w, (p)->numbytes);\
                        int l = strlen(w);\
                        char store = buff[l];\
                        buff[l] = 0;\
                        ssize_t n = (p)->numbytes - l;\
                        (p)->numbytes = l;\
                        if(!check_buffer((p))) {\
                            printf("<--- break in %zd --->\n", (p)->numbytes); \
                            break; \
                        } \
                        if(n > 0) { \
                            buff[0] = store; \
                            memmove(buff + 1, buff + l + 1, n - 1); \
                        } \
                        buff[n] = 0;\
                        (p)->numbytes = n;\
                        printf("<--- pass in %zd --->\n", (p)->numbytes); \
                    }
static void accept_action(tcpool_thread_timer_t *p) {
    if(send_data(p->accept_fd, "Welcome to simple kanban server.", 33) <= 0) {
        puts("Send banner to new client failed");
        return;
    }
    printf("Recv data from client@%d, ", p->index);
    int is_first = 1;
    while(((p)->numbytes = recv(p->accept_fd, p->data, SERVER_THREAD_BUFSZ, 0)) > 0) {
        tcpool_touch_timer(p);
        p->data[p->numbytes] = 0;
        printf("Get %d bytes: %s, Check buffer...\n", (int)(p)->numbytes, p->data);
        if(is_first) {
            is_first = 0;
            //处理允许的粘连
            take_word(p, cfg->pwd, p->data);
            take_word(p, "get",    p->data);
            take_word(p, "cat",    p->data);
            take_word(p, "quit",   p->data);
            take_word(p, cfg->sps, p->data);
            take_word(p, "ver",    p->data);
            take_word(p, "dat",    p->data);
        }
        if((p)->numbytes <= 0) {
            puts("Taking words finished");
            continue;
        }
        if(!check_buffer((p))) {
            printf("<--- break normal in %zd --->\n", p->numbytes);
            break;
        }
    }
    printf("Recv finished, remain: %zd\n", (p)->numbytes);
}

static int check_buffer(tcpool_thread_timer_t *timer) {
    printf("Status: %d\n", (int)timer->status);
    switch(timer->status) {
        case -1: return sm1_pwd(timer); break;
        case 0: return s0_init(timer); break;
        case 1: return s1_get(timer); break;
        case 2: return s2_set(timer); break;
        case 3: return s3_set_data(timer); break;
        default: return -1; break;
    }
}

static int send_all(tcpool_thread_timer_t *timer) {
    int re = 1;
    FILE *fp = open_file(timer->index, timer->isdata, 1);
    if(fp == NULL) return 1;
    uint32_t close_file_wrap_data[2] = {timer->index, (uint32_t)timer->isdata};
    pthread_cleanup_push((void (*)(void*))&close_file_wrap, (void*)close_file_wrap_data);
    off_t len = 0, file_size = get_file_size(timer->isdata);
    printf("Get file size: %d bytes, ", (int)file_size);
    #if __APPLE__
        #ifdef WORDS_BIGENDIAN
            file_size = __DARWIN_OSSwapInt32(file_size);
        #endif
        struct sf_hdtr hdtr;
        struct iovec headers;
        headers.iov_base = &file_size;
        headers.iov_len = sizeof(uint32_t);
        hdtr.headers = &headers;
        hdtr.hdr_cnt = 1;
        hdtr.trailers = NULL;
        hdtr.trl_cnt = 0;
        re = !sendfile(fileno(fp), timer->accept_fd, 0, &len, &hdtr, 0);
        if(!re) perror("sendfile");
    #else
        #ifdef WORDS_BIGENDIAN
            uint32_t little_fs = __builtin_bswap32(file_size);
            send(timer->accept_fd, &little_fs, sizeof(uint32_t), 0);
        #else
            send(timer->accept_fd, &file_size, sizeof(uint32_t), 0);
        #endif
        re = sendfile(timer->accept_fd, fileno(fp), &len, file_size) > 0;
        if(!re) perror("sendfile");
    #endif
    printf("Send %d bytes\n", (int)len);
    pthread_cleanup_pop(1);
    return re;
}

static int send_data(int accept_fd, char *data, size_t length) {
    char buf[128];
    if(length == 0) {
        puts("Send data error: zero length");
        return 0;
    }
    if(!~send(accept_fd, data, length, 0)) {
        puts("Send data error");
        return 0;
    }
    printf("Send data: ");
    if(length >= 128) {
        memcpy(buf, data, 124);
        buf[124] = '.';
        buf[125] = '.';
        buf[126] = '.';
        buf[127] =  0;
    } else memcpy(buf, data, length);
    if(buf[length]) buf[length] = 0;
    puts(buf);
    return 1;
}

static int sm1_pwd(tcpool_thread_timer_t *timer) {
    if(!strncmp(timer->data, cfg->pwd, strlen(cfg->pwd))) {
        timer->status = 0;
        puts("Password check passed");
    } else puts("Password check failed");
    return !timer->status;
}

static int s0_init(tcpool_thread_timer_t *timer) {
    if(!strncmp(timer->data, "get", 3)) {
        timer->status = 1;
        return 1;
    }
    if(!strncmp(timer->data, cfg->sps, strlen(cfg->sps))) {
        timer->status = 2;
        return 2;
    }
    if(!strncmp(timer->data, "cat", 3)) {
        timer->isdata = 1;
        return send_all(timer);
    }
    //if(!strncmp(timer->data, "quit", 4)) return 0;
    return 0;
}

// s1_get scan getxxx
static int s1_get(tcpool_thread_timer_t *timer) {
    FILE *fp = open_file(timer->index, 0, 1);
    timer->status = 0;
    if(!fp) goto GET_END;

    uint32_t close_file_wrap_data[2] = {timer->index, (uint32_t)timer->isdata};
    int r; uint32_t ver, cli_ver;

    pthread_cleanup_push((void (*)(void*))&close_file_wrap, (void*)close_file_wrap_data);
    timer->isdata = 0;
    r = fscanf(fp, "%u", &ver);
    pthread_cleanup_pop(1);

    if(r <= 0) goto GET_END;
    if(sscanf(timer->data, "%u", &cli_ver) <= 0) goto GET_END;
    if(cli_ver >= ver) goto GET_END;

    //need to send a new kanban
    r = send_all(timer);
    goto GET_SKIP;

    GET_END:
    r = send_data(timer->accept_fd, "null", 4);
    GET_SKIP:
    if(strstr(timer->data, "quit")) {
        puts("Found last cmd is quit");
        timer->numbytes = 0;
        return 0;
    }
    int i = 0;
    for(; i < timer->numbytes; i++) {
        if(!isdigit(timer->data[i])) {
            timer->numbytes -= i;
            break;
        }
    }
    return r;
}

static int s2_set(tcpool_thread_timer_t *timer) {
    FILE *fp = NULL;
    if(!strncmp(timer->data, "ver", 3)) {
        timer->isdata = 0;
    } else if(!strncmp(timer->data, "dat", 3)) {
        timer->isdata = 1;
    } else {
        timer->status = 0;
        return send_data(timer->accept_fd, "erro", 4);
    }
    timer->status = 3;
    return send_data(timer->accept_fd, "data", 4);
}

static int s3_set_data(tcpool_thread_timer_t *timer) {
    char ret[4];
    *(uint32_t*)ret = *(uint32_t*)"succ";
    timer->status = 0;
    FILE* fp = open_file(timer->index, timer->isdata, 0);
    uint32_t close_file_wrap_data[2] = {timer->index, (uint32_t)timer->isdata};
    pthread_cleanup_push((void (*)(void*))&close_file_wrap, (void*)close_file_wrap_data);
    if(timer->numbytes < 4) {
        ssize_t n = recv(timer->accept_fd, timer->data+timer->numbytes, 4-timer->numbytes, MSG_WAITALL);
        if(n < 4-timer->numbytes) {
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
    }
    #ifdef WORDS_BIGENDIAN
        uint32_t file_size = __builtin_bswap32(*(uint32_t*)(timer->data));
    #else
        uint32_t file_size = *(uint32_t*)(timer->data);
    #endif
    printf("Set data size: %u\n", file_size);
    if((timer->numbytes = recv(timer->accept_fd, timer->data, SERVER_THREAD_BUFSZ, 0)) < 0 && errno != EAGAIN) {
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    printf("Get data size: %d\n", (int)timer->numbytes);
    if(file_size <= SERVER_THREAD_BUFSZ) {
        while(timer->numbytes != file_size) {
            ssize_t n = recv(timer->accept_fd, timer->data + timer->numbytes, SERVER_THREAD_BUFSZ - timer->numbytes, MSG_WAITALL);
            if(n <= 0) {
                *(uint32_t*)ret = *(uint32_t*)"erro";
                goto S3_RETURN;
            }
            timer->numbytes += n;
        }
        if(fwrite(timer->data, file_size, 1, fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
        }
        goto S3_RETURN;
    }
    if(timer->numbytes > 0 && fwrite(timer->data, timer->numbytes, 1, fp) != 1) {
        perror("fwrite");
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    int32_t remain = file_size - timer->numbytes;
    while(remain > 0) {
        // printf("remain:%d\n", (int)remain);
        ssize_t n = recv(timer->accept_fd, timer->data, (remain>SERVER_THREAD_BUFSZ)?SERVER_THREAD_BUFSZ:remain, MSG_WAITALL);
        if(n < 0) {
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        else if(!n) {
            usleep(10000); // 10 ms
            continue;
        }
        if(fwrite(timer->data, n, 1, fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        remain -= n;
    }
S3_RETURN:
    pthread_cleanup_pop(1);
    return send_data(timer->accept_fd, ret, 4);
}

#define show_usage(program) printf("Usage: %s (-d) port kanban.txt data.bin config.sp\n\t-d: As daemon\n", program)

int main(int argc, char *argv[]) {
    if(argc != 5 && argc != 6) {
        show_usage(argv[0]);
        return 0;
    }
    uint16_t port = 0; int tmp;
    int as_daemon = !strncmp(argv[1], "-d", 3);
    sscanf(argv[as_daemon?2:1], "%d", &tmp);
    if(tmp < 0 || tmp >= 65536) {
        printf("Error port: %d\n", tmp);
        return 1;
    }
    port = (uint16_t)tmp;
    if(as_daemon && daemon(1, 1) < 0) {
        perror("Start daemon error");
        return 2;
    }
    FILE *fp = NULL;
    fp = fopen(argv[as_daemon?3:2], "rb+");
    if(!fp) fp = fopen(argv[as_daemon?3:2], "wb+");
    if(!fp) {
        printf("Error opening kanban file: ");
        perror(argv[as_daemon?3:2]);
        return 3;
    }
    kanban_path = argv[as_daemon?3:2];
    fclose(fp);
    fp = NULL;
    fp = fopen(argv[as_daemon?4:3], "rb+");
    if(!fp) fp = fopen(argv[as_daemon?4:3], "wb+");
    if(!fp) {
        printf("Error opening data file: ");
        perror(argv[as_daemon?4:3]);
        return 4;
    }
    data_path = argv[as_daemon?4:3];
    fclose(fp);
    fp = NULL;
    fp = fopen(argv[as_daemon?5:4], "rb");
    if(!fp) {
        printf("Error opening config file: ");
        perror(argv[as_daemon?5:4]);
        return 5;
    }
    read_pb_into(fp, (simple_pb_t*)(&_cfg));
    fclose(fp);
    if(!(tmp = bind_server(&port))) return 6;
    if(!listen_socket(tmp)) return 7;
    pthread_cleanup_push((void (*)(void*))&close, (void*)((long long)tmp));
    accept_client(tmp);
    pthread_cleanup_pop(1);
    return 99;
}
