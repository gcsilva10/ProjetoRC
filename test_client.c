#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "powerudp.h"

#define SERVER_IP "127.0.0.1"  // Mudar para o IP do servidor real
#define SERVER_PORT 12345
#define PSK "minha_chave_preconfigurada"

void print_usage(const char *progname) {
    printf("Uso: %s [-p porta_local] <comando> [argumentos]\n", progname);
    printf("Opções:\n");
    printf("  -p porta_local         - Porta UDP local a utilizar (default: aleatória)\n");
    printf("Comandos:\n");
    printf("  send <ip[:porta]> <mensagem>     - Envia mensagem para outro cliente\n");
    printf("  receive                  - Aguarda mensagem de outro cliente\n");
    printf("  config <r> <b> <s> <t> <m> - Solicita nova configuração\n");
    printf("    r - Ativar retransmissão (0/1)\n");
    printf("    b - Ativar backoff (0/1)\n");
    printf("    s - Ativar sequência (0/1)\n");
    printf("    t - Timeout base (ms)\n");
    printf("    m - Retransmissões máximas\n");
    printf("  loss <percentagem>       - Simula perda de pacotes\n");
    printf("Exemplos:\n");
    printf("  %s -p 10001 send 192.168.1.2:10002 \"Olá mundo\"\n", progname);
    printf("  %s -p 10002 receive\n", progname);
    printf("  %s config 1 1 1 1000 5\n", progname);
}

int main(int argc, char *argv[]) {
    int local_port = 0; // 0 significa porta aleatória
    int arg_index = 1;
    
    // Verificar se temos a opção -p
    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        local_port = atoi(argv[2]);
        arg_index = 3;
        
        if (local_port <= 0 || local_port > 65535) {
            fprintf(stderr, "Porta inválida: %d\n", local_port);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (arg_index >= argc) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Inicializar protocolo com a porta especificada
    printf("Conectando ao servidor %s:%d (porta local UDP: %d)...\n", 
           SERVER_IP, SERVER_PORT, local_port ? local_port : 0);
    if (init_protocol_with_port(SERVER_IP, SERVER_PORT, PSK, local_port) < 0) {
        fprintf(stderr, "Falha ao inicializar protocolo\n");
        return 1;
    }
    
    // Processar comando
    if (strcmp(argv[arg_index], "send") == 0) {
        if (arg_index + 2 >= argc) {
            fprintf(stderr, "Argumentos insuficientes para 'send'\n");
            print_usage(argv[0]);
            close_protocol();
            return 1;
        }
        
        char *dest_ip = argv[arg_index+1];
        char *message = argv[arg_index+2];
        int message_len = strlen(message);
        
        printf("Enviando mensagem para %s: \"%s\"\n", dest_ip, message);
        if (send_message(dest_ip, message, message_len) < 0) {
            fprintf(stderr, "Falha ao enviar mensagem\n");
            close_protocol();
            return 1;
        }
        
        int retrans, del_time;
        get_last_message_stats(&retrans, &del_time);
        printf("Mensagem enviada com sucesso!\n");
        printf("Estatísticas: %d retransmissões, %d ms de tempo total\n", 
               retrans, del_time);
        
    } else if (strcmp(argv[arg_index], "receive") == 0) {
        printf("Aguardando mensagem...\n");
        char buffer[1024];
        int received = receive_message(buffer, sizeof(buffer));
        
        if (received < 0) {
            fprintf(stderr, "Erro ao receber mensagem\n");
        } else {
            buffer[received] = '\0';  // Garantir que termina com null
            printf("Mensagem recebida (%d bytes): \"%s\"\n", received, buffer);
        }
        
    } else if (strcmp(argv[arg_index], "config") == 0) {
        if (arg_index + 5 >= argc) {
            fprintf(stderr, "Argumentos insuficientes para 'config'\n");
            print_usage(argv[0]);
            close_protocol();
            return 1;
        }
        
        int enable_retrans = atoi(argv[arg_index+1]);
        int enable_backoff = atoi(argv[arg_index+2]);
        int enable_seq = atoi(argv[arg_index+3]);
        int base_timeout = atoi(argv[arg_index+4]);
        int max_retries = atoi(argv[arg_index+5]);
        
        printf("Solicitando nova configuração:\n");
        printf("  Retransmissão: %s\n", enable_retrans ? "Ativada" : "Desativada");
        printf("  Backoff: %s\n", enable_backoff ? "Ativado" : "Desativado");
        printf("  Sequência: %s\n", enable_seq ? "Ativada" : "Desativada");
        printf("  Timeout base: %d ms\n", base_timeout);
        printf("  Retransmissões máximas: %d\n", max_retries);
        
        if (request_protocol_config(enable_retrans, enable_backoff, enable_seq,
                                   base_timeout, max_retries) < 0) {
            fprintf(stderr, "Falha ao solicitar nova configuração\n");
            close_protocol();
            return 1;
        }
        
        printf("Configuração atualizada com sucesso!\n");
        
    } else if (strcmp(argv[arg_index], "loss") == 0) {
        if (arg_index + 1 >= argc) {
            fprintf(stderr, "Argumentos insuficientes para 'loss'\n");
            print_usage(argv[0]);
            close_protocol();
            return 1;
        }
        
        int loss_pct = atoi(argv[arg_index+1]);
        printf("Configurando injeção de perda para %d%%\n", loss_pct);
        inject_packet_loss(loss_pct);
        
    } else {
        fprintf(stderr, "Comando desconhecido: %s\n", argv[arg_index]);
        print_usage(argv[0]);
        close_protocol();
        return 1;
    }
    
    close_protocol();
    return 0;
} 