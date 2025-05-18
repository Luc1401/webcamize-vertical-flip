#define VERSION "2.0.0"
#define LICENSE "BSD-2-Clause"
#define AUTHOR "W. Turner Abney"
#define YEAR "2025"

#include <ctype.h>
#include <getopt.h>
#include <gphoto2/gphoto2.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define log(color, name, format, ...)                                                        \
    fprintf(stderr, "webcamize: %s [" name "] %s " format "\n", colors_enabled ? color : "", \
            colors_enabled ? "\e[0m" : "", ##__VA_ARGS__)
#define log_debug(format, ...) log("\e[0;106m", "DBUG", format, ##__VA_ARGS__)
#define log_info(format, ...) log("\e[0;102m", "INFO", format, ##__VA_ARGS__)
#define log_warn(format, ...) log("\e[0;105m", "WARN", format, ##__VA_ARGS__)
#define log_fatal(format, ...) log("\e[0;101m", "FATL", format, ##__VA_ARGS__)

// cli
typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_FATAL } LogLevel;
LogLevel log_level = LOG_LEVEL_INFO;
char* camera_model = NULL;
int device_number = 0;
bool colors_enabled = true;
int cli(int argc, char* argv[]);
void print_usage(void);
LogLevel parse_log_level(const char* level);
bool is_valid_log_level(const char* level);
bool is_non_negative_integer(const char* str);

volatile int alive = 1;

void sig_handler(int signo) {
    if (signo == SIGINT) alive = 0;
}

int main(int argc, char* argv[]) {
    int ret = cli(argc, argv);
    if (ret != 0) return ret;

    Camera* camera = NULL;
    CameraFile* file = NULL;
    GPContext* context = gp_context_new();

    signal(SIGINT, sig_handler);

    ret = gp_camera_new(&camera);
    if (ret < GP_OK) {
        log_fatal("Failed to create a new camera: %s", gp_result_as_string(ret));
        goto end;
    }
    ret = gp_camera_init(camera, context);
    if (ret < GP_OK) {
        log_fatal("Failed to autodetect camera: %s", gp_result_as_string(ret));
        goto end;
    }
    ret = gp_file_new(&file);
    if (ret < GP_OK) {
        log_fatal("Failed to create CameraFile: %s", gp_result_as_string(ret));
        goto end;
    }

    // main loop
    const char* data;
    unsigned long size;
    while (alive) {
        ret = gp_camera_capture_preview(camera, file, context);
        if (ret < GP_OK) {
            fprintf(stderr, "Failed to capture preview: %s\n", gp_result_as_string(ret));
            break;
        }

        ret = gp_file_get_data_and_size(file, &data, &size);
        if (ret < GP_OK) {
            fprintf(stderr, "Failed to get data from file: %s\n", gp_result_as_string(ret));
            break;
        }

        // write frame size for proper framing
        fwrite(&size, sizeof(size), 1, stdout);
        // ... then write image data
        if (fwrite(data, 1, size, stdout) != size) {
            fprintf(stderr, "Failed to write all data to stdout\n");
            break;
        }

        fflush(stdout);

        usleep(10000);  // ~90 fps
    }

end:
    if (camera_model) free(camera_model);
    if (file) gp_file_free(file);
    if (camera) {
        gp_camera_exit(camera, context);
        gp_camera_free(camera);
    }
    gp_context_unref(context);

    return ret < 0 ? 1 : 0;
}

int cli(int argc, char* argv[]) {
    static struct option long_options[] = {{"camera", required_argument, 0, 'c'},
                                           {"device", required_argument, 0, 'd'},
                                           {"log-level", required_argument, 0, 'l'},
                                           {"no-color", no_argument, 0, 0},
                                           {"version", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};
    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "vc:d:l:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'v':
                log_info("Using webcamize %s", VERSION);
                return -1;

            case 'c':
                if (optarg) {
                    camera_model = strdup(optarg);
                } else {
                    log_fatal("Missing argument for --camera");
                    return 1;
                }
                break;

            case 'd':
                if (optarg) {
                    if (!is_non_negative_integer(optarg)) {
                        log_fatal("Argument for --device must be a non-negative integer");
                        return 1;
                    }
                    device_number = atoi(optarg);
                } else {
                    log_fatal("Missing argument for --device");
                    return 1;
                }
                break;

            case 'l':
                if (optarg) {
                    if (!is_valid_log_level(optarg)) {
                        log_fatal("Invalid log level. Valid options are: DEBUG, INFO, WARN, FATAL");
                        return 1;
                    }
                    log_level = parse_log_level(optarg);
                } else {
                    log_fatal("Missing argument for --log-level");
                    return 1;
                }
                break;

            case 'h':
                print_usage();
                return -1;

            case 0:
                if (strcmp(long_options[option_index].name, "no-color") == 0) {
                    colors_enabled = false;
                }
                break;

            case '?':
                // getopt_long already printed an error message
                print_usage();
                return 1;

            default:
                log_fatal("Unsupported flag %c", c);
                return 1;
        }
    }

    return 0;
}

void print_usage() {
    printf("\n");
    printf("Usage: webcamize [OPTIONS...]\n");
    printf("\n");
    printf("  -s,  --status                 Print a status report for webcamize and quit\n");
    printf("  -c,  --camera NAME            Specify a gphoto2 camera to use; autodetects by default\n");
    printf("  -d,  --device NUMBER          Specify the /dev/video_ device number to use (default: %d)\n",
           device_number);
    printf("\n");
    printf("  -l,  --log-level LEVEL        Set the log level (DEBUG, INFO, WARN, FATAL; default: INFO)\n");
    printf("       --no-color               Disable the use of colors in the terminal\n");
    printf("  -v,  --version                Print version info and quit\n");
    printf("  -h,  --help                   Show this help message\n");
    printf("\n");
    printf("Webcamize "VERSION", copyright (c) "AUTHOR" "YEAR" licensed "LICENSE"\n");
}

bool is_valid_log_level(const char* level) {
    return (strcasecmp(level, "DEBUG") == 0 || strcasecmp(level, "INFO") == 0 || strcasecmp(level, "WARN") == 0
            || strcasecmp(level, "FATAL") == 0);
}

LogLevel parse_log_level(const char* level) {
    if (strcasecmp(level, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(level, "INFO") == 0) return LOG_LEVEL_INFO;
    if (strcasecmp(level, "WARN") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(level, "FATAL") == 0) return LOG_LEVEL_FATAL;
    log_warn("Encountered impossible log level %s", level);
    return log_level;  // Default if invalid
}

bool is_non_negative_integer(const char* str) {
    if (!str || *str == '\0') return false;
    for (int i = 0; str[i] != '\0'; i++) {
        if (!isdigit((unsigned char)str[i])) return false;
    }
    return true;
}
