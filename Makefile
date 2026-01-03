CC = gcc
CFLAGS = -Wall -pthread -g -std=c99
SRCDIR = src
TARGETS = $(SRCDIR)/client $(SRCDIR)/server

all: $(TARGETS)

$(SRCDIR)/client: $(SRCDIR)/client.c $(SRCDIR)/snake.h
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/client.c

$(SRCDIR)/server: $(SRCDIR)/server.c $(SRCDIR)/snake.h
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/server.c

clean:
	rm -f $(SRCDIR)/client $(SRCDIR)/server

.PHONY: all clean

