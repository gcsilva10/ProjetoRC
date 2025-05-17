# PowerUDP - Protocolo Confiável sobre UDP

Este projeto implementa um protocolo de comunicação baseado em UDP, chamado PowerUDP, que adiciona garantias adicionais para torná-lo confiável, como:

- Retransmissão de pacotes perdidos
- Backoff exponencial para gerenciar retransmissões
- Números de sequência para garantir a entrega ordenada
- Mensagens de confirmação ACK/NACK

## Estrutura do Projeto

- `powerudp.h` - Definições do protocolo e API
- `client.c` - Implementação da biblioteca do cliente
- `server.c` - Implementação do servidor
- `test_client.c` - Programa de teste para demonstrar o uso do protocolo

## Compilação

Para compilar o projeto, use o comando:

```
make
```

Isso gerará dois executáveis:
- `servidor` - Servidor PowerUDP
- `test_client` - Cliente de teste

## Executando o Código

### Servidor

Para iniciar o servidor:

```
./servidor
```

O servidor escuta por conexões TCP na porta 12345 para registro de clientes e solicitações de configuração.

### Cliente de Teste

O cliente de teste oferece diferentes comandos:

1. Enviar uma mensagem:
```
./test_client send <ip-destino> <mensagem>
```

2. Aguardar uma mensagem:
```
./test_client receive
```

3. Solicitar uma nova configuração:
```
./test_client config <retransmissão> <backoff> <sequência> <timeout-base> <retries-max>
```
Exemplo: `./test_client config 1 1 1 1000 5`

4. Simular perda de pacotes:
```
./test_client loss <percentagem>
```
Exemplo: `./test_client loss 30` (30% de perda)

## Executando em GNS3 com Containers Linux

Para executar o código em containers do GNS3:

1. **Preparação dos arquivos**:
   - Compile o código no seu Mac: `make`
   - Crie um arquivo tar com os arquivos necessários:
     ```
     tar -czvf powerudp.tar.gz servidor test_client powerudp.h
     ```

2. **Transferência para o container GNS3**:
   - Use o método de compartilhamento de arquivos do GNS3, ou
   - Use `scp` para copiar o arquivo para o container:
     ```
     scp powerudp.tar.gz user@ip-do-container:/tmp/
     ```

3. **No container GNS3**:
   - Extraia os arquivos:
     ```
     mkdir -p /tmp/powerudp
     cd /tmp/powerudp
     tar -xzvf /tmp/powerudp.tar.gz
     ```
   - Execute o servidor em um terminal:
     ```
     cd /tmp/powerudp
     ./servidor
     ```
   - Execute o cliente em outro terminal:
     ```
     cd /tmp/powerudp
     ./test_client ...
     ```

### Considerações para GNS3

- Se os executáveis não rodarem por incompatibilidade de plataforma (compilados em ARM Mac vs x86 Linux), será necessário recompilar no container:
  ```
  cd /tmp/powerudp
  gcc -Wall -Wextra -std=c11 -o servidor server.c
  gcc -Wall -Wextra -std=c11 -o test_client test_client.c client.c -lpthread
  ```

- É importante configurar os endereços IP corretamente no cliente:
  - Edite o arquivo `test_client.c` para usar o IP correto do servidor (não 127.0.0.1)
  - Ao enviar mensagens entre clientes, use os IPs corretos dos containers

## Testes entre Múltiplos Containers

Para testar a comunicação entre múltiplos containers:

1. Inicie o servidor em um container
2. Em outros containers, execute os clientes de teste
3. Certifique-se de que os IPs estão configurados corretamente
4. Use comandos como:
   ```
   ./test_client send <ip-outro-container> "Mensagem de teste"
   ```
   e
   ```
   ./test_client receive
   ``` 