#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int transfer(int fd, unsigned char command, int direction,
                    unsigned char *bytes, int length)
{
    union i2c_smbus_data data = {0};
    struct i2c_smbus_ioctl_data request = {
        .read_write = direction,
        .command = command,
        .size = I2C_SMBUS_BLOCK_DATA,
        .data = &data,
    };

    if (direction == I2C_SMBUS_WRITE) {
        data.block[0] = length;
        memcpy(data.block + 1, bytes, length);
    }
    for (int attempt = 0; ioctl(fd, I2C_SMBUS, &request) < 0; attempt++) {
        if (direction != I2C_SMBUS_READ || errno != ENXIO || attempt == 2)
            return -1;
        nanosleep(&(struct timespec){.tv_nsec = 50000000}, NULL);
    }
    if (direction == I2C_SMBUS_READ) {
        if (data.block[0] != length) {
            errno = EPROTO;
            return -1;
        }
        memcpy(bytes, data.block + 1, length);
    }
    nanosleep(&(struct timespec){.tv_nsec = 50000000}, NULL);
    return 0;
}

static int parse_color(const char *input, unsigned char rgb[3])
{
    if (*input == '#')
        input++;
    if (strlen(input) != 6)
        return -1;
    for (int i = 0; i < 3; i++) {
        if (!isxdigit((unsigned char)input[i * 2]) ||
            !isxdigit((unsigned char)input[i * 2 + 1]))
            return -1;
        char pair[3] = { input[i * 2], input[i * 2 + 1], 0 };
        char *end;
        unsigned long value = strtoul(pair, &end, 16);
        if (end != pair + 2)
            return -1;
        rgb[i] = value;
    }
    return 0;
}

static int parse_brightness(const char *input, unsigned char *brightness)
{
    if (!*input || strspn(input, "0123456789") != strlen(input))
        return -1;
    unsigned long value = strtoul(input, NULL, 10);
    if (value > 10)
        return -1;
    *brightness = value;
    return 0;
}

static void color_packet(unsigned char packet[7],
                         const unsigned char rgb[3], unsigned char brightness)
{
    packet[0] = 0x00; /* global area mask, high byte */
    packet[1] = 0x01;
    memcpy(packet + 2, rgb, 3);
    packet[5] = brightness;
    packet[6] = 0x03;
}

int main(int argc, char **argv)
{
    unsigned char rgb[3], brightness = 0, effect[6], before[6], after[6], packet[7];
    int fd;
    int probe = argc == 2 && strcmp(argv[1], "--probe") == 0;
    FILE *model;
    char name[64];

    if ((argc != 2 && argc != 3) ||
        (!probe && parse_color(argv[1], rgb) < 0) ||
        (argc == 3 && parse_brightness(argv[2], &brightness) < 0)) {
        fprintf(stderr, "usage: %s [--probe | RRGGBB [BRIGHTNESS 0-10]]\n", argv[0]);
        return 2;
    }
    model = fopen("/sys/class/dmi/id/product_name", "r");
    if (!model || !fgets(name, sizeof(name), model)) {
        perror("read product name");
        if (model) fclose(model);
        return 1;
    }
    fclose(model);
    name[strcspn(name, "\n")] = 0;
    if (strcmp(name, "Predator PO7-660") != 0) {
        fprintf(stderr, "unsupported model: %s\n", name);
        return 1;
    }
    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0 || ioctl(fd, I2C_SLAVE, 0x29) < 0) {
        perror("open lighting controller");
        if (fd >= 0) close(fd);
        return 1;
    }
    if (probe) {
        unsigned char *values[2] = {effect, before};
        unsigned char commands[2] = {0x65, 0x67};
        for (int i = 0; i < 2; i++) {
            if (transfer(fd, commands[i], I2C_SMBUS_READ, values[i], 6) < 0) {
                fprintf(stderr, "read 0x%02x failed: %s\n", commands[i], strerror(errno));
                close(fd);
                return 1;
            }
            printf("0x%02x: %02x %02x %02x %02x %02x %02x\n", commands[i],
                   values[i][0], values[i][1], values[i][2],
                   values[i][3], values[i][4], values[i][5]);
        }
        close(fd);
        return 0;
    }
    if (transfer(fd, 0x65, I2C_SMBUS_READ, effect, 6) < 0) {
        perror("read global effect (0x65); no changes made");
        close(fd);
        return 1;
    }
    if (transfer(fd, 0x67, I2C_SMBUS_READ, before, 6) < 0) {
        perror("read global color (0x67); no changes made");
        close(fd);
        return 1;
    }
    if (memcmp(effect, (unsigned char[]){1, 0, 3, 3, 0, 255}, 6) ||
        before[4] != 0xff || before[5] != 0xff || before[3] > 100) {
        fprintf(stderr, "unexpected controller state; no changes made\n"
                "  effect:  %02x %02x %02x %02x %02x %02x\n"
                "  color:   %02x %02x %02x %02x %02x %02x\n",
                effect[0], effect[1], effect[2], effect[3], effect[4], effect[5],
                before[0], before[1], before[2], before[3], before[4], before[5]);
        close(fd);
        return 1;
    }
    if (argc == 2)
        brightness = before[3];
    color_packet(packet, rgb, brightness);
    if (transfer(fd, 0x07, I2C_SMBUS_WRITE, packet, 7) < 0) {
        perror("set color and brightness");
        close(fd);
        return 1;
    }
    if (transfer(fd, 0x67, I2C_SMBUS_READ, after, 6) < 0 ||
        memcmp(after, rgb, 3) || after[3] != brightness) {
        unsigned char original[7], restored[6];
        color_packet(original, before, before[3]);
        if (transfer(fd, 0x07, I2C_SMBUS_WRITE, original, 7) < 0)
            fprintf(stderr, "readback failed; restoring original settings also failed\n");
        else {
            if (transfer(fd, 0x67, I2C_SMBUS_READ, restored, 6) == 0 &&
                memcmp(restored, before, 4) == 0)
                fprintf(stderr, "readback failed; original color and brightness restored\n");
            else
                fprintf(stderr, "readback failed; original settings could not be verified\n");
        }
        close(fd);
        return 1;
    }
    close(fd);
    printf("Global chassis color set to #%02X%02X%02X (brightness %u)\n",
           rgb[0], rgb[1], rgb[2], brightness);
    return 0;
}
