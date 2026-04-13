/*
 * CC3064 Proyecto 01 - Cliente de chat
 * Grupo: Hugo Ernesto 23306 / Luis Palacios 23933 / Pablo Cabrera 231156
 * Uso: ./client <nombre_usuario> <IP_servidor> <puerto>
 *
 * Comandos de usuario:
 *   ALL <mensaje>                   → broadcast a todos
 *   <destino> <mensaje>             → mensaje directo
 *   /list                           → listar usuarios
 *   /whois <usuario>                → IP de un usuario
 *   /status <ACTIVO|OCUPADO|INACTIVO>
 *   /help                           → ayuda
 *   /bye                            → salir
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE 2048

static int           g_fd      = -1;
static char          g_user[64];
static char          g_status[16] = "ACTIVO";
static volatile int  g_running = 1;

/* ── Envío ────────────────────────────────────────────────────────────────── */
static void send_line(int fd, const char *msg) {
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

/* ── Parsear línea recibida ───────────────────────────────────────────────── */
typedef struct { char type[32]; char from[64]; char to[64]; char content[BUF_SIZE]; } Msg;

static void parse_msg(const char *line, Msg *m) {
    memset(m, 0, sizeof(*m));
    const char *p = line;
    char *fields[4] = { m->type, m->from, m->to, m->content };
    int   sizes[4]  = { 31, 63, 63, BUF_SIZE - 1 };
    for (int i = 0; i < 4; i++) {
        const char *pipe = (i < 3) ? strchr(p, '|') : NULL;
        size_t len = pipe ? (size_t)(pipe - p) : strlen(p);
        if (len > (size_t)sizes[i]) len = (size_t)sizes[i];
        strncpy(fields[i], p, len);
        fields[i][len] = '\0';
        p = pipe ? pipe + 1 : p + len;
    }
}

/* ── Ayuda ───────────────────────────────────────────────────────────────── */
static void print_help(void) {
    printf("\n┌─────────────────────────────────────────┐\n");
    printf("│           Comandos disponibles          │\n");
    printf("├─────────────────────────────────────────┤\n");
    printf("│ ALL <msg>          Chat general         │\n");
    printf("│ <usuario> <msg>    Mensaje directo       │\n");
    printf("│ /list              Listar usuarios       │\n");
    printf("│ /whois <usuario>   Info de usuario       │\n");
    printf("│ /status ACTIVO|OCUPADO|INACTIVO          │\n");
    printf("│ /help              Esta ayuda            │\n");
    printf("│ /bye               Salir                 │\n");
    printf("└─────────────────────────────────────────┘\n\n");
}

/* ── Thread receptor ─────────────────────────────────────────────────────── */
static void *recv_thread(void *arg) {
    (void)arg;
    char rx[BUF_SIZE];
    int  rx_len = 0;
    char line[BUF_SIZE];

    while (g_running) {
        int n = recv(g_fd, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
        if (n <= 0) {
            if (g_running) printf("\n[!] Conexión perdida con el servidor.\n");
            g_running = 0;
            break;
        }
        rx_len += n;
        rx[rx_len] = '\0';

        char *nl;
        while ((nl = strchr(rx, '\n')) != NULL) {
            *nl = '\0';
            strncpy(line, rx, sizeof(line) - 1);
            int consumed = (int)(nl - rx) + 1;
            memmove(rx, rx + consumed, rx_len - consumed);
            rx_len -= consumed;
            rx[rx_len] = '\0';

            if (!line[0]) continue;

            Msg m;
            parse_msg(line, &m);

            /* ── Mensaje de chat recibido ──────────────────────────────── */
            if (strcmp(m.type, "CHAT") == 0) {
                if (strcmp(m.to, "ALL") == 0)
                    printf("\n[BROADCAST de %s]: %s\n> ", m.from, m.content);
                else
                    printf("\n[PRIVADO de %s → %s]: %s\n> ", m.from, m.to, m.content);
                fflush(stdout);
            }

            /* ── Notificaciones del servidor ──────────────────────────── */
            else if (strcmp(m.type, "INFO") == 0) {
                printf("\n[INFO]: %s\n> ", m.content);
                fflush(stdout);
            }

            /* ── STATUS forzado por watchdog ──────────────────────────── */
            else if (strcmp(m.type, "STATUS") == 0) {
                strncpy(g_status, m.content, 15);
                printf("\n[Servidor] Tu status cambió a: %s\n> ", g_status);
                fflush(stdout);
            }

            /* ── Respuestas OK ────────────────────────────────────────── */
            else if (strcmp(m.type, "OK") == 0) {
                /* LIST: content = "LIST|user1(ST),user2(ST),..." */
                if (strncmp(m.content, "LIST|", 5) == 0) {
                    printf("\n[Usuarios conectados]: %s\n> ", m.content + 5);
                    fflush(stdout);
                }
                /* WHOIS: content = "WHOIS|usuario|ip" */
                else if (strncmp(m.content, "WHOIS|", 6) == 0) {
                    printf("\n[WHOIS]: %s\n> ", m.content + 6);
                    fflush(stdout);
                }
                /* STATUS: content = "STATUS|NUEVO" */
                else if (strncmp(m.content, "STATUS|", 7) == 0) {
                    strncpy(g_status, m.content + 7, 15);
                    printf("\n[Status actualizado]: %s\n> ", g_status);
                    fflush(stdout);
                }
                /* EXIT confirmado */
                else if (strcmp(m.content, "EXIT") == 0) {
                    g_running = 0;
                }
                /* CHAT_SENT / MSG_SENT / REGISTER → silencioso */
            }

            /* ── Errores ──────────────────────────────────────────────── */
            else if (strcmp(m.type, "ERR") == 0) {
                printf("\n[ERROR]: %s\n> ", m.content);
                fflush(stdout);
            }
        }
    }

    return NULL;
}

/* ── Registro inicial (bloqueante hasta recibir respuesta) ───────────────── */
static int do_register(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "REGISTER|%s|SERVER|\n", g_user);
    send_line(g_fd, buf);

    /* Leer línea de respuesta */
    char rx[512]; int rx_len = 0;
    for (;;) {
        int n = recv(g_fd, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
        if (n <= 0) return 0;
        rx_len += n;
        rx[rx_len] = '\0';
        char *nl = strchr(rx, '\n');
        if (!nl) continue;
        *nl = '\0';
        break;
    }

    if (strncmp(rx, "OK", 2) == 0) return 1;

    /* Mostrar razón del error */
    char *last_pipe = strrchr(rx, '|');
    fprintf(stderr, "[!] Registro rechazado: %s\n", last_pipe ? last_pipe + 1 : rx);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <nombre_usuario> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    strncpy(g_user, argv[1], 63);
    const char *server_ip = argv[2];
    int port = atoi(argv[3]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Puerto inválido.\n"); return 1; }

    signal(SIGPIPE, SIG_IGN);

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "IP inválida: %s\n", server_ip);
        close(g_fd); return 1;
    }
    if (connect(g_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(g_fd); return 1;
    }

    /* Registrar */
    if (!do_register()) { close(g_fd); return 1; }

    printf("✓ Conectado como '%s' a %s:%d\n", g_user, server_ip, port);
    print_help();

    /* Lanzar thread de recepción */
    pthread_t rtid;
    pthread_create(&rtid, NULL, recv_thread, NULL);
    pthread_detach(rtid);

    /* ── Bucle de stdin ──────────────────────────────────────────────────── */
    char input[BUF_SIZE];
    while (g_running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            /* EOF (Ctrl-D) → salir */
            break;
        }
        input[strcspn(input, "\n")] = '\0';
        if (!input[0]) continue;
        if (!g_running) break;

        char out[BUF_SIZE];

        /* /bye */
        if (strcmp(input, "/bye") == 0) {
            snprintf(out, sizeof(out), "EXIT|%s|SERVER|\n", g_user);
            send_line(g_fd, out);
            g_running = 0;
        }
        /* /list */
        else if (strcmp(input, "/list") == 0) {
            snprintf(out, sizeof(out), "LIST|%s|SERVER|\n", g_user);
            send_line(g_fd, out);
        }
        /* /whois <usuario> */
        else if (strncmp(input, "/whois ", 7) == 0) {
            snprintf(out, sizeof(out), "WHOIS|%s|SERVER|%s\n", g_user, input + 7);
            send_line(g_fd, out);
        }
        /* /status <ST> */
        else if (strncmp(input, "/status ", 8) == 0) {
            snprintf(out, sizeof(out), "STATUS|%s|SERVER|%s\n", g_user, input + 8);
            send_line(g_fd, out);
        }
        /* /help */
        else if (strcmp(input, "/help") == 0) {
            print_help();
        }
        /* ALL <mensaje> → broadcast */
        else if (strncmp(input, "ALL ", 4) == 0) {
            snprintf(out, sizeof(out), "CHAT|%s|ALL|%s\n", g_user, input + 4);
            send_line(g_fd, out);
        }
        /* <usuario> <mensaje> → directo */
        else {
            char *space = strchr(input, ' ');
            if (space && space != input) {
                *space = '\0';
                char *dest = input;
                char *msg  = space + 1;
                snprintf(out, sizeof(out), "CHAT|%s|%s|%s\n", g_user, dest, msg);
                send_line(g_fd, out);
            } else {
                printf("[!] Formato inválido. Escribe /help para ver comandos.\n");
            }
        }
    }

    sleep(1);   /* dar tiempo al thread rx para imprimir OK EXIT */
    close(g_fd);
    printf("Sesión terminada. ¡Hasta luego!\n");
    return 0;
}
