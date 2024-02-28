#include <stdio.h>
#include <signal.h>

#include "csapp.h"
#include "cache.h"

void *thread(void *vargp);
void doit(int fd);
void read_requesthdrs(rio_t *rp, void *buf, int serverfd, char *hostname, char *port);
void parse_uri(char *uri, char *hostname, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

static const int is_local_test = 0; // 도메인포트 지정을 위한 상수(0 할당시 고정, 외부접속가능)
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv)
{
    // printf("%s", user_agent_hdr);
    int listenfd, *fd;
    char hostname[MAXLINE], port[MAXLINE]; // client꺼
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    signal(SIGPIPE, SIG_IGN);

    rootp = (web_object_t *)calloc(1, sizeof(web_object_t));
    lastp = (web_object_t *)calloc(1, sizeof(web_object_t));

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    while (1)
    {
        clientlen = sizeof(clientaddr);
        fd = Malloc(sizeof(int));
        *fd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        // doit(connfd);
        // Close(connfd);
        Pthread_create(&tid, NULL, thread, fd);
    }
}

void *thread(void *vargp)
{
    int clientfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(clientfd);
    Close(clientfd);
    return NULL;
}

void doit(int fd)
{
    int serverfd, content_length;
    char req_buf[MAXLINE], rep_buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], path[MAXLINE], hostname[MAXLINE], port[MAXLINE];
    char *response_ptr, filename[MAXLINE], cgiargs[MAXLINE];
    rio_t req_rio, rep_rio;

    // req 클라 -> 프록시
    Rio_readinitb(&req_rio, fd);
    Rio_readlineb(&req_rio, req_buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", req_buf);

    sscanf(req_buf, "%s %s", method, uri);
    parse_uri(uri, hostname, port, path);

    sprintf(req_buf, "%s %s %s\r\n", method, path, "HTTP/1.0");

    if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD"))
    {
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }

    web_object_t *cached_object = find_cache(path);
    if (cached_object) {
        send_cache(cached_object, fd);
        read_cache(cached_object);
        return;
    }


    // req 전송 프록시 -> 서버
    serverfd = is_local_test ? Open_clientfd(hostname, port) : Open_clientfd("127.0.0.1", port);
    if (serverfd < 0) {
        clienterror(serverfd, method, "502", "Bad Gateway", "Failed to establish connection with the end server");
        return;
    }
    Rio_writen(serverfd, req_buf, strlen(req_buf));

    read_requesthdrs(&req_rio, req_buf, serverfd, hostname, port);

    // response 전송 s->p->c
    Rio_readinitb(&rep_rio, serverfd);
    while (strcmp(rep_buf, "\r\n")) 
    {
        Rio_readlineb(&rep_rio, rep_buf, MAXLINE);
        if (strstr(rep_buf, "Content-length"))
            content_length = atoi(strchr(rep_buf, ':') + 1);
        Rio_writen(fd, rep_buf, strlen(rep_buf));
    }

    response_ptr = (char *)Malloc(content_length);
    Rio_readnb(&rep_rio, response_ptr, content_length);
    Rio_writen(fd, response_ptr, content_length);

    if (content_length <= MAX_OBJECT_SIZE) {
        web_object_t *web_object = (web_object_t *)Calloc(1, sizeof(web_object_t));
        web_object->response_ptr = response_ptr;
        web_object->content_length = content_length;
        strcpy(web_object->path, path);
        write_cache(web_object);
    }
    else 
        Free(response_ptr);

    Close(serverfd);
}

void parse_uri(char *uri, char *hostname, char *port, char *path)
{
    char *hostname_ptr = strstr(uri, "//") ? strstr(uri, "//") + 2 : uri;
    // char *port_ptr; // port 위치
    // char *path_ptr; // path 위치

    // if ((strchr(hostname_ptr, ':')) != NULL){
    //     port_ptr = strchr(hostname_ptr, ':');
    // }
    // if ((strchr(hostname_ptr, '/')) != NULL){
    //     path_ptr = strchr(hostname_ptr, '/');
    //}
    char *port_ptr = strchr(hostname_ptr, ':'); // port 위치
    char *path_ptr = strchr(hostname_ptr, '/'); // path 위치

    strcpy(path, path_ptr);

    if (port_ptr) {
        strncpy(port, port_ptr + 1, path_ptr - port_ptr - 1);
        strncpy(hostname, hostname_ptr, port_ptr - hostname_ptr);
    }
    else {
        if (is_local_test)
            strcpy(port, "80");
        else
            strcpy(port, "8000");
        strncpy(hostname, hostname_ptr, path_ptr - hostname_ptr);
    }
}

void read_requesthdrs(rio_t *request_rio, void *request_buf, int serverfd, char *hostname, char *port)
{
    int is_host_exist;
    int is_connection_exist;
    int is_proxy_connection_exist;
    int is_user_agent_exist;

    Rio_readlineb(request_rio, request_buf, MAXLINE);
    while (strcmp(request_buf, "\r\n")) 
    {
        if (strstr(request_buf, "Proxy-Connection") != NULL) {
            sprintf(request_buf, "Proxy-Connection: close\r\n");
            is_proxy_connection_exist = 1;
        }
        else if (strstr(request_buf, "Connection") != NULL) {
            sprintf(request_buf, "Connection: close\r\n");
            is_connection_exist = 1;
        }
        else if (strstr(request_buf, "User-Agent") != NULL) {
            sprintf(request_buf, user_agent_hdr);
            is_user_agent_exist = 1;
        }
        else if (strstr(request_buf, "Host") != NULL) {
            is_host_exist = 1;
        }

        Rio_writen(serverfd, request_buf, strlen(request_buf));
        Rio_readlineb(request_rio, request_buf, MAXLINE);
    }

    // 필수 헤더 미포함 시 추가 전송
    if (!is_proxy_connection_exist) {
        sprintf(request_buf, "Proxy-Connection: close\r\n");
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_connection_exist) {
        sprintf(request_buf, "Connection: close\r\n");
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_host_exist) {
        if(!is_local_test)
            hostname = "127.0.0.1";
        sprintf(request_buf, "Host: %s:%s\r\n", hostname, port);
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }
    if (!is_user_agent_exist) {
        sprintf(request_buf, user_agent_hdr);
        Rio_writen(serverfd, request_buf, strlen(request_buf));
    }

    sprintf(request_buf, "\r\n");
    Rio_writen(serverfd, request_buf, strlen(request_buf));
    return;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0  %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
