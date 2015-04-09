#include <gtk/gtk.h>
#include <cairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct viewer_cfg {
    unsigned int width;
    unsigned int height;
    unsigned int size;
    gint channel;
    FILE *fp;
    guchar *buf;
};

struct yuv_info {
    unsigned int magic;
    unsigned int width;
    unsigned int height;
    unsigned int size;
};

struct viewer_cfg frame_cfg;
int *rgb_buf = NULL;
static int read_chunk();

static int open_file(char *fn)
{
    frame_cfg.fp = fopen(fn, "r");

    if (!frame_cfg.fp) {
        perror("File open fail!");
        return -1;
    }

    return 0;
}


/* Surface to store current scribbles */
static void close_window(void)
{
    if (frame_cfg.buf) {
        free(frame_cfg.buf);
    }

    gtk_main_quit();
}

void yuv_rgb_conversion(int *rgb, guchar *yuv420sp, int width, int height)
{
    int frameSize = width * height;
    int i, j, yp;

    for (j = 0, yp = 0; j < height; j++) {
        int uvp = frameSize + (j >> 1) * width, u = 0, v = 0;
        for (i = 0; i < width; i++, yp++) {
            int y = (0xff & ((int) yuv420sp[yp])) - 16;
            if (y < 0) y = 0;
            if ((i & 1) == 0) {
                v = (0xff & yuv420sp[uvp++]) - 128;
                u = (0xff & yuv420sp[uvp++]) - 128;
            }

            int y1192 = 1192 * y;
            int r = (y1192 + 1634 * v);
            int g = (y1192 - 833 * v - 400 * u);
            int b = (y1192 + 2066 * u);

            if (r < 0) r = 0;
            else if (r > 262143) r = 262143;
            if (g < 0) g = 0;
            else if (g > 262143) g = 262143;
            if (b < 0) b = 0;
            else if (b > 262143) b = 262143;

            rgb[yp] = 0xff000000 | ((r << 6) & 0xff0000) | ((g >> 2) & 0xff00) | ((b >> 10) & 0xff);
        }
    }
}

static void rgb_buf_create()
{
    printf("Buffer create %dx%d ch:%d\n", 
            frame_cfg.width, frame_cfg.height, frame_cfg.channel);
    rgb_buf = calloc(1, frame_cfg.width * frame_cfg.height * frame_cfg.channel);

    if (rgb_buf == NULL) {
        perror("Memory rgb calloc fail");
        return;
    }

    yuv_rgb_conversion(rgb_buf, frame_cfg.buf, frame_cfg.width, frame_cfg.height);

    return;
}

gboolean expose_event_callback(GtkWidget *widget,
                                 GdkEventExpose *event,
                                 gpointer data)
{
    GdkPixbuf *pixbuf;
    guchar *pixel;

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, 
            TRUE, 8, frame_cfg.width, frame_cfg.height);
    g_assert(gdk_pixbuf_get_colorspace (pixbuf) == GDK_COLORSPACE_RGB);

    frame_cfg.channel = gdk_pixbuf_get_n_channels(pixbuf);

    pixel = gdk_pixbuf_get_pixels(pixbuf);

    rgb_buf_create();
    if (rgb_buf == NULL)
        return FALSE;

    memcpy(pixel, rgb_buf, 
            (frame_cfg.width * frame_cfg.height * frame_cfg.channel));

    GtkAllocation *allocation = g_new0(GtkAllocation, 1);
    gtk_widget_get_allocation(GTK_WIDGET(widget), allocation);

    pixbuf = gdk_pixbuf_scale_simple(pixbuf, allocation->width,
                                     allocation->height, GDK_INTERP_BILINEAR);
    g_free(allocation);

    cairo_t *cr;
    cr = gdk_cairo_create(gtk_widget_get_window(widget));
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);

    g_object_unref(pixbuf);

    cairo_paint(cr);
    cairo_destroy(cr);
    free(rgb_buf);

    return FALSE;
}

gboolean draw_callback(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    /* Draw circle example */
    guint width, height;
    GdkRGBA color;

    width = gtk_widget_get_allocated_width (widget);
    height = gtk_widget_get_allocated_height (widget);
    cairo_arc(cr,
               width / 2.0, height / 2.0,
               MIN (width, height) / 2.0,
               0, 2 * G_PI);

    gtk_style_context_get_color(gtk_widget_get_style_context (widget),
                                0,
                                &color);
    gdk_cairo_set_source_rgba(cr, &color);

    cairo_fill(cr);

    return FALSE;
}

static gboolean
time_handler(GtkWidget *widget)
{
#if 0
    time_t curtime;
    struct tm *loctime;

    curtime = time(NULL);
    loctime = localtime(&curtime);
    strftime(buffer, 256, "%T", loctime);
#endif
    if (read_chunk() < 0) {
        printf("Read file fail. %d\n");
        return FALSE;
    }

    gtk_widget_queue_draw(widget);

    return TRUE;
}

static void print_help()
{
    printf("Usage:\n");
    printf("\tviewer file \n");
}

static void frame_cfg_init(int argc, char **argv)
{
    frame_cfg.width = atoi(argv[2]);
    frame_cfg.height = atoi(argv[3]);
}

static int verify_header(struct yuv_info *p)
{
    if (p->magic != 0x1234CCCC)
        return -1;

    if (p->size != (p->width * p->height * 3 / 2)) {
        return -1;
    }
    return 0;
}

static int read_chunk()
{
    struct yuv_info info;
    int rc = 0;

    rc = fread(&info, 1, sizeof(info), frame_cfg.fp);
    if (rc != sizeof(info)) {
        if (feof(frame_cfg.fp)) {
            printf("End of file\n");
            return -1;
        }
        if (ferror(frame_cfg.fp)) {
            printf("Error of file\n");
            return -1;
        }
        perror("Read header error!");
        printf("Error: rc=%d\n", rc);
        return -1;
    }

    if (verify_header(&info) < 0) {
        printf("Header info error!\n");
        return -1;
    }

    frame_cfg.width = info.width;
    frame_cfg.height = info.height;

    if (frame_cfg.buf == NULL) {
        frame_cfg.buf = calloc(1, info.size);
        if (frame_cfg.buf == NULL) {
            perror("Memory calloc fail");
            return -1;
        }
    }
    else {
        if (info.size != frame_cfg.size) {
            frame_cfg.buf = realloc(frame_cfg.buf, info.size);
            if (frame_cfg.buf == NULL) {
                perror("Memory calloc fail");
                return -1;
            }
            frame_cfg.size = info.size;
        }
    }

    rc = fread(frame_cfg.buf, 1, info.size, frame_cfg.fp);
    if (rc != info.size) {
        printf("Header info error!\n");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    GtkWidget *window;
    GtkWidget *draw_area;
    GtkWidget *frame;

    if (argc != 2) {
        print_help();
        return 0;
    }

    memset(&frame_cfg, 0, sizeof(frame_cfg));

    if (open_file(argv[1]) < 0) {
        printf("Open file fail.\n");
        return 0;
    }

    if (read_chunk() < 0) {
        printf("Read file fail.\n");
        return 0;
    }

    /* Below are GTK */
    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW (window), "Window");

    /* Destroy */
    g_signal_connect(window, "destroy", G_CALLBACK(close_window), NULL);

    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(window), frame);

    draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, frame_cfg.width / 1, frame_cfg.height / 1);

    gtk_container_add(GTK_CONTAINER(frame), draw_area);

#if 1
    g_signal_connect(draw_area, "draw",
                     G_CALLBACK(expose_event_callback), NULL);
#else
    /* Drawing circle test */
    g_signal_connect(draw_area, "draw",
                     G_CALLBACK(draw_callback), NULL);
#endif

    g_timeout_add(100, (GSourceFunc) time_handler, (gpointer) window);
    gtk_widget_show_all(window);
    time_handler(window);

    gtk_main();

    return 0;
}

