#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

/* Header search key */
static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

/* end server info */
static const char *end_server_host = "localhost";   // end server의 hostname은 현재 localhost
static const int end_server_port = 52185;           // proxy 서버의 소켓 번호 +1

/* functions */
void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *host, int *port, char *path);
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio);
int connect_endServer(char *hostname, int port);
// for thread
void *thread(void *vargp);

/* for cache */
#define TOTAL_CACHE_BLOCK_NUM 10            // cache에 저장 가능한 블록의 총 갯수
#define LRU_MAX_NUMBER 9999                 // 새로 채운 블록의 초기 우선순위

void cache_init();                          // cache 초기화
int cache_find(char *uri);                  // cache에 있는지 찾고 정보를 받아오기
void cache_uri(char *uri, char *buf);       // buf를 새로 cache에 추가 
void cache_LRU(int index);                  // index 이외의 캐쉬의 LRU 값을 내리기
int cache_eviction();                       // 비어 있거나 우선순위가 가장 낮은 cache 블록 인덱스 찾기

void read_before(int i);
void read_after(int i);
void write_before(int i);
void write_after(int i);

typedef struct {
  char cache_uri[MAXLINE];              // 캐쉬한 uri
  char cache_object[MAX_OBJECT_SIZE];   // 캐쉬 내용
  int LRU;                              // 우선순위 (낮은게 오래 전에 추가된 캐쉬 블록)
  int is_empty;                         // 1: 빈 캐쉬 블록, 0: 채워진 캐쉬 블록

  int read_cnt;
  sem_t read_cnt_mutex;
  sem_t write_mutex;
} cache_block;

typedef struct {
  cache_block blocks[TOTAL_CACHE_BLOCK_NUM];
} cache_struct;

cache_struct cache;                         // 전역변수로 캐쉬 선언


int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    char clienthost[MAXLINE], clientport[MAXLINE];
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    cache_init();

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);                                  // 지정한 포트 번호로 듣기 식별자 생성
    
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));                                  // 경쟁상태 회피하기 위해 동적 할당
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        
        Getnameinfo((SA *)&clientaddr, clientlen, clienthost, MAXLINE, clientport, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", clienthost, clientport);

        Pthread_create(&tid, NULL, thread, connfdp);
    }
    return 0;
}

/** 쓰레드 루틴 */
void *thread(void *vargp) {
    int connfd = *(int *)vargp;
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

/** 한 개의 HTTP 트랜잭션을 처리 */
void doit(int fd) {
    int endserver_fd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_header[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;

    rio_t rio, endserver_rio;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);
        
    if (strcasecmp(method, "GET")) {                // GET method만 처리
        clienterror(fd, method, "501", "Not implemented", "Proxy does not implement this method");
        return;
    }

    // 캐쉬 확인 절차
    char uri_copy[100];
    strcpy(uri_copy, uri);

    int cache_idx;
    if ((cache_idx = cache_find(uri_copy)) != -1) { // 해당 uri의 cache를 찾은 경우
      read_before(cache_idx);
      Rio_writen(fd, cache.blocks[cache_idx].cache_object, strlen(cache.blocks[cache_idx].cache_object)); // cache에 저장되어 있으면 그대로 보냄
      printf("Proxy sent cached data\n");   // 확인용
      read_after(cache_idx);
      return;
    }

    // uri 분석
    parse_uri(uri, hostname, &port, path);

    // 서버에 보낼 헤더 작성
    build_http_header(endserver_http_header, hostname, path, &rio);

    // 서버 연결
    endserver_fd = connect_endServer(hostname, port);
    if (endserver_fd < 0) {                                                           // 연결 실패
      printf("connection failed\n");
      return;
    }

    Rio_readinitb(&endserver_rio, endserver_fd);
    Rio_writen(endserver_fd, endserver_http_header, strlen(endserver_http_header));   // end server에 요청 전달

    char cache_buf[MAX_OBJECT_SIZE];
    int size_buf = 0;
    
    size_t n;
    while ((n = Rio_readlineb(&endserver_rio, buf, MAXLINE)) != 0) {          // end server의 응답을 buf에 받기
        size_buf += n;
        if (size_buf < MAX_OBJECT_SIZE)
          strcat(cache_buf, buf);                                             // 보낼 내용을 버퍼에 저장
        Rio_writen(fd, buf, n);                                               // client에 end server response를 출력
    }
    printf("Proxy received %ld bytes and sent\n", size_buf);                  // proxy에 end server에서 받고 client에 보낸 문자수를 출력

    Close(endserver_fd);

    if (size_buf < MAX_OBJECT_SIZE) {                                         // cache_object의 사이즈에 들어갈 수 있는 크기이면 저장
      cache_uri(uri_copy, cache_buf);
    }

    return;
}

void parse_uri(char *uri, char *host, int *port, char *path) {
    *port = end_server_port;
    char *pos = strstr(uri, "//");
    pos = pos != NULL? pos + 2 : uri;

    char *pos2 = strstr(pos, ":");
    if (pos2 != NULL) {                      // port 번호 지정되어 있는 경우
      *pos2 = '\0';
      sscanf(pos, "%s", host);
      sscanf(pos2 + 1, "%d%s", port, path);        // ':' 건너뛰기
    } else {
      pos2 = strstr(pos, "/");
      if (pos2 != NULL) {                    // path가 있는 경우
        *pos2 = '\0';
        sscanf(pos, "%s", host);
        *pos2 = '/';
        sscanf(pos2, "%s", path);
      } else {                              // host만 있는 경우
        sscanf(pos, "%s", host);
      }
    }
    if (strlen(host) == 0) strcpy(host, end_server_host);   // host명이 없는 경우 지정

    return;
}


void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio) {
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
  
  // request line
  sprintf(request_hdr, requestline_hdr_format, path);

  // get other request header for client rio and change it
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
    if (strcmp(buf, endof_hdr) == 0)
      break;  // EOF
    
    if (!strncasecmp(buf, host_key, strlen(host_key))) {
      strcpy(host_hdr, buf);
      continue;
    }

    if (strncasecmp(buf, connection_key, strlen(connection_key))
        &&strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
        &&strncasecmp(buf, user_agent_key, strlen(user_agent_key))) {
        strcat(other_hdr, buf);
      }
  }
  if (strlen(host_hdr) == 0) {
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s",
          request_hdr,
          host_hdr,
          conn_hdr,
          prox_hdr,
          user_agent_hdr,
          other_hdr,
          endof_hdr);
  return;
}

/** 에러 메세지를 클라이언트에 보냄 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

inline int connect_endServer(char *hostname, int port) {
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

void cache_init() {
  int i;
  for (i=0; i<TOTAL_CACHE_BLOCK_NUM; i++) {
    cache.blocks[i].LRU = 0;        // 초기치 우선순위 없음
    cache.blocks[i].is_empty = 1;   // empty
    Sem_init(&cache.blocks[i].write_mutex, 0, 1);
    Sem_init(&cache.blocks[i].read_cnt_mutex, 0, 1);
    cache.blocks[i].read_cnt = 0;
  }
}

int cache_find(char *uri) {
  int i;
  for (i=0; i<TOTAL_CACHE_BLOCK_NUM; i++) {
    read_before(i);

    if ((cache.blocks[i].is_empty == 0) && (strcmp(uri, cache.blocks[i].cache_uri) == 0)) { // cache 되어 있으면
      read_after(i);
      return i;
    }
    read_after(i);
  }
  return -1;
}

void cache_uri(char *uri, char *buf) {
  int i = cache_eviction();                   // 빈 캐쉬 혹은 우선순위가 가장 낮은 캐쉬 블록

  write_before(i);

  strcpy(cache.blocks[i].cache_uri, uri);     // uri 채우기
  strcpy(cache.blocks[i].cache_object, buf);  // 내용 채우기
  cache.blocks[i].is_empty = 0;               // 채워진 블록 표시
  cache.blocks[i].LRU = LRU_MAX_NUMBER;       // 가장 큰 우선순위로 갱신
  cache_LRU(i);                               // 다른 캐쉬 블록 내리기

  write_after(i);
}

void cache_LRU(int index) {
  int i;
  for (i = 0; i < TOTAL_CACHE_BLOCK_NUM; i++) {
    if (i == index) continue;             // 자기 자신은 넘기기
    write_before(i);
    if (cache.blocks[i].is_empty == 0) {  // 채워져 있으면
      cache.blocks[i].LRU--;              // 우선순위를 내리기
    }
    write_after(i);
  }
}

int cache_eviction() {
  int min = LRU_MAX_NUMBER;
  int minindex = 0;
  int i;
  for (i=0; i<TOTAL_CACHE_BLOCK_NUM; i++) {
    read_before(i);
    if (cache.blocks[i].is_empty == 1) {  // 비어 있으면 저장 가능함
      minindex = i;
      read_after(i);
      break;
    }
    if (cache.blocks[i].LRU < min) {    // 우선 순위가 낮은 캐쉬블록으로 갱신
      minindex = i;
      min = cache.blocks[i]. LRU;
    }
    read_after(i);
  }

  return minindex;
}
void read_before(int i) {
  P(&cache.blocks[i].read_cnt_mutex);
  cache.blocks[i].read_cnt++;
  if (cache.blocks[i].read_cnt == 1)
    write_before(i);
  V(&cache.blocks[i].read_cnt_mutex);
}

void read_after(int i) {
  P(&cache.blocks[i].read_cnt_mutex);
  cache.blocks[i].read_cnt--;
  if (cache.blocks[i].read_cnt == 0)
    write_after(i);
  V(&cache.blocks[i].read_cnt_mutex);
}

void write_before(int i) {
  P(&cache.blocks[i].write_mutex);
}

void write_after(int i) {
  V(&cache.blocks[i].write_mutex);
}
