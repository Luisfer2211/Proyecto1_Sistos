# CC3064 - ajustar CC según tu entorno (Linux de entrega: gcc)
CC = gcc
CFLAGS = -Wall -Wextra -pthread

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client

.PHONY: all clean
