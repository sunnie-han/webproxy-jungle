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

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    char clienthost[MAXLINE], clientport[MAXLINE];
    struct sockaddr_storage clientaddr;
    pthread_t tid;

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

    parse_uri(uri, hostname, &port, path);

    // build http request for end server
    build_http_header(endserver_http_header, hostname, path, &rio);

    // connect to end server
    endserver_fd = connect_endServer(hostname, port);
    if (endserver_fd < 0) {
      printf("connection failed\n");
      return;
    }

    Rio_readinitb(&endserver_rio, endserver_fd);
    Rio_writen(endserver_fd, endserver_http_header, strlen(endserver_http_header));   // end server에 요청 전달
    
    size_t n;
    while ((n = Rio_readlineb(&endserver_rio, buf, MAXLINE)) != 0) {          // end server의 응답을 buf에 받기
        printf("Proxy received %ld bytes, then send\n", n);                   // proxy에 end server에서 받은 문자수를 출력하고
        Rio_writen(fd, buf, n);                                               // client에 end server response를 출력
    }

    Close(endserver_fd);
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