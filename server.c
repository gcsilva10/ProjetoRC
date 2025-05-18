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

/* Lista simples de clientes registados (IP e porta TCP) */
typedef struct {
    struct sockaddr_in addr;
    time_t last_seen;
    int active;
    pid_t handler_pid;  // PID do processo que trata este cliente
} Client;

static Client clients[MAX_CLIENTS];
static int client_count = 0;
static ConfigMessage current_config = {1,1,1,1000,5};
static int running = 1;
static int listen_fd = -1;

/* Protótipos de funções */
void multicast_config(void);
void handle_new_registration(int client_fd);
void handle_client(int client_idx);

// Função para tratar sinais de filhos terminados
void handle_child_signal(int sig) {
    (void)sig;  // Marcar parâmetro como usado intencionalmente
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Processo filho %d terminou\n", pid);
        // Atualizar o estado do cliente correspondente
        for (int i = 0; i < client_count; i++) {
            if (clients[i].handler_pid == pid) {
                clients[i].active = 0;
                clients[i].handler_pid = 0;
                printf("Cliente %s:%d desconectado\n", 
                       inet_ntoa(clients[i].addr.sin_addr),
                       ntohs(clients[i].addr.sin_port));
                break;
            }
        }
    }
}

/* Handler para sinais de terminação */
void handle_signal(int sig) {
    printf("\nRecebido sinal %d, encerrando servidor...\n", sig);
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
    
    // Terminar todos os processos filhos
    for (int i = 0; i < client_count; i++) {
        if (clients[i].handler_pid > 0) {
            kill(clients[i].handler_pid, SIGTERM);
        }
    }
}

/* Função para tratar um cliente específico */
void handle_client(int client_idx) {
    Client *client = &clients[client_idx];
    printf("Iniciando tratamento do cliente %s:%d\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));
    
    // Loop principal do tratamento do cliente
    while (running) {
        // Aqui você pode adicionar a lógica específica para tratar
        // as mensagens deste cliente
        sleep(1);  // Evitar consumo excessivo de CPU
    }
    
    printf("Encerrando tratamento do cliente %s:%d\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));
    exit(0);
}

void handle_new_registration(int client_fd) {
    RegisterMessage reg;
    char buffer[BUFFER_SIZE];
    int bytes_read;
    
    bytes_read = read(client_fd, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
        perror("read");
        close(client_fd);
        return;
    }
    
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    getpeername(client_fd, (struct sockaddr*)&peer, &len);
    
    if (bytes_read == sizeof(RegisterMessage)) {
        memcpy(&reg, buffer, sizeof(reg));
        if (strcmp(reg.psk, PSK) == 0 && client_count < MAX_CLIENTS) {
            int client_idx = -1;
            
            // Verificar se o cliente já está registado
            for (int i = 0; i < client_count; i++) {
                if (clients[i].addr.sin_addr.s_addr == peer.sin_addr.s_addr) {
                    client_idx = i;
                    if (!clients[i].active) {
                        clients[i].active = 1;
                        printf("Cliente reativado: %s:%d\n",
                               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                    }
                    break;
                }
            }
            
            if (client_idx == -1) {
                // Novo cliente
                client_idx = client_count++;
                clients[client_idx].addr = peer;
                clients[client_idx].active = 1;
                printf("Novo cliente registado: %s:%d\n",
                       inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            }
            
            clients[client_idx].last_seen = time(NULL);
            
            // Criar processo filho para tratar este cliente
            pid_t pid = fork();
            if (pid == 0) {
                // Processo filho
                close(listen_fd);  // Filho não precisa do socket de escuta
                handle_client(client_idx);
                exit(0);
            } else if (pid > 0) {
                // Processo pai
                clients[client_idx].handler_pid = pid;
                printf("Criado processo %d para tratar cliente %d\n", pid, client_idx);
            } else {
                perror("fork");
            }
            
            write(client_fd, "OK", 2);
            
        } else {
            if (strcmp(reg.psk, PSK) != 0) {
                printf("Registro rejeitado: PSK inválida\n");
            } else {
                printf("Registro rejeitado: número máximo de clientes atingido\n");
            }
            write(client_fd, "NO", 2);
        }
    } else if ((size_t)bytes_read >= (1 + sizeof(ConfigMessage)) && buffer[0] == 'C') {
        // Pedido de configuração
        ConfigMessage new_config;
        memcpy(&new_config, buffer + 1, sizeof(ConfigMessage));
        
        // Validar valores da configuração
        if (new_config.base_timeout < 100) {
            new_config.base_timeout = 100;  // mínimo 100ms
        }
        if (new_config.max_retries < 1) {
            new_config.max_retries = 1;     // mínimo 1 tentativa
        }
        
        printf("\n[DEBUG] Servidor - Nova configuração recebida:\n");
        printf("  - Retransmissão: %s\n", new_config.enable_retransmission ? "Ativada" : "Desativada");
        printf("  - Backoff: %s\n", new_config.enable_backoff ? "Ativado" : "Desativado");
        printf("  - Sequência: %s\n", new_config.enable_sequence ? "Ativada" : "Desativada");
        printf("  - Timeout base: %d ms\n", new_config.base_timeout);
        printf("  - Retransmissões máximas: %d\n\n", new_config.max_retries);
        
        // Atualizar configuração atual
        memcpy(&current_config, &new_config, sizeof(ConfigMessage));
        
        // Enviar ACK
        if (write(client_fd, "OK", 2) != 2) {
            perror("write config response");
            write(client_fd, "ER", 2);
        } else {
            // Enviar multicast logo de seguida
            printf("Enviando nova configuração para todos os clientes...\n");
            multicast_config();
        }
    } else {
        printf("Recebido pedido desconhecido (%d bytes)\n", bytes_read);
        write(client_fd, "ER", 2);
    }
    
    close(client_fd);
}

void multicast_config() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket multicast");
        return;
    }
    
    // Definir TTL para pacotes multicast (aumentado para alcançar mais hops)
    unsigned char ttl = 32;  // Valor aumentado para passar por vários routers
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        perror("setsockopt multicast TTL");
        close(sock);
        return;
    }
    
    printf("\n[DEBUG] Servidor - Configuração atual:\n");
    printf("  - Retransmissão: %s\n", current_config.enable_retransmission ? "Ativada" : "Desativada");
    printf("  - Backoff: %s\n", current_config.enable_backoff ? "Ativado" : "Desativado");
    printf("  - Sequência: %s\n", current_config.enable_sequence ? "Ativada" : "Desativada");
    printf("  - Timeout base: %d ms\n", current_config.base_timeout);
    printf("  - Retransmissões máximas: %d\n", current_config.max_retries);
    printf("  - TTL multicast: %d\n\n", ttl);
    
    // Validar configuração antes de enviar
    if (current_config.base_timeout < 100) {
        current_config.base_timeout = 100;  // mínimo 100ms
    }
    if (current_config.max_retries < 1) {
        current_config.max_retries = 1;     // mínimo 1 tentativa
    }
    
    struct sockaddr_in maddr = { 
        .sin_family = AF_INET, 
        .sin_port = htons(MCAST_PORT),
        .sin_addr = {0}
    };
    
    if (inet_pton(AF_INET, MCAST_ADDR, &maddr.sin_addr) <= 0) {
        perror("inet_pton multicast");
        close(sock);
        return;
    }
    
    // Tentar enviar algumas vezes para garantir que os clientes recebam
    for (int i = 0; i < 3; i++) {
        if (sendto(sock, &current_config, sizeof(current_config), 0,
                   (struct sockaddr*)&maddr, sizeof(maddr)) < 0) {
            perror("sendto multicast");
            break;
        } else {
            printf("Configuração enviada via multicast (tentativa %d)\n", i + 1);
            if (i < 2) {
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = 100000000  // 100ms
                };
                nanosleep(&ts, NULL);
            }
        }
    }
    
    close(sock);
}

int main() {
    /* Configurar handlers de sinal */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, handle_child_signal);
    
    /* Socket TCP para registos e pedidos de config */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }
    
    struct sockaddr_in serv = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    if (bind(listen_fd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    
    printf("Servidor PowerUDP iniciado na porta %d!\n", TCP_PORT);
    printf("Configuração inicial:\n");
    printf("  Retransmissão: %s\n", current_config.enable_retransmission ? "Ativada" : "Desativada");
    printf("  Backoff: %s\n", current_config.enable_backoff ? "Ativado" : "Desativado");
    printf("  Sequência: %s\n", current_config.enable_sequence ? "Ativada" : "Desativada");
    printf("  Timeout base: %d ms\n", current_config.base_timeout);
    printf("  Retransmissões máximas: %d\n", current_config.max_retries);
    
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        
        struct timeval tv = {5, 0};  // 5 segundos de timeout
        
        int ret = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (running) perror("select");
            break;
        }
        
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                if (running) perror("accept");
                continue;
            }
            
            printf("Nova conexão de %s:%d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            handle_new_registration(client_fd);
        }
    }
    
    /* Limpeza final */
    if (listen_fd >= 0) close(listen_fd);
    
    // Aguardar todos os processos filhos terminarem
    while (wait(NULL) > 0);
    
    printf("Servidor encerrado.\n");
    return 0;
}
