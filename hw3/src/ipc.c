#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auction.h"

static void set_names(Ipc* ipc, const char* session, int named) {
    memset(ipc, 0, sizeof(*ipc));
    ipc->named = named;
    snprintf(ipc->memory_name, sizeof(ipc->memory_name), "/a3_%s_mem", session);
    snprintf(ipc->lock_name, sizeof(ipc->lock_name), "/a3_%s_lock", session);
    snprintf(ipc->events_name, sizeof(ipc->events_name), "/a3_%s_events", session);
}

int ipc_create(Ipc* ipc, const char* session, int named, const Auction* initial) {
    set_names(ipc, session, named);
    int fd = shm_open(ipc->memory_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;  // Чужой сеанс не удаляем и не перезаписываем.
    if (ftruncate(fd, sizeof(Auction)) < 0) {
        close(fd);
        goto error;
    }
    void* memory = mmap(NULL, sizeof(Auction), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) goto error;
    ipc->auction = memory;
    *ipc->auction = *initial;
    if (named) {
        ipc->events = sem_open(ipc->events_name, O_CREAT | O_EXCL, 0600, 0);
        if (ipc->events == SEM_FAILED) { ipc->events = NULL; goto error; }
        // Семафор доступа создаётся последним: после него сеанс готов к подключению.
        ipc->lock = sem_open(ipc->lock_name, O_CREAT | O_EXCL, 0600, 1);
        if (ipc->lock == SEM_FAILED) { ipc->lock = NULL; goto error; }
    } else {
        if (sem_init(&ipc->auction->lock_sem, 1, 1) < 0) goto error;
        ipc->lock = &ipc->auction->lock_sem;
        if (sem_init(&ipc->auction->events_sem, 1, 0) < 0) goto error;
        ipc->events = &ipc->auction->events_sem;
    }
    return 0;
error: {
        int saved = errno;
        ipc_close(ipc, 1);
        errno = saved;
        return -1;
    }
}

int ipc_connect(Ipc* ipc, const char* session) {
    set_names(ipc, session, 1);
    int fd = shm_open(ipc->memory_name, O_RDWR, 0);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) < 0 || info.st_size < (off_t)sizeof(Auction)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    void* memory = mmap(NULL, sizeof(Auction), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) return -1;
    ipc->auction = memory;
    ipc->events = sem_open(ipc->events_name, 0);
    if (ipc->events == SEM_FAILED) { ipc->events = NULL; goto error; }
    ipc->lock = sem_open(ipc->lock_name, 0);
    if (ipc->lock == SEM_FAILED) { ipc->lock = NULL; goto error; }
    return 0;
error: {
        int saved = errno;
        ipc_close(ipc, 0);
        errno = saved;
        return -1;
    }
}

void ipc_close(Ipc* ipc, int owner) {
    if (ipc->named) {
        if (ipc->lock) {
            sem_close(ipc->lock);
            if (owner) sem_unlink(ipc->lock_name);
        }
        if (ipc->events) {
            sem_close(ipc->events);
            if (owner) sem_unlink(ipc->events_name);
        }
    } else if (owner) {
        if (ipc->lock) sem_destroy(ipc->lock);
        if (ipc->events) sem_destroy(ipc->events);
    }
    if (ipc->auction) munmap(ipc->auction, sizeof(Auction));
    if (owner) shm_unlink(ipc->memory_name);
}

void ipc_lock(Ipc* ipc) {
    while (sem_wait(ipc->lock) < 0 && errno == EINTR) {}
}

void ipc_unlock(Ipc* ipc) {
    sem_post(ipc->lock);
}

void ipc_wait_event(Ipc* ipc, int milliseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    // Ограниченное ожидание позволяет проверять сигналы и увеличивать такты.
    sem_timedwait(ipc->events, &deadline);
}
