#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#define PROGRAM_NAME "mbpfan"
#define PROGRAM_VERSION "2.4.0-macsmc"
#define PROGRAM_PID "/var/run/mbpfan.pid"

extern int daemonize;
extern int verbose;

struct s_sensors {
    FILE *file;
    char *path;
    unsigned int temperature;
    struct s_sensors *next;
};

struct s_fans {
    FILE *file;
    char *path; // TODO: unused
    char *label;
    char *fan_output_path;
    char *fan_manual_path;
    int step_up;
    int step_down;
    int fan_id;
    int old_speed;
    int fan_max_speed;
    int fan_min_speed;
    struct s_fans *next;
};

typedef struct s_sensors t_sensors;
typedef struct s_fans t_fans;

extern t_sensors *sensors;
extern t_fans *fans;

/** Which driver provides the fan interface */
enum fan_backend {
    FAN_BACKEND_APPLESMC = 0,   /* sysfs fan*_output / fan*_manual */
    FAN_BACKEND_MACSMC,         /* hwmon fan*_target (kernel 7.3+) */
    FAN_BACKEND_SMC,            /* SMC key ioctls via /dev/smc */
};

extern enum fan_backend fan_backend;

#endif
