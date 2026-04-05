CC = gcc
CFLAGS = -Wall -Wextra -g -O2
LDFLAGS = -lm

TARGETS = sender receiver stats_graph
OBJS = multicast.o

.PHONY: all clean

all: $(TARGETS)

sender: sender.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

receiver: receiver.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stats_graph: stats_graph.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

multicast.o: multicast.c multicast.h
	$(CC) $(CFLAGS) -c multicast.c

clean:
	rm -f $(TARGETS) $(OBJS) *.o

.PHONY: run-sender run-receiver
run-sender: sender
	./sender share/descr.txt

run-receiver: receiver
	./receiver
