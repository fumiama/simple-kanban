#ifndef _FILE_H_
#define _FILE_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static char* file_filepath[2];

static volatile uint16_t has_file_opened[2];
static volatile uint16_t is_file_opening[2];
static volatile uint32_t file_owner_index[2] = {(uint32_t)-1, (uint32_t)-1};

static FILE* file_fp[2];
static pthread_rwlock_t mu[2];

static inline off_t get_file_size(int isdata) {
    struct stat statbuf;
    if(stat(file_filepath[!!isdata], &statbuf)==0) {
        return statbuf.st_size;
    }
    return -1;
}

static int init_file(char* file_path[2]) {
    for (int i = 0; i < 2; i++) {
        FILE* fp = fopen(file_path[i], "rb+");
        if(!fp) {
            perror("Open file error");
            return 2;
        }
        int err = pthread_rwlock_init(&mu[i], NULL);
        if(err) {
            perror("Init lock error");
            return 1;
        }
        file_filepath[i] = file_path[i];
        fclose(fp);
    }
    return 0;
}

static inline FILE* open_file(uint32_t index, int isdata, int isro) {
    isdata = !!isdata;
    is_file_opening[isdata] = 1;
    if(pthread_rwlock_wrlock(&mu[isdata])) {
        perror("Open file: Writelock busy");
        is_file_opening[isdata] = 0;
        return NULL;
    }
    is_file_opening[isdata] = 0;
    file_fp[isdata] = fopen(file_filepath[isdata], isro?"rb":"rb+");
    if(!file_fp[isdata]) {
        perror("Open file: fopen");
        pthread_rwlock_unlock(&mu[isdata]);
        return NULL;
    }
    has_file_opened[isdata] = 1;
    file_owner_index[isdata] = index;
    puts("Open file");
    return file_fp[isdata];
}

static inline int require_shared_lock(int isdata) {
    if(pthread_rwlock_rdlock(&mu[!!isdata])) {
        perror("Open file: Readlock busy");
        return 1;
    }
    puts("Shared lock required");
    return 0;
}

static inline void release_shared_lock(int isdata) {
    pthread_rwlock_unlock(&mu[!!isdata]);
    puts("Release shared lock");
}

static inline void close_file(uint32_t index, int isdata) {
    isdata = !!isdata;
    if(index != file_owner_index[isdata]) return;
    if(has_file_opened[isdata]) {
        fclose(file_fp[isdata]);
        file_fp[isdata] = NULL;
        has_file_opened[isdata] = 0;
        file_owner_index[isdata] = (uint32_t)-1;
        pthread_rwlock_unlock(&mu[isdata]);
        puts("Close file");
    } else puts("file already closed");
}

static void close_file_wrap(uint32_t index_isdata[2]) {
    close_file(index_isdata[0], (int)index_isdata[1]);
}

#endif
