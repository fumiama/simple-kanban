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
#include <unistd.h>
#include "config.h"

#if !__APPLE__
    #include <sys/sendfile.h>
    #include <endian.h>
    #ifndef FD_COPY
        #define FD_COPY(f, t)  bcopy(f, t, sizeof(*(f)))
    #endif
#else
    #include <machine/endian.h>
#endif


static uint8_t _cfg[sizeof(SIMPLE_PB)+sizeof(config_t)];
#define cfg ((const const_config_t*)(_cfg+sizeof(SIMPLE_PB)))        // 存储 pwd 和 sps

static int fd;               // server socket fd

#ifdef LISTEN_ON_IPV6
    static socklen_t struct_len = sizeof(struct sockaddr_in6);
    static struct sockaddr_in6 server_addr;
    static struct sockaddr_in6 client_addr;
#else
    static socklen_t struct_len = sizeof(struct sockaddr_in);
    static struct sockaddr_in server_addr;
    static struct sockaddr_in client_addr;
#endif

static char *data_path;    // cat 命令读取的文件位置
static char *kanban_path;  // get 命令读取的文件位置

/* lock operations for open_file */
#define LOCK_ALLF_UN         0x00            /* unlock all file */
#define LOCK_DATA_SH         0x01            /* shared data file lock */
#define LOCK_DATA_EX         0x02            /* exclusive data file lock */
#define LOCK_KANB_SH         0x04            /* shared kanban file lock */
#define LOCK_KANB_EX         0x08            /* exclusive kanban file lock */

static int file_mode = LOCK_ALLF_UN;
static int kanb_file_ro_cnt, data_file_ro_cnt;

// THREADCNT 并行的监听队列/select队列长度
#define THREADCNT 16
// MAXWAITSEC 最大等待时间
#define MAXWAITSEC 16
#define TIMERDATSZ (BUFSIZ-sizeof(int)-sizeof(time_t)-sizeof(int)-sizeof(ssize_t)-sizeof(int8_t)-sizeof(uint8_t)*3-sizeof(FILE*))

static struct timeval timeout;

// 会话的上下文
// 包含了本次 accept 的全部信息
struct threadtimer_t {
    int index;          // 自身位置
    int accept_fd;      // 本次 accept 的 fd
    time_t touch;       // 最后访问时间，与当前时间差超过 MAXWAITSEC 将强行中断连接
    ssize_t numbytes;   // 本次接收的数据长度
    FILE *fp;           // 本会话打开的文件
    int8_t status;      // 本会话所处的状态
    uint8_t is_open;    // 标识 fp 是否正在使用
    uint8_t lock_type;  // 打开文件类型
    uint8_t again_cnt;  // EAGAIN 次数
    char data[TIMERDATSZ];
};
typedef struct threadtimer_t threadtimer_t;

static threadtimer_t timers[THREADCNT];

static fd_set rdfds, wrfds, erfds, tmpfds;

#define show_usage(program) printf("Usage: %s (-d) port kanban.txt data.bin config.sp\n\t-d: As daemon\n", program)


/*
 * accept_client 接受新连接，
 * 调用 select 处理
 * 处理入口点为 handle_accept
 * 当 client 超过 MAXWAITSEC 未响应时
 * 调用 clean_timer 回收
 * 未被释放的资源以防止内存泄漏等
*/
static void accept_client();
static int bind_server(int* port);
/*
 * check_buffer 检查接收到的数据，结合
 * 当前会话所处状态决定接下来的处理流程
*/
static int check_buffer(threadtimer_t *timer);
static void clean_timer(threadtimer_t* timer);
static void close_file(threadtimer_t *timer);
static int close_file_and_send(threadtimer_t *timer, char *data, size_t numbytes);
// handle_accept 初步解析指令，处理部分粘连
static int handle_accept(threadtimer_t* p);
static void handle_end(int signo);
static void handle_int(int signo);
static void handle_quit(int signo);
static void handle_segv(int signo);
static int listen_socket();
static FILE *open_file(char* file_path, int lock_type, char* mode);
static int send_all(char* file_path, threadtimer_t *timer);
static int send_data(int accept_fd, char *data, size_t length);
static off_t size_of_file(const char* fname);
static int sm1_pwd(threadtimer_t *timer);
static int s0_init(threadtimer_t *timer);
static int s1_get(threadtimer_t *timer);
static int s2_set(threadtimer_t *timer);
static int s3_set_data(threadtimer_t *timer);


static void accept_client() {
    int i;
    signal(SIGINT,  handle_int);
    signal(SIGQUIT, handle_quit);
    signal(SIGKILL, handle_end);
    signal(SIGSEGV, handle_segv);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_end);
    FD_SET(fd, &tmpfds);
    while(1) {
        FD_COPY(&tmpfds, &rdfds);
        FD_COPY(&tmpfds, &erfds);
        puts("Selecting...");
        timeout.tv_sec = MAXWAITSEC/4;
        int r = select(THREADCNT+8, &rdfds, &wrfds, &erfds, &timeout);
        if(r < 0) {
            perror("select");
            return;
        }
        if(r == 0) { // 超时
            for(i = 0; i < THREADCNT; i++) {
                if(timers[i].touch && timers[i].accept_fd) {
                    time_t waitsec = time(NULL) - timers[i].touch;
                    if(waitsec > MAXWAITSEC) {
                        printf("Close@%d, wait sec: %u, max: %u\n", i, (unsigned int)waitsec, MAXWAITSEC);
                        clean_timer(&timers[i]);
                    }
                }
            }
            continue;
        }
        puts("\nSelected");
        // 正常
        if(FD_ISSET(fd, &rdfds)) { // 有新连接
            int p = 0;
            while(p < THREADCNT && timers[p].touch) p++;
            if(p >= THREADCNT) {
                puts("Max thread cnt exceeded");
                int nfd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
                if(nfd > 0) close(nfd);
                goto HANDLE_CLIENTS;
            }
            threadtimer_t* timer = &timers[p];
            timer->accept_fd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
            if(timer->accept_fd <= 0) {
                perror("accept");
                goto HANDLE_CLIENTS;
            }
            #ifdef LISTEN_ON_IPV6
                uint16_t port = ntohs(client_addr.sin6_port);
                struct in6_addr in = client_addr.sin6_addr;
                char str[INET6_ADDRSTRLEN];	// 46
                inet_ntop(AF_INET6, &in, str, sizeof(str));
            #else
                uint16_t port = ntohs(client_addr.sin_port);
                struct in_addr in = client_addr.sin_addr;
                char str[INET_ADDRSTRLEN];	// 16
                inet_ntop(AF_INET, &in, str, sizeof(str));
            #endif
            time_t t = time(NULL);
            printf("> %sAccept client(%d) %s:%u at slot No.%d, ", ctime(&t), timer->accept_fd, str, port, p);
            timer->index = p;
            timer->touch = time(NULL);
            timer->is_open = 0;
            timer->fp = NULL;
            timer->status = -1;
            timer->again_cnt = 0;
            if(send_data(timer->accept_fd, "Welcome to simple kanban server.", 33) <= 0) {
                puts("Send banner to new client failed");
                clean_timer(timer);
                goto HANDLE_CLIENTS;
            }
            FD_SET(timer->accept_fd, &tmpfds);
            puts("Add new client into select list");
        } else if(FD_ISSET(fd, &erfds)) { // 主套接字错误
            int nfd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
            perror("main fd in erfds");
            if(nfd > 0) close(nfd);
            return;
        }
        HANDLE_CLIENTS:
        for(i = 0; i < THREADCNT; i++) {
            if(timers[i].touch && timers[i].accept_fd) {
                if(FD_ISSET(timers[i].accept_fd, &rdfds)) {
                    if(!handle_accept(&timers[i])) clean_timer(&timers[i]);
                    else FD_SET(timers[i].accept_fd, &tmpfds);
                } else if(FD_ISSET(timers[i].accept_fd, &erfds)) {
                    printf("Close@%d due to error\n", i);
                    clean_timer(&timers[i]);
                }
            }
        }
    }
}

static int bind_server(int* port) {
    #ifdef LISTEN_ON_IPV6
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_port = htons((uint16_t)(*port));
        bzero(&(server_addr.sin6_addr), sizeof(server_addr.sin6_addr));
        int fd = socket(PF_INET6, SOCK_STREAM, 0);
    #else
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons((uint16_t)(*port));
        server_addr.sin_addr.s_addr = INADDR_ANY;
        bzero(&(server_addr.sin_zero), 8);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    int on = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) {
        perror("Set socket option failure");
        return 0;
    }
    if(!~bind(fd, (struct sockaddr *)&server_addr, struct_len)) {
        perror("Bind server failure");
        return 0;
    }
    #ifdef LISTEN_ON_IPV6
        *port = ntohs(server_addr.sin6_port);
        struct in6_addr in = server_addr.sin6_addr;
        char str[INET6_ADDRSTRLEN];	// 46
        inet_ntop(AF_INET6, &in, str, sizeof(str));
    #else
        *port = ntohs(server_addr.sin_port);
        struct in_addr in = server_addr.sin_addr;
        char str[INET_ADDRSTRLEN];	// 16
        inet_ntop(AF_INET, &in, str, sizeof(str));
    #endif
    printf("Bind server successfully on %s:%u\n", str, *port);
    return fd;
}

static int check_buffer(threadtimer_t *timer) {
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

static void clean_timer(threadtimer_t* timer) {
    printf("Start cleaning: ");
    if(timer->is_open) {
        close_file(timer);
        printf("Close file, ");
    }
    FD_CLR(timer->accept_fd, &tmpfds);
    if(timer->accept_fd) {
        close(timer->accept_fd);
        timer->accept_fd = 0;
        printf("Close accept, ");
    }
    timer->touch = 0;
    timer->status = -1;
    timer->lock_type = 0;
    puts("Finish cleaning");
}

static void close_file(threadtimer_t *timer) {
    if(timer->is_open && timer->fp != NULL) {
        int lock_type = timer->lock_type;
        puts("Close file");
        fclose(timer->fp);
        timer->is_open = 0;
        timer->fp = NULL;
        file_mode &= ~lock_type;
        if((lock_type&LOCK_KANB_SH) > 0 && --kanb_file_ro_cnt > 0) {
            file_mode |= LOCK_KANB_SH;
        }
        if((lock_type&LOCK_DATA_SH) > 0 && --data_file_ro_cnt > 0) {
            file_mode |= LOCK_DATA_SH;
        }
        timer->lock_type = 0;
    }
}

static int close_file_and_send(threadtimer_t *timer, char *data, size_t numbytes) {
    close_file(timer);
    return send_data(timer->accept_fd, data, numbytes);
}

#define take_word(p, w, buff) if((p)->numbytes >= strlen(w) && strncmp(buff, w, strlen(w)) == buff) {\
                        printf("<--- Taking: %s in %zd --->\n", w, (p)->numbytes);\
                        int l = strlen(w);\
                        char store = buff[l];\
                        buff[l] = 0;\
                        ssize_t n = (p)->numbytes - l;\
                        (p)->numbytes = l;\
                        if(!(r = check_buffer((p)))) {\
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
#define touch_timer(x) ((x)->touch = time(NULL))
#define my_fd(x) ((x)->accept_fd)
#define my_dat(x) ((x)->data)
static int handle_accept(threadtimer_t* p) {
    int r = 1;
    printf("Recv data from client@%d, ", p->index);
    if((p)->status == 3) return s3_set_data(p);
    while(((p)->numbytes = recv(my_fd(p), my_dat(p), TIMERDATSZ, MSG_DONTWAIT)) > 0) {
        touch_timer(p);
        my_dat(p)[(p)->numbytes] = 0;
        printf("Get %d bytes: %s, Check buffer...\n", (int)(p)->numbytes, my_dat(p));
        //处理允许的粘连
        take_word(p, cfg->pwd, my_dat(p));
        take_word(p, "get",    my_dat(p));
        take_word(p, "cat",    my_dat(p));
        take_word(p, "quit",   my_dat(p));
        take_word(p, cfg->sps, my_dat(p));
        take_word(p, "ver",    my_dat(p));
        take_word(p, "dat",    my_dat(p));
        if((p)->numbytes <= 0) {
            puts("Taking words finished");
            break;
        }
        puts("Last check_buffer");
        r = check_buffer((p));
        break;
    }
    if((p)->numbytes <= 0) {
        if(errno == EAGAIN || errno == EINVAL) {
            if(!++(p)->again_cnt) {
                r = 0;
                puts("Max EAGAIN/EINVAL cnt exceeded");
            }
        } else if(errno) {
            perror("recv");
            r = 0;
        }
    }
    printf("Recv finished, remain: %zd, continue: %s\n", (p)->numbytes, r?"true":"false");
    return r;
}

static void handle_end(int signo) {
    puts("Handle kill/term");
    close(fd);
    fflush(stdout);
    exit(0);
}

static void handle_int(int signo) {
    puts("Keyboard interrupted");
    close(fd);
    fflush(stdout);
    exit(0);
}

static void handle_quit(int signo) {
    puts("Handle sigquit");
    close(fd);
    fflush(stdout);
    exit(0);
}

static void handle_segv(int signo) {
    puts("Handle sigsegv");
    close(fd);
    fflush(stdout);
    exit(0);
}

static int listen_socket() {
    int flags = fcntl(fd, F_GETFL, 0);
    if(!~listen(fd, THREADCNT)) return 1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static FILE *open_file(char* file_path, int lock_type, char* mode) {
    FILE *fp = NULL;
    if((lock_type&LOCK_KANB_SH)) {
        if((file_mode&LOCK_KANB_EX) > 0) {
            puts("open_file(KANB_SH): file is busy");
            return NULL;
        }
        file_mode |= LOCK_KANB_SH;
        kanb_file_ro_cnt++;
    } else if((lock_type&LOCK_DATA_SH)) {
        if((file_mode&LOCK_DATA_EX) > 0) {
            puts("open_file(DATA_SH): file is busy");
            return NULL;
        }
        file_mode |= LOCK_DATA_SH;
        data_file_ro_cnt++;
    } else if(lock_type&LOCK_KANB_EX) {
        if((file_mode&(LOCK_KANB_EX|LOCK_KANB_SH)) > 0) {
            puts("open_file(KANB_EX): file is busy");
            return NULL;
        }
        file_mode |= LOCK_KANB_EX;
    } else if(lock_type&LOCK_DATA_EX) {
        if((file_mode&(LOCK_DATA_EX|LOCK_DATA_SH)) > 0) {
            puts("open_file(DATA_EX): file is busy");
            return NULL;
        }
        file_mode |= LOCK_DATA_EX;
    }
    fp = fopen(file_path, mode);
    if(!fp) {
        perror("fopen");
        file_mode &= ~lock_type;
        return NULL;
    }
    printf("Open file %s in mode %s\n", file_path, mode);
    return fp;
}

static int send_all(char* file_path, threadtimer_t *timer) {
    int re = 1;
    FILE *fp = open_file(file_path, timer->lock_type, "rb");
    if(fp) {
        timer->fp = fp;
        timer->is_open = 1;
        uint32_t file_size = (uint32_t)size_of_file(file_path);
        printf("Get file size: %d bytes, ", (int)file_size);
        off_t len = 0;
        int flags = fcntl(timer->accept_fd, F_GETFL, 0);
        fcntl(timer->accept_fd, F_SETFL, flags & ~O_NONBLOCK);
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
        close_file(timer);
        fcntl(timer->accept_fd, F_SETFL, flags);
    }
    return re;
}

static int send_data(int accept_fd, char *data, size_t length) {
    char buf[128];
    if(length == 0) {
        puts("Send data error: zero length");
        return 0;
    }
    if(!~send(accept_fd, data, length, MSG_DONTWAIT)) {
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

static off_t size_of_file(const char* fname) {
    struct stat statbuf;
    if(stat(fname, &statbuf)==0) return statbuf.st_size;
    else return -1;
}

static int sm1_pwd(threadtimer_t *timer) {
    if(!strncmp(timer->data, cfg->pwd, strlen(cfg->pwd))) {
        timer->status = 0;
        puts("Password check passed");
    } else puts("Password check failed");
    return !timer->status;
}

static int s0_init(threadtimer_t *timer) {
    if(!strncmp(timer->data, "get", 3)) timer->status = 1;
    else if(!strncmp(timer->data, cfg->sps, strlen(cfg->sps))) timer->status = 2;
    else if(!strncmp(timer->data, "cat", 3)) {
        timer->lock_type = LOCK_DATA_SH;
        return send_all(data_path, timer);
    }
    else if(!strncmp(timer->data, "quit", 4)) return 0;
    return send_data(timer->accept_fd, timer->data, timer->numbytes);
}

static int s1_get(threadtimer_t *timer) {        //get kanban
    FILE *fp = open_file(kanban_path, LOCK_KANB_SH, "rb");
    timer->status = 0;
    if(fp) {
        timer->fp = fp;
        timer->is_open = 1;
        timer->lock_type = LOCK_KANB_SH;
        uint32_t ver, cli_ver;
        if(fscanf(fp, "%u", &ver) > 0) {
            if(sscanf(timer->data, "%u", &cli_ver) > 0) {
                if(cli_ver < ver) {     //need to send a new kanban
                    timer->is_open = 0;
                    close_file(timer);
                    int r = send_all(kanban_path, timer);
                    if(!strncmp(timer->data, "quit", 4)) {
                        puts("Found last cmd is quit");
                        return 0;
                    }
                    return r;
                }
            }
        }
    }
    int r = close_file_and_send(timer, "null", 4);
    if(!strncmp(timer->data, "quit", 4)) {
        puts("Found last cmd is quit");
        return 0;
    }
    return r;
}

static int s2_set(threadtimer_t *timer) {
    FILE *fp = NULL;
    int lktp;
    if(!strncmp(timer->data, "ver", 3)) {
        fp = open_file(kanban_path, LOCK_KANB_EX, "wb");
        lktp = LOCK_KANB_EX;
    } else if(!strncmp(timer->data, "dat", 3)) {
        fp = open_file(data_path, LOCK_DATA_EX, "wb");
        lktp = LOCK_DATA_EX;
    }
    if(fp) {
        timer->status = 3;
        timer->fp = fp;
        timer->is_open = 1;
        timer->lock_type = lktp;
        return send_data(timer->accept_fd, "data", 4);
    } else {
        timer->status = 0;
        return send_data(timer->accept_fd, "erro", 4);
    }
}

static int s3_set_data(threadtimer_t *timer) {
    char ret[4] = "succ";
    int flags = fcntl(timer->accept_fd, F_GETFL, 0);
    fcntl(timer->accept_fd, F_SETFL, flags & ~O_NONBLOCK);
    timer->status = 0;
    ssize_t n = recv(timer->accept_fd, timer->data, 4, MSG_WAITALL);
    if(n < 4) {
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    #ifdef WORDS_BIGENDIAN
        uint32_t file_size = __builtin_bswap32(*(uint32_t*)(timer->data));
    #else
        uint32_t file_size = *(uint32_t*)(timer->data);
    #endif
    printf("Set data size: %u\n", file_size);
    if((timer->numbytes = recv(timer->accept_fd, timer->data, TIMERDATSZ, 0)) < 0 && errno != EAGAIN) {
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    printf("Get data size: %d\n", (int)timer->numbytes);
    if(file_size <= TIMERDATSZ) {
        while(timer->numbytes != file_size) {
            ssize_t n = recv(timer->accept_fd, timer->data + timer->numbytes, TIMERDATSZ - timer->numbytes, MSG_WAITALL);
            if(n <= 0) {
                *(uint32_t*)ret = *(uint32_t*)"erro";
                goto S3_RETURN;
            }
            timer->numbytes += n;
        }
        if(fwrite(timer->data, file_size, 1, timer->fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
        }
        goto S3_RETURN;
    }
    if(timer->numbytes > 0 && fwrite(timer->data, timer->numbytes, 1, timer->fp) != 1) {
        perror("fwrite");
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    int32_t remain = file_size - timer->numbytes;
    while(remain > 0) {
        // printf("remain:%d\n", (int)remain);
        ssize_t n = recv(timer->accept_fd, timer->data, (remain>TIMERDATSZ)?TIMERDATSZ:remain, MSG_WAITALL);
        if(n < 0) {
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        else if(!n) {
            usleep(10000); // 10 ms
            continue;
        }
        if(fwrite(timer->data, n, 1, timer->fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        remain -= n;
    }
S3_RETURN:
    fcntl(timer->accept_fd, F_SETFL, flags);
    return close_file_and_send(timer, ret, 4);
}

int main(int argc, char *argv[]) {
    if(argc != 5 && argc != 6) {
        show_usage(argv[0]);
        return 0;
    }
    int port = 0;
    int as_daemon = !strncmp(argv[1], "-d", 3);
    sscanf(argv[as_daemon?2:1], "%d", &port);
    if(port < 0 || port >= 65536) {
        printf("Error port: %d\n", port);
        return 1;
    }
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
    read_pb_into(fp, (SIMPLE_PB*)(&_cfg));
    fclose(fp);
    if(!(fd = bind_server(&port))) {
        return 6;
    }
    if(listen_socket()) {
        perror("Listen failed");
        return 7;
    }
    /*
    printf("password: ");
    puts(cfg->pwd);
    printf("set password: ");
    puts(cfg->sps);
    */
    accept_client();
    close(fd);
    return 99;
}
