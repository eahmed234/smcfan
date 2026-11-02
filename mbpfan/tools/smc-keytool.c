/*
 * smc-keytool - read/write Apple SMC keys through /dev/smc
 *
 * Kernels 7.3+ expose the SMC through macsmc-core's misc device instead of
 * the removed applesmc driver. That device supports the key read/write
 * ioctls defined in include/linux/mfd/macsmc.h, which is enough to inspect
 * (and, with care, drive) the fans from userspace.
 *
 * Build:   cc -O2 -o smc-keytool smc-keytool.c
 * Usage:   sudo ./smc-keytool -n 1 FNum
 *          sudo ./smc-keytool -n 2 F0Ac
 *          sudo ./smc-keytool -i F0Mn            (type/size/flags)
 *          sudo ./smc-keytool -w F0Mn 1f40      (hex bytes)
 *          sudo ./smc-keytool -d /dev/smcfan -w F0Tg 0d05
 *
 * -d selects the device: /dev/smc (default, macsmc-core) or /dev/smcfan
 * (the smcfan shim module). Both use the same ioctl ABI.
 *
 * Root is required: both devices are mode 0600.
 *
 * Note: macsmc-core returns EPERM for writes to a hardcoded set of keys
 * (fan targets/modes and charging keys), so -w only works for other keys.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef uint32_t smc_key;

struct ioctl_smc_rw_payload {
    smc_key key;
    size_t size;
    unsigned char buf[32];
};

struct apple_smc_key_info {
    uint32_t type_code;
    unsigned char size;
    unsigned char flags; /* 0x10: function, 0x40: writable, 0x80: readable */
};

struct ioctl_smc_info_payload {
    smc_key key;
    struct apple_smc_key_info info;
};

#define APPLE_SMC_MAGIC 'S'
#define IOCTL_SMC_READ     _IOWR(APPLE_SMC_MAGIC, 1, struct ioctl_smc_rw_payload)
#define IOCTL_SMC_WRITE    _IOW(APPLE_SMC_MAGIC, 2, struct ioctl_smc_rw_payload)
#define IOCTL_SMC_KEY_INFO _IOWR(APPLE_SMC_MAGIC, 4, struct ioctl_smc_info_payload)

#define SMC_KEY(a, b, c, d) \
    ((smc_key)(((unsigned char)(a) << 24) | ((unsigned char)(b) << 16) | \
               ((unsigned char)(c) << 8) | (unsigned char)(d)))

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-r|-w|-i] [-n SIZE] KEY [HEXBYTES]\n"
            "  KEY       four-letter SMC key, e.g. FNum, F0Ac, FS! \n"
            "  HEXBYTES  data for -w, e.g. 1f40\n"
            "  SIZE      bytes to read (default 32)\n"
            "  -d DEV    /dev/smc (default) or /dev/smcfan\n"
            "  default   -r; -i prints type/size/flags instead\n", prog);
}

int main(int argc, char **argv)
{
    int write_mode = 0;
    int info_mode = 0;
    size_t read_size = 32;
    int opt;
    const char *device = "/dev/smc";
    const char *keyarg;
    const char *dataarg = NULL;
    struct ioctl_smc_rw_payload payload;
    int fd;

    while ((opt = getopt(argc, argv, "rwin:d:")) != -1) {
        switch (opt) {
        case 'r':
            write_mode = 0;
            info_mode = 0;
            break;
        case 'w':
            write_mode = 1;
            info_mode = 0;
            break;
        case 'i':
            write_mode = 0;
            info_mode = 1;
            break;
        case 'd':
            device = optarg;
            break;
        case 'n':
            read_size = (size_t)strtoul(optarg, NULL, 0);
            if (read_size == 0 || read_size > 32) {
                fprintf(stderr, "size must be 1..32\n");
                return 2;
            }
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (optind >= argc || strlen(argv[optind]) != 4) {
        usage(argv[0]);
        return 2;
    }
    keyarg = argv[optind++];

    if (write_mode) {
        if (optind >= argc) {
            usage(argv[0]);
            return 2;
        }
        dataarg = argv[optind];
        if (strlen(dataarg) % 2 != 0 || strlen(dataarg) / 2 > sizeof(payload.buf)) {
            fprintf(stderr, "bad hex data\n");
            return 2;
        }
    }

    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s (need root?)\n", device, strerror(errno));
        return 1;
    }

    if (info_mode) {
        struct ioctl_smc_info_payload info;
        char type[5];

        memset(&info, 0, sizeof(info));
        info.key = SMC_KEY(keyarg[0], keyarg[1], keyarg[2], keyarg[3]);

        if (ioctl(fd, IOCTL_SMC_KEY_INFO, &info) < 0) {
            fprintf(stderr, "SMC info %s: %s\n", keyarg, strerror(errno));
            close(fd);
            return 1;
        }

        for (int i = 0; i < 4; i++)
            type[i] = (char)((info.info.type_code >> (24 - 8 * i)) & 0xff);
        type[4] = '\0';

        printf("%s type '%s' size %u flags 0x%02x%s%s%s\n", keyarg, type,
               info.info.size, info.info.flags,
               (info.info.flags & 0x80) ? " readable" : "",
               (info.info.flags & 0x40) ? " writable" : "",
               (info.info.flags & 0x10) ? " function" : "");
        close(fd);
        return 0;
    }

    memset(&payload, 0, sizeof(payload));
    payload.key = SMC_KEY(keyarg[0], keyarg[1], keyarg[2], keyarg[3]);

    if (write_mode) {
        size_t len = strlen(dataarg) / 2;
        for (size_t i = 0; i < len; i++) {
            unsigned int byte;
            if (sscanf(dataarg + 2 * i, "%2x", &byte) != 1) {
                fprintf(stderr, "bad hex data\n");
                close(fd);
                return 2;
            }
            payload.buf[i] = (unsigned char)byte;
        }
        payload.size = len;

        if (ioctl(fd, IOCTL_SMC_WRITE, &payload) < 0) {
            fprintf(stderr, "SMC write %s: %s\n", keyarg, strerror(errno));
            close(fd);
            return 1;
        }
        printf("wrote %s len %zu\n", keyarg, len);
        close(fd);
        return 0;
    }

    payload.size = read_size;
    if (ioctl(fd, IOCTL_SMC_READ, &payload) < 0) {
        fprintf(stderr, "SMC read %s: %s\n", keyarg, strerror(errno));
        close(fd);
        return 1;
    }

    printf("%s len %zu:", keyarg, payload.size);
    for (size_t i = 0; i < payload.size && i < sizeof(payload.buf); i++)
        printf(" %02x", payload.buf[i]);
    printf("\n");

    close(fd);
    return 0;
}
