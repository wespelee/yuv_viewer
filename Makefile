
all: gtk_viewer.c
	gcc `pkg-config --cflags gtk+-3.0` -o gtk_viewer gtk_viewer.c `pkg-config --libs gtk+-3.0`

gtk_player: gtk_player.c
	gcc `pkg-config --cflags gtk+-3.0` -o gtk_player gtk_player.c `pkg-config --libs gtk+-3.0`

intel_va_viewer: intel_va_viewer.c
	gcc -g -Wall `pkg-config --cflags libva x11` -o intel_va_viewer intel_va_viewer.c `pkg-config --libs libva libva-x11 x11`

clean:
	rm -f gtk_viewer gtk_player
