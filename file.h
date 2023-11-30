#ifndef _FILE_H_
#define _FILE_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

// FILE_CACHE_MAX_SIZE 1G
#define FILE_CACHE_MAX_SIZE (1024*1024*1024)

struct file_cache_t {
    pthread_rwlock_t mu;
    char const *path;
    char *data;
    size_t size;
};
typedef struct file_cache_t file_cache_t;

int file_cache_init(file_cache_t* fc, char* path) {
    static int page_size;
    int fd;
    struct stat sb;
    char* mapped;
    if(page_size <= 0) page_size = (int)sysconf(_SC_PAGE_SIZE);
    if(pthread_rwlock_init(&fc->mu, NULL)) {
        perror("pthread_rwlock_init");
        return -1;
    }
    fd = open(path, O_RDWR|O_CREAT, 0644);
    if(fd < 0) {
        perror("open");
        return -2;
    }
    if(fstat(fd, &sb) < 0) {
        perror("fstat");
        return -3;
    }
    if(sb.st_size < page_size) {
        if(ftruncate(fd, page_size) < 0) {
            perror("ftruncate");
            return -4;
        }
        sb.st_size = page_size;
    }
    mapped = mmap(NULL, (size_t)sb.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(mapped == MAP_FAILED) {
        perror("mmap");
        return -5;
    }
    fc->path = path;
    fc->data = mapped+sizeof(uint64_t);
    fc->size = (size_t)sb.st_size;
    return 0;
}

uint64_t file_cache_get_data_size(file_cache_t* fc) {
    #ifdef WORDS_BIGENDIAN
        return __builtin_bswap64(*(uint64_t*)(fc->data - sizeof(uint64_t)));
    #else
        return *(uint64_t*)(fc->data - sizeof(uint64_t));
    #endif
}

void file_cache_set_data_size(file_cache_t* fc, uint64_t size) {
    #ifdef WORDS_BIGENDIAN
        *(uint64_t*)(fc->data - sizeof(uint64_t)) = __builtin_bswap64(size);
    #else
        *(uint64_t*)(fc->data - sizeof(uint64_t)) = size;
    #endif
}

int file_cache_read_lock(file_cache_t* fc) {
    if(pthread_rwlock_rdlock(&fc->mu)) {
        perror("pthread_rwlock_rdlock");
        return 1;
    }
    puts("file_cache_read_lock: obtained");
    return 0;
}

int file_cache_write_lock(file_cache_t* fc) {
    if(pthread_rwlock_wrlock(&fc->mu)) {
        perror("pthread_rwlock_wrlock");
        return 1;
    }
    puts("file_cache_write_lock: obtained");
    return 0;
}

int file_cache_unlock(file_cache_t* fc) {
    if(pthread_rwlock_unlock(&fc->mu)) {
        perror("file_cache_unlock");
        return 1;
    }
    puts("file_cache_unlock: success");
    return 0;
}

// file_cache_realloc must be used after obtaining write lock
int file_cache_realloc(file_cache_t* fc, uint64_t newsize) {
    if(newsize > FILE_CACHE_MAX_SIZE) {
        printf("file_cache_realloc: too big size %llu\n", newsize);
        return -1;
    }
    if(newsize <= fc->size - sizeof(uint64_t)) {
        file_cache_set_data_size(fc, newsize);
        printf("file_cache_realloc: new data size %llu bytes (fast)\n", newsize);
        return 0;
    }
    if(munmap(fc->data - sizeof(uint64_t), fc->size) < 0) {
        perror("munmap");
        return -2;
    }
    int fd = open(fc->path, O_RDWR|O_CREAT);
    if(fd < 0) {
        perror("open");
        return -3;
    }
    fc->size = (size_t)newsize + sizeof(uint64_t);
    if(ftruncate(fd, fc->size) < 0) {
        perror("ftruncate");
        return -4;
    }
    fc->data = mmap(NULL, fc->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(fc->data == MAP_FAILED) {
        perror("mmap");
        return -5;
    }
    fc->data += sizeof(uint64_t);
    file_cache_set_data_size(fc, newsize);
    printf("file_cache_realloc: new data size %llu bytes\n", newsize);
    return 0;
}

int file_cache_close(file_cache_t* fc) {
    if(munmap(fc->data - sizeof(uint64_t), fc->size) < 0) {
        perror("munmap");
        return -1;
    }
    if(pthread_rwlock_destroy(&fc->mu)) {
        perror("pthread_rwlock_destroy");
        return -2;
    }
    return 0;
}

#endif
