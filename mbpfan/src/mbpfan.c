/**
 *  mbpfan.c - automatically control fan for MacBook Pro
 *  Copyright (C) 2010  Allan McRae <allan@archlinux.org>
 *  Modifications by Rafael Vega <rvega@elsoftwarehamuerto.org>
 *  Modifications (2012) by Ismail Khatib <ikhatib@gmail.com>
 *  Modifications (2012-present) by Daniel Graziotin <daniel@ineed.coffee> [CURRENT MAINTAINER]
 *  Modifications (2017-present) by Robert Musial <rmusial@fastmail.com>
 *  Modifications (2018-present) by Ati Sharma <ati.sharma@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *
 *  Notes:
 *    Assumes any number of processors, cores, sensors and fans
 *    (as defined in NUM_PROCESSORS, NUM_HWMONS, NUM_TEMP_INPUTS and NUM_FANS)
 *    It uses only the temperatures from the processors as input.
 *    Requires coretemp plus either applesmc or (kernel 7.3+) macsmc-hwmon.
 *    Requires root use
 *
 *  Tested models: see README.md
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <syslog.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include "mbpfan.h"
#include "global.h"
#include "settings.h"
#include "util.h"

/* lazy min/max... */
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define CORETEMP_PATH "/sys/devices/platform/coretemp.0"
#define APPLESMC_PATH "/sys/devices/platform/applesmc.768"
#define ALT_APPLESMC_PATH "/sys/bus/acpi/drivers/applesmc"
#define MACSMC_HWMON_CLASS_PATH "/sys/class/hwmon"
#define MACSMC_HWMON_NAME "macsmc_hwmon"
#define MACSMC_FAN_CONTROL_PARAM "/sys/module/macsmc_hwmon/parameters/fan_control"
#define SMC_DEVICE_PATH "/dev/smc"
#define SMCFAN_DEVICE_PATH "/dev/smcfan"

/* Matches include/linux/mfd/macsmc.h from kernel 7.3 */
struct smc_rw_payload {
    uint32_t key;
    size_t size;
    unsigned char buf[32];
};

#define SMC_MAGIC 'S'
#define SMC_IOCTL_READ  _IOWR(SMC_MAGIC, 1, struct smc_rw_payload)
#define SMC_IOCTL_WRITE _IOW(SMC_MAGIC, 2, struct smc_rw_payload)

/* temperature thresholds
 * low_temp - temperature below which fan speed will be at minimum
 * high_temp - fan will increase speed when higher than this temperature
 * max_temp - fan will run at full speed above this temperature */
int low_temp = 63;  // try ranges 55-63
int high_temp = 66; // try ranges 58-66
int max_temp = 86;  // do not set it > 90

// maximum number of processors etc supported
#define NUM_PROCESSORS 6
#define NUM_HWMONS 12
#define NUM_TEMP_INPUTS 64
#define NUM_FANS 10
// sane defaults when user provides unexpected values
#define MIN_FAN_SPEED_DEFAULT 500
#define MAX_FAN_SPEED_DEFAULT 6500

int polling_interval = 1;

t_sensors *sensors = NULL;
t_fans *fans = NULL;
char applesmc_path[PATH_MAX];
char applesmc_fan_path[PATH_MAX];
enum fan_backend fan_backend = FAN_BACKEND_APPLESMC;
static int smc_fd = -1;
static const char *smc_device_path = SMC_DEVICE_PATH;


char *smprintf(const char *fmt, ...)
{
    char *buf;
    int cnt;
    va_list ap;

    // find buffer length
    va_start(ap, fmt);
    cnt = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (cnt < 0) {
        return NULL;
    }

    // create and write to buffer
    buf = malloc(cnt + 1);
    va_start(ap, fmt);
    vsnprintf(buf, cnt + 1, fmt, ap);
    va_end(ap);
    return buf;
}

bool is_modern_sensors_path()
{
    struct utsname kernel;
    uname(&kernel);

    char *str_kernel_version;
    str_kernel_version = strtok(kernel.release, ".");

    if (atoi(str_kernel_version) < 3) {
        mbp_log(LOG_ERR, "mbpfan detected a pre-3.x.x linux kernel. Detected version: %s. Exiting.\n", kernel.release);
        exit(EXIT_FAILURE);
    }

    int counter;

    for (counter = 0; counter < NUM_HWMONS; counter++) {
        int temp;
        for (temp = 1; temp < NUM_TEMP_INPUTS; ++temp) {
            char *path = smprintf("/sys/devices/platform/coretemp.0/hwmon/hwmon%d/temp%d_input", counter, temp);
            int res = access(path, R_OK);
            free(path);
            if (res == 0) {
                return 1;
            }
        }
    }

    return 0;
}

t_sensors *retrieve_sensors()
{
    t_sensors *sensors_head = NULL;
    t_sensors *s = NULL;

    char *path = NULL;
    char *path_begin = NULL;

    const char *path_end = "_input";
    int sensors_found = 0;

    if (!is_modern_sensors_path()) {
        if (verbose) {
            mbp_log(LOG_INFO, "Using legacy path for kernel < 3.15.0");
        }

        path_begin = strdup("/sys/devices/platform/coretemp.0/temp");

    } else {

        if (verbose) {
            mbp_log(LOG_INFO, "Using new sensor path for kernel >= 3.15.0 or some CentOS versions with kernel 3.10.0 ");
        }

        // loop over up to 6 processors
        int processor;
        for (processor = 0; processor < NUM_PROCESSORS; processor++) {

            if (path_begin != NULL) {
                free(path_begin);
            }
            path_begin = smprintf("/sys/devices/platform/coretemp.%d/hwmon/hwmon", processor);

            int counter;
            for (counter = 0; counter < NUM_HWMONS; counter++) {

                char *hwmon_path = smprintf("%s%d", path_begin, counter);

                int res = access(hwmon_path, R_OK);
                if (res == 0) {

                    free(path_begin);
                    path_begin = smprintf("%s/temp", hwmon_path);

                    if (verbose) {
                        mbp_log(LOG_INFO, "Found hwmon path at %s", path_begin);
                    }

                    free(hwmon_path);
                    break;
                }

                free(hwmon_path);
            }

            int core = 0;
            for (core = 0; core < NUM_TEMP_INPUTS; core++) {
                path = smprintf("%s%d%s", path_begin, core, path_end);

                FILE *file = fopen(path, "r");

                if (file != NULL) {
                    s = (t_sensors *)malloc(sizeof(t_sensors));
                    s->path = strdup(path);
                    fscanf(file, "%d", &s->temperature);

                    if (sensors_head == NULL) {
                        sensors_head = s;
                        sensors_head->next = NULL;

                    } else {
                        t_sensors *tmp = sensors_head;

                        while (tmp->next != NULL) {
                            tmp = tmp->next;
                        }

                        tmp->next = s;
                        tmp->next->next = NULL;
                    }

                    s->file = file;
                    sensors_found++;
                }

                free(path);
                path = NULL;
            }
        }
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d sensors", sensors_found);
    }

    if (sensors_found == 0) {
        mbp_log(LOG_CRIT, "mbpfan could not detect any temp sensor. Please contact the developer.");
        exit(EXIT_FAILURE);
    }

    free(path_begin);
    path_begin = NULL;

    return sensors_head;
}

static int read_value(const char *path)
{
    int value = -1;
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fscanf(file, "%d", &value);
        fclose(file);
    }
    return value;
}

static void read_value_str(const char *path, char *str, size_t len)
{
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fgets(str, len, file);
        fclose(file);
    }
}

static void trim_trailing_whitespace(char *str)
{
    for (ssize_t i = strlen(str) - 1; i >= 0; --i) {
        if (isspace(str[i]) || str[i] == '\n') {
            str[i] = '\0';
        }
    }
}

static bool read_trimmed_string(const char *path, char *buf, size_t len)
{
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return false;
    }

    if (fgets(buf, len, file) == NULL) {
        fclose(file);
        return false;
    }

    fclose(file);
    trim_trailing_whitespace(buf);
    return true;
}

/*
 * Kernels 7.3+ no longer ship applesmc; the same SMC is driven by the macsmc
 * stack (macsmc-acpi for ACPI APP0001 machines, macsmc-hwmon for the fans).
 * There is no fixed platform device name, so find the hwmon device by name.
 */
static char *find_macsmc_hwmon(void)
{
    DIR *dir = opendir(MACSMC_HWMON_CLASS_PATH);

    if (dir == NULL) {
        return NULL;
    }

    char *result = NULL;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        char *name_path = smprintf("%s/%s/name", MACSMC_HWMON_CLASS_PATH, ent->d_name);
        char name[64] = { 0 };
        bool match = read_trimmed_string(name_path, name, sizeof(name)) &&
                     strcmp(name, MACSMC_HWMON_NAME) == 0;
        free(name_path);

        if (match) {
            result = smprintf("%s/%s", MACSMC_HWMON_CLASS_PATH, ent->d_name);
            break;
        }
    }

    closedir(dir);
    return result;
}

/* Fan target writes are gated behind macsmc_hwmon's fan_control parameter. */
static bool enable_macsmc_fan_control(void)
{
    FILE *file = fopen(MACSMC_FAN_CONTROL_PARAM, "w");

    if (file == NULL) {
        return false;
    }

    if (fputs("1\n", file) < 0) {
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static uint32_t smc_key(const char *key)
{
    return ((uint32_t)(unsigned char)key[0] << 24) |
           ((uint32_t)(unsigned char)key[1] << 16) |
           ((uint32_t)(unsigned char)key[2] << 8) |
           (uint32_t)(unsigned char)key[3];
}

static int smc_read_bytes(const char *key, void *buf, size_t size)
{
    struct smc_rw_payload payload;

    if (smc_fd < 0) {
        return -1;
    }

    memset(&payload, 0, sizeof(payload));
    payload.key = smc_key(key);
    payload.size = size;

    if (ioctl(smc_fd, SMC_IOCTL_READ, &payload) < 0) {
        return -1;
    }

    memcpy(buf, payload.buf, size);
    return 0;
}

static int smc_write_bytes(const char *key, const void *buf, size_t size)
{
    struct smc_rw_payload payload;

    if (smc_fd < 0 || size > sizeof(payload.buf)) {
        return -1;
    }

    memset(&payload, 0, sizeof(payload));
    payload.key = smc_key(key);
    payload.size = size;
    memcpy(payload.buf, buf, size);

    return ioctl(smc_fd, SMC_IOCTL_WRITE, &payload) < 0 ? -1 : 0;
}

/*
 * macsmc-core's /dev/smc ioctl deliberately refuses writes to the keys that
 * drive fans and charging (F0Tg/F1Tg, F0Md/F1Md, FS! , BCLM, CH0*) with
 * EPERM. Writing a fan target back with its current value is a no-op for the
 * hardware, so it is a safe way to find out whether we may control the fans.
 * The failing errno is left in errno for the caller.
 */
static int smc_probe_write(void)
{
    unsigned char raw[2];

    if (smc_read_bytes("F0Tg", raw, sizeof(raw)) < 0) {
        return -1;
    }

    return smc_write_bytes("F0Tg", raw, sizeof(raw));
}

/* SMC fan speeds are 14-bit values stored big-endian in the upper bits. */
static int smc_fan_speed(int fan_id, const char *suffix)
{
    char key[8];
    unsigned char raw[2];

    snprintf(key, sizeof(key), "F%d%s", fan_id, suffix);
    if (smc_read_bytes(key, raw, sizeof(raw)) < 0) {
        return -1;
    }

    return (((unsigned int)raw[0] << 8) | raw[1]) >> 2;
}

/* applesmc uses <base>/fan<N>_*, macsmc hwmon uses <base>/fan<N>_target. */
static void set_fan_path(const char *base)
{
    char *path = smprintf("%s/fan", base);

    if (path == NULL) {
        mbp_log(LOG_CRIT, "Out of memory");
        exit(EXIT_FAILURE);
    }

    strncpy(applesmc_fan_path, path, PATH_MAX - 1);
    applesmc_fan_path[PATH_MAX - 1] = '\0';
    free(path);
}

static t_fans *retrieve_smc_fans(void)
{
    unsigned char count_raw;
    t_fans *head = NULL;
    t_fans *tail = NULL;
    int fans_found = 0;

    if (smc_read_bytes("FNum", &count_raw, 1) < 0) {
        mbp_log(LOG_ERR, "Could not read SMC fan count (FNum): %s", strerror(errno));
        return NULL;
    }

    if (count_raw > NUM_FANS) {
        count_raw = NUM_FANS;
    }

    for (int i = 0; i < count_raw; i++) {
        t_fans *fan = calloc(1, sizeof(t_fans));
        char key[8];
        unsigned char id[16];

        if (fan == NULL) {
            break;
        }

        fan->fan_id = i;
        fan->old_speed = 0;
        fan->file = NULL;
        fan->fan_output_path = NULL;
        fan->fan_manual_path = NULL;

        fan->label = calloc(64, 1);
        snprintf(key, sizeof(key), "F%dID", i);
        if (fan->label != NULL && smc_read_bytes(key, id, sizeof(id)) == 0) {
            memcpy(fan->label, id + 4, sizeof(id) - 4);
            fan->label[sizeof(id) - 4] = '\0';
            trim_trailing_whitespace(fan->label);
        }
        if (fan->label != NULL && fan->label[0] == '\0') {
            snprintf(fan->label, 64, "Fan %d", i);
        }

        fan->fan_min_speed = smc_fan_speed(i, "Mn");
        if (fan->fan_min_speed < MIN_FAN_SPEED_DEFAULT) {
            fan->fan_min_speed = MIN_FAN_SPEED_DEFAULT;
        }

        fan->fan_max_speed = smc_fan_speed(i, "Mx");
        if (fan->fan_max_speed < MIN_FAN_SPEED_DEFAULT ||
            fan->fan_max_speed > MAX_FAN_SPEED_DEFAULT) {
            fan->fan_max_speed = MAX_FAN_SPEED_DEFAULT;
        }

        if (head == NULL) {
            head = fan;
        } else {
            tail->next = fan;
        }
        tail = fan;
        fans_found++;
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d fans via SMC ioctl", fans_found);
    }

    return head;
}

t_fans *retrieve_fans()
{
    t_fans *fans_head = NULL;
    t_fans *fan = NULL;

    char *path_output = NULL;
    char *path_label = NULL;
    char *path_manual = NULL;
    char *path_fan_max = NULL;
    char *path_fan_min = NULL;

    const char *path_begin = (const char *) &applesmc_fan_path;
    const char *path_output_end = (fan_backend == FAN_BACKEND_MACSMC) ? "_target" : "_output";
    const char *path_label_end = "_label";
    const char *path_man_end = "_manual";
    const char *path_max_speed = "_max";
    const char *path_min_speed = "_min";

    int counter = 0;
    int fans_found = 0;

    if (fan_backend == FAN_BACKEND_SMC) {
        fans_head = retrieve_smc_fans();
        if (fans_head == NULL) {
            mbp_log(LOG_CRIT, "mbpfan could not read the fan list through %s. Exiting.", SMC_DEVICE_PATH);
            exit(EXIT_FAILURE);
        }
        return fans_head;
    }

    for (counter = 0; counter < NUM_FANS; counter++) {

        path_output = smprintf("%s%d%s", path_begin, counter, path_output_end);
        path_label = smprintf("%s%d%s", path_begin, counter, path_label_end);
        path_manual = smprintf("%s%d%s", path_begin, counter, path_man_end);
        path_fan_min = smprintf("%s%d%s", path_begin, counter, path_min_speed);
        path_fan_max = smprintf("%s%d%s", path_begin, counter, path_max_speed);

        FILE *file = fopen(path_output, "w");

        if (file != NULL) {
            fan = (t_fans *)malloc(sizeof(t_fans));
            fan->fan_output_path = strdup(path_output);
            /* macsmc hwmon has no per-fan switch; enabling the module's
             * fan_control parameter above is the equivalent. */
            fan->fan_manual_path = (fan_backend == FAN_BACKEND_MACSMC) ? NULL : strdup(path_manual);
            fan->fan_id = counter;

            int fan_speed = read_value(path_fan_min);
            if (fan_speed == -1 || fan_speed < MIN_FAN_SPEED_DEFAULT)
                fan->fan_min_speed = MIN_FAN_SPEED_DEFAULT;
            else
                fan->fan_min_speed = fan_speed;

            fan_speed = read_value(path_fan_max);
            if (fan_speed == -1 || fan_speed > MAX_FAN_SPEED_DEFAULT)
                fan->fan_max_speed = MAX_FAN_SPEED_DEFAULT;
            else
                fan->fan_max_speed = fan_speed;

            size_t max_label_len = 64;
            fan->label = calloc(max_label_len, 1);
            read_value_str(path_label, fan->label, max_label_len);
            trim_trailing_whitespace(fan->label);

            if (fan->label[0] == '\0') {
                snprintf(fan->label, max_label_len, "Fan %d", counter);
            }

            fan->old_speed = 0;

            if (fans_head == NULL) {
                fans_head = fan;
                fans_head->next = NULL;

            } else {
                t_fans *tmp = fans_head;

                while (tmp->next != NULL) {
                    tmp = tmp->next;
                }

                tmp->next = fan;
                tmp->next->next = NULL;
            }

            fan->file = file;
            fans_found++;
        }
        free(path_fan_min);
        path_fan_min = NULL;
        free(path_label);
        path_label = NULL;
        free(path_fan_max);
        path_fan_max = NULL;
        free(path_output);
        path_output = NULL;
        free(path_manual);
        path_manual = NULL;
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d fans", fans_found);
    }

    if (fans_found == 0) {
        if (fan_backend == FAN_BACKEND_MACSMC) {
            mbp_log(LOG_CRIT, "mbpfan found no fan*_target under %s. Boot with macsmc_hwmon.fan_control=1 so macsmc-hwmon exposes fan control. Exiting.", (char *) &applesmc_path);
        } else {
            mbp_log(LOG_CRIT, "mbpfan could not detect any fan. Please contact the developer.");
        }
        exit(EXIT_FAILURE);
    }

    return fans_head;
}

static void set_smc_fans_mode(t_fans *fans, int mode)
{
    unsigned char mask_raw[2];
    t_fans *tmp;

    if (smc_read_bytes("FS! ", mask_raw, sizeof(mask_raw)) == 0) {
        unsigned int mask = ((unsigned int)mask_raw[0] << 8) | mask_raw[1];

        for (tmp = fans; tmp != NULL; tmp = tmp->next) {
            if (mode) {
                mask |= 1u << tmp->fan_id;
            } else {
                mask &= ~(1u << tmp->fan_id);
            }
        }

        mask_raw[0] = (mask >> 8) & 0xff;
        mask_raw[1] = mask & 0xff;
        if (smc_write_bytes("FS! ", mask_raw, sizeof(mask_raw)) < 0) {
            perror("Could not set fan mode");
        }
        return;
    }

    /* Newer SMCs use a per-fan manual flag instead of the FS! bitmask. */
    for (tmp = fans; tmp != NULL; tmp = tmp->next) {
        char key[8];
        unsigned char value = mode ? 1 : 0;

        snprintf(key, sizeof(key), "F%dMd", tmp->fan_id);
        if (smc_write_bytes(key, &value, 1) < 0) {
            perror("Could not set fan mode");
        }
    }
}

static void set_fans_mode(t_fans *fans, int mode)
{
    t_fans *tmp = fans;
    FILE *file;

    if (fan_backend == FAN_BACKEND_SMC) {
        set_smc_fans_mode(fans, mode);
        return;
    }

    while (tmp != NULL) {
        if (tmp->fan_manual_path == NULL) {
            tmp = tmp->next;
            continue;
        }

        file = fopen(tmp->fan_manual_path, "rw+");

        if (file != NULL) {
            fprintf(file, "%d", mode);
            fclose(file);
        }

        tmp = tmp->next;
    }
}

void set_fans_man(t_fans *fans)
{

    set_fans_mode(fans, 1);
}

void set_fans_auto(t_fans *fans)
{

    set_fans_mode(fans, 0);
}

t_sensors *refresh_sensors(t_sensors *sensors)
{
    t_sensors *tmp = sensors;

    while (tmp != NULL) {
        if (tmp->file != NULL) {
            char buf[16];
            int len = pread(fileno(tmp->file), buf, sizeof(buf), /*offset=*/0);
            buf[len] = '\0';
            tmp->temperature = strtod(buf, NULL);
        }

        tmp = tmp->next;
    }

    return sensors;
}

/* Controls the speed of a fan */
void set_fan_speed(t_fans *fan, int speed)
{
    if (fan == NULL) {
        return;
    }

    if (fan_backend == FAN_BACKEND_SMC) {
        char key[8];
        unsigned char raw[2];
        unsigned int value = (speed & 0x3fff) << 2;

        if (fan->old_speed == speed) {
            return;
        }

        snprintf(key, sizeof(key), "F%dTg", fan->fan_id);
        raw[0] = (value >> 8) & 0xff;
        raw[1] = value & 0xff;

        if (smc_write_bytes(key, raw, sizeof(raw)) < 0) {
            perror("Could not set fan speed");
        } else {
            fan->old_speed = speed;
        }
        return;
    }

    if (fan->file != NULL && fan->old_speed != speed) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", speed);
        int res = pwrite(fileno(fan->file), buf, len, /*offset=*/0);
        if (res == -1) {
            perror("Could not set fan speed");
        }
        fan->old_speed = speed;
    }
}

void set_fan_minimum_speed(t_fans *fans)
{
    t_fans *tmp = fans;

    while (tmp != NULL) {
        set_fan_speed(tmp, tmp->fan_min_speed);
        tmp = tmp->next;
    }
}
unsigned short get_temp(t_sensors *sensors)
{
    sensors = refresh_sensors(sensors);
    unsigned int temp = 0;

    t_sensors *tmp = sensors;

    while (tmp != NULL) {
        temp = max(temp, tmp->temperature);
        tmp = tmp->next;
    }

    return temp / 1000;
}

void retrieve_settings(const char *settings_path, t_fans *fans)
{
    Settings *settings = NULL;
    int result = 0;
    FILE *f = NULL;

    if (settings_path == NULL) {
        f = fopen("/etc/mbpfan.conf", "r");

    } else {
        f = fopen(settings_path, "r");
    }

    if (f == NULL) {
        /* Could not open configfile */
        if (verbose) {
            mbp_log(LOG_INFO, "Couldn't open configfile, using defaults");
        }

    } else {
        settings = settings_open(f);
        fclose(f);

        if (settings == NULL) {
            /* Could not read configfile */
            if (verbose) {
                mbp_log(LOG_WARNING, "Couldn't read configfile");
            }

        } else {

            t_fans *fan = fans;

            while (fan != NULL) {

                char *config_key;
                config_key = smprintf("min_fan%d_speed", fan->fan_id);
                /* Read configfile values */
                result = settings_get_int(settings, "general", config_key);
                if (result != 0) {
                    fan->fan_min_speed = result;
                }
                free(config_key);

                config_key = smprintf("max_fan%d_speed", fan->fan_id);
                result = settings_get_int(settings, "general", config_key);

                if (result != 0) {
                    fan->fan_max_speed = result;
                }
                free(config_key);
                fan = fan->next;
            }
            result = settings_get_int(settings, "general", "low_temp");

            if (result != 0) {
                low_temp = result;
            }

            result = settings_get_int(settings, "general", "high_temp");

            if (result != 0) {
                high_temp = result;
            }

            result = settings_get_int(settings, "general", "max_temp");

            if (result != 0) {
                max_temp = result;
            }

            result = settings_get_int(settings, "general", "polling_interval");

            if (result != 0) {
                polling_interval = result;
            }

            /* Destroy the settings object */
            settings_delete(settings);
        }
    }
}

void check_requirements(const char *program_path)
{

    /**
     * Check for root
     */

    uid_t uid = getuid(), euid = geteuid();

    if (uid != 0 || euid != 0) {
        mbp_log(LOG_ERR, "%s needs root privileges. Please run %s as root. Exiting.", program_path, program_path);
        exit(EXIT_FAILURE);
    }

    /**
     * Check for coretemp and applesmc modules
     */
    DIR *dir = opendir(CORETEMP_PATH);

    if (dir == NULL) {
        mbp_log(LOG_ERR, "%s needs coretemp support. Please either load it or build it into the kernel. Exiting.", program_path);
        exit(EXIT_FAILURE);
    }

    closedir(dir);
    memset(&applesmc_path, 0, PATH_MAX);
    memset(&applesmc_fan_path, 0, PATH_MAX);

    dir = opendir(APPLESMC_PATH);

    if (dir != NULL) {
        closedir(dir);
        strncpy((char *) &applesmc_path, APPLESMC_PATH, PATH_MAX - 1);
    } else {
        /**
        * Check for alternate ACPI device path for newer macbooks
        */
        dir = opendir(ALT_APPLESMC_PATH);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp("APP", (const char *) &ent->d_name, 3) == 0) {
                    snprintf((char *) &applesmc_path, PATH_MAX, "%s/%s", ALT_APPLESMC_PATH, (char *) &ent->d_name);
                    break;
                }
            }
            closedir(dir);
        }
    }

    if (strlen(applesmc_path) != 0) {
        fan_backend = FAN_BACKEND_APPLESMC;
        set_fan_path((char *) &applesmc_path);
        if (verbose) mbp_log(LOG_INFO, "applesmc device path: %s", (char *) &applesmc_path);
        return;
    }

    /*
     * No applesmc: kernels 7.3+ publish the hardware through the macsmc stack.
     * Prefer macsmc-hwmon's fan*_target attributes when the driver actually
     * registered fans, but it only does so if fan_control was set before it
     * probed, and some Intel machines get none at all. In that case fall back
     * to driving the SMC keys directly through macsmc-core's /dev/smc.
     */
    char *hwmon_path = find_macsmc_hwmon();

    if (hwmon_path != NULL) {
        char *target = smprintf("%s/fan1_target", hwmon_path);
        bool has_fan_control = target != NULL && access(target, R_OK) == 0;
        free(target);

        if (has_fan_control) {
            fan_backend = FAN_BACKEND_MACSMC;
            strncpy((char *) &applesmc_path, hwmon_path, PATH_MAX - 1);
            set_fan_path(hwmon_path);
            free(hwmon_path);

            if (!enable_macsmc_fan_control()) {
                mbp_log(LOG_ERR, "%s could not enable %s. Boot with macsmc_hwmon.fan_control=1 to allow fan control. Exiting.", program_path, MACSMC_FAN_CONTROL_PARAM);
                exit(EXIT_FAILURE);
            }

            if (verbose) mbp_log(LOG_INFO, "macsmc hwmon path: %s", (char *) &applesmc_path);
            return;
        }

        if (verbose) mbp_log(LOG_INFO, "macsmc hwmon at %s exposes no fan controls", hwmon_path);
        free(hwmon_path);
    }

    /*
     * /dev/smc is macsmc-core's raw key interface, but it refuses writes to the
     * fan keys. /dev/smcfan is the optional shim module that exposes the same
     * keys through apple_smc_write(). Try both, first one that can write wins.
     */
    const char *smc_devices[] = { SMC_DEVICE_PATH, SMCFAN_DEVICE_PATH };
    const char *failed_device = NULL;
    int write_errno = 0;
    int open_errno = 0;

    for (size_t i = 0; i < sizeof(smc_devices) / sizeof(smc_devices[0]); i++) {
        unsigned char count;

        smc_fd = open(smc_devices[i], O_RDWR);

        if (smc_fd < 0) {
            open_errno = errno;
            continue;
        }

        if (smc_read_bytes("FNum", &count, 1) < 0 || count == 0) {
            open_errno = errno;
            close(smc_fd);
            smc_fd = -1;
            continue;
        }

        if (smc_probe_write() == 0) {
            fan_backend = FAN_BACKEND_SMC;
            smc_device_path = smc_devices[i];
            if (verbose) mbp_log(LOG_INFO, "using SMC key fan control via %s (%d fans)", smc_device_path, count);
            return;
        }

        write_errno = errno;
        failed_device = smc_devices[i];
        close(smc_fd);
        smc_fd = -1;
    }

    if (write_errno != 0) {
        mbp_log(LOG_ERR, "%s can read fan keys through %s but not write them (%s). Exiting.", program_path, failed_device, strerror(write_errno));
    } else {
        mbp_log(LOG_ERR, "%s found no usable SMC fan interface (%s). Load the smcfan module (modprobe smcfan), or use applesmc. Exiting.", program_path, strerror(open_errno));
    }

    exit(EXIT_FAILURE);
}

int get_max_mhz(void)
{
    int max_mhz = -1;
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (dir == NULL) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cpu", 3) != 0 || strcmp(ent->d_name, "cpufreq") == 0 || strcmp(ent->d_name, "cpuidle") == 0) {
            continue;
        }
        char *path = smprintf("/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", ent->d_name);
        max_mhz = max(max_mhz, read_value(path) / 1000);
        free(path);
    }
    closedir(dir);
    return max_mhz;
}

void mbpfan()
{
    int old_temp, new_temp, fan_speed, steps;
    int temp_change;

    sensors = retrieve_sensors();
    fans = retrieve_fans();

    retrieve_settings(NULL, fans);

    t_fans *fan = fans;
    while (fan != NULL) {

        if (fan->fan_min_speed > fan->fan_max_speed) {
            mbp_log(LOG_ERR, "Invalid fan speeds: %d %d", fan->fan_min_speed, fan->fan_max_speed);
            exit(EXIT_FAILURE);
        }
        fan = fan->next;
    }

    if (low_temp > high_temp || high_temp > max_temp) {
        mbp_log(LOG_ERR, "Invalid temperatures: %d %d %d", low_temp, high_temp, max_temp);
        exit(EXIT_FAILURE);
    }

    set_fans_man(fans);

    new_temp = get_temp(sensors);
    set_fan_minimum_speed(fans);

    fan = fans;
    while (fan != NULL) {

        fan->step_up = (float)(fan->fan_max_speed - fan->fan_min_speed) / (float)((max_temp - high_temp) * (max_temp - high_temp + 1) / 2.0);

        fan->step_down = (float)(fan->fan_max_speed - fan->fan_min_speed) / (float)((max_temp - low_temp) * (max_temp - low_temp + 1) / 2.0);
        fan = fan->next;
    }

recalibrate:
    if (verbose) {
        mbp_log(LOG_INFO, "Sleeping for 2 seconds to get first temp delta");
    }
    sleep(2);

    while (1) {
        old_temp = new_temp;
        new_temp = get_temp(sensors);

        fan = fans;

        while (fan != NULL) {
            fan_speed = fan->old_speed;

            if (new_temp >= max_temp && fan->old_speed != fan->fan_max_speed) {
                fan_speed = fan->fan_max_speed;
            }

            if (new_temp <= low_temp && fan_speed != fan->fan_min_speed) {
                fan_speed = fan->fan_min_speed;
            }

            temp_change = new_temp - old_temp;

            if (temp_change > 0 && new_temp > high_temp && new_temp < max_temp) {
                steps = (new_temp - high_temp) * (new_temp - high_temp + 1) / 2;
                fan_speed = max(fan_speed, ceil(fan->fan_min_speed + steps * fan->step_up));
            }

            if (temp_change < 0 && new_temp > low_temp && new_temp < max_temp) {
                steps = (max_temp - new_temp) * (max_temp - new_temp + 1) / 2;
                fan_speed = min(fan_speed, floor(fan->fan_max_speed - steps * fan->step_down));
            }

            if (verbose) {
                mbp_log(LOG_INFO, "Old Temp: %d New Temp: %d Fan: %s Speed: %d Max MHz: %d", old_temp, new_temp, fan->label, fan_speed, get_max_mhz());
            }

            set_fan_speed(fan, fan_speed);
            fan = fan->next;
        }

        if (verbose) {
            mbp_log(LOG_INFO, "Sleeping for %d seconds", polling_interval);
        }

        time_t before_sleep = time(NULL);

        // call nanosleep instead of sleep to avoid rt_sigprocmask and
        // rt_sigaction
        struct timespec ts;
        ts.tv_sec = polling_interval;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);

        time_t after_sleep = time(NULL);
        if (after_sleep - before_sleep > 2 * polling_interval) {
            mbp_log(LOG_INFO, "Clock skew detected - slept for %ld seconds but expected %d", after_sleep - before_sleep, polling_interval);
            set_fans_man(fans);
            goto recalibrate;
        }
    }
}
