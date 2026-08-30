/* M17 stage 1: map the framebuffer and draw a checkerboard the kernel
 * test verifies. Also checks the info ioctl, exclusive acquisition, a
 * fork sharing the mapping, and that munmap of the device region works. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static struct fb_info *fbi;
static unsigned char *fbmem;

/* Pack 0x00RRGGBB into the framebuffer's own layout and store it. */
static void put_pixel(unsigned x, unsigned y, unsigned rgb)
{
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    unsigned pix = ((r >> (8 - fbi->red_size)) << fbi->red_shift) |
                   ((g >> (8 - fbi->green_size)) << fbi->green_shift) |
                   ((b >> (8 - fbi->blue_size)) << fbi->blue_shift);
    unsigned char *p = fbmem + (size_t)y * fbi->pitch + (size_t)x * (fbi->bpp / 8);
    if (fbi->bpp == 32) {
        *(unsigned *)p = pix;
    } else {
        p[0] = (unsigned char)pix;
        p[1] = (unsigned char)(pix >> 8);
        p[2] = (unsigned char)(pix >> 16);
    }
}

static unsigned get_pixel(unsigned x, unsigned y)
{
    unsigned char *p = fbmem + (size_t)y * fbi->pitch + (size_t)x * (fbi->bpp / 8);
    unsigned pix = fbi->bpp == 32 ? *(unsigned *)p : (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    unsigned r = (pix >> fbi->red_shift) & ((1u << fbi->red_size) - 1);
    unsigned g = (pix >> fbi->green_shift) & ((1u << fbi->green_size) - 1);
    unsigned b = (pix >> fbi->blue_shift) & ((1u << fbi->blue_size) - 1);
    return ((r << (8 - fbi->red_size)) << 16) | ((g << (8 - fbi->green_size)) << 8) | (b << (8 - fbi->blue_size));
}

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("fbtest: open: %s\n", strerror(errno));
        return 1;
    }
    struct fb_info info;
    if (ioctl(fd, FBIOGET_INFO, &info) < 0 || (info.bpp != 32 && info.bpp != 24) || info.width < 256 || info.height < 256) {
        printf("fbtest: bad info %ux%u %u bpp\n", info.width, info.height, info.bpp);
        return 1;
    }
    printf("fbtest: %ux%u %u bpp r%u@%u g%u@%u b%u@%u\n", info.width, info.height, info.bpp,
           info.red_size, info.red_shift, info.green_size, info.green_shift, info.blue_size, info.blue_shift);
    size_t size = (size_t)info.pitch * info.height;
    unsigned char *fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        printf("fbtest: mmap: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, FBIO_ACQUIRE, 0) < 0) {
        printf("fbtest: acquire: %s\n", strerror(errno));
        return 1;
    }
    int fd2 = open("/dev/fb0", O_RDWR);
    if (ioctl(fd2, FBIO_ACQUIRE, 0) == 0 || errno != EBUSY) {
        printf("fbtest: second acquire not refused\n");
        return 1;
    }
    close(fd2);
    fbi = &info;
    fbmem = fb;
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++)
            put_pixel(x + 100, y + 100, ((x ^ y) & 1) ? 0x00ff8800 : 0x000044ff);
    /* A child sees and may draw into the same framebuffer. */
    pid_t pid = fork();
    if (pid == 0) {
        int ok = get_pixel(101, 100) == 0x00ff8800;
        put_pixel(100, 100, 0x000044ff);
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    if (status != 0 || get_pixel(100, 100) != 0x000044ff) {
        printf("fbtest: child mapping failed (status 0x%x)\n", status);
        return 1;
    }
    /* Signal the kernel test through a marker file, then wait for a line
     * on the console before giving the display back. */
    int m = open("/fb.ready", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m >= 0)
        close(m);
    char line[16];
    read(0, line, sizeof line);
    if (munmap(fb, size) < 0) {
        printf("fbtest: munmap: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, FBIO_RELEASE, 0) < 0) {
        printf("fbtest: release: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    return 0;
}
