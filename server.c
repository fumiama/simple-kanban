#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <simple_protobuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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
#else
    #include <machine/endian.h>
#endif

static config_t* cfg;        // 存储 pwd 和 sps

static int fd;             // server socket fd

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

#define THREADCNT 16
static pthread_t accept_threads[THREADCNT];
static pthread_attr_t attr;

static pthread_rwlock_t mu;

#define MAXWAITSEC 10
#define TIMERDATSZ BUFSIZ
// accept_timer 使用的结构体
// 包含了本次 accept 的全部信息
// 以方便退出后清理空间
struct threadtimer_t {
    int index;          // 指向 accept_threads 某个槽位的下标
    time_t touch;       // 最后访问时间，与当前时间差超过 MAXWAITSEC 将由 timer 强行回收线程
    int accept_fd;      // 本次 accept 的 fd，会自行关闭或出错时由 timer 负责回收
    ssize_t numbytes;   // 本次接收的数据长度
    char status;        // 本会话所处的状态
    char is_open;       // 标识 fp 是否正在使用
    FILE *fp;           // 本会话打开的文件，会自行关闭或出错时由 timer 负责回收
    char data[TIMERDATSZ];
};
typedef struct threadtimer_t threadtimer_t;

#define show_usage(program) printf("Usage: %s [-d] listen_port kanban_file data_file config_file\n\t-d: As daemon\n", program)

static void accept_client();
static void accept_timer(void *p);
static int bind_server(uint16_t port);
static int check_buffer(threadtimer_t *timer);
static void clean_timer(threadtimer_t* timer);
static void close_file(FILE *fp);
static int close_file_and_send(threadtimer_t *timer, char *data, size_t numbytes);
static void handle_accept(void *accept_fd_p);
static void handle_int(int signo);
static void handle_pipe(int signo);
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

static pid_t pid;
/***************************************
 * accept_client 接受新连接，创建线程处理
 * 创建的线程入口点为 handle_accept
 * 与其伴生的结构体为 timer，负责管理
 * 该线程使用的资源，当线程（正常/异常）退出
 * 或 client 超过 MAXWAITSEC 未响应时
 * 将由与其伴生的 accept_timer 线程读取
 * timer 的信息，调用 clean_timer 回收
 * 未被释放的资源以防止内存泄漏
***************************************/
static void accept_client() {
    /*pid_t pid = fork();
    while (pid > 0) {      // 主进程监控子进程状态，如果子进程异常终止则重启之
        wait(NULL);
        puts("Server subprocess exited. Restart...");
        pid = fork();
    }
    while(pid < 0) {
        perror("Error when forking a subprocess");
        sleep(1);
    }*/
    signal(SIGINT,  handle_int);
    signal(SIGQUIT, handle_quit);
    signal(SIGKILL, handle_segv);
    signal(SIGSEGV, handle_segv);
    signal(SIGPIPE, handle_pipe);
    signal(SIGTERM, handle_segv);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    while(1) {
        puts("\nReady for accept, waitting...");
        int p = 0;
        while(p < THREADCNT && accept_threads[p] && !pthread_kill(accept_threads[p], 0)) p++;
        if(p >= THREADCNT) {
            puts("Max thread cnt exceeded");
            sleep(1);
            continue;
        }
        threadtimer_t *timer = malloc(sizeof(threadtimer_t));
        if(!timer) {
            puts("Allocate timer error");
            continue;
        }
        timer->accept_fd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
        if(timer->accept_fd <= 0) {
            free(timer);
            puts("Accept client error.");
            continue;
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
        printf("\n> %sAccept client %s:%u at slot No.%d\n", ctime(&t), str, port, p);
        timer->index = p;
        timer->touch = time(NULL);
        timer->is_open = 0;
        timer->fp = NULL;
        if (pthread_create(&accept_threads[p], &attr, (void *)&handle_accept, timer)) {
            perror("pthread_create");
            clean_timer(timer);
        } else puts("Creating thread succeeded");
    }
}

#define timer_ptr(x) ((threadtimer_t*)(x))
#define my_thread(timer) accept_threads[timer->index]
/***************************************
 * accept_timer 是与 handle_accept 伴生的
 * 线程，负责监控其会话状态，并在超时时杀死它
***************************************/
static void accept_timer(void *p) {
    while(my_thread(timer_ptr(p)) && !pthread_kill(my_thread(timer_ptr(p)), 0)) {
        sleep(MAXWAITSEC / 4);
        time_t waitsec = time(NULL) - timer_ptr(p)->touch;
        printf("Wait sec: %d, max: %d\n", (int)waitsec, MAXWAITSEC);
        if(waitsec > MAXWAITSEC) break;
    }
    clean_timer(timer_ptr(p));
    puts("Timer has been freed");
}

static int bind_server(uint16_t port) {
    int fail_count = 0;
    int result = -1;
    #ifdef LISTEN_ON_IPV6
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_port = htons(port);
        bzero(&(server_addr.sin6_addr), sizeof(server_addr.sin6_addr));
        fd = socket(PF_INET6, SOCK_STREAM, 0);
    #else
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;
        bzero(&(server_addr.sin_zero), 8);
        fd = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    if(!~bind(fd, (struct sockaddr *)&server_addr, struct_len)) return 1;
    return 0;
}

/***************************************
 * check_buffer 检查接收到的数据，结合
 * 当前会话所处状态决定接下来的处理流程
***************************************/
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

// clean_timer 清理 timer
static void clean_timer(threadtimer_t* timer) {
    puts("Start cleaning.");
    if(my_thread(timer)) {
        pthread_kill(my_thread(timer), SIGQUIT);
        my_thread(timer) = 0;
        puts("Kill thread.");
    }
    if(timer->is_open) {
        timer->is_open = 0;
        close_file(timer->fp);
        puts("Close file.");
    }
    if(timer->accept_fd) {
        close(timer->accept_fd);
        timer->accept_fd = 0;
        puts("Close accept.");
    }
    free(timer);
    puts("Finish cleaning.");
}

static void close_file(FILE *fp) {
    puts("Close file");
    pthread_rwlock_unlock(&mu);
    fclose(fp);
}

static int close_file_and_send(threadtimer_t *timer, char *data, size_t numbytes) {
    timer->is_open = 0;
    close_file(timer->fp);
    return send_data(timer->accept_fd, data, numbytes);
}

#define chkbuf(p) if(!check_buffer(timer_ptr(p))) break
#define take_word(p, w, buff) if(timer_ptr(p)->numbytes > strlen(w) && strstr(buff, w) == buff) {\
                        int l = strlen(w);\
                        char store = buff[l];\
                        buff[l] = 0;\
                        ssize_t n = timer_ptr(p)->numbytes - l;\
                        timer_ptr(p)->numbytes = l;\
                        chkbuf(p);\
                        buff[0] = store;\
                        memmove(buff + 1, buff + l + 1, n - 1);\
                        buff[n] = 0;\
                        timer_ptr(p)->numbytes = n;\
                        printf("Split cmd: %s\n", w);\
                    }
#define touch_timer(x) (timer_ptr(x)->touch = time(NULL))
#define my_fd(x) (timer_ptr(x)->accept_fd)
#define my_dat(x) (timer_ptr(x)->data)
// handle_accept 初步解析指令，处理部分粘连
static void handle_accept(void *p) {
    puts("Connected to the client.");
    pthread_t thread;
    if (pthread_create(&thread, &attr, (void *)&accept_timer, p)) {
        puts("Error creating timer thread");
        close(my_fd(p));
        free(p);
        return;
    }
    puts("Creating timer thread succeeded");
    pthread_cleanup_push((void*)clean_timer, p);
    send_data(my_fd(p), "Welcome to simple kanban server.", 33);
    timer_ptr(p)->status = -1;
    while(my_thread(timer_ptr(p)) && (timer_ptr(p)->numbytes = recv(my_fd(p), my_dat(p), TIMERDATSZ, 0)) > 0) {
        touch_timer(p);
        my_dat(p)[timer_ptr(p)->numbytes] = 0;
        printf("Get %d bytes: %s\n", (int)timer_ptr(p)->numbytes, my_dat(p));
        puts("Check buffer");
        //处理部分粘连
        take_word(p, cfg->pwd, my_dat(p));
        take_word(p, "get",    my_dat(p));
        take_word(p, "set",    my_dat(p));
        take_word(p, "cat",    my_dat(p));
        take_word(p, "quit",   my_dat(p));
        take_word(p, cfg->sps, my_dat(p));
        take_word(p, "ver",    my_dat(p));
        take_word(p, "dat",    my_dat(p));
        if(timer_ptr(p)->numbytes > 0) chkbuf(p);
    }
    printf("Break: recv %d bytes\n", (int)timer_ptr(p)->numbytes);
    my_thread(timer_ptr(p)) = 0;
    pthread_cleanup_pop(1);
}

static void handle_int(int signo) {
    puts("Keyboard interrupted");
    pthread_exit(NULL);
}

static void handle_pipe(int signo) {
    puts("Pipe error, exit thread...");
    pthread_exit(NULL);
}

static void handle_quit(int signo) {
    puts("Handle sigquit");
    pthread_exit(NULL);
}

static void handle_segv(int signo) {
    puts("Handle kill/segv/term");
    fflush(stdout);
    pthread_exit(NULL);
}

static int listen_socket() {
    int fail_count = 0;
    int result = -1;
    if(!~listen(fd, 10)) return 1;
    return 0;
}

static FILE *open_file(char* file_path, int lock_type, char* mode) {
    FILE *fp = NULL;
    if(lock_type&LOCK_SH) {
        if(pthread_rwlock_tryrdlock(&mu)) {
            perror("rdlock");
            return NULL;
        }
    } else if(lock_type&LOCK_EX) {
        if(pthread_rwlock_wrlock(&mu)) {
            perror("wrlock");
            return NULL;
        }
    }
    fp = fopen(file_path, mode);
    if(!fp) {
        perror("fopen");
        pthread_rwlock_unlock(&mu);
        return NULL;
    }
    printf("Open file in mode %s\n", mode);
    return fp;
}

static int send_all(char* file_path, threadtimer_t *timer) {
    int re = 1;
    FILE *fp = open_file(file_path, LOCK_SH, "rb");
    if(fp) {
        pthread_cleanup_push((void*)&close_file, fp);
        timer->fp = fp;
        timer->is_open = 1;
        uint32_t file_size = (uint32_t)size_of_file(file_path);
        printf("Get file size: %d bytes.\n", (int)file_size);
        off_t len = 0;
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
            re = !sendfile(timer->accept_fd, fileno(fp), &len, file_size) >= 0;
            if(!re) perror("sendfile");
        #endif
        printf("Send %d bytes.\n", (int)len);
        timer->is_open = 0;
        pthread_cleanup_pop(1);
    }
    return re;
}

static int send_data(int accept_fd, char *data, size_t length) {
    if(!~send(accept_fd, data, length, 0)) {
        puts("Send data error");
        return 0;
    }
    printf("Send data: ");
    if(length > 128) {
        data[124] = '.';
        data[125] = '.';
        data[126] = '.';
        data[127] =  0;
    }
    puts(data);
    return 1;
}

static off_t size_of_file(const char* fname) {
    struct stat statbuf;
    if(stat(fname, &statbuf)==0) return statbuf.st_size;
    else return -1;
}

static int sm1_pwd(threadtimer_t *timer) {
    if(!strcmp(cfg->pwd, timer->data)) timer->status = 0;
    return !timer->status;
}

static int s0_init(threadtimer_t *timer) {
    if(!strcmp("get", timer->data)) timer->status = 1;
    else if(!strcmp(cfg->sps, timer->data)) timer->status = 2;
    else if(!strcmp("cat", timer->data)) return send_all(data_path, timer);
    else if(!strcmp("quit", timer->data)) return 0;
    return send_data(timer->accept_fd, timer->data, timer->numbytes);
}

static int s1_get(threadtimer_t *timer) {        //get kanban
    FILE *fp = open_file(kanban_path, LOCK_SH, "rb");
    timer->status = 0;
    if(fp) {
        timer->fp = fp;
        timer->is_open = 1;
        uint32_t ver, cli_ver;
        if(fscanf(fp, "%u", &ver) > 0) {
            if(sscanf(timer->data, "%u", &cli_ver) > 0) {
                if(cli_ver < ver) {     //need to send a new kanban
                    timer->is_open = 0;
                    close_file(fp);
                    int r = send_all(kanban_path, timer);
                    if(strstr(timer->data, "quit") == timer->data + timer->numbytes - 4) {
                        puts("Found last cmd is quit.");
                        return 0;
                    }
                    return r;
                }
            }
        }
    }
    return close_file_and_send(timer, "null", 4);
}

static int s2_set(threadtimer_t *timer) {
    FILE *fp = NULL;
    if(!strcmp(timer->data, "ver")) {
        fp = open_file(kanban_path, LOCK_EX, "wb");
    } else if(!strcmp(timer->data, "dat")) {
        fp = open_file(data_path, LOCK_EX, "wb");
    }
    if(fp) {
        timer->status = 3;
        timer->fp = fp;
        timer->is_open = 1;
        return send_data(timer->accept_fd, "data", 4);
    } else {
        timer->status = 0;
        return send_data(timer->accept_fd, "erro", 4);
    }
}

static int s3_set_data(threadtimer_t *timer) {
    char ret[4] = "succ";
    timer->status = 0;
    #ifdef WORDS_BIGENDIAN
        uint32_t file_size = __builtin_bswap32(*(uint32_t*)(timer->data));
    #else
        uint32_t file_size = *(uint32_t*)(timer->data);
    #endif
    printf("Set data size: %u\n", file_size);
    int is_first_data = 0;
    pthread_cleanup_push((void*)close_file, timer->fp);
    if(timer->numbytes == sizeof(uint32_t)) {
        if((timer->numbytes = recv(timer->accept_fd, timer->data, TIMERDATSZ, 0)) <= 0) {
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        is_first_data = 1;
        printf("Get data size: %d\n", (int)timer->numbytes);
    }
    size_t offset = (is_first_data?0:sizeof(uint32_t));
    if(file_size <= TIMERDATSZ - offset) {
        while(timer->numbytes != file_size - offset) {
            ssize_t n = recv(timer->accept_fd, timer->data + timer->numbytes + offset, TIMERDATSZ - timer->numbytes - offset, MSG_WAITALL);
            if(n <= 0) {
                *(uint32_t*)ret = *(uint32_t*)"erro";
                goto S3_RETURN;
            }
            timer->numbytes += n;
        }
        if(fwrite(timer->data + offset, file_size, 1, timer->fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
        }
        goto S3_RETURN;
    }
    if(fwrite(timer->data + offset, timer->numbytes - offset, 1, timer->fp) != 1) {
        perror("fwrite");
        *(uint32_t*)ret = *(uint32_t*)"erro";
        goto S3_RETURN;
    }
    int32_t remain = file_size - timer->numbytes;
    while(remain > 0) {
        // printf("remain:%d\n", (int)remain);
        ssize_t n = recv(timer->accept_fd, timer->data, (remain>TIMERDATSZ)?TIMERDATSZ:remain, MSG_WAITALL);
        if(n <= 0) {
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        if(fwrite(timer->data, n, 1, timer->fp) != 1) {
            perror("fwrite");
            *(uint32_t*)ret = *(uint32_t*)"erro";
            goto S3_RETURN;
        }
        remain -= n;
    }
    pthread_cleanup_pop(0);
S3_RETURN:
    return close_file_and_send(timer, ret, 4);
}

int main(int argc, char *argv[]) {
    if(argc != 5 && argc != 6) {
        show_usage(argv[0]);
        return 0;
    }
    int port = 0;
    int as_daemon = !strcmp("-d", argv[1]);
    sscanf(argv[as_daemon?2:1], "%d", &port);
    if(port <= 0 || port >= 65536) {
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
    SIMPLE_PB* spb = get_pb(fp);
    cfg = (config_t*)spb->target;
    fclose(fp);
    if(bind_server(port)) {
        perror("Bind server failed");
        return 6;
    }
    puts("Bind server success!");
    if(listen_socket()) {
        perror("Listen failed");
        return 7;
    }
    pthread_cleanup_push((void*)close, (void*)(uintptr_t)fd);
    accept_client();
    pthread_cleanup_pop(1);
    return 99;
}
