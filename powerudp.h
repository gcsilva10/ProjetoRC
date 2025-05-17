#ifndef POWERUDP_H
#define POWERUDP_H

#include <stdint.h>

#define PSK_LEN 64
#define MAX_UDP_DATA 1024

/* Tipos de mensagem PowerUDP */
#define PUDP_DATA 0x01  /* Mensagem de dados */
#define PUDP_ACK  0x02  /* Confirmação positiva */
#define PUDP_NACK 0x03  /* Confirmação negativa */

/* Cabeçalho PowerUDP */
typedef struct {
    uint8_t type;         /* Tipo de mensagem: DATA, ACK, NACK */
    uint32_t seq_num;     /* Número de sequência */
    uint16_t data_len;    /* Comprimento dos dados */
} PowerUDPHeader;

/* Mensagem completa PowerUDP */
typedef struct {
    PowerUDPHeader header;
    char data[MAX_UDP_DATA];
} PowerUDPMessage;

/* Mensagem de configuração (multicast e requests TCP) */
typedef struct {
    uint8_t enable_retransmission;
    uint8_t enable_backoff;
    uint8_t enable_sequence;
    uint16_t base_timeout;
    uint8_t max_retries;
} ConfigMessage;

/* Pedido de registo TCP */
typedef struct {
    char psk[PSK_LEN];
} RegisterMessage;

/* API cliente */
int init_protocol(const char *server_ip, int server_port, const char *psk);
int init_protocol_with_port(const char *server_ip, int server_port, const char *psk, int udp_port);
void close_protocol();
int request_protocol_config(int enable_retransmission,
                            int enable_backoff,
                            int enable_sequence,
                            uint16_t base_timeout,
                            uint8_t max_retries);
int send_message(const char *destination, const char *message, int len);
int receive_message(char *buffer, int bufsize);
int get_last_message_stats(int *retransmissions, int *delivery_time);
void inject_packet_loss(int probability);

#endif /* POWERUDP_H */
