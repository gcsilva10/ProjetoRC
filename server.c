#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include "powerudp.h"

#define TCP_PORT 12345
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321
#define PSK "minha_chave_preconfigurada"
#define BUFFER_SIZE 1024
#define MAX_THREADS 50

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

// Mutex para proteger o acesso à lista de clientes
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Protótipos de funções */
void handle_signal(int sig);
void cleanup_clients();
void *handle_client_connection(void *arg);
void multicast_config();

/* Handler para sinais de terminação */
void handle_signal(int sig) {
    printf("\nRecebido sinal %d, encerrando servidor...\n", sig);
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
}

/* Limpa registros de clientes inativos */
void cleanup_clients() {
    pthread_mutex_lock(&clients_mutex);
    time_t now = time(NULL);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].active && now - clients[i].last_seen > 300) { // 5 minutos
            printf("Removendo cliente inativo: %s:%d\n", 
                  inet_ntoa(clients[i].addr.sin_addr), 
                  ntohs(clients[i].addr.sin_port));
            clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/* Thread para lidar com uma conexão de cliente */
void *handle_client_connection(void *arg) {
    int client_fd = *((int *)arg);
    free(arg);  // Liberar memória alocada para o argumento
    
    RegisterMessage reg;
    char buffer[BUFFER_SIZE];
    int bytes_read;
    
    printf("Nova thread iniciada para cliente (fd=%d)\n", client_fd);
    
    bytes_read = read(client_fd, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
        perror("read");
        close(client_fd);
        return NULL;
    }
    
    // Verificar se é um pedido de registro ou de configuração
    if (bytes_read == sizeof(RegisterMessage)) {
        pthread_mutex_lock(&clients_mutex);
        
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
            
            pthread_mutex_unlock(&clients_mutex);
            
            // Enviar ACK por TCP
            write(client_fd, "OK", 2);
        } else {
            pthread_mutex_unlock(&clients_mutex);
            
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
        
        printf("Recebido pedido de configuração:\n");
        printf("  Retransmissão: %s\n", new_config.enable_retransmission ? "Ativada" : "Desativada");
        printf("  Backoff: %s\n", new_config.enable_backoff ? "Ativado" : "Desativado");
        printf("  Sequência: %s\n", new_config.enable_sequence ? "Ativada" : "Desativada");
        printf("  Timeout base: %d ms\n", new_config.base_timeout);
        printf("  Retransmissões máximas: %d\n", new_config.max_retries);
        
        // Atualizar configuração atual
        pthread_mutex_lock(&clients_mutex);
        memcpy(&current_config, &new_config, sizeof(ConfigMessage));
        pthread_mutex_unlock(&clients_mutex);
        
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
    printf("Thread de cliente encerrada (fd=%d)\n", client_fd);
    return NULL;
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
    printf("Socket TCP criado com sucesso\n");
    
    /* Permitir reutilização de endereço */
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }
    printf("Opção SO_REUSEADDR configurada\n");
    
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(TCP_PORT);
    serv.sin_addr.s_addr = htonl(INADDR_ANY);
    
    printf("Tentando vincular a todas as interfaces (0.0.0.0:%d)...\n", TCP_PORT);
    if (bind(listen_fd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    printf("Socket TCP vinculado com sucesso\n");
    
    if (listen(listen_fd, MAX_THREADS) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    printf("Socket TCP escutando na porta %d em todas as interfaces\n", TCP_PORT);
    
    printf("Servidor PowerUDP iniciado!\n");
    printf("Configuração inicial:\n");
    printf("  Retransmissão: %s\n", current_config.enable_retransmission ? "Ativada" : "Desativada");
    printf("  Backoff: %s\n", current_config.enable_backoff ? "Ativado" : "Desativado");
    printf("  Sequência: %s\n", current_config.enable_sequence ? "Ativada" : "Desativada");
    printf("  Timeout base: %d ms\n", current_config.base_timeout);
    printf("  Retransmissões máximas: %d\n", current_config.max_retries);
    
    /* 2) Loop principal com threads */
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Aceitar nova conexão
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (*client_fd < 0) {
            if (running) {
                perror("accept");
            }
            free(client_fd);
            continue;
        }
        
        printf("Nova conexão de %s:%d\n", 
              inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Criar thread para lidar com o cliente
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client_connection, client_fd) != 0) {
            fprintf(stderr, "Erro ao criar thread para cliente: %s\n", strerror(errno));
            close(*client_fd);
            free(client_fd);
            continue;
        }
        
        // Desanexar a thread para que ela seja liberada automaticamente
        pthread_detach(thread_id);
        
        // A cada 5 conexões, limpar clientes inativos
        static int conn_count = 0;
        if (++conn_count % 5 == 0) {
            cleanup_clients();
        }
    }
    
    /* 3) Limpeza final */
    pthread_mutex_destroy(&clients_mutex);
    if (listen_fd >= 0) close(listen_fd);
    printf("Servidor encerrado.\n");
    return 0;
}
