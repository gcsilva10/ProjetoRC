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
#include <errno.h>
#include "powerudp.h"

#define TCP_TIMEOUT_SEC 2  // Reduzido de 5 para 2 segundos
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321

static int udp_sock = -1;
static int tcp_sock = -1;
static struct sockaddr_in server_tcp_addr;
static ConfigMessage current_config = {1, 1, 1, 1000, 5};
static int last_retransmissions = 0;
static int last_delivery_time  = 0;  // em ms
static int loss_probability = 0;      // 0–100%
static uint32_t send_seq_num = 0;     // Número de sequência para envio
static uint32_t recv_seq_num = 0;     // Número de sequência esperado na recepção
static int local_udp_port = 0;        // Porta UDP local usada

// Helper: calcula se deve "perder" o pacote
static int should_drop_packet() {
    if (loss_probability <= 0) return 0;
    return (rand() % 100) < loss_probability;
}

// Thread para receber atualizações multicast em background
static void *multicast_listener(void *arg) {
    (void)arg;
    struct sockaddr_in maddr;
    socklen_t addrlen = sizeof(maddr);
    ConfigMessage config;

    while (1) {
        int received = recvfrom(udp_sock, &config, sizeof(config), 0,
                               (struct sockaddr*)&maddr, &addrlen);
        if (received == sizeof(ConfigMessage)) {
            if (config.base_timeout == 0) config.base_timeout = 1000;
            if (config.max_retries == 0)  config.max_retries = 5;
            memcpy(&current_config, &config, sizeof(ConfigMessage));
            printf("Nova configuração recebida via multicast:\n");
            printf("  Retransmissão: %s\n", config.enable_retransmission ? "Ativada" : "Desativada");
            printf("  Backoff: %s\n",        config.enable_backoff        ? "Ativado"  : "Desativado");
            printf("  Sequência: %s\n",      config.enable_sequence       ? "Ativada"  : "Desativada");
            printf("  Timeout base: %d ms\n", config.base_timeout);
            printf("  Retransmissões máximas: %d\n", config.max_retries);
        }
    }
    return NULL;
}

int init_protocol_with_port(const char *server_ip, int server_port, const char *psk, int udp_port) {
    srand(time(NULL));
    local_udp_port = udp_port;

    // TCP
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("socket TCP"); return -1; }
    memset(&server_tcp_addr, 0, sizeof(server_tcp_addr));
    server_tcp_addr.sin_family = AF_INET;
    server_tcp_addr.sin_port   = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_tcp_addr.sin_addr) <= 0) {
        perror("inet_pton falhou"); close(tcp_sock); return -1;
    }
    struct timeval tv = { .tv_sec = TCP_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(tcp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(tcp_sock, (struct sockaddr*)&server_tcp_addr, sizeof(server_tcp_addr)) < 0) {
        perror("connect TCP"); close(tcp_sock); return -1;
    }

    // Registro
    RegisterMessage reg;
    strncpy(reg.psk, psk, PSK_LEN);
    if (write(tcp_sock, &reg, sizeof(reg)) != sizeof(reg)) {
        perror("write reg"); close(tcp_sock); return -1;
    }
    char resp[3] = {0};
    if (read(tcp_sock, resp, 2) != 2 || strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "Registro falhou: %s\n", resp);
        close(tcp_sock); return -1;
    }
    close(tcp_sock);

    // UDP + Multicast
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("socket UDP"); return -1; }
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(local_udp_port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(udp_sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind UDP"); close(udp_sock); return -1;
    }
    if (local_udp_port == 0) {
        struct sockaddr_in actual;
        socklen_t alen = sizeof(actual);
        if (getsockname(udp_sock, (struct sockaddr*)&actual, &alen) == 0)
            local_udp_port = ntohs(actual.sin_port);
    }
    printf("Cliente UDP local: %d\n", local_udp_port);

    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct ip_mreq mreq;
    inet_pton(AF_INET, MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    pthread_t tid;
    if (pthread_create(&tid, NULL, multicast_listener, NULL) == 0)
        pthread_detach(tid);

    printf("Protocolo PowerUDP inicializado\n");
    return 0;
}

int init_protocol(const char *server_ip, int server_port, const char *psk) {
    return init_protocol_with_port(server_ip, server_port, psk, 0);
}

void close_protocol() {
    if (udp_sock >= 0) close(udp_sock);
    printf("Protocolo PowerUDP finalizado\n");
}

int request_protocol_config(int enable_retransmission,
                            int enable_backoff,
                            int enable_sequence,
                            uint16_t base_timeout,
                            uint8_t max_retries) {
    // Reabre TCP só para config
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("socket TCP cfg"); return -1; }
    if (connect(tcp_sock, (struct sockaddr*)&server_tcp_addr, sizeof(server_tcp_addr)) < 0) {
        perror("connect TCP cfg"); close(tcp_sock); return -1;
    }

    ConfigMessage req = {
        .enable_retransmission = (uint8_t)enable_retransmission,
        .enable_backoff        = (uint8_t)enable_backoff,
        .enable_sequence       = (uint8_t)enable_sequence,
        .base_timeout          = base_timeout,
        .max_retries           = max_retries
    };

    // Envio único
    uint8_t buf[1 + sizeof(ConfigMessage)];
    buf[0] = 'C';
    memcpy(buf + 1, &req, sizeof(req));
    ssize_t total = 1 + sizeof(req);
    if (write(tcp_sock, buf, total) != total) {
        perror("write config request"); close(tcp_sock); return -1;
    }

    char resp[3] = {0};
    if (read(tcp_sock, resp, 2) != 2 || strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "Pedido de configuração falhou: %s\n", resp);
        close(tcp_sock); return -1;
    }
    close(tcp_sock);
    printf("Pedido de configuração enviado com sucesso\n");
    return 0;
}

int send_message(const char *destination, const char *message, int len) {
    if (len > MAX_UDP_DATA) {
        fprintf(stderr, "Mensagem muito grande (max %d)\n", MAX_UDP_DATA);
        return -1;
    }

    char dest_ip[64];
    int dest_port = local_udp_port;
    strncpy(dest_ip, destination, sizeof(dest_ip)-1);
    char *sep = strchr(dest_ip, ':');
    if (sep) {
        *sep = '\0';
        dest_port = atoi(sep + 1);
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(dest_port),
        .sin_addr   = {0}
    };
    if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) <= 0) {
        fprintf(stderr, "IP inválido: %s\n", dest_ip);
        return -1;
    }

    PowerUDPMessage pudp = { .header = {PUDP_DATA, send_seq_num, (uint16_t)len} };
    memcpy(pudp.data, message, len);

    int total_size = sizeof(PowerUDPHeader) + len;
    last_retransmissions = 0;
    clock_t start = clock();

    int attempts = 0, max = current_config.max_retries;
    int tbase = current_config.base_timeout;
    int success = 0;

    while (!success && attempts <= max) {
        if (!should_drop_packet()) {
            sendto(udp_sock, &pudp, total_size, 0, (struct sockaddr*)&dest, sizeof(dest));
        } else {
            printf("Perda simulada seq=%u\n", pudp.header.seq_num);
        }

        int timeout_ms = tbase;
        if (current_config.enable_backoff && attempts > 0) {
            int exp = attempts < 3 ? attempts : 3;
            timeout_ms = tbase * (1 << exp);
            if (timeout_ms > 8000) timeout_ms = 8000;
        }

        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        fd_set rfds; FD_ZERO(&rfds); FD_SET(udp_sock, &rfds);

        int rv = select(udp_sock+1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(udp_sock, &rfds)) {
            PowerUDPMessage resp;
            struct sockaddr_in sa; socklen_t sl = sizeof(sa);
            int rec = recvfrom(udp_sock, &resp, sizeof(resp), 0, (struct sockaddr*)&sa, &sl);
            if (rec >= (int)sizeof(PowerUDPHeader) &&
                resp.header.seq_num == pudp.header.seq_num) {
                if (resp.header.type == PUDP_ACK) {
                    success = 1;
                    if (current_config.enable_sequence) send_seq_num++;
                    break;
                } else if (resp.header.type == PUDP_NACK) {
                    attempts++; last_retransmissions++;
                    continue;
                }
            }
        }
        if (!success) {
            if (!current_config.enable_retransmission) break;
            attempts++; last_retransmissions++;
        }
    }

    last_delivery_time = (int)((clock() - start) * 1000 / CLOCKS_PER_SEC);
    return success ? len : -1;
}

int receive_message(char *buffer, int bufsize) {
    if (!buffer || bufsize <= 0) return -1;
    PowerUDPMessage pudp;
    struct sockaddr_in src; socklen_t sl = sizeof(src);

    int rec = recvfrom(udp_sock, &pudp, sizeof(pudp), 0, (struct sockaddr*)&src, &sl);
    if (rec < (int)sizeof(PowerUDPHeader)) return -1;
    if (pudp.header.type != PUDP_DATA) return -1;

    if (current_config.enable_sequence) {
        if (pudp.header.seq_num != recv_seq_num) {
            PowerUDPMessage nack = { .header = {PUDP_NACK, pudp.header.seq_num, 0} };
            sendto(udp_sock, &nack, sizeof(PowerUDPHeader), 0, (struct sockaddr*)&src, sl);
            return -1;
        }
        recv_seq_num++;
    }

    int dlen = pudp.header.data_len;
    if (dlen > bufsize) {
        PowerUDPMessage nack = { .header = {PUDP_NACK, pudp.header.seq_num, 0} };
        sendto(udp_sock, &nack, sizeof(PowerUDPHeader), 0, (struct sockaddr*)&src, sl);
        return -1;
    }

    memcpy(buffer, pudp.data, dlen);
    PowerUDPMessage ack = { .header = {PUDP_ACK, pudp.header.seq_num, 0} };
    sendto(udp_sock, &ack, sizeof(PowerUDPHeader), 0, (struct sockaddr*)&src, sl);
    return dlen;
}

int get_last_message_stats(int *retransmissions, int *delivery_time) {
    if (!retransmissions || !delivery_time) return -1;
    *retransmissions = last_retransmissions;
    *delivery_time   = last_delivery_time;
    return 0;
}

void inject_packet_loss(int probability) {
    if (probability < 0) probability = 0;
    if (probability > 100) probability = 100;
    loss_probability = probability;
    printf("Perda simulada: %d%%\n", probability);
}
