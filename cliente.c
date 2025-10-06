#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_TEXTO 512
#define MAX_NOMBRE 50

#define T_JOIN 1
#define T_JOIN_ACK 2
#define T_CHAT 3
#define T_CMD_LIST 4
#define T_CMD_USERS 5
#define T_LEAVE 6
#define T_SERVER_BCAST 10

typedef struct {
    long mtype;
    pid_t pid;
    key_t client_key;
    char remitente[MAX_NOMBRE];
    char sala[MAX_NOMBRE];
    char texto[MAX_TEXTO];
} mensaje_t;

int msqid_global = -1;
int msqid_priv = -1;
key_t key_priv;
char nombre_usuario[MAX_NOMBRE];
char sala_actual[MAX_NOMBRE] = "";


void *recibir(void *arg) {
    mensaje_t msg;
    ssize_t r;
    while (1) {
        r = msgrcv(msqid_priv, &msg, sizeof(msg) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv cliente");
            break;
        }
        if (msg.mtype == T_JOIN_ACK) {
            printf("[Servidor] %s\n", msg.texto);
        } else if (msg.mtype == T_SERVER_BCAST) {
            // Mostrar con formato: [Sala] Remitente: Texto Si msg.sala está vacío, solo muestra Remitente: Texto 
            if (msg.sala[0] != '\0') {
                printf("[%s] %s: %s\n", msg.sala, msg.remitente, msg.texto);
            } else {
                printf("%s: %s\n", msg.remitente, msg.texto);
            }
        } else {
            if (msg.remitente[0] != '\0') {
                if (msg.sala[0] != '\0')
                    printf("[%s] %s: %s\n", msg.sala, msg.remitente, msg.texto);
                else
                    printf("%s: %s\n", msg.remitente, msg.texto);
            } else {
                printf("%s\n", msg.texto);
            }
        }
    }
    return NULL;
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <nombre_usuario>\n", argv[0]);
        exit(1);
    }
    strncpy(nombre_usuario, argv[1], MAX_NOMBRE-1);

    // crear archivo temporal para ftok (para que la key sea única)
    pid_t pid = getpid();
    char fname[128];
    snprintf(fname, sizeof(fname), "/tmp/chat_%d.key", pid);
    int fd = open(fname, O_CREAT | O_RDWR, 0644);
    if (fd == -1) {
        perror("open keyfile");
        exit(1);
    }
    close(fd);
    key_priv = ftok(fname, 'C');
    if (key_priv == -1) {
        perror("ftok");
        unlink(fname);
        exit(1);
    }
    msqid_priv = msgget(key_priv, IPC_CREAT | 0666);
    if (msqid_priv == -1) {
        perror("msgget cola privada");
        unlink(fname);
        exit(1);
    }

    // conectar a la cola global
    key_t key_global = ftok("/tmp", 'A');
    if (key_global == -1) {
        perror("ftok /tmp");
        exit(1);
    }
    msqid_global = msgget(key_global, 0666);
    if (msqid_global == -1) {
        perror("Error: servidor no arrancado o cola global no existe");
        printf("Asegúrate de iniciar primero './servidor'\n");
        msgctl(msqid_priv, IPC_RMID, NULL);
        unlink(fname);
        exit(1);
    }

    printf("Bienvenido, %s. Usa '/join <sala>' para entrar a una sala.\n", nombre_usuario);

    // lanzar hilo receptor
    pthread_t thr;
    pthread_create(&thr, NULL, recibir, NULL);

    char linea[1024];
    while (1) {
        printf("> ");
        if (!fgets(linea, sizeof(linea), stdin)) break;
        linea[strcspn(linea, "\n")] = 0;
        if (strlen(linea) == 0) continue;

        if (linea[0] == '/') {
            if (strncmp(linea, "/join ", 6) == 0) {
                char sala[64];
                sscanf(linea + 6, "%63s", sala);
                mensaje_t m;
                m.mtype = T_JOIN;
                m.pid = getpid();
                m.client_key = key_priv;
                strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1);
                strncpy(m.sala, sala, MAX_NOMBRE-1);
                m.texto[0] = '\0';

                if (msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0) == -1) {
                    perror("msgsnd JOIN");
                } else {
                    // esperamos el JOIN_ACK en el hilo receptor
                    strncpy(sala_actual, sala, MAX_NOMBRE-1);
                }
            } 
            
            else if (strcmp(linea, "/list") == 0) {
                mensaje_t m; m.mtype = T_CMD_LIST; m.pid = getpid(); m.client_key = key_priv;
                strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1); m.texto[0] = '\0';
                msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0);
            } 
            
            else if (strncmp(linea, "/users ", 7) == 0) {
                char sala[64] = "";
                
                // Caso 1: el usuario escribió "/users sala"
                if (strlen(linea) > 7) {
                    sscanf(linea + 7, "%63s", sala);
                }

                // Caso 2: el usuario escribió "/users" -> usar la sala actual en la que se encuentra
                else if (strlen(sala_actual) > 0) {
                    strncpy(sala, sala_actual, sizeof(sala) - 1);
                }

                if (strlen(sala) == 0) {
                    printf("No estás en ninguna sala. Usa '/join <sala>' primero.\n");
                    continue;
                }

                mensaje_t m; 
                m.mtype = T_CMD_USERS; 
                m.pid = getpid(); 
                m.client_key = key_priv;
                strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1);
                strncpy(m.sala, sala, MAX_NOMBRE-1);

                if (msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0) == -1) {
                    perror("msgsnd /users");
                }
            } 
            
            else if (strcmp(linea, "/leave") == 0) {
                if (strlen(sala_actual) == 0) {
                    printf("No estás en ninguna sala.\n");
                    continue;
                }
                mensaje_t m; m.mtype = T_LEAVE; m.pid = getpid(); m.client_key = key_priv;
                strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1);
                strncpy(m.sala, sala_actual, MAX_NOMBRE-1);
                msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0);
                sala_actual[0] = '\0';
            }
            
            else if (strcmp(linea, "/quit") == 0) {
                if (strlen(sala_actual) > 0) {
                    mensaje_t m; m.mtype = T_LEAVE; m.pid = getpid(); m.client_key = key_priv;
                    strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1);
                    strncpy(m.sala, sala_actual, MAX_NOMBRE-1);
                    msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0);
                }
                msgctl(msqid_priv, IPC_RMID, NULL);
                unlink(fname);
                printf("Adiós.\n");
                exit(0);
            } else {
                printf("Comando desconocido. Usa /join /list /users /leave /quit\n");
            }
        } else {
            // enviar chat solo sí ya está en una sala
            if (strlen(sala_actual) == 0) {
                printf("No estás en ninguna sala. Usa '/join <sala>' primero.\n");
                continue;
            }
            mensaje_t m;
            m.mtype = T_CHAT; m.pid = getpid(); m.client_key = key_priv;
            strncpy(m.remitente, nombre_usuario, MAX_NOMBRE-1);
            strncpy(m.sala, sala_actual, MAX_NOMBRE-1);
            strncpy(m.texto, linea, MAX_TEXTO-1);
            if (msgsnd(msqid_global, &m, sizeof(m) - sizeof(long), 0) == -1) {
                perror("msgsnd CHAT");
            }
        }
    }

    msgctl(msqid_priv, IPC_RMID, NULL);
    unlink(fname);
    return 0;
}
