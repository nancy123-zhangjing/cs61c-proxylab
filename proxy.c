#define _GNU_SOURCE
#include <stdio.h>
/*
proxylab
part1:
简单来讲就是要写一个中转站的程序，监听并接受来自客户端的请求，
做适当的重组之后，发送给服务器，然后接受来自服务器的数据，传回给客户端
在这一部分的结构可以参考tiny.c
part2：
在原来的基础上加上线程函数即可，比较简单
part3:
需要为代理加一个缓存，在内存中储存最近使用的Web对象
但注意，单个对象不大于1kb,cache总容量不大于1mb，那我们就让cacheline数组有10个元素
单个对象如果大于1kb，则不缓存，如果总缓存区满了，就用LRU策略淘汰
对于缓存的访问必须线程安全，同一时间必须支持多个线程读取缓存
但同一时间只能有一个人在写
考虑效仿书上的读者优先的写法

另外，在原来实现了GET方法的基础上，我还想尝试一下CONNECT方法
实现能连接一些现代的网页
*/
/* Recommended max cache and object sizes */
#include "csapp.h"
#include <pthread.h>
void doit(int fd);
void parse_uri(char *uri, char *hostname, char *port, char *path);
void build_head(char *http_header, char *hostname, char *path, rio_t *client_rio);
void send_head(int server_fd, char *http_header);
void *thread(void *clientfd_addr);
void init_cache();   
int search_data_in_cache(char * uri);
void add_to_cache (char *uri, char* object_buf, int total_size);
void updatetime( int index);
int LRU_strategy();
int find_available();
void bridge_tunnel(int client_fd, int server_fd);

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_OBJ_COUNT 10

typedef struct cacheline{
char cache_uri[MAXLINE];//作为key
char cache_data[MAX_OBJECT_SIZE];//数据部分
int cache_length;//数据大小
int timestamp; //时间戳
int valid;//有效位,0代表未分配
} cacheline;

cacheline cache[CACHE_OBJ_COUNT];
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
pthread_rwlock_t cache_lock;
int main(int argc, char **argv) 
{
    Signal(SIGPIPE, SIG_IGN); //按照题目要求处理SIGPIPE信号
    int listenfd, *client_fd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t child_thread;

    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(1);
    }
    pthread_rwlock_init(&cache_lock, NULL);//初始化锁
    init_cache();//初始化缓存区
    listenfd = Open_listenfd(argv[1]);
    while (1) {
	clientlen = sizeof(clientaddr);
    //接受客户端的请求并得到文件描述符
    client_fd = Malloc(sizeof(int));
	*client_fd = Accept(listenfd, (SA *)&clientaddr, &clientlen); 
    Pthread_create(&child_thread,NULL,thread,client_fd);
    }
}

void *thread(void *clientfd_addr) {
    int client_fd = *((int *)clientfd_addr);
    Pthread_detach(pthread_self());//这样子线程运行完会自动回收
    Free(clientfd_addr);
    doit(client_fd);
    Close(client_fd);
    return NULL;
}

void doit(int client_fd) {
    /*
    首先，读一行到缓冲区，GET http://www.cmu.edu:8080/hub/index.html HTTP/1.1
    然后分离出method uri version
    我们先看cache里面有没有对应的uri,如果有，则直接读，
    如果没有，我们就再去链接服务器，链接完之后把内容加到缓存（如果能加的话）
    对于http://www.cmu.edu:8080/hub/index.html，分离出hostname,port,path,port没写的话默认80
    然后把报头整理好，然后发送给服务器
    再连接服务器，原封不动的把服务器发过来的数据发回去
    */
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE],port[MAXLINE],path[MAXLINE];
    char http_header[MAXLINE];
    //为缓存设立的变量
    char object_buf[MAX_OBJECT_SIZE];
    int total_size = 0;
    int can_cache = 1;

    rio_t client_rio;
    int server_fd;
    size_t n;
    char buf1[MAXLINE];

    Rio_readinitb(&client_rio, client_fd);
    if (!Rio_readlineb(&client_rio, buf, MAXLINE))  
        return;
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("Request from Firefox: %s %s\n", method, uri);
    fflush(stdout);
    //CONNECT的逻辑
    if (strcasecmp(method, "CONNECT") == 0) {
        char hostname1[MAXLINE], port1[MAXLINE];
        char *p = strchr(uri, ':');
        if (p) {
        *p = '\0';
        strcpy(hostname, uri);
        strcpy(port, p + 1);
    } else {
        strcpy(hostname, uri);
        strcpy(port, "443"); // 默认 HTTPS 端口
    }
    //连接服务器
    server_fd = open_clientfd(hostname, port);
    if (server_fd < 0) return;

    // 3. 告诉浏览器：隧道已建好，你可以发加密数据了
    char *reply = "HTTP/1.1 200 Connection Established\r\n\r\n";
    Rio_writen(client_fd, reply, strlen(reply));

    // 4. 进入“盲传”模式 
    bridge_tunnel(client_fd, server_fd);

    Close(server_fd);
    return;
    }
    //得到uri之后，先看cache里面有没有对应的内容
    //在检查cache里面有没有想要的内容之前加读锁
    if (strcasecmp(method, "GET") == 0) {
        pthread_rwlock_rdlock(&cache_lock);
    if ( search_data_in_cache(uri) >= 0) {
        int index = search_data_in_cache(uri);
        Rio_writen(client_fd,cache[index].cache_data,cache[index].cache_length);
        pthread_rwlock_unlock(&cache_lock);//读完解锁
        //更新时间戳
        pthread_rwlock_wrlock(&cache_lock);
        updatetime(index);
        pthread_rwlock_unlock(&cache_lock);
        return;
    }
    else { //cache里面没有内容，则去连接服务器
    //分离出主机名，端口，路径
    pthread_rwlock_unlock(&cache_lock);//先把前面加的读锁解开
    char uri_copy[MAXLINE];
    strcpy(uri_copy, uri);//因为parse_uri里面会更改uri的内容，如果用修改后的uri来存，则永远不会命中了
    parse_uri(uri_copy,hostname,port,path);
    //构建报头
    build_head(http_header,hostname,path,&client_rio);
    //链接服务器
    server_fd = open_clientfd(hostname,port);
    if (server_fd < 0) return;//连接失败
    //发送报头到服务器
    send_head(server_fd,http_header);
    //把server_fd的响应搬运到客户端
    while ((n = Rio_readn(server_fd, buf1, MAXLINE)) > 0) { 
        //Rio_readn会阻塞等待服务器的数据，直到读满 MAXLINE 字节或者读到文件末尾（EOF）
        Rio_writen(client_fd, buf1, n);
        if (total_size + n <= MAX_OBJECT_SIZE){
            //用memcpy是因为这个函数是二进制安全的，不会遇到/0就停下来
            memcpy(object_buf + total_size,buf1,n);//追加
            total_size += n;
        } else {
            can_cache = 0;
        }
    }
    if (can_cache){
        add_to_cache(uri,object_buf,total_size);
    }
    Close(server_fd);
    }
    }
}

void bridge_tunnel(int client_fd, int server_fd) {
    unsigned char buf[MAXLINE];
    fd_set readfds;//想要监控的描述符
    int maxfd = (client_fd > server_fd ? client_fd : server_fd) + 1;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        FD_SET(server_fd, &readfds);

        // 阻塞等待，直到其中一个 FD 有数据
        //select函数就是会监测所有的描述符
        if (select(maxfd, &readfds, NULL, NULL, NULL) < 0) break;

        // 如果浏览器有数据 -> 发给服务器
        if (FD_ISSET(client_fd, &readfds)) {
            int n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break; // 连接断开了
            if (write(server_fd, buf, n) != n) break;
        }

        // 如果服务器有数据 -> 发给浏览器
        if (FD_ISSET(server_fd, &readfds)) {
            int n = read(server_fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (write(client_fd, buf, n) != n) break;
        }
    }
}

void parse_uri(char *uri, char *hostname, char *port, char *path) {

    strcpy(port, "80"); //端口默认80
    char *ptr = strstr(uri, "//"); //去除协议头
    if (ptr) {
        ptr += 2;
    } else {
        ptr = uri;
    }

    char *p_path = strchr(ptr, '/');
    if (p_path) {
        strcpy(path, p_path);
        *p_path = '\0';//在这里截断，方便获取主机名和端口
    } else { //说明访问根目录
        strcpy(path, "/");
    }

    char *p_port = strchr(ptr, ':');
    if (p_port) {//如果附有端口号
        strcpy(port, p_port + 1);
        *p_port = '\0';
        strcpy(hostname, ptr);
    } else { //没写端口号
        strcpy(hostname, ptr);
    }
}

//按照要求构建并发送报头
void build_head(char *http_header, char *hostname, char *path, rio_t *client_rio) {
    char buf[MAXLINE];
    char request_hdr[MAXLINE];
    char host_hdr[MAXLINE];
    char other_hdr[MAXBUF] = "";//用来放置浏览器可能传过来的其他报头

    //请求行
    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);
    //host
    sprintf(host_hdr, "Host: %s\r\n", hostname);
    //user_agent前面已经给了
    //Connection, Proxy-Connection都是硬编码为close
    //接收浏览器可能发过来的其他报头部分
    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) { //不停取一行
        if (strcmp(buf, "\r\n") == 0) break; // 遇到空行说明报头结束

        // 过滤掉我们需要手动控制的四个核心报头
        if (strncasecmp(buf, "Host", 4) == 0) continue;
        if (strncasecmp(buf, "User-Agent", 10) == 0) continue;
        if (strncasecmp(buf, "Connection", 10) == 0) continue;
        if (strncasecmp(buf, "Proxy-Connection", 16) == 0) continue;

        // 如果是浏览器发来的其他报头（如 Accept），原样保留
        strcat(other_hdr, buf);
    }
    //把所有部分拼接起来
    sprintf(http_header, "%s%s%s%s%s%s\r\n", 
            request_hdr,
            host_hdr,
            user_agent_hdr,
            "Connection: close\r\n",       // 强制 close
            "Proxy-Connection: close\r\n", // 强制 close
            other_hdr);
}


void send_head(int server_fd, char *http_header) {
    Rio_writen(server_fd, http_header, strlen(http_header));
}
void init_cache(){
    //初始化
    for (int i = 0; i < CACHE_OBJ_COUNT; i++) {
        cache[i].timestamp = 0;
        cache[i].valid = 0;
    }
    return ;
}

int search_data_in_cache(char * uri) {
    for(int i = 0; i < CACHE_OBJ_COUNT; i++){
        if (strcmp(cache[i].cache_uri, uri) == 0 && cache[i].valid == 1) { //为了避免一些错误，我们选择判断valid位
            return i;
        }
    }
    return -1;
}

void add_to_cache (char *uri, char* object_buf, int total_size){
    //改前加锁
    pthread_rwlock_wrlock(&cache_lock);
    int index = find_available();
    if (index < 0) {
        index = LRU_strategy();
    }
    strcpy(cache[index].cache_uri,uri);
    memcpy(cache[index].cache_data,object_buf,total_size);
    cache[index].cache_length = total_size;
    cache[index].timestamp = 0;
    cache[index].valid = 1;
    updatetime(index);
    pthread_rwlock_unlock(&cache_lock);
    //改完解锁
}

void updatetime( int index){
    for (int i = 0; i < CACHE_OBJ_COUNT; i++){
        if(cache[i].valid) {
            cache[i].timestamp += 1;
        }
    }
    if (cache[index].valid == 1) {
        cache[index].timestamp = 0;
    }
}

int LRU_strategy(){
    int maxtime = 0;
    int to_eviction = 0;
    for(int i = 0; i < CACHE_OBJ_COUNT; i++){
        if(cache[i].timestamp > maxtime) {
            maxtime = cache[i].timestamp;
            to_eviction = i;
        }
    }
    return to_eviction;
}

int find_available(){
    int index = -1;
    for(int i = 0; i < CACHE_OBJ_COUNT; i++) {
        if (cache[i].valid == 0) {
            index = i;
            return index;
        }
    }
    return index;
}