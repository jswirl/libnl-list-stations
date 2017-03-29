# apt-get install libnl-3-dev and libnl-genl-3-dev build-essential pkg-config

EXEC=list-stations
CFLAGS += -Wall -pipe -g `pkg-config --cflags libnl-3.0 libnl-genl-3.0`
LDFLAGS += `pkg-config --libs libnl-3.0 libnl-genl-3.0`

all:
	gcc -o $(EXEC) $(CFLAGS) main.c $(LDFLAGS)

clean:
	rm -f $(EXEC)

