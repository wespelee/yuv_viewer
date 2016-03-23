#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <va/va.h>
#include <va/va_x11.h>
#include <X11/Xlib.h>

#define SWAP_UINT(a, b) do { \
        uint32_t v = a;         \
        a = b;               \
        b = v;               \
    } while (0)

#define DEAD_SURFACE_ID (VASurfaceID) 0xbeefdead

struct window_s {
    Window win;
    uint32_t width;
    uint32_t height;
};

struct display_s {
    void *native_dpy;
    VADisplay va_dpy;
    struct window_s x11_win;
};

struct frame_info {
    int32_t width;
    int32_t height;
    int32_t channel;
    void *buf;
};

static uint32_t get_tick_count(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL))
        return 0;

    return tv.tv_usec / 1000 + tv.tv_sec * 1000;
}

static int open_file(char *fn, uint8_t *buf, size_t size)
{
    FILE *fp = NULL;
    struct stat st;

    if (stat(fn, &st) < 0) {
        perror("check_file_size");
        return -1;
    }

    if (st.st_size != size) {
        printf("File size is not match resolution setting!\n");
        return -1;
    }

    fp = fopen(fn, "r");

    if (!fp) {
        perror("File open fail!");
        return -1;
    }

    if (fread(buf, 1, size, fp) != size) {
        perror("File read fail!");
        fclose(fp);
        return -1;
    };

    fclose(fp);

    return 0;
}

static uint8_t *input_buffer_init(char *fn, size_t size)
{
    uint8_t *buf = calloc(1, size);

    if (buf == NULL) {
        perror("Memory calloc fail");
        return NULL;
    }

    if (open_file(fn, buf, size) < 0) {
        printf("Open file error!\n");
        free(buf);
        return NULL;
    }

    return buf;
}

static int check_window_event(void *win_display, void *drawable,
                              uint32_t *width,
                              uint32_t *height, uint32_t *quit)
{
    int is_event = 0;
    XEvent event;
    Window win = (Window)drawable;
    Display *x11_display = (Display *)win_display;


    is_event = XCheckWindowEvent(x11_display, win,
                                 StructureNotifyMask | KeyPressMask, &event);

    if (is_event == 0)
        return 0;

    /* bail on any focused key press */
    if (event.type == KeyPress) {
        *quit = 1;
        return 0;
    }

    /* rescale the video to fit the window */
    if (event.type == ConfigureNotify) {
        *width = event.xconfigure.width;
        *height = event.xconfigure.height;
        printf("Scale window to %dx%d\n", *width, *height);
    }

    return 0;
}

int32_t vaapi_check_status(VAStatus status, char *msg)
{
    if (status != VA_STATUS_SUCCESS) {
        printf("%s: %s", msg, vaErrorStr(status));
        return -1;
    }

    return 0;
}

static  VAImageFormat *va_image_formats;
static int
ensure_image_formats(VADisplay va_dpy)
{
    VAStatus va_status;
    VAImageFormat *image_formats;
    int num_image_formats;

    num_image_formats = vaMaxNumImageFormats(va_dpy);

    if (num_image_formats == 0)
        return 0;

    image_formats = malloc(num_image_formats * sizeof(*image_formats));

    if (!image_formats)
        return 0;

    va_status = vaQueryImageFormats(va_dpy, image_formats, &num_image_formats);

    if (vaapi_check_status(va_status, "vaQuerySurfaceAttributes()")) {
        return 0;
    }

    va_image_formats = image_formats;
    return num_image_formats;
}

static const VAImageFormat *
lookup_image_format(struct display_s *b_dpy)
{
    int i;
    uint32_t fourcc = VA_FOURCC_NV12;

    int va_num_image_formats = ensure_image_formats(b_dpy->va_dpy);

    if (va_num_image_formats == 0)
        return NULL;

    for (i = 0; i < va_num_image_formats; i++) {
        const VAImageFormat *const image_format = &va_image_formats[i];

        if (image_format->fourcc == fourcc)
            return image_format;
    }

    printf("looks like we do not support VA_FOURCC_NV12\n");
    return NULL;
}

static void print_help()
{
    printf("Usage:\n");
    printf("\tviewer [file] [width] [height]\n");
}

static void x11_create_simple_win(struct display_s *dpy,
                                    struct frame_info *frame)
{
    Window root_win = DefaultRootWindow(dpy->native_dpy);
    uint32_t w, h;

    if (root_win < 0) {
        printf("Failed to obtain the root windows\n");
        return;
    }

    XWindowAttributes xwAttr;
    XGetWindowAttributes(dpy->native_dpy, root_win, &xwAttr);

    /* Setup proper display window size */
    if (frame->width > xwAttr.width) {
        w = frame->width / 2;
        h = frame->height / 2;
    }
    else {
        w = frame->width;
        h = frame->height;
    }

    dpy->x11_win.width = w;
    dpy->x11_win.height = h;
    dpy->x11_win.win = XCreateSimpleWindow(dpy->native_dpy,
                                           RootWindow(dpy->native_dpy, 0),
                                           0, 0, w, h, 0, 0,
                                           WhitePixel(dpy->native_dpy, 0));
    XMapWindow(dpy->native_dpy, dpy->x11_win.win);
    XSelectInput(dpy->native_dpy, dpy->x11_win.win,
      KeyPressMask | StructureNotifyMask);
    XSync(dpy->native_dpy, False);
}

int main(int argc, char *argv[])
{
    struct display_s base_dpy;
    struct frame_info frame;
    uint32_t quit = 0, buf_size;
    VAStatus status;
    int major_version, minor_version;
    VASurfaceID surfaces_id[2];

    if (argc != 4) {
        print_help();
        return 0;
    }

    memset(&frame, 0, sizeof(frame));
    frame.width = atoi(argv[2]);
    frame.height = atoi(argv[3]);
    buf_size = frame.width * frame.height * 3 / 2;

    if ((frame.buf = input_buffer_init(argv[1], buf_size)) == NULL) {
        printf("Buffer initial fail.\n");
        return 0;
    }

    base_dpy.native_dpy = XOpenDisplay(NULL);

    if (!base_dpy.native_dpy) {
        printf("failed to open display\n");
        return 0;
    }

    x11_create_simple_win(&base_dpy, &frame);

    /* Intel libva */
    base_dpy.va_dpy = vaGetDisplay(base_dpy.native_dpy);

    if (base_dpy.va_dpy == NULL) {
        printf("failed to get VA display\n");
        return 0;
    }

    status = vaInitialize(base_dpy.va_dpy, &major_version, &minor_version);

    if (vaapi_check_status(status, "vaInitialize")) {
        return 0;
    }

    memset(surfaces_id, 0xff, sizeof(surfaces_id));
    surfaces_id[1] = DEAD_SURFACE_ID;
    status = vaCreateSurfaces(base_dpy.va_dpy, VA_RT_FORMAT_YUV420,
                              frame.width, frame.height, surfaces_id, 1, NULL, 0);

    if (vaapi_check_status(status, "vaCreateSurfaces")) {
        return 0;
    }

    const VAImageFormat *va_format = lookup_image_format(&base_dpy);

    if (!va_format) {
        printf("Invalid va format\n");
        return 0;
    }

    VAImage va_image;
    status = vaCreateImage(base_dpy.va_dpy,
                           (VAImageFormat *)va_format,
                           frame.width,
                           frame.height,
                           &va_image
                          );

    if (vaapi_check_status(status, "vaCreateImage")) {
        return 0;
    }

    VAImageID image_id = va_image.image_id;
    va_image.format = *va_format;

    uint8_t *image_data = NULL;
    status = vaMapBuffer(base_dpy.va_dpy,
                         va_image.buf, (void **)&image_data);

    if (vaapi_check_status(status, "vaMapBuffer")) {
        return 0;
    }

    uint8_t *pixels[3];
    uint32_t stride[3];
    int32_t i;

    for (i = 0; i < va_image.num_planes; i++) {
        pixels[i] = image_data + va_image.offsets[i];
        stride[i] = va_image.pitches[i];
        printf("stride[%d] = %d, offset[%d] = %d data_size = %u\n", i, stride[i], i,
               va_image.offsets[i], va_image.data_size);
    }

    /* Need better copy */
    memcpy(pixels[0], frame.buf, frame.width * frame.height);
    memcpy(pixels[1], frame.buf + frame.width * frame.height,
           frame.width * frame.height / 2);

    status = vaUnmapBuffer(base_dpy.va_dpy, va_image.buf);

    if (vaapi_check_status(status, "vaUnmapBuffer")) {
        return 0;
    }

    status = vaPutImage(base_dpy.va_dpy, surfaces_id[0],
                        image_id, 0, 0, frame.width, frame.height,
                        0, 0, frame.width, frame.height);

    if (vaapi_check_status(status, "vaPutImage")) {
        return 0;
    }

    uint32_t start_time, putsurface_time, frame_num = 0;

    while (!quit) {
        start_time = get_tick_count();
        status = vaPutSurface(base_dpy.va_dpy, surfaces_id[0],
                              base_dpy.x11_win.win, 0, 0, frame.width, frame.height, 0, 0,
                              base_dpy.x11_win.width, base_dpy.x11_win.height, NULL, 0, VA_FRAME_PICTURE);

        if (vaapi_check_status(status, "vaPutSurface")) {
            return 0;
        }

        putsurface_time += (get_tick_count() - start_time);

        if ((frame_num % 0xff) == 0) {
            printf("%.2f FPS\n", 256000.0 / (float)putsurface_time);
            putsurface_time = 0;
        }

        check_window_event(base_dpy.native_dpy, (void *)base_dpy.x11_win.win, &base_dpy.x11_win.width,
                           &base_dpy.x11_win.height, &quit);
        frame_num++;
    }

    status = vaDestroyImage(base_dpy.va_dpy, va_image.image_id);

    if (vaapi_check_status(status, "vaDestroyImage")) {
        return 0;
    }

    status = vaDestroySurfaces(base_dpy.va_dpy, surfaces_id, 1);

    if (vaapi_check_status(status, "vaDestroySurfaces")) {
        return 0;
    }

    free(va_image_formats);
    free(frame.buf);
    vaTerminate(base_dpy.va_dpy);
    XUnmapWindow(base_dpy.native_dpy, base_dpy.x11_win.win);
    XDestroyWindow(base_dpy.native_dpy, base_dpy.x11_win.win);
    XCloseDisplay(base_dpy.native_dpy);

    return 0;
}
