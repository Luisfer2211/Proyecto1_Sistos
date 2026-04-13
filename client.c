/*
 * CC3064 Proyecto 01 - Cliente de chat (esqueleto)
 * Uso: ./client <nombre_usuario> <IP_servidor> <puerto>
 * Pendiente: segundo hilo para recv, REGISTER, menú / parsing "<usuario> <msg>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void usage(const char *argv0) {
    fprintf(stderr, "Uso: %s <nombre_usuario> <IP_servidor> <puerto>\n", argv0);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *username = argv[1];
    const char *server_ip = argv[2];
    int port = atoi(argv[3]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto inválido.\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "IP del servidor inválida.\n");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    printf("Conectado como '%s' (esqueleto — falta REGISTER y hilo de recepción)\n", username);

    /* TODO: enviar REGISTER, pthread para leer socket, stdin para comandos */
    (void)getchar();

    close(fd);
    return 0;
}
