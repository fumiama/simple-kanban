#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#if !__APPLE__
    #include <sys/sendfile.h> 
#endif

#define PASSWORD "fumiama"

#define SETPASS "minamoto"

int fd;
socklen_t struct_len = sizeof(struct sockaddr_in);
struct sockaddr_in server_addr;

char *data_path;
char *kanban_path;

pthread_t accept_threads[8];

#define MAXWAITSEC 10
struct THREADTIMER {
    pthread_t *thread;
    time_t touch;
    int accept_fd;
    ssize_t numbytes;
    char *data;
    char status;
    char is_open;
    FILE *fp;
};
typedef struct THREADTIMER THREADTIMER;

#define showUsage(program) printf("Usage: %s [-d] listen_port try_times kanban_file data_file\n\t-d: As daemon\n", program)

void acceptClient();
void acceptTimer(void *p);
int bindServer(uint16_t port, u_int try_times);
int checkBuffer(THREADTIMER *timer);
int freeAfterSend(int accept_fd, char *data, size_t length);
void closeFile(FILE *fp);
int closeFileAndSend(THREADTIMER *timer, char *data, size_t numbytes);
off_t fileSize(const char* fname);
void handleAccept(void *accept_fd_p);
void handle_pipe(int signo);
void handle_quit(int signo);
int listenSocket(u_int try_times);
FILE *openFile(char* file_path, int lock_type, char* mode);
int sendAll(char* file_path, THREADTIMER *timer);
int sendData(int accept_fd, char *data, size_t length);
int sm1_pwd(THREADTIMER *timer);
int s0_init(THREADTIMER *timer);
int s1_get(THREADTIMER *timer);
int s2_set(THREADTIMER *timer);
int s3_setData(THREADTIMER *timer);

int bindServer(uint16_t port, u_int try_times) {
    int fail_count = 0;
    int result = -1;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(server_addr.sin_zero), 8);
    
    fd = socket(AF_INET, SOCK_STREAM, 0);
    while(!~(result = bind(fd, (struct sockaddr *)&server_addr, struct_len)) && fail_count++ < try_times) sleep(1);
    if(!~result && fail_count >= try_times) {
        puts("Bind server failure!");
        return 0;
    } else{
        puts("Bind server success!");
        return 1;
    }
}

int listenSocket(u_int try_times) {
    int fail_count = 0;
    int result = -1;
    while(!~(result = listen(fd, 10)) && fail_count++ < try_times) sleep(1);
    if(!~result && fail_count >= try_times) {
        puts("Listen failed!");
        return 0;
    } else{
        puts("Listening....");
        return 1;
    }
}

int freeAfterSend(int accept_fd, char *data, size_t length) {
    int re = sendData(accept_fd, data, length);
    free(data);\
    return re;
}

int sendData(int accept_fd, char *data, size_t length) {
    if(!~send(accept_fd, data, length, 0)) {
        puts("Send data error");
        return 0;
    } else {
        printf("Send data: ");
        puts(data);
        return 1;
    }
}

int sendAll(char* file_path, THREADTIMER *timer) {
    int re = 1;
    FILE *fp = openFile(file_path, LOCK_SH, "rb");
    size_t numbytes;
    if(fp) {
        timer->fp = fp;
        timer->is_open = 1;
        uint32_t file_size = (uint32_t)fileSize(file_path);
        printf("Get file size: %u bytes.\n", file_size);
        off_t len = 0;
        #if __APPLE__
            struct sf_hdtr hdtr;
            struct iovec headers;
            headers.iov_base = &file_size;
            headers.iov_len = sizeof(uint32_t);
            hdtr.headers = &headers;
            hdtr.hdr_cnt = 1;
            sendfile(fileno(fp), timer->accept_fd, 0, &len, &hdtr, 0);
        #else
            send(timer->accept_fd, &file_size, sizeof(uint32_t), 0);
            sendfile(timer->accept_fd, fileno(fp), &len, file_size);
        #endif
        printf("Send %lld bytes.\n", len);
        closeFile(fp);
        timer->is_open = 0;
    }
    return re;
}

int sm1_pwd(THREADTIMER *timer) {
    if(!strcmp(PASSWORD, timer->data)) timer->status = 0;
    return !timer->status;
}

int s0_init(THREADTIMER *timer) {
    if(!strcmp("get", timer->data)) timer->status = 1;
    else if(!strcmp("set" SETPASS, timer->data)) timer->status = 2;
    else if(!strcmp("cat", timer->data)) return sendAll(data_path, timer);
    else if(!strcmp("quit", timer->data)) return 0;
    return sendData(timer->accept_fd, timer->data, timer->numbytes);
}

int s1_get(THREADTIMER *timer) {        //get kanban
    FILE *fp = openFile(kanban_path, LOCK_SH, "rb");
    timer->status = 0;
    if(fp) {
        timer->fp = fp;
        timer->is_open = 1;
        uint32_t ver, cli_ver;
        if(fscanf(fp, "%u", &ver) > 0) {
            if(sscanf(timer->data, "%u", &cli_ver) > 0) {
                if(cli_ver < ver) {     //need to send a new kanban
                    closeFile(fp);
                    return sendAll(kanban_path, timer);
                }
            }
        }
    }
    return closeFileAndSend(timer, "null", 4);
}

int s2_set(THREADTIMER *timer) {
    FILE *fp = NULL;
    if(!strcmp(timer->data, "ver")) {
        fp = openFile(kanban_path, LOCK_EX, "wb");
    } else if(!strcmp(timer->data, "dat")) {
        fp = openFile(data_path, LOCK_EX, "wb");
    }
    if(fp) {
        timer->status = 3;
        timer->fp = fp;
        timer->is_open = 1;
        return sendData(timer->accept_fd, "data", 4);
    } else {
        timer->status = 0;
        return sendData(timer->accept_fd, "erro", 4);
    }
}

int s3_setData(THREADTIMER *timer) {
    timer->status = 0;
    uint32_t file_size = *(uint32_t*)(timer->data);
    printf("Set data size: %u\n", file_size);
    int is_first_data = 0;
    if(timer->numbytes == sizeof(uint32_t)) {
        if((timer->numbytes = recv(timer->accept_fd, timer->data, BUFSIZ, 0)) <= 0) 
            return closeFileAndSend(timer, "erro", 4);
        else {
            is_first_data = 1;
            printf("Get data size: %tu\n", timer->numbytes);
        }
    }
    if(file_size <= BUFSIZ - (is_first_data?0:sizeof(uint32_t))) {
        while(timer->numbytes != file_size - (is_first_data?0:sizeof(uint32_t))) {
            timer->numbytes += recv(timer->accept_fd, timer->data + timer->numbytes + (is_first_data?0:sizeof(uint32_t)), BUFSIZ - timer->numbytes - (is_first_data?0:sizeof(uint32_t)), 0);
        }
        if(fwrite(timer->data + (is_first_data?0:sizeof(uint32_t)), file_size, 1, timer->fp) != 1) {
            puts("Set data error.");
            return closeFileAndSend(timer, "erro", 4);
        } else return closeFileAndSend(timer, "succ", 4);
    } else {
        if(fwrite(timer->data + (is_first_data?0:sizeof(uint32_t)), timer->numbytes - (is_first_data?0:sizeof(uint32_t)), 1, timer->fp) != 1) {
            puts("Set data error.");
            return closeFileAndSend(timer, "erro", 4);
        }
        int32_t remain = file_size - timer->numbytes;
        while(remain > 0) {
            printf("remain:%d\n", remain);
            timer->numbytes = recv(timer->accept_fd, timer->data, BUFSIZ, 0);
            if(fwrite(timer->data, timer->numbytes, 1, timer->fp) != 1) {
                puts("Set data error.");
                return closeFileAndSend(timer, "erro", 4);
            }
            remain -= timer->numbytes;
        }
        return closeFileAndSend(timer, "succ", 4);
    }
    return closeFileAndSend(timer, "erro", 4);
}

off_t fileSize(const char* fname) {
    struct stat statbuf;
    if(stat(fname, &statbuf)==0) return statbuf.st_size;
    else return -1;
}

int checkBuffer(THREADTIMER *timer) {
    printf("Status: %d\n", timer->status);
    switch(timer->status) {
        case -1: return sm1_pwd(timer); break;
        case 0: return s0_init(timer); break;
        case 1: return s1_get(timer); break;
        case 2: return s2_set(timer); break;
        case 3: return s3_setData(timer); break;
        default: return -1; break;
    }
}

void handle_quit(int signo) {
    printf("Handle sig %d\n", signo);
    pthread_exit(NULL);
}

#define timerPointerOf(x) ((THREADTIMER*)(x))
#define touchTimer(x) timerPointerOf(x)->touch = time(NULL)

void acceptTimer(void *p) {
    THREADTIMER *timer = timerPointerOf(p);
    signal(SIGQUIT, handle_pipe);
    signal(SIGPIPE, handle_pipe);
    while(*timer->thread && !pthread_kill(*timer->thread, 0)) {
        sleep(MAXWAITSEC);
        puts("Check accept status");
        if(time(NULL) - timer->touch > MAXWAITSEC) {
            pthread_kill(*timer->thread, SIGQUIT);
            close(timer->accept_fd);
            if(timer->data) free(timer->data);
            if(timer->is_open) closeFile(timer->fp);
            *timer->thread = 0;
        }
    }
    free(p);
}

void handle_pipe(int signo) {
    puts("Pipe error");
}

void handleAccept(void *p) {
    int accept_fd = timerPointerOf(p)->accept_fd;
    if(accept_fd > 0) {
        puts("Connected to the client.");
        signal(SIGQUIT, handle_quit);
        signal(SIGPIPE, handle_pipe);
        pthread_t thread;
        if (pthread_create(&thread, NULL, (void *)&acceptTimer, p)) puts("Error creating timer thread");
        else puts("Creating timer thread succeeded");
        sendData(accept_fd, "Welcome to simple kanban server.", 33);
        timerPointerOf(p)->status = -1;
        char *buff = calloc(BUFSIZ, sizeof(char));
        if(buff) {
            timerPointerOf(p)->data = buff;
            while(*timerPointerOf(p)->thread && (timerPointerOf(p)->numbytes = recv(accept_fd, buff, BUFSIZ, 0)) > 0) {
                touchTimer(p);
                buff[timerPointerOf(p)->numbytes] = 0;
                printf("Get %zd bytes: %s\n", timerPointerOf(p)->numbytes, buff);
                puts("Check buffer");
                if(!checkBuffer(timerPointerOf(p))) break;
            }
            printf("Recv %zd bytes\n", timerPointerOf(p)->numbytes);
        } else puts("Error allocating buffer");
        close(accept_fd);
    } else puts("Error accepting client");
}

void acceptClient() {
    pid_t pid = fork();
    while (pid > 0) {      //主进程监控子进程状态，如果子进程异常终止则重启之
        wait(NULL);
        puts("Server subprocess exited. Restart...");
        pid = fork();
    }
    if(pid < 0) puts("Error when forking a subprocess.");
    else while(1) {
        puts("Ready for accept, waitting...");
        int p = 0;
        while(p < 8 && accept_threads[p] && !pthread_kill(accept_threads[p], 0)) p++;
        if(p < 8) {
            printf("Run on thread No.%d\n", p);
            THREADTIMER *timer = malloc(sizeof(THREADTIMER));
            if(timer) {
                struct sockaddr_in client_addr;
                timer->accept_fd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
                timer->thread = &accept_threads[p];
                timer->touch = time(NULL);
                timer->data = NULL;
                if (pthread_create(timer->thread, NULL, (void *)&handleAccept, timer)) puts("Error creating thread");
                else puts("Creating thread succeeded");
            } else puts("Allocate timer error");
        } else {
            puts("Max thread cnt exceeded");
            sleep(1);
        }
    }
}

FILE *openFile(char* file_path, int lock_type, char* mode) {
    FILE *fp = NULL;
    fp = fopen(file_path, mode);
    if(fp) {
        if(!~flock(fileno(fp), lock_type | LOCK_NB)) {
            printf("Error: ");
            fp = NULL;
        }
        printf("Open dict in mode %d\n", lock_type);
    } else puts("Open dict error");
    return fp;
}

int closeFileAndSend(THREADTIMER *timer, char *data, size_t numbytes) {
    closeFile(timer->fp);
    timer->is_open = 0;
    return sendData(timer->accept_fd, data, numbytes);
}

void closeFile(FILE *fp) {
    puts("Close dict");
    if(fp) {
        flock(fileno(fp), LOCK_UN);
        fclose(fp);
    }
}

int main(int argc, char *argv[]) {
    if(argc != 5 && argc != 6) showUsage(argv[0]);
    else {
        int port = 0;
        int as_daemon = !strcmp("-d", argv[1]);
        sscanf(argv[as_daemon?2:1], "%d", &port);
        if(port > 0 && port < 65536) {
            int times = 0;
            sscanf(argv[as_daemon?3:2], "%d", &times);
            if(times > 0) {
                if(!as_daemon || (as_daemon && (daemon(1, 1) >= 0))) {
                    FILE *fp = NULL;
                    fp = fopen(argv[as_daemon?4:3], "rb+");
                    if(!fp) fp = fopen(argv[as_daemon?4:3], "wb+");
                    if(fp) {
                        kanban_path = argv[as_daemon?4:3];
                        fclose(fp);
                        fp = NULL;
                        fp = fopen(argv[as_daemon?5:4], "rb+");
                        if(!fp) fp = fopen(argv[as_daemon?5:4], "wb+");
                        if(fp) {
                            data_path = argv[as_daemon?5:4];
                            fclose(fp);
                            signal(SIGQUIT, handle_pipe);
                            signal(SIGPIPE, handle_pipe);
                            if(bindServer(port, times)) if(listenSocket(times)) acceptClient();
                        }
                    } else printf("Error opening kanban file: %s\n", argv[as_daemon?4:3]);
                } else puts("Start daemon error");
            } else printf("Error times: %d\n", times);
        } else printf("Error port: %d\n", port);
    }
    close(fd);
    exit(EXIT_FAILURE);
}
