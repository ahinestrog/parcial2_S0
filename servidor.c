#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_SALAS 20
#define MAX_USUARIOS_POR_SALA 50
#define MAX_TEXTO 512
#define MAX_NOMBRE 50

// Tipos de mensaje
#define T_JOIN 1
#define T_JOIN_ACK 2
#define T_CHAT 3
#define T_CMD_LIST 4
#define T_CMD_USERS 5
#define T_LEAVE 6
#define T_SERVER_BCAST 10

typedef struct {
    long mtype;                 // tipo
    pid_t pid;                  // pid del remitente
    key_t client_key;           // key de la cola privada del cliente (en JOIN)
    char remitente[MAX_NOMBRE];
    char sala[MAX_NOMBRE];
    char texto[MAX_TEXTO];
} mensaje_t;

typedef struct {
    char nombre[MAX_NOMBRE];
    int num_usuarios;
    key_t user_keys[MAX_USUARIOS_POR_SALA];
    char usuarios[MAX_USUARIOS_POR_SALA][MAX_NOMBRE];
    pid_t pids[MAX_USUARIOS_POR_SALA];
} sala_t;

sala_t salas[MAX_SALAS];
int num_salas = 0;
int cola_global = -1;
key_t key_global;

void cleanup_and_exit(int signo) {
    if (cola_global != -1) {
        msgctl(cola_global, IPC_RMID, NULL);
        printf("\nCola global borrada. Servidor finalizado.\n");
    }
    exit(0);
}

int buscar_sala(const char *nombre) {
    for (int i = 0; i < num_salas; i++) {
        if (strcmp(salas[i].nombre, nombre) == 0) return i;
    }
    return -1;
}

int crear_sala(const char *nombre) {
    if (num_salas >= MAX_SALAS) return -1;
    strncpy(salas[num_salas].nombre, nombre, MAX_NOMBRE-1);
    salas[num_salas].num_usuarios = 0;
    num_salas++;
    // crear archivo de log inicial
    char path[256];
    snprintf(path, sizeof(path), "logs/%s.log", nombre);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "== Log de sala %s creado ==\n", nombre);
        fclose(f);
    }
    return num_salas - 1;
}

int agregar_usuario_a_sala(int idx_sala, key_t client_key, const char *nombre_usuario, pid_t pid) {
    if (idx_sala < 0 || idx_sala >= num_salas) return -1;
    sala_t *s = &salas[idx_sala];
    if (s->num_usuarios >= MAX_USUARIOS_POR_SALA) return -1;
    for (int i = 0; i < s->num_usuarios; i++) {
        if (s->pids[i] == pid) return -1; // ya existe
        if (strcmp(s->usuarios[i], nombre_usuario) == 0) return -1;
    }
    s->user_keys[s->num_usuarios] = client_key;
    strncpy(s->usuarios[s->num_usuarios], nombre_usuario, MAX_NOMBRE-1);
    s->pids[s->num_usuarios] = pid;
    s->num_usuarios++;
    return 0;
}

int quitar_usuario_de_sala(int idx_sala, pid_t pid) {
    if (idx_sala < 0 || idx_sala >= num_salas) return -1;
    sala_t *s = &salas[idx_sala];
    int found = -1;
    for (int i = 0; i < s->num_usuarios; i++) {
        if (s->pids[i] == pid) { found = i; break; }
    }
    if (found == -1) return -1;
    // desplazar
    for (int j = found; j < s->num_usuarios - 1; j++) {
        s->user_keys[j] = s->user_keys[j+1];
        s->pids[j] = s->pids[j+1];
        strncpy(s->usuarios[j], s->usuarios[j+1], MAX_NOMBRE);
    }
    s->num_usuarios--;
    return 0;
}

void append_log(const char *sala, const char *remitente, const char *texto) {
    char path[256];
    snprintf(path, sizeof(path), "logs/%s.log", sala);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "[%s] %s: %s\n", timebuf, remitente, texto);
    fclose(f);
}

void enviar_a_todos_en_sala(int idx_sala, mensaje_t *msg_in) {
    if (idx_sala < 0 || idx_sala >= num_salas) return;
    sala_t *s = &salas[idx_sala];
    mensaje_t out;
    out.mtype = T_SERVER_BCAST;
    out.pid = getpid();
    strncpy(out.remitente, msg_in->remitente, MAX_NOMBRE-1);
    strncpy(out.sala, msg_in->sala, MAX_NOMBRE-1);
    strncpy(out.texto, msg_in->texto, MAX_TEXTO-1);

    for (int i = 0; i < s->num_usuarios; i++) {
        if (s->pids[i] == msg_in->pid) continue;
        int msqid_client = msgget(s->user_keys[i], 0666);
        if (msqid_client == -1) {
            continue; // cliente puede haber cerrado su cola, continuar
        }
        if (msgsnd(msqid_client, &out, sizeof(out) - sizeof(long), 0) == -1) {
            perror("Error msgsnd a cliente");
        }
    }
    // guardar en log
    append_log(msg_in->sala, msg_in->remitente, msg_in->texto);
}

int main() {
    signal(SIGINT, cleanup_and_exit);

    // Crear carpeta logs si no existe
    mkdir("logs", 0755);

    // Crear cola global (ftok con "/tmp",'A')
    key_global = ftok("/tmp", 'A');
    if (key_global == -1) {
        perror("ftok");
        exit(1);
    }
    cola_global = msgget(key_global, IPC_CREAT | 0666);
    if (cola_global == -1) {
        perror("msgget cola global");
        exit(1);
    }

    printf("Servidor de chat iniciado. Cola global creada (key %d). Esperando clientes...\n", (int)key_global);

    mensaje_t msg;
    while (1) {
        ssize_t r = msgrcv(cola_global, &msg, sizeof(msg) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv servidor");
            continue;
        }

        if (msg.mtype == T_JOIN) {
            printf("JOIN: usuario=%s pid=%d sala=%s key=%d\n", msg.remitente, msg.pid, msg.sala, (int)msg.client_key);
            int idx = buscar_sala(msg.sala);
            if (idx == -1) {
                idx = crear_sala(msg.sala);
                if (idx == -1) {
                    fprintf(stderr, "No puedo crear más salas\n");
                    continue;
                }
                printf("Sala creada: %s (idx=%d)\n", msg.sala, idx);
            }

            if (agregar_usuario_a_sala(idx, msg.client_key, msg.remitente, msg.pid) == 0) {
                // enviar confirmación al cliente en su cola privada
                int msqid_client = msgget(msg.client_key, 0666);
                if (msqid_client != -1) {
                    mensaje_t resp;
                    resp.mtype = T_JOIN_ACK;
                    resp.pid = getpid();
                    strncpy(resp.remitente, "Servidor", MAX_NOMBRE-1);
                    snprintf(resp.texto, MAX_TEXTO-1, "Te has unido a la sala: %s", msg.sala);
                    msgsnd(msqid_client, &resp, sizeof(resp) - sizeof(long), 0);
                }

                // avisar a los demás de la sala
                mensaje_t nota;
                nota.mtype = T_SERVER_BCAST;
                nota.pid = getpid();
                strncpy(nota.remitente, "Servidor", MAX_NOMBRE-1);
                strncpy(nota.sala, msg.sala, MAX_NOMBRE-1);
                snprintf(nota.texto, MAX_TEXTO-1, "%s se ha unido a la sala.", msg.remitente);
                // Lo enviamos a todos excepto al remitente
                enviar_a_todos_en_sala(idx, &nota);
                printf("Usuario %s agregado a %s\n", msg.remitente, msg.sala);
            } else {
                fprintf(stderr, "No se pudo agregar %s a %s\n", msg.remitente, msg.sala);
            }
        } 
        
        else if (msg.mtype == T_CHAT) {
            // reenviar a todos de la sala
            printf("CHAT [%s] %s: %s\n", msg.sala, msg.remitente, msg.texto);
            int idx = buscar_sala(msg.sala);
            if (idx != -1) {
                enviar_a_todos_en_sala(idx, &msg);
            } else {
                int msqid_client = msgget(msg.client_key, 0666);
                if (msqid_client != -1) {
                    mensaje_t resp;
                    resp.mtype = T_SERVER_BCAST;
                    resp.pid = getpid();
                    strncpy(resp.remitente, "Servidor", MAX_NOMBRE-1);
                    snprintf(resp.texto, MAX_TEXTO-1, "Sala %s no existe.", msg.sala);
                    msgsnd(msqid_client, &resp, sizeof(resp) - sizeof(long), 0);
                }
            }
        } 
        
        else if (msg.mtype == T_CMD_LIST) {
            // construir lista de salas
            char buffer[MAX_TEXTO];
            buffer[0] = '\0';
            for (int i = 0; i < num_salas; i++) {
                strncat(buffer, salas[i].nombre, sizeof(buffer) - strlen(buffer) - 2);
                strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
            }
            int msqid_client = msgget(msg.client_key, 0666);
            if (msqid_client != -1) {
                mensaje_t resp;
                resp.mtype = T_SERVER_BCAST;
                resp.pid = getpid();
                strncpy(resp.remitente, "Servidor", MAX_NOMBRE-1);
                strncpy(resp.texto, buffer, MAX_TEXTO-1);
                msgsnd(msqid_client, &resp, sizeof(resp) - sizeof(long), 0);
            }
        } 
        
        else if (msg.mtype == T_CMD_USERS) {
            int idx = buscar_sala(msg.sala);
            char buffer[MAX_TEXTO];
            buffer[0] = '\0';
            if (idx != -1) {
                sala_t *s = &salas[idx];
                for (int i = 0; i < s->num_usuarios; i++) {
                    strncat(buffer, s->usuarios[i], sizeof(buffer) - strlen(buffer) - 2);
                    strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
                }
            } else {
                snprintf(buffer, sizeof(buffer), "Sala %s no existe.\n", msg.sala);
            }

            int msqid_client = msgget(msg.client_key, 0666);
            if (msqid_client != -1) {
                mensaje_t resp;
                resp.mtype = T_SERVER_BCAST;
                resp.pid = getpid();
                strncpy(resp.remitente, "Servidor", MAX_NOMBRE-1);
                strncpy(resp.texto, buffer, MAX_TEXTO-1);
                if (msgsnd(msqid_client, &resp, sizeof(resp) - sizeof(long), 0) == -1) {
                    perror("msgsnd respuesta /users");
                }
            } else {
                printf("SERVIDOR: no se puede enviar la lista /users: cola cliente (key %d) no encontrada\n", (int)msg.client_key);
            }
        } 
        
        else if (msg.mtype == T_LEAVE) {
            int idx = buscar_sala(msg.sala);
            if (idx != -1) {
                if (quitar_usuario_de_sala(idx, msg.pid) == 0) {
                    printf("SERVIDOR: %s (pid %d) ha abandonado la sala '%s'\n", msg.remitente, msg.pid, msg.sala);

                    // notificar a los demas
                    mensaje_t nota;
                    nota.mtype = T_SERVER_BCAST;
                    nota.pid = getpid();
                    strncpy(nota.remitente, "Servidor", MAX_NOMBRE-1);
                    strncpy(nota.sala, msg.sala, MAX_NOMBRE-1);
                    snprintf(nota.texto, MAX_TEXTO-1, "%s ha abandonado la sala.", msg.remitente);
                    enviar_a_todos_en_sala(idx, &nota);
                    // confirmar a usuario
                    int msqid_client = msgget(msg.client_key, 0666);
                    if (msqid_client != -1) {
                        mensaje_t resp; resp.mtype = T_SERVER_BCAST; resp.pid = getpid();
                        strncpy(resp.remitente, "Servidor", MAX_NOMBRE-1);
                        strncpy(resp.texto, "Has salido de la sala.\n", MAX_TEXTO-1);
                        msgsnd(msqid_client, &resp, sizeof(resp) - sizeof(long), 0);
                    }
                }
            }
        } else {
            // tipo desconocido
            fprintf(stderr, "Mensaje con tipo desconocido: %ld\n", msg.mtype);
        }
    }

    return 0;
}
