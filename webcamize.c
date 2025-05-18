#include <gphoto2/gphoto2.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

volatile int keep_running = 1;

void sig_handler(int signo) {
    if (signo == SIGINT) keep_running = 0;
}

int main(void) {
    int ret;
    Camera* camera;
    GPContext* context = gp_context_new();

    signal(SIGINT, sig_handler);

    ret = gp_camera_new(&camera);
    if (ret < GP_OK) {
        fprintf(stderr, "Failed to create camera: %s\n",
                gp_result_as_string(ret));
        return 1;
    }
    ret = gp_camera_init(camera, context);
    if (ret < GP_OK) {
        fprintf(stderr, "No camera auto detected: %s\n",
                gp_result_as_string(ret));
        gp_camera_free(camera);
        return 1;
    }

    CameraFile* file;
    ret = gp_file_new(&file);
    if (ret < GP_OK) {
        fprintf(stderr, "Failed to create file: %s\n",
                gp_result_as_string(ret));
        gp_camera_exit(camera, context);
        gp_camera_free(camera);
        return 1;
    }

    const char* data;
    unsigned long size;
    while (keep_running) {
        ret = gp_camera_capture_preview(camera, file, context);
        if (ret < GP_OK) {
            fprintf(stderr, "Failed to capture preview: %s\n",
                    gp_result_as_string(ret));
            break;
        }

        ret = gp_file_get_data_and_size(file, &data, &size);
        if (ret < GP_OK) {
            fprintf(stderr, "Failed to get data from file: %s\n",
                    gp_result_as_string(ret));
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

        // usleep(10000); // 10ms; ~90 fps
    }

    gp_file_free(file);
    gp_camera_exit(camera, context);
    gp_camera_free(camera);
    gp_context_unref(context);

    return 0;
}
