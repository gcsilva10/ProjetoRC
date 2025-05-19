#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "powerudp.h"

#define DEFAULT_SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define PSK "minha_chave_preconfigurada"
#define MAX_MSG_SIZE 1024

void print_usage(const char *progname) {
    printf("Uso: %s [-p porta_local] [-s ip_servidor] send <destino> [loss_pct]\n", progname);
    printf("       %s [-p porta_local] [-s ip_servidor] receive\n", progname);
    printf("       %s [-p porta_local] [-s ip_servidor] config <r> <b> <s> <t> <m>\n", progname);
}

void interactive_send_mode(const char *dest_ip) {
    char message[MAX_MSG_SIZE];
    printf("Modo de envio interativo iniciado. Digite suas mensagens (END para terminar):\n");

    while (1) {
        printf("> ");
        if (fgets(message, sizeof(message), stdin) == NULL) {
            break;
        }
        
        size_t len = strlen(message);
        if (len > 0 && message[len-1] == '\n') {
            message[len-1] = '\0';
            len--;
        }

        printf("Enviando mensagem para %s: \"%s\"\n", dest_ip, message);
        if (send_message(dest_ip, message, len) < 0) {
            fprintf(stderr, "Falha ao enviar mensagem\n");
        }

        int retrans, del_time;
        get_last_message_stats(&retrans, &del_time);
        printf("Estatísticas: %d retransmissões, %d ms de tempo total\n", retrans, del_time);

        if (strcmp(message, "END") == 0) {
            break;
        }
    }
}

void continuous_receive_mode() {
    printf("Modo de recebimento iniciado. Aguardando mensagens (receberá até mensagem END)...\n");
    char buffer[MAX_MSG_SIZE];

    while (1) {
        int received = receive_message(buffer, sizeof(buffer));
        if (received < 0) {
            usleep(100000);  // espera 100ms para não spammar erro
            continue;
        }
        buffer[received] = '\0';
        printf("Mensagem recebida (%d bytes): \"%s\"\n", received, buffer);
        if (strcmp(buffer, "END") == 0) {
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    int local_port = 0;
    int arg = 1;
    const char *server_ip = DEFAULT_SERVER_IP;

    while (arg < argc && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "-p") == 0 && arg + 1 < argc) {
            local_port = atoi(argv[++arg]);
        } else if (strcmp(argv[arg], "-s") == 0 && arg + 1 < argc) {
            server_ip = argv[++arg];
        } else {
            print_usage(argv[0]);
            return 1;
        }
        arg++;
    }

    if (arg >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[arg++];

    if (init_protocol_with_port(server_ip, SERVER_PORT, PSK, local_port) < 0) {
        fprintf(stderr, "Falha ao inicializar protocolo\n");
        return 1;
    }

    if (strcmp(cmd, "send") == 0) {
        if (arg >= argc) {
            print_usage(argv[0]);
            close_protocol();
            return 1;
        }
        const char *dest_ip = argv[arg++];
        if (arg < argc) {
            int loss_pct = atoi(argv[arg]);
            printf("Configurando perda de pacotes para %d%% antes do envio...\n", loss_pct);
            inject_packet_loss(loss_pct);
        }
        interactive_send_mode(dest_ip);

    } else if (strcmp(cmd, "receive") == 0) {
        continuous_receive_mode();

    } else if (strcmp(cmd, "config") == 0) {
        if (arg + 4 >= argc) {
            print_usage(argv[0]);
            close_protocol();
            return 1;
        }
        int r = atoi(argv[arg]);
        int b = atoi(argv[arg+1]);
        int s = atoi(argv[arg+2]);
        int t = atoi(argv[arg+3]);
        int m = atoi(argv[arg+4]);
        if (request_protocol_config(r,b,s,t,m) < 0) {
            fprintf(stderr, "Falha ao solicitar configuração\n");
        }

    } else {
        print_usage(argv[0]);
    }

    close_protocol();
    return 0;
}
