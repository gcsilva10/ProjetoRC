#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/select.h>
#include <time.h>
#include <pthread.h>
#include "powerudp.h"

// Em vez de usar uma porta fixa, torna-a configurável
// #define DEFAULT_UDP_PORT 10000
#define TCP_TIMEOUT_SEC 5
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321

static int udp_sock = -1;
static int tcp_sock = -1;
static struct sockaddr_in server_tcp_addr;
static ConfigMessage current_config = {1, 1, 1, 1000, 5}; // Configuração padrão
static int last_retransmissions = 0;
static int last_delivery_time  = 0;  // em ms
static int loss_probability = 0;      // 0–100%
static uint32_t send_seq_num = 0;     // Número de sequência para envio
static uint32_t recv_seq_num = 0;     // Número de sequência esperado na recepção
static int local_udp_port = 0;        // Armazena a porta UDP local usada

// Helper: calcula se deve "perder" o pacote
static int should_drop_packet() {
    if (loss_probability <= 0) return 0;
    return (rand() % 100) < loss_probability;
}

// Thread para receber atualizações multicast em background
static void *multicast_listener(void *arg) {
    (void)arg;  // Explicitly acknowledge unused parameter
    struct sockaddr_in maddr;
    socklen_t addrlen = sizeof(maddr);
    ConfigMessage config;
    
    while (1) {
        int received = recvfrom(udp_sock, &config, sizeof(config), 0, 
                               (struct sockaddr*)&maddr, &addrlen);
        if (received == sizeof(ConfigMessage)) {
            memcpy(&current_config, &config, sizeof(ConfigMessage));
            printf("Nova configuração recebida via multicast:\n");
            printf("  Retransmissão: %s\n", config.enable_retransmission ? "Ativada" : "Desativada");
            printf("  Backoff: %s\n", config.enable_backoff ? "Ativado" : "Desativado");
            printf("  Sequência: %s\n", config.enable_sequence ? "Ativada" : "Desativada");
            printf("  Timeout base: %d ms\n", config.base_timeout);
            printf("  Retransmissões máximas: %d\n", config.max_retries);
        }
    }
    return NULL;
}

// Inicializa e regista no servidor via TCP
// Adicionar parâmetro para porta UDP local
int init_protocol(const char *server_ip, int server_port, const char *psk) {
    return init_protocol_with_port(server_ip, server_port, psk, 0); // 0 = porta aleatória
}

// Nova função que permite especificar a porta UDP
int init_protocol_with_port(const char *server_ip, int server_port, const char *psk, int udp_port) {
    srand(time(NULL));
    local_udp_port = udp_port; // Guarda a porta local para uso posterior
    
    // 1) TCP
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&server_tcp_addr, 0, sizeof(server_tcp_addr));
    server_tcp_addr.sin_family = AF_INET;
    server_tcp_addr.sin_port   = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_tcp_addr.sin_addr);

    struct timeval tv = { .tv_sec = TCP_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(tcp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(tcp_sock, (struct sockaddr*)&server_tcp_addr, sizeof(server_tcp_addr)) < 0) {
        perror("connect TCP");
        return -1;
    }
    // envia RegisterMessage
    RegisterMessage reg;
    strncpy(reg.psk, psk, PSK_LEN);
    if (write(tcp_sock, &reg, sizeof(reg)) != sizeof(reg)) {
        perror("write reg");
        close(tcp_sock);
        return -1;
    }
    char resp[3] = {0};
    if (read(tcp_sock, resp, 2) != 2 || strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "Registro falhou: %s\n", resp);
        close(tcp_sock);
        return -1;
    }
    close(tcp_sock);

    // 2) UDP unicast e multicast (mesmo socket)
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket UDP");
        return -1;
    }
    // bind a porta especificada ou aleatória (se port=0)
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(local_udp_port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(udp_sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind UDP");
        close(udp_sock);
        return -1;
    }
    
    // Obter a porta atribuída se usamos porta 0 (aleatória)
    if (local_udp_port == 0) {
        struct sockaddr_in actual_local;
        socklen_t addr_len = sizeof(actual_local);
        if (getsockname(udp_sock, (struct sockaddr*)&actual_local, &addr_len) == 0) {
            local_udp_port = ntohs(actual_local.sin_port);
        }
    }
    
    printf("Cliente inicializado com porta UDP local: %d\n", local_udp_port);
    
    // Permitir reutilização de endereço
    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // junta ao multicast de config
    struct ip_mreq mreq;
    inet_pton(AF_INET, MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("join multicast group");
        // Não falhar por causa disto, apenas logar o erro
    }
    
    // Iniciar thread para escutar multicast
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, multicast_listener, NULL) != 0) {
        perror("create multicast thread");
        // Não falhar por causa disto, apenas logar o erro
    } else {
        pthread_detach(thread_id);
    }

    printf("Protocolo PowerUDP inicializado com sucesso\n");
    return 0;
}

// Fecha sockets
void close_protocol() {
    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }
    printf("Protocolo PowerUDP finalizado\n");
}

// Pedido de alteração de config via TCP
int request_protocol_config(int enable_retransmission,
                            int enable_backoff,
                            int enable_sequence,
                            uint16_t base_timeout,
                            uint8_t max_retries) {
    // Reabre TCP só para config
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(tcp_sock, (struct sockaddr*)&server_tcp_addr, sizeof(server_tcp_addr)) < 0) {
        perror("connect TCP config");
        return -1;
    }
    ConfigMessage req = {
        .enable_retransmission = (uint8_t)enable_retransmission,
        .enable_backoff        = (uint8_t)enable_backoff,
        .enable_sequence       = (uint8_t)enable_sequence,
        .base_timeout          = base_timeout,
        .max_retries           = max_retries
    };
    
    // Enviar comando específico para identificar que é um pedido de configuração
    char cmd = 'C';
    write(tcp_sock, &cmd, 1);
    
    if (write(tcp_sock, &req, sizeof(req)) != sizeof(req)) {
        perror("write config req");
        close(tcp_sock);
        return -1;
    }
    
    char resp[3] = {0};
    if (read(tcp_sock, resp, 2) != 2 || strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "Pedido de configuração falhou: %s\n", resp);
        close(tcp_sock);
        return -1;
    }
    
    close(tcp_sock);
    printf("Pedido de configuração enviado com sucesso\n");
    return 0;
}

// Envia mensagem UDP unicast com PowerUDP
int send_message(const char *destination, const char *message, int len) {
    if (len > MAX_UDP_DATA) {
        fprintf(stderr, "Mensagem muito grande para PowerUDP (max: %d)\n", MAX_UDP_DATA);
        return -1;
    }
    
    // Use a porta do cliente de destino ou a porta padrão
    int dest_port = local_udp_port; // Por padrão, assume mesma porta
    
    // Verifica se temos porta no formato IP:PORTA
    char dest_ip[64] = {0};
    strncpy(dest_ip, destination, sizeof(dest_ip)-1);
    char *port_sep = strchr(dest_ip, ':');
    if (port_sep != NULL) {
        *port_sep = '\0';
        dest_port = atoi(port_sep + 1);
    }
    
    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(dest_port) };
    inet_pton(AF_INET, dest_ip, &dest.sin_addr);

    printf("Enviando para %s:%d\n", dest_ip, dest_port);
    
    PowerUDPMessage pudp_msg;
    pudp_msg.header.type = PUDP_DATA;
    pudp_msg.header.seq_num = send_seq_num;
    pudp_msg.header.data_len = len;
    memcpy(pudp_msg.data, message, len);
    
    int total_size = sizeof(PowerUDPHeader) + len;
    last_retransmissions = 0;
    clock_t start = clock();

    int attempts = 0;
    int max = current_config.max_retries;
    int tbase = current_config.base_timeout;
    int success = 0;

    while (!success && attempts <= max) {
        // Enviar pacote (se não estiver injetando perda)
        if (!should_drop_packet()) {
            sendto(udp_sock, &pudp_msg, total_size, 0, 
                   (struct sockaddr*)&dest, sizeof(dest));
            printf("Enviado pacote seq=%u para %s (tentativa %d)\n", 
                   pudp_msg.header.seq_num, destination, attempts + 1);
        } else {
            printf("Simulando perda do pacote seq=%u (tentativa %d)\n", 
                   pudp_msg.header.seq_num, attempts + 1);
        }
        
        // Calcular timeout (com backoff exponencial se ativado)
        int timeout_ms = tbase;
        if (current_config.enable_backoff && attempts > 0) {
            timeout_ms = tbase * (1 << attempts);
        }
        
        // Aguardar ACK com timeout
        struct timeval tv = { 
            .tv_sec = timeout_ms / 1000, 
            .tv_usec = (timeout_ms % 1000) * 1000 
        };
        
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_sock, &rfds);
        
        int rv = select(udp_sock + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0) {
            // Recebeu alguma resposta
            PowerUDPMessage resp;
            struct sockaddr_in resp_addr;
            socklen_t resp_len = sizeof(resp_addr);
            
            int received = recvfrom(udp_sock, &resp, sizeof(resp), 0,
                                   (struct sockaddr*)&resp_addr, &resp_len);
            
            if ((size_t)received >= sizeof(PowerUDPHeader)) {
                if (resp.header.seq_num == pudp_msg.header.seq_num) {
                    if (resp.header.type == PUDP_ACK) {
                        printf("Recebido ACK para seq=%u\n", resp.header.seq_num);
                        success = 1;
                        // Incrementar seq_num para próxima mensagem
                        if (current_config.enable_sequence) {
                            send_seq_num++;
                        }
                        break;
                    } else if (resp.header.type == PUDP_NACK) {
                        printf("Recebido NACK para seq=%u, retransmitindo\n", 
                               resp.header.seq_num);
                        // Aqui consideramos NACK como tentativa falha,
                        // mas não incrementamos seq_num
                    }
                } else {
                    // Resposta para outro pacote, ignorar
                    printf("Recebida resposta para seq=%u, ignorando\n", 
                           resp.header.seq_num);
                }
            }
        }
        
        // Se não tiver recebido resposta válida ou timeout
        if (!success) {
            if (!current_config.enable_retransmission) {
                // Se retransmissão desativada, consideramos falha
                printf("Retransmissão desativada, desistindo após tentativa %d\n", 
                       attempts + 1);
                break;
            }
            
            attempts++;
            last_retransmissions++;
        }
    }

    last_delivery_time = (int)((clock() - start) * 1000 / CLOCKS_PER_SEC);
    
    if (success) {
        return len;
    } else {
        return -1; // falhou após máximo de tentativas
    }
}

// Recebe mensagem UDP (blocking)
int receive_message(char *buffer, int bufsize) {
    PowerUDPMessage pudp_msg;
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    
    int received = recvfrom(udp_sock, &pudp_msg, sizeof(pudp_msg), 0,
                           (struct sockaddr*)&src, &src_len);
    
    if ((size_t)received < sizeof(PowerUDPHeader)) {
        return -1;  // Mensagem inválida
    }
    
    // Verificar se é uma mensagem de dados
    if (pudp_msg.header.type != PUDP_DATA) {
        return -1;  // Não é uma mensagem de dados
    }
    
    // Verificar sequência se ativado
    if (current_config.enable_sequence) {
        if (pudp_msg.header.seq_num != recv_seq_num) {
            printf("Sequência errada: recebido=%u, esperado=%u\n",
                   pudp_msg.header.seq_num, recv_seq_num);
            
            // Enviar NACK se fora de ordem
            PowerUDPMessage nack;
            nack.header.type = PUDP_NACK;
            nack.header.seq_num = pudp_msg.header.seq_num;
            nack.header.data_len = 0;
            
            sendto(udp_sock, &nack, sizeof(PowerUDPHeader), 0,
                   (struct sockaddr*)&src, src_len);
            return -1;
        }
        
        // Incrementar seq_num esperado para próximo pacote
        recv_seq_num++;
    }
    
    // Verificar tamanho dos dados
    int data_len = pudp_msg.header.data_len;
    if (data_len > bufsize) {
        data_len = bufsize;  // Truncar se buffer menor
    }
    
    // Copiar dados para buffer do usuário
    memcpy(buffer, pudp_msg.data, data_len);
    
    // Enviar ACK
    PowerUDPMessage ack;
    ack.header.type = PUDP_ACK;
    ack.header.seq_num = pudp_msg.header.seq_num;
    ack.header.data_len = 0;
    
    sendto(udp_sock, &ack, sizeof(PowerUDPHeader), 0,
           (struct sockaddr*)&src, src_len);
    
    printf("Recebido pacote seq=%u de %s, enviado ACK\n",
           pudp_msg.header.seq_num, inet_ntoa(src.sin_addr));
    
    return data_len;
}

// Estatísticas da última mensagem
int get_last_message_stats(int *retransmissions, int *delivery_time) {
    if (!retransmissions || !delivery_time) return -1;
    *retransmissions = last_retransmissions;
    *delivery_time  = last_delivery_time;
    return 0;
}

// Define a percentagem de perda simulada
void inject_packet_loss(int probability) {
    if (probability < 0) probability = 0;
    if (probability > 100) probability = 100;
    loss_probability = probability;
    printf("Injeção de perda definida para %d%%\n", probability);
}
