#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#define ISspace(x) isspace((int)(x))


#define SERVER_STRING "Server: wsj's http/0.1.0\r\n"   //  定义个人httpserver名称

void *accept_request(void* client);
void bad_request(int);
void cat(int, FILE*);
void cannot_execute(int);
void error_die(const char*);
void execute_cgi(int, const char*, const char*, const char*);
int get_line(int, char*, int);
void headers(int, const char*);
void not_found(int);
void serve_file(int , const char*);
int startup(u_short *);
void unimplemented(int);


// 开辟的线程的main函数, 处理监听的http请求
void* accept_request(void* from_client)
{
    int client = *((int*)from_client);
    char buf[1024];     //数据缓冲
    int numchars;         //读取的字节数
    char method[255];   // 保存请求方法
    char url[255];       //保存url
    char path[512];     // 保存请求文件路径
    size_t i, j; 
    struct stat st;   
    int cgi = 0;      //是否需要执行cgi解析
    char *query_string = NULL;
    numchars = get_line(client, buf, sizeof(buf));   //读取第一行 请求行数据
    i = 0;
    j = 0;
    while(!ISspace(buf[j]) && (i < sizeof(method) - 1))  //获取请求的方法，存在method
    {
        method[i] = buf[j];
        i++;
        j++;
    }
    method[i] = '\0';
    if(strcasecmp(method, "GET") && strcasecmp(method, "POST"))   //如果不是get 或post方法，则告知未实现
    {
        unimplemented(client);
        return NULL;
    }
    if(strcasecmp(method, "POST") == 0)  // post请求需要cgi解析
    {
        cgi = 1;
    }
    i = 0;

    while(ISspace(buf[j]) && j < sizeof(buf))   //跳过空字符
    {
        j++;
    }
    while(!ISspace(buf[j]) && (i < sizeof(url)-1) && (j < sizeof(buf)))  //提取url
    {
        url[i] = buf[j];
        i++;
        j++;
    }
    url[i] = '\0';
    if(strcasecmp(method, "GET") == 0)
    {
        query_string = url;
        while(*query_string != '?' && *query_string != '\0')   //get请求的url可能带有参数，将参数截取下来
        {
            query_string++;          
        }
        if(*query_string == '?')
        {
            cgi = 1;
            *query_string = '\0';
            query_string++; 
        }
    }
    sprintf(path, "httpdocs%s", url);
    printf("url is: %s\n", url);
    printf("path is: %s\n", path); 
    if(path[strlen(path)-1] == '/')           //默认地址为test.html
    { 
        strcat(path, "test.html");
    }
    if(stat(path, &st) == -1)        //stat(const char* filename, struct stat *st) 将参数filename所指的文件状态复制到结构体st中
    {
        while((numchars > 0) && strcmp("\n" , buf))
        {
            numchars = get_line(client, buf, sizeof(buf));    //若函数stat()调用失败，则将剩余http信息读取完毕并丢弃
        } 
        not_found(client);                     //告知不存在
    }
    else
    {
        if((st.st_mode & S_IFMT) == S_IFDIR)    //若是一个目录，则默认请求test.html
        {
            strcat(path, "/test.html");
        }
        if((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH))   //若有执行权限，解析cgi程序
        {
            cgi = 1;
        }
        if(!cgi)
        {
            serve_file(client, path);   //请求静态文件，直接发送给客户端
        }
        else
        {
            execute_cgi(client, path, method, query_string);  //执行cgi文件
        }
        
    }
    close(client);   //执行完毕后关闭socket
    return NULL;
    
}

void bad_request(int client)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Ypur browser send a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a Post without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}

void cat(int client, FILE* resourse)
{
    char buf[1024];
    fgets(buf, sizeof(buf), resourse);
    while(!feof(resourse))            
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resourse);
    }
}

void cannot_execute(int client)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

void error_die(const char* sc)
{
    perror(sc);
    exit(1);
}

void execute_cgi(int client, const char* path, const char* method, const char *query_string)
{
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;
    buf[0] = 'A';
    buf[1] = '\0';
    if(strcasecmp(method, "GET") == 0)   //若为get方法，则读取剩余信息
    {
        while(numchars > 0 && strcmp("\n", buf))
        {
            numchars = get_line(client, buf, sizeof(buf));
        }
    }
    else
    {
        numchars = get_line(client, buf, sizeof(buf));  //post方法
        while(numchars > 0 && strcmp("\n", buf))
        {
            buf[15] = '\0';
            if(strcasecmp(buf, "Content-Length:") == 0)    //查找conten-lenghth长度，记录请求题长度
            {
                content_length = atoi(&buf[16]);
            }
            numchars = get_line(client, buf, sizeof(buf));

        }
        if(content_length == -1)
        {
            bad_request(client);
            return;
        }
    }
    printf("content-length:%d\n", content_length);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    if(pipe(cgi_output) < 0)          //建立管道
    {
        cannot_execute(client);
        return;
    }
    if(pipe(cgi_input) < 0)
    {
        cannot_execute(client);
        return;
    }
    if((pid = fork()) < 0 )   //创建子进程
    {
        cannot_execute(client);
        return;
    }
/*
    管道是进程间通信的一种方式，由pipe()创立的是无名管道，可用于父子进程通信。这里的cgi_output是子进程向父进程的输出管道，因此子进程关闭cgi_output[0],父进程关闭cgi_input[1]
    而cgi_input是父进程向子进程的输入管道，因此子进程关闭cgi_input[1],父进程关闭cgi_input[0]。避免浪费资源
    同时子进程重定向标准输入到cgi_input[0], 标准输出到cgi_output[1]，这样子进程的输入就来自父进程，输出是输出到父进程


*/
    if(pid == 0)  // 子进程执行cgi脚本
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];
        dup2(cgi_output[1], 1);  //重定向标准输出到cgi_output[1]写通道
        dup2(cgi_input[0], 0);  //重定向标准输入到cgi_input[0] 读通道
        close(cgi_input[1]);   //关闭cgi_input 写通道
        close(cgi_output[0]);   // 关闭cgi_output 读通道
        //设置cgi环境遍历 
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
      
        if(strcasecmp(method, "GET") == 0)   
        {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else
        {
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
	printf("path is: %s", path);
        execl(path, path, NULL);
        exit(0);
        
    }
    else   //父进程
    {
        close(cgi_input[0]);
        close(cgi_output[1]);
        if(strcasecmp(method, "POST") == 0)
        {
            for(i = 0; i < content_length; i++)
            {
                recv(client, &c, 1, 0);     //post请求数据写到input管道
                write(cgi_input[1], &c, 1);
            }
        }
        while(read(cgi_output[0], &c, 1) > 0)
        {
            send(client, &c, 1, 0);    //读到子进程输出信息，send到客户端
        }
        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);  //等待子进程返回
    }
   
}
/*
读取一行数据， 只要发现c是‘\n’,就认为是一行结束，如果读到\r, 再用msgpeek方式读取下一个字符，如果是\n，则读出
如果是下一个字符，则不做处理，将c置为\n，结束。
*/

int get_line(int sock, char* buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;
    while (i < size - 1 && c != '\n')
    {
        n = recv(sock, &c, 1, 0);
        if(n > 0)
        {
            if(c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK);
                if(n > 0 && c == '\n')
                {
                    recv(sock, &c, 1, 0);
                }
                else
                {
                    c = '\n';
                }
            }
            buf[i] = c;
            i++;
        }
        else
        {
            c = '\n';
        }
    }
    buf[i] = '\0';
    return i;
    
}

void headers(int client, const char* filename)
{
    char buf[1024];

    (void)filename;  /* could use filename to determine file type */
    //发送HTTP头
    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);

}

void not_found(int client)
{
    char buf[1024];
    //返回404
    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}  


void serve_file(int client, const char* filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    char buf[1024];
    buf[0] = 'A';
    buf[1] = '\0';
    while(numchars > 0 && strcmp("\n", buf))
    {
        numchars = get_line(client, buf, sizeof(buf));
    }

    resource = fopen(filename, "r");
    if(resource == NULL)
    {
        not_found(client);
    }
    else
    {
        headers(client, filename);
        cat(client, resource);
    }
    fclose(resource);
    
}

int startup(u_short *port)
{
    int httpd = 0, option;  //option 为套接字复用标志
    struct sockaddr_in name;
    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if(httpd == -1)
    {
        error_die("socket");
    }
    socklen_t optlen;
    optlen = sizeof(option);
    option = 1;
    setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, (void*)&option, optlen); // 此设置可将time-wait 状态下的套接字端口重新分配， 否则需要等待一段时间服务器端口号才可运行，
                                                                          //如果没有这一设置， 服务器断开连接后需要过一段时间才能bind
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(httpd, (struct sockaddr*)&name, sizeof(name)) < 0)
    {
        error_die("bind");
    }
    if(*port == 0)                                     //如果没有提供端口号， 就提供一个随机端口
    {
        socklen_t namelen = sizeof(name);
        if(getsockname(httpd, (struct sockaddr*)&name, &namelen) == -1)         
        {
            error_die("getsockname");
        }
        *port = ntohs(name.sin_port);
    }
    if(listen(httpd, 5) < 0)      //监听
    {
        error_die("listen");
    }
    return httpd;
}

void unimplemented(int client)
{
    char buf[1024];
        //发送501说明相应方法没有实现
    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

int main(void)
{
    int server_sock = -1;  // 初始化服务端套接字
    u_short port = 9190;   // 初始化服务端端口号
    int client_sock = -1;  // 初始化客户端套接字
    struct sockaddr_in client_name;  //保存连接的客户端地址信息
    socklen_t client_name_len = sizeof(client_name);  
    pthread_t thread_id;   // 保存创建的线程id
    server_sock = startup(&port);    // 调用startup()返回一个处于监听状态的服务端套接字，其端口号为传入的参数值
    printf("http server_sock is %d \n", server_sock);
    printf("http running on port %d\n", port);
    while(1)
    {
        client_sock = accept(server_sock, (struct sockaddr*)&client_name, &client_name_len);   //调用accept()函数接收请求
        printf("New connection ... ip: %s, port: %d\n", inet_ntoa(client_name.sin_addr), ntohs(client_name.sin_port));   //打印请求的客户端地址信息
        if(client_sock == -1)
        {
            error_die("accept");
        }
        if(pthread_create(&thread_id, NULL, accept_request, (void*)&client_sock) != 0)      //pthread_create()成功时返回0，失败时返回其他值
        {
            perror("pthread create");
        }
	printf("线程id:%ld\n", thread_id);
    }
    close(server_sock);
    return 0;

}
