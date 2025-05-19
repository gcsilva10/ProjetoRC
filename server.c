#define _DEFAULT_SOURCE  // Para usar usleep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "powerudp.h"

#define TCP_PORT 12345
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321
#define PSK "minha_chave_preconfigurada"
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10

typedef struct {
    struct sockaddr_in addr;
    time_t last_seen;
    int active;
    pid_t handler_pid;
} Client;

static Client clients[MAX_CLIENTS];
static int client_count = 0;
static ConfigMessage current_config = {1,1,1,1000,5};
static int running = 1;
static int listen_fd = -1;

void multicast_config(void);
void handle_new_registration(int client_fd);
void handle_client(int client_idx);

void handle_child_signal(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < client_count; i++) {
            if (clients[i].handler_pid == pid) {
                clients[i].active = 0;
                clients[i].handler_pid = 0;
                printf("Cliente %s:%d desconectado (PID %d)\n",
                       inet_ntoa(clients[i].addr.sin_addr),
                       ntohs(clients[i].addr.sin_port),
                       pid);
                break;
            }
        }
    }
}

void handle_signal(int sig) {
    (void)sig;
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].handler_pid > 0) {
            kill(clients[i].handler_pid, SIGTERM);
        }
    }
}

void handle_client(int client_idx) {
    Client *client = &clients[client_idx];
    close(listen_fd);
    printf("Tratar cliente %s:%d (PID %d)\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port),
           getpid());
    while (running) {
        sleep(1);
    }
    exit(0);
}

void handle_new_registration(int client_fd) {
    char buffer[BUFFER_SIZE];
    int bytes_read = read(client_fd, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
        perror("read");
        close(client_fd);
        return;
    }

    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    getpeername(client_fd, (struct sockaddr*)&peer, &len);

    if (bytes_read == (int)sizeof(RegisterMessage)) {
        RegisterMessage reg;
        memcpy(&reg, buffer, sizeof(reg));
        if (strcmp(reg.psk, PSK)==0 && client_count<MAX_CLIENTS) {
            int idx = -1;
            for (int i=0;i<client_count;i++){
                if (clients[i].addr.sin_addr.s_addr==peer.sin_addr.s_addr){
                    idx=i;
                    if(!clients[i].active){
                        clients[i].active=1;
                        printf("Reativado: %s:%d\n",
                               inet_ntoa(peer.sin_addr),
                               ntohs(peer.sin_port));
                    }
                    break;
                }
            }
            if(idx<0){
                idx = client_count++;
                clients[idx].addr=peer;
                clients[idx].active=1;
                printf("Novo registo: %s:%d\n",
                       inet_ntoa(peer.sin_addr),
                       ntohs(peer.sin_port));
            }
            clients[idx].last_seen=time(NULL);
            pid_t pid = fork();
            if(pid==0){
                close(client_fd);
                handle_client(idx);
            } else if(pid>0){
                clients[idx].handler_pid=pid;
                printf("Fork pid=%d para cliente %d\n", pid, idx);
            } else perror("fork");
            write(client_fd,"OK",2);
        } else {
            write(client_fd, strcmp(((RegisterMessage*)buffer)->psk,PSK)? "NO":"NO",2);
        }

    } else if(bytes_read>=(1+(int)sizeof(ConfigMessage)) && buffer[0]=='C'){
        ConfigMessage cfg;
        memcpy(&cfg, buffer+1, sizeof(cfg));
        if(cfg.base_timeout<100) cfg.base_timeout=100;
        if(cfg.max_retries<1)    cfg.max_retries=1;
        printf("Nova config solicitada por %s:%d\n",
               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        memcpy(&current_config, &cfg, sizeof(cfg));
        write(client_fd,"OK",2);
        multicast_config();

    } else {
        write(client_fd,"ER",2);
    }
    close(client_fd);
}

void multicast_config() {
    int mcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(mcast_sock<0){ perror("mcast sock"); return;}
    unsigned char ttl=32;
    setsockopt(mcast_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in maddr = {
        .sin_family=AF_INET,
        .sin_port=htons(MCAST_PORT)
    };
    inet_pton(AF_INET, MCAST_ADDR, &maddr.sin_addr);

    for(int i=0;i<3;i++){
        sendto(mcast_sock, &current_config, sizeof(current_config),0,
               (struct sockaddr*)&maddr,sizeof(maddr));
        usleep(100000);
    }
    close(mcast_sock);
}

int main(){
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, handle_child_signal);

    listen_fd=socket(AF_INET,SOCK_STREAM,0);
    int reuse=1;
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in serv={ .sin_family=AF_INET,
        .sin_port=htons(TCP_PORT),
        .sin_addr.s_addr=htonl(INADDR_ANY)};
    bind(listen_fd,(struct sockaddr*)&serv,sizeof(serv));
    listen(listen_fd,5);

    printf("Servidor na porta %d\n", TCP_PORT);
    while(running){
        fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_fd,&rfds);
        struct timeval tv={5,0};
        if(select(listen_fd+1,&rfds,NULL,NULL,&tv)>0 &&
           FD_ISSET(listen_fd,&rfds)){
            struct sockaddr_in cli; socklen_t clilen=sizeof(cli);
            int fd=accept(listen_fd,(struct sockaddr*)&cli,&clilen);
            handle_new_registration(fd);
        }
    }
    close(listen_fd);
    while(wait(NULL)>0);
    printf("Servidor encerrado\n");
    return 0;
}
