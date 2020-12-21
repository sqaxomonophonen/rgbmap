all: rgbmap

#OPT=-O0 -g
OPT=-O2
CFLAGS=$(OPT) -Wall
LINK=-lm

main.o: main.c
	$(CC) $(CFLAGS) -c $<

stbim.o: stbim.c
	$(CC) $(CFLAGS) -c $<


rgbmap: main.o stbim.o
	$(CC) $^ -o $@ $(LINK)

clean:
	rm -f rgbmap *.o
