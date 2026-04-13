/*
 * CC3064 Proyecto 01 - Servidor de chat
 * Grupo: Hugo Ernesto 23306 / Luis Palacios 23933 / Pablo Cabrera 231156
 * Uso: ./server <puerto>
 *
 * Protocolo (una línea por mensaje, UTF-8, delimitador '\n'):
 *   TIPO|ORIGEN|DESTINO|CONTENIDO
 *
 * Condiciones de carrera documentadas en el PDF:
 *   1. Registro simultáneo con el mismo nombre  → protegido con g_mutex en REGISTER
 *   2. Broadcast mientras otro thread borra un cliente → protegido con g_mutex en broadcast()
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ── Constantes ─────────────────────────────────────────────────────────── */
#define MAX_CLIENTS    32
#define BUF_SIZE       2048
#define INACTIVE_SECS  30   /* segundos sin tráfico → marcar INACTIVO */

/*
 * CHECK_DUPLICATE_IP: 1 = activado (producción, demo con switch)
 *                     0 = desactivado (pruebas locales en la misma máquina)
 * Para compilar sin check:  make server EXTRA="-DCHECK_DUPLICATE_IP=0"
 */
#ifndef CHECK_DUPLICATE_IP
#define CHECK_DUPLICATE_IP 1
#endif

/* ── Tipos ───────────────────────────────────────────────────────────────── */
typedef enum { ST_ACTIVO = 0, ST_OCUPADO, ST_INACTIVO } Status;
static const char *STATUS_STR[] = { "ACTIVO", "OCUPADO", "INACTIVO" };

typedef struct {
    int       fd;
    char      name[64];
    char      ip[INET_ADDRSTRLEN];
    Status    status;
    time_t    last_active;
    pthread_t tid;
    int       in_use;           /* 1 = slot ocupado */
} Client;

/* ── Estado global (protegido por g_mutex) ────────────────────────────────
 * Recurso compartido 1: clients[] y n_clients
 * Por qué se protege: múltiples threads de cliente leen/escriben la lista
 * simultáneamente (registro, broadcast, desconexión). Sin mutex habría
 * lecturas sucias y escrituras perdidas.
 */
static Client          clients[MAX_CLIENTS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Utilidades de envío ─────────────────────────────────────────────────── */
static void send_line(int fd, const char *msg) {
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

/* ── Búsquedas (llamar con g_mutex tomado) ───────────────────────────────── */
static int find_by_name(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use && strcmp(clients[i].name, name) == 0)
            return i;
    return -1;
}

static int find_by_ip(const char *ip) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use && strcmp(clients[i].ip, ip) == 0)
            return i;
    return -1;
}

static int find_free(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].in_use)
            return i;
    return -1;
}

/* ── Broadcast ───────────────────────────────────────────────────────────────
 * Recurso compartido 2: iteración sobre clients[] para enviar a todos.
 * Condición de carrera: si otro thread elimina un cliente mientras se itera,
 * se podría enviar a un fd cerrado o leer datos inválidos.
 * Solución: tomar g_mutex durante toda la iteración.
 */
static void broadcast(const char *msg, int skip_fd) {
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use && clients[i].fd != skip_fd)
            send_line(clients[i].fd, msg);
    pthread_mutex_unlock(&g_mutex);
}

/* Enviar a un usuario por nombre; retorna 1 si existía */
static int send_to_user(const char *dest, const char *msg) {
    pthread_mutex_lock(&g_mutex);
    int idx = find_by_name(dest);
    if (idx >= 0) send_line(clients[idx].fd, msg);
    pthread_mutex_unlock(&g_mutex);
    return idx >= 0;
}

/* Eliminar cliente por fd (llamar con g_mutex tomado) */
static void remove_locked(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].fd == fd) {
            clients[i].in_use = 0;
            clients[i].fd = -1;
            break;
        }
    }
}

/* ── Parsear línea: TIPO|ORIGEN|DESTINO|CONTENIDO ────────────────────────── */
typedef struct { char type[32]; char from[64]; char to[64]; char content[BUF_SIZE]; } Msg;

static int parse_msg(const char *line, Msg *m) {
    /* Dividir en máximo 4 campos; el contenido puede contener '|' */
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
    return m->type[0] != '\0';
}

/* ── Thread watchdog de inactividad ─────────────────────────────────────── */
static void *monitor_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(5);
        time_t now = time(NULL);
        pthread_mutex_lock(&g_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && clients[i].status == ST_ACTIVO &&
                difftime(now, clients[i].last_active) >= INACTIVE_SECS) {
                clients[i].status = ST_INACTIVO;
                char buf[128];
                snprintf(buf, sizeof(buf), "STATUS|SERVER|%s|INACTIVO\n", clients[i].name);
                send_line(clients[i].fd, buf);
                printf("[watchdog] %s marcado INACTIVO\n", clients[i].name);
            }
        }
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

/* ── Thread por cliente ──────────────────────────────────────────────────── */
typedef struct { int fd; char ip[INET_ADDRSTRLEN]; } ThreadArg;

static void *handle_client(void *raw) {
    ThreadArg arg = *(ThreadArg *)raw;
    free(raw);

    int  fd = arg.fd;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, arg.ip, sizeof(ip) - 1);

    char rx[BUF_SIZE];        /* buffer de recv acumulativo */
    int  rx_len = 0;
    char line[BUF_SIZE];
    char username[64] = {0};
    int  slot = -1;
    int  registered = 0;

    /* ── Fase REGISTER ─────────────────────────────────────────────────── */
    while (!registered) {
        int n = recv(fd, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
        if (n <= 0) goto cleanup;
        rx_len += n;
        rx[rx_len] = '\0';

        char *nl = strchr(rx, '\n');
        if (!nl) continue;
        *nl = '\0';
        strncpy(line, rx, sizeof(line) - 1);
        int consumed = (int)(nl - rx) + 1;
        memmove(rx, rx + consumed, rx_len - consumed);
        rx_len -= consumed;

        Msg m = {0};
        if (!parse_msg(line, &m) || strcmp(m.type, "REGISTER") != 0 || !m.from[0]) {
            send_line(fd, "ERR|SERVER|?|INVALID_MSG\n");
            continue;
        }

        /* ─ Sección crítica: verificar duplicados y registrar ─
         * (Condición de carrera 1 evitada aquí)                */
        pthread_mutex_lock(&g_mutex);
        int dup_name = find_by_name(m.from);
        int dup_ip   = find_by_ip(ip);
        slot         = find_free();

        if (dup_name >= 0) {
            pthread_mutex_unlock(&g_mutex);
            send_line(fd, "ERR|SERVER|?|DUPLICATE_NAME\n");
            continue;
        }
#if CHECK_DUPLICATE_IP
        if (dup_ip >= 0) {
            pthread_mutex_unlock(&g_mutex);
            send_line(fd, "ERR|SERVER|?|DUPLICATE_IP\n");
            continue;
        }
#else
        (void)dup_ip;   /* ignorado en modo desarrollo */
#endif
        if (slot < 0) {
            pthread_mutex_unlock(&g_mutex);
            send_line(fd, "ERR|SERVER|?|SERVER_FULL\n");
            goto cleanup;
        }

        /* Registrar */
        clients[slot].fd          = fd;
        clients[slot].status      = ST_ACTIVO;
        clients[slot].last_active = time(NULL);
        clients[slot].in_use      = 1;
        clients[slot].tid         = pthread_self();
        strncpy(clients[slot].name, m.from, 63);
        strncpy(clients[slot].ip,   ip,     INET_ADDRSTRLEN - 1);
        pthread_mutex_unlock(&g_mutex);

        strncpy(username, m.from, 63);
        registered = 1;

        char ok[128];
        snprintf(ok, sizeof(ok), "OK|SERVER|%s|REGISTER\n", username);
        send_line(fd, ok);
        printf("[server] REGISTER %s desde %s\n", username, ip);

        /* Notificar al resto */
        char info[256];
        snprintf(info, sizeof(info), "INFO|SERVER|ALL|%s se conectó\n", username);
        broadcast(info, fd);
    }

    /* ── Bucle de sesión ─────────────────────────────────────────────────── */
    for (;;) {
        int n = recv(fd, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
        if (n <= 0) break;
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

            /* Actualizar actividad */
            pthread_mutex_lock(&g_mutex);
            if (slot >= 0 && clients[slot].in_use) {
                clients[slot].last_active = time(NULL);
                if (clients[slot].status == ST_INACTIVO)
                    clients[slot].status = ST_ACTIVO;
            }
            pthread_mutex_unlock(&g_mutex);

            Msg m = {0};
            if (!parse_msg(line, &m)) continue;

            /* ── LIST ─────────────────────────────────────────────────── */
            if (strcmp(m.type, "LIST") == 0) {
                char resp[BUF_SIZE];
                int  pos = snprintf(resp, sizeof(resp), "OK|SERVER|%s|LIST|", username);
                pthread_mutex_lock(&g_mutex);
                int first = 1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].in_use) continue;
                    pos += snprintf(resp + pos, sizeof(resp) - pos, "%s%s(%s)",
                        first ? "" : ",",
                        clients[i].name, STATUS_STR[clients[i].status]);
                    first = 0;
                }
                pthread_mutex_unlock(&g_mutex);
                pos += snprintf(resp + pos, sizeof(resp) - pos, "\n");
                send_line(fd, resp);
            }

            /* ── WHOIS ────────────────────────────────────────────────── */
            else if (strcmp(m.type, "WHOIS") == 0) {
                char resp[256];
                pthread_mutex_lock(&g_mutex);
                int idx = find_by_name(m.content);
                if (idx >= 0)
                    snprintf(resp, sizeof(resp), "OK|SERVER|%s|WHOIS|%s|%s\n",
                        username, m.content, clients[idx].ip);
                else
                    snprintf(resp, sizeof(resp), "ERR|SERVER|%s|NOT_FOUND\n", username);
                pthread_mutex_unlock(&g_mutex);
                send_line(fd, resp);
            }

            /* ── STATUS ───────────────────────────────────────────────── */
            else if (strcmp(m.type, "STATUS") == 0) {
                Status ns;
                int valid = 1;
                if      (strcmp(m.content, "ACTIVO")   == 0) ns = ST_ACTIVO;
                else if (strcmp(m.content, "OCUPADO")  == 0) ns = ST_OCUPADO;
                else if (strcmp(m.content, "INACTIVO") == 0) ns = ST_INACTIVO;
                else    valid = 0;

                char resp[128];
                if (valid) {
                    pthread_mutex_lock(&g_mutex);
                    if (slot >= 0) clients[slot].status = ns;
                    pthread_mutex_unlock(&g_mutex);
                    snprintf(resp, sizeof(resp), "OK|SERVER|%s|STATUS|%s\n", username, m.content);
                } else {
                    snprintf(resp, sizeof(resp), "ERR|SERVER|%s|INVALID_STATUS\n", username);
                }
                send_line(fd, resp);
            }

            /* ── CHAT ─────────────────────────────────────────────────── */
            else if (strcmp(m.type, "CHAT") == 0) {
                char out[BUF_SIZE];
                if (strcmp(m.to, "ALL") == 0) {
                    /* Broadcast (condición de carrera 2 evitada en broadcast()) */
                    snprintf(out, sizeof(out), "CHAT|%s|ALL|%s\n", username, m.content);
                    broadcast(out, fd);
                    char echo[64];
                    snprintf(echo, sizeof(echo), "OK|SERVER|%s|CHAT_SENT\n", username);
                    send_line(fd, echo);
                } else {
                    /* Mensaje directo */
                    snprintf(out, sizeof(out), "CHAT|%s|%s|%s\n", username, m.to, m.content);
                    if (send_to_user(m.to, out)) {
                        char echo[64];
                        snprintf(echo, sizeof(echo), "OK|SERVER|%s|MSG_SENT\n", username);
                        send_line(fd, echo);
                    } else {
                        char err[128];
                        snprintf(err, sizeof(err), "ERR|SERVER|%s|USER_NOT_FOUND\n", username);
                        send_line(fd, err);
                    }
                }
            }

            /* ── EXIT ─────────────────────────────────────────────────── */
            else if (strcmp(m.type, "EXIT") == 0) {
                char ok[64];
                snprintf(ok, sizeof(ok), "OK|SERVER|%s|EXIT\n", username);
                send_line(fd, ok);
                goto cleanup;
            }

            /* ── Desconocido ──────────────────────────────────────────── */
            else {
                char err[128];
                snprintf(err, sizeof(err), "ERR|SERVER|%s|UNKNOWN_CMD\n", username);
                send_line(fd, err);
            }
        }
    }

cleanup:
    printf("[server] Desconectando %s\n", username[0] ? username : "(sin registrar)");
    if (username[0]) {
        char info[256];
        snprintf(info, sizeof(info), "INFO|SERVER|ALL|%s se desconectó\n", username);
        broadcast(info, fd);
    }
    pthread_mutex_lock(&g_mutex);
    remove_locked(fd);
    pthread_mutex_unlock(&g_mutex);
    close(fd);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Uso: %s <puerto>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Puerto inválido.\n"); return 1; }

    signal(SIGPIPE, SIG_IGN);   /* evitar crash si el cliente se desconecta abruptamente */
    memset(clients, 0, sizeof(clients));

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind");   close(lfd); return 1; }
    if (listen(lfd, 16) < 0)                                   { perror("listen"); close(lfd); return 1; }

    printf("[server] Escuchando en puerto %d (inactividad: %ds)\n", port, INACTIVE_SECS);

    /* Watchdog de inactividad */
    pthread_t wtid;
    pthread_create(&wtid, NULL, monitor_thread, NULL);
    pthread_detach(wtid);

    /* Bucle accept */
    for (;;) {
        struct sockaddr_in caddr = {0};
        socklen_t clen = sizeof(caddr);
        int cfd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        ThreadArg *targ = malloc(sizeof(ThreadArg));
        if (!targ) { close(cfd); continue; }
        targ->fd = cfd;
        inet_ntop(AF_INET, &caddr.sin_addr, targ->ip, sizeof(targ->ip));
        printf("[server] Conexión entrante desde %s\n", targ->ip);

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, targ) != 0) {
            perror("pthread_create");
            close(cfd);
            free(targ);
        } else {
            pthread_detach(tid);
        }
    }

    close(lfd);
    return 0;
}
