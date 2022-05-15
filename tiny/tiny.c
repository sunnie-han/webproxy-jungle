/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);                                                                      // 한 개의 HTTP 트랜잭션을 처리
void read_requesthdrs(rio_t *rp);                                                       // 요청 헤더를 읽고 무시..?
int parse_uri(char *uri, char *filename, char *cgiargs);                                // HTML uri를 분석
void serve_static(int fd, char *filename, int filesize, char *method);                  // 정적 콘텐츠를 클라이언트에게 제공
void get_filetype(char *filename, char *filetype);                                      // 파일명으로부터 파일타입을 알아냄
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);                // 동적 콘텐츠를 클라이언트에게 제공
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);     // 에러 메세지를 클라이언트에 보냄
void echo(int connfd);                                                                  // 텍스트 줄을 echo해줌

int main(int argc, char **argv) {
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);                              // 지정한 포트 번호로 듣기 식별자 생성
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);   // line:netp:tiny:accept
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);   // line:netp:tiny:doit
        // echo(connfd);   //echo
        Close(connfd);  // line:netp:tiny:close
    }
}

/** 텍스트 줄을 echo해줌 */
void echo(int connfd) {
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}

/** 한 개의 HTTP 트랜잭션을 처리 */
void doit(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {                // GET과 HEAD method만 처리
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }

    read_requesthdrs(&rio);

    is_static = parse_uri(uri, filename, cgiargs);
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {        // 읽기 권한 확인
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size, method);                           // 정적 콘텐츠 제공
    }
    else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {        // 실행 권한 확인
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs, method);                               // 동적 콘텐츠 제공
    }
}

/** 에러 메세지를 클라이언트에 보냄 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/** 요청 헤더를 읽고 무시 */
void read_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
}

/** HTML uri를 분석 */
// filename : 원하는 파일 위치 (홈에서 시작)
// cgiargs : 동적 콘텐츠의 인자
int parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {          // 정적 콘텐츠
        strcpy(cgiargs, "");                // 인자 없음
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri) - 1] == '/') strcat(filename, "home.html"); // uri에 지정된 파일명이 없으면 home.html을 지정
        return 1;
    } 
    else {                                  // 동적 콘텐츠
        ptr = index(uri, '?');              // 인자 부분
        if (ptr) {                          // 인자가 있을 때
            strcpy(cgiargs, ptr + 1);       // 인자 부분를 cgiargs에 복사
            *ptr = '\0';                    // 인자 시작점을 NULL로
        } 
        else {
            strcpy(cgiargs, "");
        }

        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}

/** 정적 콘텐츠를 클라이언트에게 제공 */
void serve_static(int fd, char *filename, int filesize, char *method) {
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));
    printf("Response headers:\n");
    printf("%s", buf);

    if (strcasecmp(method, "HEAD") == 0) return;                    // HEAD method는 본문을 출력하지 않음

    srcfd = Open(filename, O_RDONLY, 0);                            // 대상 콘텐츠를 읽기 전용으로 열기
    // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);     // PROT_READ: 읽기 가능한 페이지, MAP_PRIVATE: 다른 프로세스와 공유하지 않음
    srcp = (char *)malloc(filesize);
    Rio_readn(srcfd, srcp, filesize);
    Close(srcfd);
    Rio_writen(fd, srcp, filesize);
    // Munmap(srcp, filesize);
    free(srcp);
}

/** 파일명으로부터 파일타입을 알아냄 */
void get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) {
        strcpy(filetype, "text/html");
    }
    else if (strstr(filename, ".gif")) {
        strcpy(filetype, "image/gif");
    }
    else if (strstr(filename, ".png")) {
        strcpy(filetype, "image/png");
    }
    else if (strstr(filename, ".jpg")) {
        strcpy(filetype, "image/jpeg");
    }
    else if (strstr(filename, ".mp4")) {    // MPEG-4 : .mp4
        strcpy(filetype, "video/mp4");
    }
    else {
        strcpy(filetype, "text/plain");
    }
}

/** 동적 콘텐츠를 클라이언트에게 제공 */
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method) {
    char buf[MAXLINE], *emptylist[] =  { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {                          // 자식 프로세스 생성
        setenv("QUERY_STRING", cgiargs, 1);     // 인자를 복사 (CGI 환경변수 - program argument)
        setenv("REQUEST_METHOD", method, 1);    // 인자를 복사 (CGI 환경변수 - name of the HTTP method; GET, HEAD, POST, etc.)
        Dup2(fd, STDOUT_FILENO);                // 자식의 표준 출력을 연결 파일 식별자로 재지정
        Execve(filename, emptylist, environ);   // CGI program을 로드하고 실행
    }
    Wait(NULL);                                 // 부모 프로세스는 자식 프로세스의 종료를 기다림
}