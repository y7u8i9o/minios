/* Pure socket shape validation, also exercised by host sanitizer fuzzing. */
#include <ipc/socket_validate.h>
#include <errno.h>

int socket_parse_address(const uint8_t *bytes, size_t length)
{
    if (length < 2 || length > 128)
        return -EINVAL;
    unsigned family = bytes[0] | (unsigned)bytes[1] << 8;
    switch (family) {
    case 1: /* AF_UNIX */
        return 0;
    case 2: /* AF_INET */
        return length >= 16 ? 0 : -EINVAL;
    default:
        return -EAFNOSUPPORT;
    }
}
int socket_iovec_size(const size_t *lengths, size_t count, size_t *total)
{
    if (count > 32)
        return -EINVAL;
    const size_t limit = (size_t)1 << 30;
    size_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        if (lengths[i] > limit - sum)
            return -EINVAL;
        sum += lengths[i];
    }
    *total = sum;
    return 0;
}
