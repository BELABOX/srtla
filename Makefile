CFLAGS=-g -O2 -Wall

all: sender receiver

sender: sender.o common.o

receiver: receiver.o common.o

clean:
	rm -f *.o sender receiver
