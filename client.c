#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#if !__APPLE__
    #include <sys/sendfile.h> 
#else
    struct sf_hdtr hdtr; 
#endif

int sockfd;
char buf[BUFSIZ];
char bufr[BUFSIZ];
struct sockaddr_in their_addr;
pthread_t thread;
uint32_t file_size;
int recv_bin = 0;
FILE* fp;

static void __attribute__((destructor)) defer_close_fp() {
    if (fp) fclose(fp);
}

void getMessage(void *p) {
    int c, i;
    while((c = recv(sockfd, bufr, BUFSIZ, 0)) > 0) {
        printf("Recv %u bytes: ", c);
        if(recv_bin) {
            if(fp == NULL) fp = fopen("dump.bin", "w+");
            fwrite(bufr, c, 1, fp);
        } else for(i = 0; i < c; i++) putchar(bufr[i]);
        putchar('\n');
    }
}

off_t size_of_file(const char* fname) {
    struct stat statbuf;
    if(stat(fname, &statbuf)==0) return statbuf.st_size;
    else return -1;
}

int main(int argc,char *argv[]) {   //usage: ./client host port
    ssize_t numbytes;
    puts("break!");
    while((sockfd = socket(AF_INET,SOCK_STREAM,0)) == -1);
    puts("Get sockfd");
    their_addr.sin_family = AF_INET;
    their_addr.sin_port = htons(atoi(argv[2]));
    their_addr.sin_addr.s_addr=inet_addr(argv[1]);
    bzero(&(their_addr.sin_zero), 8);
    
    while(connect(sockfd,(struct sockaddr*)&their_addr,sizeof(struct sockaddr)) == -1);
    puts("Connected to server");
    numbytes = recv(sockfd, buf, BUFSIZ,0);
    buf[numbytes]='\0';  
    puts(buf);
    if(!pthread_create(&thread, NULL, (void*)&getMessage, NULL)) {
        puts("Thread create succeeded");
        while(1) {
            printf("Enter command:");
            scanf("%s",buf);
            if(!strcmp(buf, "bin")) recv_bin = !recv_bin;
            else if(!strcmp(buf, "file")) {
                printf("Enter file path:");
                scanf("%s",buf);
                printf("Open:");
                puts(buf);
                FILE *fp = NULL;
                fp = fopen(buf, "rb");
                if(fp) {
                    off_t len = 0;
                    file_size = (uint32_t)size_of_file(buf);
                    #if __APPLE__
                        struct iovec headers;
                        headers.iov_base = &file_size;
                        headers.iov_len = sizeof(uint32_t);
                        hdtr.headers = &headers;
                        hdtr.hdr_cnt = 1;
                        hdtr.trailers = NULL;
                        hdtr.trl_cnt = 0;
                        if(!sendfile(fileno(fp), sockfd, 0, &len, &hdtr, 0)) puts("Send file success.");
                        else puts("Send file error.");
                    #else
                        send(sockfd, &file_size, sizeof(uint32_t), 0);
                        if(!sendfile(sockfd, fileno(fp), &len, file_size)) puts("Send file success.");
                        else puts("Send file error.");
                    #endif
                    fclose(fp);
                    printf("Send count:%d\n", (int)len);
                } else puts("Open file error!");
            } else {
                send(sockfd, buf, strlen(buf), 0);
                if(!strcmp(buf, "quit")) exit(EXIT_SUCCESS);
            }
            sleep(1);
        }
    } else perror("Create msg thread failed");
    close(sockfd);
    return 0;
}
