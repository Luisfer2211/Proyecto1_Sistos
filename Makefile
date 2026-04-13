# CC3064 Proyecto 01 - Chat Multithread
# Uso normal (demo con switch, check de IP duplicada ON):
#   make
#
# Para pruebas locales en la misma máquina (múltiples clientes misma IP):
#   make dev

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -g -std=c11
TARGETS = server client

.PHONY: all dev clean

all: $(TARGETS)

# Modo producción: CHECK_DUPLICATE_IP=1 (default)
server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

# Modo desarrollo: deshabilita check de IP duplicada para probar varios
# clientes desde la misma máquina
dev: server.c client.c
	$(CC) $(CFLAGS) -DCHECK_DUPLICATE_IP=0 -o server server.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f $(TARGETS)
