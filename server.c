/*
 * CC3064 Proyecto 01 - Servidor de chat (esqueleto)
 * Uso: ./server <puerto>
 * Pendiente: lista de clientes, mutex, hilos por sesión, protocolo completo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void usage(const char *argv0) {
    fprintf(stderr, "Uso: %s <puerto>\n", argv0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto inválido.\n");
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("Servidor escuchando en puerto %d (esqueleto — falta accept loop + threads)\n", port);

    /* TODO: bucle accept + pthread_create por cliente, REGISTER, mutex en lista */
    (void)getchar();

    close(listen_fd);
    return 0;
}
