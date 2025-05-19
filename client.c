#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>
#include "powerudp.h"

#define TCP_TIMEOUT_SEC 2
#define MCAST_ADDR "239.0.0.1"
#define MCAST_PORT 54321

static int udp_sock=-1, mcast_sock=-1, tcp_sock=-1;
static struct sockaddr_in server_tcp_addr;
static ConfigMessage current_config={1,1,1,1000,5};
static int last_retrans=0, last_time=0;
static int loss_prob=0;
static uint32_t send_seq=0, recv_seq=0;
static volatile int running=1;

static int should_drop_packet(){
    return loss_prob>0 ? ((rand()%100)<loss_prob) : 0;
}

static void *multicast_listener(void *arg){
    (void)arg;
    struct sockaddr_in src; socklen_t sl=sizeof(src);
    ConfigMessage cfg;
    char buf[sizeof(cfg)];
    while(running){
        int n = recvfrom(mcast_sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&src, &sl);
        if(n==sizeof(cfg)){
            memcpy(&cfg, buf, sizeof(cfg));
            if(cfg.base_timeout==0) cfg.base_timeout=1000;
            if(cfg.max_retries==0)  cfg.max_retries=5;
            memcpy(&current_config,&cfg,sizeof(cfg));
            printf("Configuração atualizada via multicast\n");
        }
    }
    return NULL;
}

int init_protocol_with_port(const char *ip,int port,const char *psk,int udp_port){
    srand(time(NULL));
    // --- TCP registro ---
    tcp_sock=socket(AF_INET,SOCK_STREAM,0);
    memset(&server_tcp_addr,0,sizeof(server_tcp_addr));
    server_tcp_addr.sin_family=AF_INET;
    server_tcp_addr.sin_port=htons(port);
    inet_pton(AF_INET,ip,&server_tcp_addr.sin_addr);
    struct timeval tv={TCP_TIMEOUT_SEC,0};
    setsockopt(tcp_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    if(connect(tcp_sock,(struct sockaddr*)&server_tcp_addr,sizeof(server_tcp_addr))<0){
        perror("connect TCP"); return -1;
    }
    RegisterMessage reg; strncpy(reg.psk,psk,PSK_LEN);
    write(tcp_sock,&reg,sizeof(reg));
    char resp[3]={0}; read(tcp_sock,resp,2);
    close(tcp_sock);

    // --- UDP dados ---
    udp_sock=socket(AF_INET,SOCK_DGRAM,0);
    int reuse=1;
    setsockopt(udp_sock,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in local={.sin_family=AF_INET,
        .sin_port=htons(udp_port),.sin_addr.s_addr=INADDR_ANY};
    bind(udp_sock,(struct sockaddr*)&local,sizeof(local));
    if(udp_port==0){
        struct sockaddr_in a; socklen_t al=sizeof(a);
        getsockname(udp_sock,(struct sockaddr*)&a,&al);
        udp_port=ntohs(a.sin_port);
    }

    // --- UDP multicast config ---
    mcast_sock=socket(AF_INET,SOCK_DGRAM,0);
    setsockopt(mcast_sock,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in mb={.sin_family=AF_INET,
        .sin_port=htons(MCAST_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
    bind(mcast_sock,(struct sockaddr*)&mb,sizeof(mb));
    struct ip_mreq mreq;
    inet_pton(AF_INET,MCAST_ADDR,&mreq.imr_multiaddr);
    mreq.imr_interface.s_addr=htonl(INADDR_ANY);
    setsockopt(mcast_sock,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq));
    pthread_t tid; pthread_create(&tid,NULL,multicast_listener,NULL);
    pthread_detach(tid);

    printf("Cliente UDP local: %d; multicast em %s:%d\n",udp_port,MCAST_ADDR,MCAST_PORT);
    return 0;
}

int init_protocol(const char *ip,int port,const char *psk){
    return init_protocol_with_port(ip,port,psk,0);
}

void close_protocol(){
    running=0;
    shutdown(udp_sock,SHUT_RDWR); close(udp_sock);
    shutdown(mcast_sock,SHUT_RDWR); close(mcast_sock);
    printf("Protocolo PowerUDP finalizado\n");
}

int request_protocol_config(int enable_retransmission,
                            int enable_backoff,
                            int enable_sequence,
                            uint16_t base_timeout,
                            uint8_t max_retries) {
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
    uint8_t buf[1 + sizeof(req)];
    buf[0] = 'C';
    memcpy(buf + 1, &req, sizeof(req));
    if (write(tcp_sock, buf, 1 + sizeof(req)) != 1 + (int)sizeof(req)) {
        perror("write config req"); close(tcp_sock); return -1;
    }
    char resp[3]={0};
    if (read(tcp_sock, resp, 2) != 2 || strncmp(resp,"OK",2)!=0) {
        fprintf(stderr, "config falhou: %s\n", resp);
        close(tcp_sock); return -1;
    }
    close(tcp_sock);
    printf("Configuração enviada com sucesso\n");
    return 0;
}

int send_message(const char *dest,const char *msg,int len){
    if(len>MAX_UDP_DATA) return -1;
    char dip[64]; int dport;
    strcpy(dip,dest);
    char *p=strchr(dip,':');
    if(p){ *p='\0'; dport=atoi(p+1);} else dport=ntohs(((struct sockaddr_in*)&dip)->sin_port);
    struct sockaddr_in d={.sin_family=AF_INET,
        .sin_port=htons(dport)};
    inet_pton(AF_INET,dip,&d.sin_addr);

    PowerUDPMessage pkt={ .header={PUDP_DATA,send_seq,(uint16_t)len} };
    memcpy(pkt.data,msg,len);
    int total=sizeof(PowerUDPHeader)+len;
    last_retrans=0; clock_t start=clock();
    int attempts=0, max=current_config.max_retries, base=current_config.base_timeout;
    int success=0;
    while(!success && attempts<=max){
        if(!should_drop_packet()){
            sendto(udp_sock,&pkt,total,0,(struct sockaddr*)&d,sizeof(d));
        } else printf("Perda simulada seq=%u\n",pkt.header.seq_num);
        int timeout=base;
        if(current_config.enable_backoff && attempts>0){
            timeout=base*(1<<(attempts<3?attempts:3));
            if(timeout>8000) timeout=8000;
        }
        struct timeval tv={timeout/1000,(timeout%1000)*1000};
        fd_set f; FD_ZERO(&f); FD_SET(udp_sock,&f);
        int rv=select(udp_sock+1,&f,NULL,NULL,&tv);
        if(rv>0){
            PowerUDPMessage r; struct sockaddr_in sa; socklen_t sl=sizeof(sa);
            int n=recvfrom(udp_sock,&r,sizeof(r),0,(struct sockaddr*)&sa,&sl);
            if(n>=(int)sizeof(PowerUDPHeader) && r.header.seq_num==pkt.header.seq_num){
                if(r.header.type==PUDP_ACK){
                    success=1;
                    if(current_config.enable_sequence) send_seq++;
                    break;
                } else if(r.header.type==PUDP_NACK){
                    attempts++; last_retrans++;
                    continue;
                }
            }
        }
        if(!success){
            if(!current_config.enable_retransmission) break;
            attempts++; last_retrans++;
        }
    }
    last_time=(int)((clock()-start)*1000/CLOCKS_PER_SEC);
    return success?len:-1;
}

int receive_message(char *buf,int size){
    PowerUDPMessage p; struct sockaddr_in sa; socklen_t sl=sizeof(sa);
    int n=recvfrom(udp_sock,&p,sizeof(p),0,(struct sockaddr*)&sa,&sl);
    if(n<(int)sizeof(PowerUDPHeader)||p.header.type!=PUDP_DATA) return -1;
    if(current_config.enable_sequence && p.header.seq_num!=recv_seq){
        PowerUDPMessage nack={ .header={PUDP_NACK,p.header.seq_num,0} };
        sendto(udp_sock,&nack,sizeof(PowerUDPHeader),0,(struct sockaddr*)&sa,sl);
        return -1;
    }
    recv_seq++;
    int dlen=p.header.data_len;
    if(dlen>size){
        PowerUDPMessage nack={ .header={PUDP_NACK,p.header.seq_num,0} };
        sendto(udp_sock,&nack,sizeof(PowerUDPHeader),0,(struct sockaddr*)&sa,sl);
        return -1;
    }
    memcpy(buf,p.data,dlen);
    PowerUDPMessage ack={ .header={PUDP_ACK,p.header.seq_num,0} };
    sendto(udp_sock,&ack,sizeof(PowerUDPHeader),0,(struct sockaddr*)&sa,sl);
    return dlen;
}

int get_last_message_stats(int *r,int *t){
    if(!r||!t) return -1;
    *r=last_retrans; *t=last_time;
    return 0;
}

void inject_packet_loss(int pct){
    loss_prob=pct<0?0:(pct>100?100:pct);
    printf("Perda simulada: %d%%\n",loss_prob);
}
