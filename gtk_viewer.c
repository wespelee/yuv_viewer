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
    int width;
    int height;
    gint channel;
};

struct viewer_cfg frame_cfg;
guchar *input_buf = NULL;
int *rgb_buf = NULL;

static int open_file(char *fn)
{
    FILE *fp = NULL;
    struct stat st;
    unsigned int tsize = (frame_cfg.width * frame_cfg.height * 3 / 2);

    if (stat(fn, &st) < 0) {
        perror("check_file_size");
        return -1;
    }

    if (st.st_size != tsize) {
        printf("File size is not match resolution setting!\n");
        return -1;
    }

    fp = fopen(fn, "r");

    if (!fp) {
        perror("File open fail!");
        return -1;
    }

    fread(input_buf, tsize, 1, fp);

    fclose(fp);
    return 0;
}


/* Surface to store current scribbles */
static void close_window(void)
{
    if (input_buf) {
        free(input_buf);
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

    yuv_rgb_conversion(rgb_buf, input_buf, frame_cfg.width, frame_cfg.height);

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

static int input_buffer_init(char *fn)
{
    input_buf = calloc(1, frame_cfg.width * frame_cfg.height * 3 / 2);

    if (input_buf == NULL) {
        perror("Memory calloc fail");
        return -1;
    }

    if (open_file(fn) < 0) {
        printf("Open file error!\n");
        return -1;
    }

    return 0;
}

static void print_help()
{
    printf("Usage:\n");
    printf("\tviewer file width height\n");
}

static void frame_cfg_init(int argc, char **argv)
{
    frame_cfg.width = atoi(argv[2]);
    frame_cfg.height = atoi(argv[3]);
}

int main(int argc, char *argv[])
{
    GtkWidget *window;
    GtkWidget *draw_area;
    GtkWidget *frame;

    if (argc != 4) {
        print_help();
        return 0;
    }

    memset(&frame_cfg, 0, sizeof(frame_cfg));

    frame_cfg_init(argc, argv);

    if (input_buffer_init(argv[1]) < 0) {
        printf("Buffer initial fail.\n");
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
    gtk_widget_set_size_request(draw_area, frame_cfg.width / 4, frame_cfg.height / 4);

    gtk_container_add(GTK_CONTAINER(frame), draw_area);

#if 1
    g_signal_connect(draw_area, "draw",
                     G_CALLBACK(expose_event_callback), NULL);
#else
    /* Drawing circle test */
    g_signal_connect(draw_area, "draw",
                     G_CALLBACK(draw_callback), NULL);
#endif

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}

