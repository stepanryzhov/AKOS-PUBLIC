#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "queue.h"

static struct timespec deadline_after(int milliseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

int queues_open(Queues* q, const char* session, int create) {
    memset(q, 0, sizeof(*q));
    q->requests = q->replies = (mqd_t)-1;
    snprintf(q->requests_name, sizeof(q->requests_name), "/a3q_%s_in", session);
    snprintf(q->replies_name, sizeof(q->replies_name), "/a3q_%s_out", session);
    snprintf(q->gate_name, sizeof(q->gate_name), "/a3q_%s_gate", session);
    int flags = O_RDWR | (create ? O_CREAT | O_EXCL : 0);
    struct mq_attr attributes = {.mq_maxmsg = 1, .mq_msgsize = sizeof(Request)};
    q->requests = mq_open(q->requests_name, flags, 0600, &attributes);
    if (q->requests == (mqd_t)-1) return -1;
    attributes.mq_msgsize = sizeof(Reply);
    q->replies = mq_open(q->replies_name, flags, 0600, &attributes);
    if (q->replies == (mqd_t)-1) goto error;
    q->gate = create ? sem_open(q->gate_name, O_CREAT | O_EXCL, 0600, 1)
                     : sem_open(q->gate_name, 0);
    if (q->gate == SEM_FAILED) { q->gate = NULL; goto error; }
    return 0;
error: {
        int saved = errno;
        queues_close(q, create);
        errno = saved;
        return -1;
    }
}

void queues_close(Queues* q, int owner) {
    if (q->gate) {
        sem_close(q->gate);
        if (owner) sem_unlink(q->gate_name);
    }
    if (q->replies != (mqd_t)-1) {
        mq_close(q->replies);
        if (owner) mq_unlink(q->replies_name);
    }
    if (q->requests != (mqd_t)-1) {
        mq_close(q->requests);
        if (owner) mq_unlink(q->requests_name);
    }
}

int queues_request(Queues* q, const Request* request, Reply* reply) {
    struct timespec deadline = deadline_after(5000);
    int result;
    do {
        result = sem_timedwait(q->gate, &deadline);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return -1;

    // Семафор защищает весь обмен: ответ получает именно отправитель запроса.
    // Аукционист семафор не захватывает и продолжает принимать сообщения.
    do {
        result = mq_timedsend(q->requests, (const char*)request, sizeof(*request), 0, &deadline);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        ssize_t size;
        do {
            size = mq_timedreceive(q->replies, (char*)reply, sizeof(*reply), NULL, &deadline);
        } while (size < 0 && errno == EINTR);
        if (size != (ssize_t)sizeof(*reply) || reply->pid != request->pid) {
            if (size >= 0) errno = EPROTO;
            result = -1;
        }
    }
    int saved = errno;
    sem_post(q->gate);
    errno = saved;
    return result;
}

int queues_receive(Queues* q, Request* request, int milliseconds) {
    struct timespec deadline = deadline_after(milliseconds);
    ssize_t size = mq_timedreceive(q->requests, (char*)request, sizeof(*request), NULL, &deadline);
    if (size < 0 && (errno == ETIMEDOUT || errno == EINTR)) return 1;
    if (size != (ssize_t)sizeof(*request)) {
        if (size >= 0) errno = EPROTO;
        return -1;
    }
    return 0;
}

int queues_reply(Queues* q, const Reply* reply) {
    struct timespec deadline = deadline_after(5000);
    int result;
    do {
        result = mq_timedsend(q->replies, (const char*)reply, sizeof(*reply), 0, &deadline);
    } while (result < 0 && errno == EINTR);
    return result;
}
