#include <lib/random.h>
#include <ipc/socket.h>
#include <console.h>
#include <tests/ktest.h>
#include <errno.h>
#include "net_helpers.h"

static void test_random(void)
{
    ktest_assert(random_ready(), "VirtIO entropy seeded before networking");
    uint32_t values[64];
    for (unsigned i = 0; i < 64; i++) {
        ktest_assert(random_u32(&values[i]) == 0, "random provider draw");
        for (unsigned j = 0; j < i; j++)
            ktest_assert(values[i] != values[j], "rekeyed random outputs differ");
    }
    kprintf("net_random: seeded and rekeyed, ok\n");
}
KTEST_DEFINE("net_random", test_random);
static void test_unavailable(void)
{
    ktest_assert(!random_ready(), "missing or rejected entropy remains unavailable");
    uint32_t value = 0x12345678;
    ktest_assert(random_u32(&value) == -EAGAIN && value == 0x12345678,
                 "unavailable source never returns fallback bytes");
    struct file *file;
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, O_NONBLOCK, &file) == 0,
                 "socket creation remains available");
    struct sockaddr_storage name;
    address(&name, IPV4_LOOPBACK, 9000);
    ktest_assert(socket_connect(socket_from_file(file), &name, 16) == -EAGAIN,
                 "active TCP fails closed without entropy");
    ktest_assert(socket_bind(socket_from_file(file), &name, 16) == 0 &&
                     socket_listen(socket_from_file(file), 1) == -EAGAIN,
                 "passive TCP fails closed without entropy");
    file_put(file);
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, 0, &file) == 0, "UDP endpoint available");
    address(&name, IPV4_LOOPBACK, 0);
    ktest_assert(socket_bind(socket_from_file(file), &name, 16) == -EAGAIN,
                 "automatic UDP port has no predictable fallback");
    file_put(file);
    kprintf("net_random_unavailable: boot and explicit failures, ok\n");
}
KTEST_DEFINE("net_random_unavailable", test_unavailable);
