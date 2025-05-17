#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include "powerudp.h"

#define TCP_PORT 12345
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321
#define PSK "minha_chave_preconfigurada"
#define BUFFER_SIZE 1024

/* Lista simples de clientes registados (IP e porta TCP) */
typedef struct {
    struct sockaddr_in addr;
    time_t last_seen;
    int active;
} Client;

#define MAX_CLIENTS 10
static Client clients[MAX_CLIENTS];
static int client_count = 0;
static ConfigMessage current_config = {1,1,1,1000,5};
static int running = 1;
static int listen_fd = -1;

/* Protótipos de funções */
void handle_signal(int sig);
void cleanup_clients();
void handle_new_registration(int client_fd);
void multicast_config();

/* Handler para sinais de terminação */
void handle_signal(int sig) {
    printf("\nRecebido sinal %d, encerrando servidor...\n", sig);
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
}

/* Limpa registros de clientes inativos (não implementado completamente) */
void cleanup_clients() {
    time_t now = time(NULL);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].active && now - clients[i].last_seen > 300) { // 5 minutos
            printf("Removendo cliente inativo: %s:%d\n", 
                  inet_ntoa(clients[i].addr.sin_addr), 
                  ntohs(clients[i].addr.sin_port));
            clients[i].active = 0;
        }
    }
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
    
    // Verificar se é um pedido de registro ou de configuração
    if (bytes_read == sizeof(RegisterMessage)) {
        // Provavelmente um pedido de registro
        memcpy(&reg, buffer, sizeof(reg));
        if (strcmp(reg.psk, PSK) == 0 && client_count < MAX_CLIENTS) {
            // Guardar endereço do cliente
            struct sockaddr_in peer;
            socklen_t len = sizeof(peer);
            getpeername(client_fd, (struct sockaddr*)&peer, &len);
            
            // Verificar se o cliente já está registado
            int client_idx = -1;
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
                clients[client_count].addr = peer;
                clients[client_count].last_seen = time(NULL);
                clients[client_count].active = 1;
                client_count++;
                printf("Novo cliente registado: %s:%d\n", 
                      inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            } else {
                // Cliente existente, atualizar timestamp
                clients[client_idx].last_seen = time(NULL);
            }
            
            // Enviar ACK por TCP
            write(client_fd, "OK", 2);
        } else {
            if (strcmp(reg.psk, PSK) != 0) {
                printf("Registro rejeitado: PSK inválida\n");
            } else {
                printf("Registro rejeitado: número máximo de clientes atingido\n");
            }
            write(client_fd, "NO", 2);
        }
    } else if (bytes_read >= (1 + sizeof(ConfigMessage)) && buffer[0] == 'C') {
        // Pedido de configuração
        ConfigMessage new_config;
        memcpy(&new_config, buffer + 1, sizeof(ConfigMessage));
        
        printf("Recebido pedido de configuração:\n");
        printf("  Retransmissão: %s\n", new_config.enable_retransmission ? "Ativada" : "Desativada");
        printf("  Backoff: %s\n", new_config.enable_backoff ? "Ativado" : "Desativado");
        printf("  Sequência: %s\n", new_config.enable_sequence ? "Ativada" : "Desativada");
        printf("  Timeout base: %d ms\n", new_config.base_timeout);
        printf("  Retransmissões máximas: %d\n", new_config.max_retries);
        
        // Atualizar configuração atual
        memcpy(&current_config, &new_config, sizeof(ConfigMessage));
        
        // Enviar ACK
        write(client_fd, "OK", 2);
        
        // Enviar multicast logo de seguida
        printf("Enviando nova configuração para todos os clientes...\n");
        multicast_config();
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
    
    // Definir TTL para pacotes multicast
    unsigned char ttl = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        perror("setsockopt multicast TTL");
        close(sock);
        return;
    }
    
    struct sockaddr_in maddr = { .sin_family=AF_INET, .sin_port=htons(MCAST_PORT) };
    inet_pton(AF_INET, MCAST_ADDR, &maddr.sin_addr);
    
    if (sendto(sock, &current_config, sizeof(current_config), 0,
               (struct sockaddr*)&maddr, sizeof(maddr)) < 0) {
        perror("sendto multicast");
    } else {
        printf("Configuração enviada via multicast\n");
    }
    
    close(sock);
}

int main() {
    /* Configurar handlers de sinal */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    /* 1) TCP server para registos e pedidos de config */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    /* Permitir reutilização de endereço */
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }
    
    struct sockaddr_in serv = { .sin_family=AF_INET, .sin_port=htons(TCP_PORT), .sin_addr.s_addr=INADDR_ANY };
    if (bind(listen_fd, (void*)&serv, sizeof(serv)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    
    printf("Servidor PowerUDP iniciado!\n");
    printf("Configuração inicial:\n");
    printf("  Retransmissão: %s\n", current_config.enable_retransmission ? "Ativada" : "Desativada");
    printf("  Backoff: %s\n", current_config.enable_backoff ? "Ativado" : "Desativado");
    printf("  Sequência: %s\n", current_config.enable_sequence ? "Ativada" : "Desativada");
    printf("  Timeout base: %d ms\n", current_config.base_timeout);
    printf("  Retransmissões máximas: %d\n", current_config.max_retries);
    printf("Servidor TCP a escutar em %d...\n", TCP_PORT);
    
    /* 2) Loop principal */
    fd_set rfds;
    struct timeval tv;
    
    while (running) {
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        
        // Timeout para tratamento periódico
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        
        int ret = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (running) perror("select");
            break;
        } else if (ret == 0) {
            // Timeout: limpar clientes inativos periodicamente
            cleanup_clients();
            continue;
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
    
    /* 3) Limpeza final */
    if (listen_fd >= 0) close(listen_fd);
    printf("Servidor encerrado.\n");
    return 0;
}
