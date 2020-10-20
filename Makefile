CFLAGS=-g -O2 -Wall

all: srtla_send srtla_rec

srtla_send: srtla_send.o common.o

srtla_rec: srtla_rec.o common.o

clean:
	rm -f *.o srtla_send srtla_rec
