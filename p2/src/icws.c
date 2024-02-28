#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include "pcsa_net.h"
#include "parse.h"
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>

#define BUFSIZE 8192
#define MAXTHREAD 256
#define PERSISTENT 1
#define CLOSE 0

char *port, *directory, *cgi_directory, *connection_type;
int num_thread, timeout, num_tasks = 0;
int isHeadRequest, connection;
const char *filename;

pthread_t thread_pool[MAXTHREAD];
pthread_mutex_t mutex_q = PTHREAD_MUTEX_INITIALIZER, mutex_parse = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition_var = PTHREAD_COND_INITIALIZER;

typedef struct sockaddr SA;

struct survival_bag
{
    struct sockaddr_storage clientAddr;
    int connFd;
};

struct survival_bag taskQ[256];

void *conn_handler(struct survival_bag *task)
{
    int connection = PERSISTENT;
    while (connection == PERSISTENT)
    {
        connection = handle_request(task->connFd, directory);
    }
    close(task->connFd);
    return NULL;
}

void signal_handler(int signal)
{
    struct survival_bag *poison = (struct survival_bag *)malloc(sizeof(struct survival_bag));
    poison->connFd = -1;

    for (int i = 0; i < num_thread; i++)
    {
        pthread_mutex_lock(&mutex_q);
        taskQ[num_tasks] = *poison;
        num_tasks++;
        pthread_mutex_unlock(&mutex_q);
        pthread_cond_signal(&condition_var);
    }
}

char *get_mime(char *ext)
{
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0)
    {
        return "text/html";
    }
    else if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0)
    {
        return "image/jpeg";
    }
    else if (strcmp(ext, "css") == 0)
    {
        return "text/css";
    }
    else if (strcmp(ext, "csv") == 0)
    {
        return "text/csv";
    }
    else if (strcmp(ext, "txt") == 0)
    {
        return "text/plain";
    }
    else if (strcmp(ext, "png") == 0)
    {
        return "image/png";
    }
    else if (strcmp(ext, "gif") == 0)
    {
        return "image/gif";
    }
    else
    {
        return "null";
    }
}

void current_server_date(char *date)
{
    time_t current = time(0);
    struct tm local = *gmtime(&current);
    strftime(date, 30, "%a, %d %b %Y %H:%S %Z", &local);
}

void server_date_last_modified(char *last_modified, struct stat statbuf)
{
    struct tm local = *gmtime(&statbuf.st_mtime);
    strftime(last_modified, 30, "%a, %d %b %Y %H:%M:%S %Z", &local);
}

void display_connetion(struct sockaddr_storage *clientAddr)
{
    socklen_t clientLen = sizeof(struct sockaddr_storage);
    char hostBuf[BUFSIZE], svcBuf[BUFSIZE];
    if (getnameinfo((SA *)clientAddr, clientLen,
                    hostBuf, BUFSIZE, svcBuf, BUFSIZE, 0) == 0)
        printf("Connection from %s:%s\n", hostBuf, svcBuf);
    else
        printf("Connection from ?UNKNOWN?\n");
}

void error_response(int status, int connFd)
{
    char *response;
    switch (status)
    {
    case 501:
        response = "HTTP/1.1 501 Method Not Implemented\r\n\r\n";
        break;
    case 505:
        response = "HTTP/1.1 505 Bad Version\r\n\r\n";
        break;
    case 400:
        response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        break;
    }
    write_all(connFd, response, strlen(response));
}

void respond_with_404(int connFd)
{
    char *msg = "<h1>404 File not found</h1>";
    char buf[BUFSIZE];
    sprintf(buf,
            "HTTP/1.1 404 Not Found\r\n"
            "Server: icws\r\n"
            "Content-length: %lu\r\n"
            "Connection: close\r\n"
            "Content-type: text/html\r\n",
            strlen(msg));
    write_all(connFd, buf, strlen(buf));
    write_all(connFd, msg, strlen(msg));
}

int piper(int connFd, Request *request)
{
    char *args[2] = {cgi_directory, NULL};
    char *headers[] = {"CONNECTION", "ACCEPT", "REFERER", "ACCEPT-ENCODING", "ACCEPT-LANGUAGE",
                       "COTENT-LENGTH", "USER-AGENT", "ACCEPT-COOKIE", "ACCEPT-CHARSET", "HOST", "CONTENT-TYPE"};

    for (int i = 0; i < request->header_count; i++)
    {
        char *header_name = request->headers[i].header_name;
        char *header_value = request->headers[i].header_value;

        for (int j = 0; j < sizeof(headers) / sizeof(headers[0]); j++)
        {
            if (!strcasecmp(header_name, headers[j]))
            {
                char env_var[BUFSIZE];
                snprintf(env_var, BUFSIZE, "HTTP_%s", headers[j]);
                setenv(env_var, header_value, 1);
            }
        }
    }

    char *token = strtok(request->http_uri, "?");
    char *tokenList[BUFSIZE] = {token, strtok(NULL, "")};

    char addr[20];
    sprintf(addr, "%d", connFd);
    setenv("SERVER_SOFTWARE", "icws", 1);
    setenv("GATE_INTERFACE", "CGI/1.1", 1);
    setenv("REQUEST_METHOD", request->http_method, 1);
    setenv("REQUEST_URI", request->http_uri, 1);
    setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
    setenv("QUERY_STRING", tokenList[1], 1);
    setenv("REMOTE_ADDR", addr, 1);
    setenv("PATH_INFO", tokenList[0], 1);
    setenv("SERVER_PORT", port, 1);

    int pipefd[2];

    pid_t pid = 0;
    pipe(pipefd);

    pid = fork();
    if (!pid)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        execv(args[0], args);
    }

    int w;
    waitpid(pid, &w, WNOHANG);

    char buf[BUFSIZE];
    ssize_t numRead;
    while ((numRead = read(pipefd[0], buf, BUFSIZE)) > 0)
    {
        write_all(connFd, buf, numRead);
    }

    return 0;
}

const char *get_file_extension(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename)
    {
        return dot + 1;
    }
    return NULL;
}

void serve_file(int connFd, const char *rootFolder, int isHeaderRequest)
{
    char path[BUFSIZE];
    snprintf(path, sizeof(path), "%s/%s", rootFolder, filename);

    int file_fd = open(path, O_RDONLY);
    if (file_fd == -1)
    {
        respond_with_404(connFd);
        close(file_fd);
        return;
    }
    else
    {
        struct stat st;
        fstat(file_fd, &st);
        const char *content_type = get_file_extension(filename);

        char date[50];
        char last_modified[50];
        current_server_date(date);
        server_date_last_modified(last_modified, st);

        dprintf(connFd, "HTTP/1.1 200 OK\r\n");
        dprintf(connFd, "Date: %s\r\n", date);
        dprintf(connFd, "Content-Length: %ld\r\n", st.st_size);
        dprintf(connFd, "Connection: keep-alive\r\n");
        dprintf(connFd, "Content-Type: %s\r\n", content_type);
        dprintf(connFd, "Last-Modified: %s\r\n\r\n", last_modified);

        if (!isHeadRequest)
        {
            off_t offset = 0;
            sendfile(connFd, file_fd, &offset, st.st_size);
        }

        close(file_fd);
    }
}

int handle_request(int connFd, const char *rootFolder)
{
    char buf[BUFSIZE];
    int bytes_received = 0;
    int total_bytes = 0;

    while (total_bytes < BUFSIZE - 1)
    {
        int bytes = read(connFd, buf + total_bytes, BUFSIZE - 1 - total_bytes);

        if (bytes <= 0)
        {
            break;
        }

        total_bytes += bytes;
        buf[total_bytes] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL)
        {
            break;
        }
    }

    if (total_bytes <= 0)
    {
        return CLOSE;
    }

    pthread_mutex_lock(&mutex_parse);
    Request *request = parse(buf, BUFSIZE, connFd);
    pthread_mutex_unlock(&mutex_parse);

    connection = PERSISTENT;

    if (request == NULL)
    {
        error_response(400, connFd);
        return connection;
    }

    if (strcmp(request->http_version, "HTTP/1.0") == 0)
    {
        connection = CLOSE;
        connection_type = "close";
    }
    else
    {
        for (int i = 0; i < request->header_count; i++)
        {
            if (!strcmp(request->headers[i].header_name, "Connection"))
            {
                if (!strcmp(request->headers[i].header_value, "close"))
                {
                    connection = CLOSE;
                    connection_type = "close";
                }
                else
                {
                    connection = PERSISTENT;
                    connection_type = "keep-alive";
                }
                break;
            }
        }
    }

    char check_cgi[BUFSIZE];
    strncpy(check_cgi, request->http_uri, 5);

    if (strcmp(request->http_version, "HTTP/1.1") != 0 && strcmp(request->http_version, "HTTP/1.0") != 0)
    {
        error_response(505, connFd);
        free(request->headers);
        free(request);
        memset(buf, 0, BUFSIZE);
        return connection;
    }

    if (strcmp(request->http_method, "GET") == 0 || strcmp(request->http_method, "HEAD") == 0)
    {
        if (strncmp(request->http_uri, "/cgi/", 5) == 0)
        {
            piper(connFd, request);
        }
        else if (strcmp(request->http_method, "POST") == 0)
        {
            handle_post_request(connFd, rootFolder, request);
        }
        else
        {
            filename = request->http_uri;
            serve_file(connFd, rootFolder, strcmp(request->http_method, "HEAD") == 0);
        }
    }

    free(request->headers);
    free(request);
    return connection;
}

void handle_post_request(int connFd, const char *rootFolder, Request *request)
{
    // printf("POST\n");
}

void *thread_process(void *args)
{
    for (;;)
    {
        struct survival_bag task;
        pthread_mutex_lock(&mutex_q);
        while (!num_tasks)
        {
            pthread_cond_wait(&condition_var, &mutex_q);
        }

        task = taskQ[0];
        for (int i = 0; i < num_tasks - 1; i++)
        {
            taskQ[i] = taskQ[i + 1];
        }

        num_tasks--;
        pthread_mutex_unlock(&mutex_q);
        conn_handler(&task);
        if (task.connFd < 0)
            break;
    }
}

int main(int argc, char const *argv[])
{
    static struct option long_ops[] =
        {
            {"port", required_argument, NULL, 'p'},
            {"root", required_argument, NULL, 'r'},
            {"numThreads", required_argument, NULL, 'th'},
            {"timeout", required_argument, NULL, 't'},
            {"cgiHandler", required_argument, 'cgi'}};

    int ch;
    while ((ch = getopt_long(argc, argv, "p:r:th:t:", long_ops, NULL)) != -1)
    {
        switch (ch)
        {
        case 'p':
            port = optarg;
            break;
        case 'r':
            directory = optarg;
            break;
        case 'th':
            num_thread = atoi(optarg);
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'cgi':
            cgi_directory = optarg;
            break;
        default:
            printf("Option error.\r\n");
        }
    }

    int listenFd = open_listenfd(port);

    for (int i = 0; i < num_thread; i++)
    {
        if (pthread_create(&thread_pool[i], NULL, thread_process, NULL) != 0)
        {
            printf("Thread creation failed.");
        }
    }

    for (;;)
    {
        struct sockaddr_storage clientAddr;
        socklen_t clientLen = sizeof(struct sockaddr_storage);
        pthread_t threadInfo;

        int connFd = accept(listenFd, (SA *)&clientAddr, &clientLen);
        if (connFd < 0)
        {
            fprintf(stderr, "Failed to accept\n");
            continue;
        }

        struct survival_bag *context =
            (struct survival_bag *)malloc(sizeof(struct survival_bag));

        context->connFd = connFd;
        memcpy(&context->clientAddr, &clientAddr, sizeof(struct sockaddr_storage));

        display_connetion(&clientAddr);

        pthread_mutex_lock(&mutex_q);
        taskQ[num_tasks] = *context;
        num_tasks++;
        pthread_mutex_unlock(&mutex_q);
        pthread_cond_signal(&condition_var);
    }

    for (int i = 0; i < num_thread; i++)
    {
        if (pthread_join(thread_pool[i], NULL) != 0)
        {
            printf("Failed to join thread.");
        }
    }

    pthread_mutex_destroy(&mutex_q);
    pthread_mutex_destroy(&mutex_parse);
    pthread_cond_destroy(&condition_var);
    return 0;
}