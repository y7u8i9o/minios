/* M17 stage 2: message queues, shared memory and poll between a parent
 * and a child. Exits 0 on success. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    printf("mqtest: pid %d\n", getpid());
    CHECK(mq_open("nosuch", 0) < 0 && errno == ENOENT, "open missing queue");
    int req = mq_open("test.req", MQ_CREATE | MQ_EXCL);
    int rep = mq_open("test.rep", MQ_CREATE);
    CHECK(req >= 0 && rep >= 0, "create queues: %s", strerror(errno));
    CHECK(mq_open("test.req", MQ_CREATE | MQ_EXCL) < 0 && errno == EEXIST, "exclusive create");

    int shm = shm_open("test.buf", SHM_CREATE, 2 * 4096);
    CHECK(shm >= 0, "shm_open: %s", strerror(errno));
    unsigned *buf = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    CHECK(buf != MAP_FAILED && buf[0] == 0 && buf[2047] == 0, "shm mmap zero filled");
    buf[0] = 0x1111;

    /* Nothing is readable yet; poll with a timeout returns 0. */
    struct pollfd pf[2] = { { req, POLLIN, 0 }, { rep, POLLIN, 0 } };
    CHECK(poll(pf, 2, 20) == 0, "poll timeout");

    pid_t pid = fork();
    if (pid == 0) {
        /* The child opens the objects by name and maps the memory anew. */
        int creq = mq_open("test.req", 0), crep = mq_open("test.rep", 0);
        int cshm = shm_open("test.buf", 0, 0);
        unsigned *cbuf = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, cshm, 0);
        if (creq < 0 || crep < 0 || cshm < 0 || cbuf == MAP_FAILED)
            _exit(2);
        if (cbuf[0] != 0x1111 || buf[0] != 0x1111)
            _exit(3);   /* inherited mapping and fresh mapping both see the parent's write */
        char msg[64];
        for (int i = 0; i < 100; i++) {
            ssize_t n = mq_recv(creq, msg, sizeof msg);
            if (n <= 0)
                _exit(4);
            if (strcmp(msg, "quit") == 0)
                break;
            cbuf[1 + i] = (unsigned)(n * 1000 + i);
            buf[2047] = (unsigned)i;
            snprintf(msg, sizeof msg, "ack %d", i);
            mq_send(crep, msg, strlen(msg) + 1);
        }
        _exit(0);
    }
    char msg[64];
    for (int i = 0; i < 5; i++) {
        snprintf(msg, sizeof msg, "hello %d", i);
        CHECK(mq_send(req, msg, strlen(msg) + 1) == (ssize_t)strlen(msg) + 1, "send %d", i);
        struct pollfd p = { rep, POLLIN, 0 };
        CHECK(poll(&p, 1, -1) == 1 && (p.revents & POLLIN), "poll reply %d", i);
        ssize_t n = mq_recv(rep, msg, sizeof msg);
        char want[16];
        snprintf(want, sizeof want, "ack %d", i);
        CHECK(n == (ssize_t)strlen(want) + 1 && strcmp(msg, want) == 0, "reply %d: '%s'", i, msg);
        CHECK(buf[1 + i] == (unsigned)(8 * 1000 + i) && buf[2047] == (unsigned)i, "shared memory %d: %x", i, buf[1 + i]);
    }
    /* Message boundaries survive a short receive buffer. */
    mq_send(req, "abcdef", 7);
    mq_send(req, "quit", 5);
    CHECK(mq_send(req, buf, MQ_MSG_MAX + 1) < 0 && errno == EMSGSIZE, "oversized message");
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status 0x%x", status);

    /* Unlink: existing descriptors continue to work, the name is gone. */
    CHECK(mq_unlink("test.req") == 0 && mq_open("test.req", 0) < 0 && errno == ENOENT, "mq_unlink");
    CHECK(shm_unlink("test.buf") == 0 && shm_open("test.buf", 0, 0) < 0, "shm_unlink");
    CHECK(mq_send(req, "x", 1) == 1 && mq_recv(req, msg, sizeof msg) == 1, "queue usable after unlink");
    CHECK(buf[0] == 0x1111, "memory usable after unlink");
    CHECK(munmap(buf, 2 * 4096) == 0, "munmap");
    close(req);
    close(rep);
    close(shm);
    mq_unlink("test.rep");
    printf("mqtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
