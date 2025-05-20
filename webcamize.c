#define VERSION "2.0.0"
#define LICENSE "BSD-2-Clause"
#define AUTHOR "W. Turner Abney"
#define YEAR "2025"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gphoto2/gphoto2.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#if defined(_WIN32) || defined(_WIN64) || defined(__WIN32__) || defined(__WINDOWS__)
    #define OS_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
    #define OS_MACOS
#elif defined(__linux__) || defined(__FreeBSD__)
    #define OS_LINUX
#endif

typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_FATAL } LogLevel;
LogLevel log_level = LOG_LEVEL_INFO;
bool colors_enabled = true;

char* camera_model = NULL;
int file_sink = -1;
int width = 640;
int height = 480;
long desired_fps = 90;
bool no_convert = false;
bool should_wait = false;

#define log(color, name, format, ...)                                                        \
    fprintf(stderr, "webcamize: %s [" name "] %s " format "\n", colors_enabled ? color : "", \
            colors_enabled ? "\e[0m" : "", ##__VA_ARGS__)
#define log_debug(format, ...) \
    if (log_level <= LOG_LEVEL_DEBUG) log("\e[0;106m", "DBUG", format, ##__VA_ARGS__)
#define log_info(format, ...) \
    if (log_level <= LOG_LEVEL_INFO) log("\e[0;102m", "INFO", format, ##__VA_ARGS__)
#define log_warn(format, ...) \
    if (log_level <= LOG_LEVEL_WARN) log("\e[0;105m", "WARN", format, ##__VA_ARGS__)
#define log_fatal(format, ...) log("\e[0;101m", "FATL", format, ##__VA_ARGS__)

#if defined(OS_LINUX)
    #include <linux/videodev2.h>
    #include <sys/ioctl.h>
int v4l2_dev_num = -1;
int v4l2_fd = -1;

int init_v4l2_device(void) {
    char dev_path[20];  // Buffer to hold the resulting string
    sprintf(dev_path, "/dev/video%d", v4l2_dev_num);
    log_debug("Initializing V4L2 device: /dev/video%s", dev_path);

    // Open the V4L2 device
    v4l2_fd = open(dev_path, O_RDWR);
    if (v4l2_fd < 0) {
        log_fatal("Failed to open V4L2 device %s: %s", dev_path, strerror(errno));
        return -1;
    }

    // Check if this is a valid V4L2 device
    struct v4l2_capability cap;
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        log_fatal("Device %s is not a valid V4L2 device: %s", dev_path, strerror(errno));
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // Check if it supports video output
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        log_fatal("Device %s does not support video output", dev_path);
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    log_debug("V4L2 device initialized successfully");
    return 0;
}

int setup_v4l2_format(void) {
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width * 3;  // RGB24 = 3 bytes per pixel
    fmt.fmt.pix.sizeimage = width * height * 3;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_fatal("Could not set format for /dev/video%d: %s", v4l2_dev_num, strerror(errno));
        return -1;
    }

    log_debug("V4L2 format set to %dx%d RGB24", width, height);
    return 0;
}

int write_to_v4l2_device(const uint8_t* rgb_data, int data_size) {
    int n = write(v4l2_fd, rgb_data, data_size);
    if (n < 0) {
        log_fatal("Failed to write to V4L2 device: %s", strerror(errno));
        return -1;
    } else if (n != data_size) {
        log_warn("Short write to V4L2 device: wrote %d of %d bytes", n, data_size);
    }
    return 0;
}
#endif

AVCodecContext* decoder_ctx = NULL;
const AVCodec* decoder = NULL;
AVFrame* input_frame = NULL;
AVFrame* output_frame = NULL;
AVPacket* packet_obj = NULL;
struct SwsContext* sws_ctx = NULL;
uint8_t* rgb_buffer = NULL;
int rgb_buffer_size = 0;

int init_ffmpeg_conversion(void);
int convert_to_rgb24(const char* image_data, unsigned long image_data_size, uint8_t** rgb_data, int* rgb_data_size);

volatile bool alive = true;
void sig_handler(int signo) {
    if (signo == SIGINT) alive = false;
}
int cli(int argc, char* argv[]);
void print_usage(void);

int main(int argc, char* argv[]) {
    int ret = cli(argc, argv);
    if (ret != 0) return ret;

    Camera* gp2_camera = NULL;
    CameraFile* gp2_file = NULL;
    GPContext* gp2_context = gp_context_new();

    signal(SIGINT, sig_handler);

    ret = gp_camera_new(&gp2_camera);
    if (ret < GP_OK) {
        log_fatal("Failed to create a new camera: %s", gp_result_as_string(ret));
        goto cleanup;
    }

    bool waiting = should_wait;
    while (waiting && alive) {
        log_debug("waiting...");
        ret = gp_camera_init(gp2_camera, gp2_context);
        if (ret < GP_OK) {
            if (should_wait) {
                // 3s polling rate
                usleep(3000000);
                continue;
            } else {
                log_fatal("Failed to autodetect camera: %s", gp_result_as_string(ret));
                goto cleanup;
            }
        }
        waiting = false;
    }
    ret = gp_file_new(&gp2_file);
    if (ret < GP_OK) {
        log_fatal("Failed to create CameraFile: %s", gp_result_as_string(ret));
        goto cleanup;
    }

    if (!no_convert) {
        ret = init_ffmpeg_conversion();
        if (ret < 0) {
            log_fatal("Failed to initialize FFmpeg conversion");
            goto cleanup;
        }
    }

#if defined(OS_LINUX)
    if (v4l2_dev_num >= 0) {
        ret = init_v4l2_device();
        if (ret < 0) {
            log_fatal("Failed to initialize V4L2 device");
            goto cleanup;
        }

        ret = setup_v4l2_format();
        if (ret < 0) {
            log_fatal("Failed to set V4L2 format");
            goto cleanup;
        }
    }
#endif

    // main loop
    const char* image_data = NULL;
    unsigned long image_data_size;
    uint8_t* output_data = NULL;
    int output_data_size = 0;
    long desired_frame_time = CLOCKS_PER_SEC / desired_fps;
    long frame_start;
    long frame_time;

    while (alive) {
        frame_start = clock();

        ret = gp_camera_capture_preview(gp2_camera, gp2_file, gp2_context);
        if (ret != GP_OK) {
            log_fatal("Failed to capture preview: %s\n", gp_result_as_string(ret));
            break;
        }
        ret = gp_file_get_data_and_size(gp2_file, &image_data, &image_data_size);
        if (ret != GP_OK) {
            log_fatal("Failed to get data from camera file: %s\n", gp_result_as_string(ret));
            break;
        }

        if (!no_convert) {
            ret = convert_to_rgb24(image_data, image_data_size, &output_data, &output_data_size);
            if (ret < 0) {
                log_warn("Failed to convert image to RGB24, using original image data instead");
                output_data = (uint8_t*)image_data;
                output_data_size = image_data_size;
            }
        } else {
            output_data = (uint8_t*)image_data;
            output_data_size = image_data_size;
        }

        if (file_sink != -1) {
            int n = write(file_sink, output_data, output_data_size);
            if (n != output_data_size) {
                ret = errno;
                log_fatal("Failed to write all data to file sink, wrote %d of %d bytes: %s\n", n, output_data_size,
                          strerror(errno));
                break;
            }
            goto loop_end;
        }

#if defined(OS_LINUX)
        if (v4l2_fd > 0) {
            ret = write_to_v4l2_device(rgb_data, rgb_data_size);
            if (ret < 0) {
                log_fatal("Failed to write to V4L2 device");
                break;
            }
        }
#endif

    loop_end: {
        frame_time = clock() - frame_start;
        if (frame_time < desired_frame_time) {
            usleep((desired_frame_time - frame_time) * 1000000 / CLOCKS_PER_SEC);
        }
    }
    }

cleanup:
    // general
    if (file_sink) close(file_sink);

#if defined(OS_LINUX)
    // v4l2
    if (v4l2_fd >= 0) {
        close(v4l2_fd);
        v4l2_fd = -1;
    }
#endif

    // restart if we are daemonized
    if (should_wait && alive) main(argc, argv);

    // ffmpeg
    if (rgb_buffer) av_free(rgb_buffer);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (output_frame) av_frame_free(&output_frame);
    if (input_frame) av_frame_free(&input_frame);
    if (decoder_ctx) avcodec_free_context(&decoder_ctx);
    if (packet_obj) av_packet_free(&packet_obj);

    // gphoto
    if (camera_model) free(camera_model);
    if (gp2_file) gp_file_free(gp2_file);
    if (gp2_camera) {
        gp_camera_exit(gp2_camera, gp2_context);
        gp_camera_free(gp2_camera);
    }
    gp_context_unref(gp2_context);

    return ret < 0 ? 1 : 0;
}

int init_ffmpeg_conversion(void) {
    // Allocate frames
    input_frame = av_frame_alloc();
    if (!input_frame) {
        log_fatal("Failed to allocate input frame");
        return -1;
    }

    output_frame = av_frame_alloc();
    if (!output_frame) {
        log_fatal("Failed to allocate output frame");
        return -1;
    }
    return 0;
}

int convert_to_rgb24(const char* image_data, unsigned long image_data_size, uint8_t** rgb_data, int* rgb_data_size) {
    int ret;

    if (!decoder_ctx) {
        AVFormatContext* format_ctx = NULL;

        // Create a memory buffer for FFmpeg to read from
        AVIOContext* avio_ctx = avio_alloc_context((unsigned char*)av_malloc(image_data_size), image_data_size, 0, NULL,
                                                   NULL, NULL, NULL);

        if (!avio_ctx) {
            log_warn("Failed to create AVIO context");
            return -1;
        }

        // Copy image data to the AVIO buffer
        memcpy(avio_ctx->buffer, image_data, image_data_size);

        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Failed to allocate format context");
            return -1;
        }

        // Set the AVIO context
        format_ctx->pb = avio_ctx;

        // Open input
        ret = avformat_open_input(&format_ctx, NULL, NULL, NULL);
        if (ret < 0) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avformat_free_context(format_ctx);
            log_warn("Failed to open input: %s", av_err2str(ret));
            return -1;
        }

        // Find stream info
        ret = avformat_find_stream_info(format_ctx, NULL);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Failed to find stream info: %s", av_err2str(ret));
            return -1;
        }

        // Find the first video stream
        int stream_index = -1;
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                stream_index = i;
                break;
            }
        }

        if (stream_index == -1) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("No video stream found");
            return -1;
        }

        // Get codec parameters
        AVCodecParameters* codec_params = format_ctx->streams[stream_index]->codecpar;

        // Find decoder
        decoder = avcodec_find_decoder(codec_params->codec_id);
        if (!decoder) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Decoder not found for codec ID: %d", codec_params->codec_id);
            return -1;
        }

        log_debug("Found decoder: %s for format: %s", decoder->name, format_ctx->iformat->name);

        // Create decoder context
        decoder_ctx = avcodec_alloc_context3(decoder);
        if (!decoder_ctx) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            log_warn("Could not allocate decoder context");
            return -1;
        }

        // Copy codec parameters to decoder context
        ret = avcodec_parameters_to_context(decoder_ctx, codec_params);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avcodec_free_context(&decoder_ctx);
            log_warn("Failed to copy codec parameters to decoder context: %s", av_err2str(ret));
            return -1;
        }

        AVDictionary* opts = NULL;
        av_dict_set(&opts, "threads", "auto", 0);
        av_dict_set(&opts, "thread_type", "frame", 0);
        decoder_ctx->thread_count = 0;
        decoder_ctx->thread_type = FF_THREAD_FRAME;
        decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder_ctx->get_buffer2 = avcodec_default_get_buffer2;
        decoder_ctx->lowres = 0;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

        enum AVHWDeviceType type = av_hwdevice_find_type_by_name("auto");
        if (type != AV_HWDEVICE_TYPE_NONE) {
            AVBufferRef* hw_device_ctx = NULL;
            ret = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0);
            if (ret >= 0) {
                decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                av_buffer_unref(&hw_device_ctx);
            }
        }

        // Open decoder
        ret = avcodec_open2(decoder_ctx, decoder, &opts);
        if (ret < 0) {
            avformat_close_input(&format_ctx);
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avcodec_free_context(&decoder_ctx);
            log_warn("Could not open decoder: %s", av_err2str(ret));
            return -1;
        }

        // Set image dimensions from decoder context
        width = decoder_ctx->width;
        height = decoder_ctx->height;

        log_debug("Image dimensions: %dx%d", width, height);

        // Clean up format context (we only needed it for setup)
        avformat_close_input(&format_ctx);
        av_free(avio_ctx->buffer);
        avio_context_free(&avio_ctx);
    }

    // Allocate packet
    packet_obj = av_packet_alloc();
    packet_obj->data = (uint8_t*)image_data;
    packet_obj->size = image_data_size;

    // Send packet to decoder
    ret = avcodec_send_packet(decoder_ctx, packet_obj);
    if (ret < 0) {
        log_warn("Error sending packet to decoder: %s", av_err2str(ret));
        return -1;
    }

    // Receive frame from decoder
    ret = avcodec_receive_frame(decoder_ctx, input_frame);
    if (ret < 0) {
        log_warn("Error receiving frame from decoder: %s", av_err2str(ret));
        return -1;
    }

    // Initialize/update SwsContext if needed
    if (!sws_ctx || width != input_frame->width || height != input_frame->height) {
        width = input_frame->width;
        height = input_frame->height;

        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }

        sws_ctx = sws_getContext(width, height, input_frame->format, width, height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR,
                                 NULL, NULL, NULL);

        if (!sws_ctx) {
            log_warn("Could not initialize SwsContext");
            return -1;
        }

        // Reallocate RGB buffer if needed
        int new_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        if (!rgb_buffer || new_size > rgb_buffer_size) {
            if (rgb_buffer) {
                av_free(rgb_buffer);
            }
            rgb_buffer = (uint8_t*)av_malloc(new_size);
            if (!rgb_buffer) {
                log_warn("Failed to reallocate RGB buffer");
                return -1;
            }
            rgb_buffer_size = new_size;
        }

        // Set up output frame
        ret = av_image_fill_arrays(output_frame->data, output_frame->linesize, rgb_buffer, AV_PIX_FMT_RGB24, width,
                                   height, 1);
        if (ret < 0) {
            log_warn("Failed to set up output frame: %s", av_err2str(ret));
            return -1;
        }

        output_frame->width = width;
        output_frame->height = height;
        output_frame->format = AV_PIX_FMT_RGB24;
    }

    // Convert image to RGB24
    ret = sws_scale(sws_ctx, (const uint8_t* const*)input_frame->data, input_frame->linesize, 0, height,
                    output_frame->data, output_frame->linesize);
    if (ret <= 0) {
        log_warn("Failed to convert image: %s", av_err2str(ret));
        return -1;
    }

    // Set output parameters
    *rgb_data = rgb_buffer;
    *rgb_data_size = rgb_buffer_size;

    return 0;
}

int cli(int argc, char* argv[]) {
    if (!isatty(STDERR_FILENO)) {
        colors_enabled = false;
    }

    static struct option long_options[]
        = {{"camera", required_argument, 0, 'c'}, {"file", optional_argument, 0, 'f'},
           {"device", required_argument, 0, 'd'}, {"log-level", required_argument, 0, 'l'},
           {"wait", no_argument, 0, 'w'},         {"no-convert", no_argument, 0, 'x'},
           {"no-color", no_argument, 0, 0},       {"version", no_argument, 0, 'v'},
           {"help", no_argument, 0, 'H'},         {0, 0, 0, 0}};
    int option_index = 0;
    int c;
    // opterr = 0;
    while ((c = getopt_long(argc, argv, "vxc:f::wd:l:H", long_options, &option_index)) != -1) {
        switch (c) {
            case 'v':
                log_info("Using webcamize %s", VERSION);
                return -1;

            case 'x':
                no_convert = true;
                break;

            case 'w':
                log_debug("Should wait!");
                should_wait = true;
                break;

            case 'c':
                if (optarg) {
                    camera_model = strdup(optarg);
                } else {
                    log_fatal("Missing argument for --camera");
                    return 1;
                }
                break;

            case 'f':
                if (optarg) {
                    file_sink = open(optarg, O_RDWR, O_NONBLOCK);
                    if (file_sink < 0) {
                        log_fatal("Failed to open file sink `%s`: %s", optarg, strerror(errno));
                        return 1;
                    }
                } else {
                    file_sink = STDOUT_FILENO;
                    log_info("Sink set to stdout because no argument was passed for option --file");
                }
                break;

            case 'd':
#if defined(OS_LINUX)
                if (optarg && !(!optarg || *optarg == '\0')) {
                    for (int i = 0; optarg[i] != '\0'; i++) {
                        if (!isdigit((unsigned char)optarg[i])) {
                            log_fatal("Argument for --device must be a non-negative integer, got %s", optarg);
                            return 1;
                        };
                    }
                    v4l2_dev_num = atoi(optarg);
                } else {
                    log_fatal("Missing argument for --device");
                    return 1;
                }
#else
                log_warn("Option --device ignored as it does nothing on your operating system");
#endif
                break;

            case 'l':
                if (optarg) {
                    if (strcasecmp(optarg, "DEBUG") == 0) {
                        log_level = LOG_LEVEL_DEBUG;
                    } else if (strcasecmp(optarg, "INFO") == 0) {
                        log_level = LOG_LEVEL_INFO;
                    } else if (strcasecmp(optarg, "WARN") == 0) {
                        log_level = LOG_LEVEL_WARN;
                    } else if (strcasecmp(optarg, "FATAL") == 0) {
                        log_level = LOG_LEVEL_FATAL;
                    } else {
                        log_fatal("Invalid log level `%s`", optarg);
                        return 1;
                    }
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
    printf("  -c,  --camera NAME            Specify a camera to use by its name; autodetects by default\n");
    printf("  -f,  --file [PATH]            Output to a file; if no argument is passed, output to stdout\n");
#if defined(OS_LINUX)
    printf("  -d,  --device NUMBER          Specify the /dev/video_ device number to use (default: 0)\n");
#endif
    printf("  -x,  --no-convert             Don't convert from input format before writing\n");
    printf("  -w,  --wait                   Daemonize the process, preventing it from exiting\n");
    printf("\n");
    printf("  -l,  --log-level LEVEL        Set the log level (DEBUG, INFO, WARN, FATAL; default: INFO)\n");
    printf("       --no-color               Disable the use of colors in the terminal\n");
    printf("  -v,  --version                Print version info and quit\n");
    printf("  -H,  --help                   Show this help message\n");
    printf("\n");
    printf("Webcamize %s, copyright (c) %s %s licensed %s\n", VERSION, AUTHOR, YEAR, LICENSE);
}
