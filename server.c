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
#include <sys/uio.h>
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

static file_cache_t data_file_cache;
static file_cache_t kanban_file_cache;

static uint8_t _cfg[sizeof(simple_pb_t)+sizeof(config_t)];
#define cfg ((const const_config_t*)(_cfg+sizeof(simple_pb_t)))        // 存储 pwd 和 sps

#define TCPOOL_THREAD_TIMER_T_SZ 65536

#define TCPOOL_MAXWAITSEC 16

#define SERVER_THREAD_BUFSZ ( \
    TCPOOL_THREAD_TIMER_T_SZ    \
    -TCPOOL_THREAD_TIMER_T_HEAD_SZ  \
    -sizeof(ssize_t)-3*sizeof(uint8_t)  \
)

#define TCPOOL_THREAD_CONTEXT   \
    ssize_t numbytes;   /* 本次接收的数据长度 */    \
    int8_t status;      /* 本会话所处的状态 */      \
    uint8_t isdata;     /* 是否为 datfile */       \
    uint8_t isopen;     /* 是否获得了文件锁 */       \
    char data[SERVER_THREAD_BUFSZ]

#define TCPOOL_TOUCH_TIMER_CONDITION 0

#define TCPOOL_INIT_ACTION \
    file_cache_init(&data_file_cache, data_path); \
    file_cache_init(&kanban_file_cache, kanban_path);

#define TCPOOL_PREHANDLE_ACCEPT_ACTION(timer)   \
    timer->status = -1; \
    timer->isdata = 0;

#define TCPOOL_CLEANUP_THREAD_ACTION(timer) \
    if(timer->isopen) file_cache_unlock(timer->isdata?&data_file_cache:&kanban_file_cache); \
    timer->isopen = 0; \
    timer->isdata = 0; \
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
    if(send_data(p->accept_fd, "Welcome to simple kanban server. ", 33) <= 0) {
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
    int re;
    file_cache_t* fc = timer->isdata?&data_file_cache:&kanban_file_cache;
    if(file_cache_read_lock(fc)) {
        return 0;
    }
    pthread_cleanup_push((void (*)(void*))&file_cache_unlock, (void*)fc);
    uint64_t file_size = file_cache_get_data_size(fc);
    printf("Get file size: %llu bytes, ", file_size);
    #ifdef WORDS_BIGENDIAN
        uint32_t little_fs = __builtin_bswap32(file_size);
    #endif
    struct iovec iov[2] = {
        #ifdef WORDS_BIGENDIAN
            {&little_fs, sizeof(uint32_t)},
        #else
            {&file_size, sizeof(uint32_t)},
        #endif
        {(void*)fc->data, file_size}
    };
    re = writev(timer->accept_fd, (const struct iovec *)&iov, 2);
    pthread_cleanup_pop(1);
    if(re <= 0) {
        perror("writev");
        return 0;
    }
    printf("Send %d bytes\n", re);
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
    file_cache_t* fc = timer->isdata?&data_file_cache:&kanban_file_cache;
    uint32_t close_file_wrap_data[2] = {timer->index, (uint32_t)timer->isdata};
    int r; uint32_t ver, cli_ver;

    r = send_data(timer->accept_fd, "get", 3);
    if (!r) goto GET_END;
    if(file_cache_read_lock(fc)) {
        goto GET_END;
    }
    timer->status = 0;
    pthread_cleanup_push((void (*)(void*))&file_cache_unlock, (void*)fc);
    r = sscanf(fc->data, "%u", &ver);
    pthread_cleanup_pop(1);

    if(r <= 0) goto GET_END;
    if(sscanf(timer->data, "%u", &cli_ver) <= 0) goto GET_END;
    if(cli_ver >= ver) goto GET_END;

    // need to send a new kanban
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
    int recv_bufsz;
    socklen_t optlen = sizeof(recv_bufsz);
    if(getsockopt(timer->accept_fd, SOL_SOCKET, SO_RCVBUF, &recv_bufsz, &optlen)) {
        perror("getsockopt");
        *(uint32_t*)ret = *(uint32_t*)"erop";
        goto S3_RETURN;
    }
    printf("Set recv buffer size: %d\n", recv_bufsz);
    file_cache_t* fc = timer->isdata?&data_file_cache:&kanban_file_cache;
    if(file_cache_write_lock(fc)) {
        *(uint32_t*)ret = *(uint32_t*)"erwl";
        goto S3_RETURN;
    }
    pthread_cleanup_push((void (*)(void*))&file_cache_unlock, (void*)fc);
    if(timer->numbytes < 4) {
        ssize_t n = recv(timer->accept_fd, timer->data+timer->numbytes, 4-timer->numbytes, MSG_WAITALL);
        if(n < 4-timer->numbytes) {
            *(uint32_t*)ret = *(uint32_t*)"ercN";
            perror("recv");
            goto S3_RETURN;
        }
    }
    #ifdef WORDS_BIGENDIAN
        uint32_t file_size = __builtin_bswap32(*(uint32_t*)(timer->data));
    #else
        uint32_t file_size = *(uint32_t*)(timer->data);
    #endif
    printf("Client set data size: %u\n", file_size);
    timer->numbytes -= 4;
    if(file_cache_realloc(fc, (uint64_t)file_size)) {
        *(uint32_t*)ret = *(uint32_t*)"eral";
        goto S3_RETURN;
    }
    if(timer->numbytes >= file_size) {
        memcpy(fc->data, timer->data+4, file_size);
        puts("All data received and copied");
        goto S3_RETURN;
    }
    ssize_t recvlen = 0, p = 0;
    if(timer->numbytes > 0) {
        p = timer->numbytes;
        memcpy(fc->data, timer->data+4, p);
        file_size -= p;
        printf("Copy received data: %zd bytes, remain: %u bytes\n", p, file_size);
    }
    if((uint64_t)file_size <= (uint64_t)recv_bufsz) {
        if((recvlen = recv(timer->accept_fd, fc->data+p, (size_t)file_size, MSG_WAITALL)) != (ssize_t)file_size) {
            *(uint32_t*)ret = *(uint32_t*)"ercA";
            perror("recv");
            goto S3_RETURN;
        }
        printf("Recv from client: %zd bytes\n", recvlen);
    } else {
        puts("Start loop recv");
        while((recvlen = recv(
                timer->accept_fd, fc->data+p,
                (size_t)(((uint64_t)file_size>(uint64_t)recv_bufsz)?recv_bufsz:file_size), MSG_WAITALL)
            ) > 0) {
            if(recvlen <= 0 || (uint32_t)recvlen > file_size) {
                *(uint32_t*)ret = *(uint32_t*)"ercM";
                perror("recv");
                goto S3_RETURN;
            }
            file_size -= (uint32_t)recvlen;
            p += recvlen;
            printf("Loop recv from client: %zd bytes, remain: %u bytes\n", recvlen, file_size);
            if(file_size == 0) break;
        }
        if(recvlen <= 0) {
            *(uint32_t*)ret = *(uint32_t*)"ercF";
            perror("recv");
            goto S3_RETURN;
        }
        puts("Finish loop recv");
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

static void __attribute__((destructor)) defer_close_cache_files() {
    file_cache_close(&data_file_cache);
    file_cache_close(&kanban_file_cache);
}
