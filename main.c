
#define _REENTRANT

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define CONNMAX 1000
#define MAXFILESIZE 300000000
#define THREADMAX 1
#define DEFAULTPORT "8080"
#define MAXPAYLOAD 10240
#define N CONNMAX / THREADMAX
#define MAX_BUFFER 1024



enum ErrorLog { LOG_ERROR, LOG_WARN, LOG_INFO };
const char *severityMessage[] = { "ERROR", "WARN", "INFO" };

enum RequestState {
    STATE_CONNECT, STATE_READ,
    STATE_INDENTIFY, STATE_SEND,
    STATE_COMPLETE, STATE_ERROR
};

long maxThreads = THREADMAX;
long maxConnections = CONNMAX;
int fdArray[THREADMAX][N];

// #define rootDir "./static"

char *rootDir;
int listenfd = 0;
const char *serverPort = DEFAULTPORT;
pthread_t *threads;

struct httpRequest {
    char host[160];
    char userAgent[1024];
};

int logLevel = LOG_INFO;

typedef struct httpReadBufferStruct {
    char initialBuffer[MAXPAYLOAD];
    int bytesRead;
    int bytesToRead;
} httpReadBuffer, *httpReadBufferPtr;

typedef struct httpWriteBufferStruct {
    char *data;
    int size;
    int bytesWritten;
} httpWriteBuffer, *httpWriteBufferPtr;

typedef struct httpRequestResponseStruct {
    httpReadBufferPtr readBuffer;
    httpWriteBufferPtr writeBuffer;
    int state;
    int errorCode;
    int socket;
} httpRequestResponse, *httpRequestResponsePtr;

httpRequestResponsePtr httpRRArray;

void startServer(char *);
void *responseLoop(void *);
void logMessage(int, char *, int);
void printUsage(char *prog);
void readRequest(httpRequestResponsePtr);
void identifyResource(httpRequestResponsePtr);
void sendResponse(httpRequestResponsePtr);
void shutdownServer(int signum);

/******************************************************************************
 strnstr() - Find the first occurrence of find in s, where the search is 
 limited to the first slen characters of s.
 *****************************************************************************/

int endswith(const char *str, const char *suffix) {
    if (!str || !suffix)
        return 0;

    const size_t str_len = strlen(str);
    const size_t suffix_len = strlen(suffix);
    if (suffix_len >  str_len)
        return 0;

    return strncmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

void send_response(httpRequestResponsePtr request, int status, const char *status_text, const char *content_type, const char* body, const size_t body_length)
{
    char header[MAX_BUFFER];
    const int header_length = snprintf(header, MAX_BUFFER,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, body_length, content_type);

    send(request->socket, header, header_length, 0);
    if (body && body_length > 0)
        send(request->socket, body, body_length, 0);

    char log_msg[MAX_BUFFER];
    snprintf(log_msg, sizeof(log_msg), "Response: %d %s", status, status_text);
    logMessage(LOG_WARN, log_msg, request->socket);
    request->state = STATE_COMPLETE;
}


char *strnstr(const char *s, const char *find, size_t slen) {
  char c, sc;
  size_t len;

  if ((c = *find++) != '\0') {
    len = strlen(find);
    do {
      do {
        if (slen-- < 1 || (sc = *s++) == '\0')
          return (NULL);
      } while (sc != c);
      if (len > slen)
        return (NULL);
      } while (strncmp(s, find, len) != 0);
    s--;
  }
  return ((char *)s);
}

void shutdownServer(int signum) {
    logMessage(LOG_INFO, "Shutting down.", 0);
    for (int i = 0; i < maxConnections; i++) {
        if (httpRRArray[i].socket != -1) {
            shutdown(httpRRArray[i].socket, SHUT_RDWR);
            close(httpRRArray[i].socket);
        }
    }
    if (listenfd)
        close(listenfd);
    exit(0);
}

void startServer(char *port) {
    struct addrinfo hints, *res, *p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &res) != 0)
        logMessage(LOG_ERROR, strerror(errno), 0);

    for (p = res; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, 0);
        if (listenfd == -1) continue;
        int on = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
            (const char *)&on, sizeof(on));
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) break;
    }
    if (p == NULL)
        logMessage(LOG_ERROR, strerror(errno), 0);

    freeaddrinfo(res);

    if (listen(listenfd, 1000000) != 0)
        logMessage(LOG_ERROR, strerror(errno), 0);
}

void initThreads() {
    pthread_t tmpThread;
    threads = malloc(sizeof(pthread_t) * maxThreads);
    httpRRArray = malloc(sizeof(httpRequestResponse) * maxConnections);
    for (int i = 0; i < maxConnections; i++) {
        httpRRArray[i].readBuffer = malloc(sizeof(httpReadBuffer));
        httpRRArray[i].writeBuffer = malloc(sizeof(httpWriteBuffer));
        // httpRRArray[i].socket = -1; // Initialize socket to -1
    }
    for (int i = 0; i < maxThreads; i++)
    {
        for (int j = 0; j < N; j++)
        {
            fdArray[i][j] = -1;
        }
    }
    for (int i = 0; i < maxThreads; i++) {
        int *slot = malloc(sizeof(int));
        *slot = i;
        if (pthread_create(&threads[i], NULL, responseLoop, slot))
        {
            logMessage(LOG_ERROR, strerror(errno), 0);

        }
    }
}

void *responseLoop(void *arg) {
    int n = *((int *) arg);
    httpRequestResponsePtr request;
    fd_set read_fds, write_fds;
    struct timespec timeout;
    while (1) {
        // printf("in thread hehehe\n");
        int max_fd;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        max_fd = -1;

        for (int i = 0; i < N; i++) {
            if (fdArray[n][i] != -1) {
                FD_SET(fdArray[n][i], &read_fds);
                FD_SET(fdArray[n][i], &write_fds);
                // printf("%d was setted\n", i + 1);
                if (fdArray[n][i] > max_fd) {
                    max_fd = fdArray[n][i];
                }
            }
        }
        // printf("maxfd= %d\n", max_fd);
        timeout.tv_sec = 3;
        timeout.tv_nsec =0;
        int activity = pselect(max_fd + 1, &read_fds, &write_fds, NULL, &timeout, NULL);
        // printf("actv= %d\n", activity);

        if (activity < 0) {
            logMessage(LOG_ERROR, strerror(errno), 0);
            continue;
        }
        if (activity == 0)
        {
            printf("time out. no event\n");
        }

        for (int i = 0; i < maxConnections; i++) {
            // printf("socket %d\n", httpRRArray[i].socket);
            if (httpRRArray[i].socket != -1) {
                if (FD_ISSET(httpRRArray[i].socket, &read_fds)) {
                    printf("read fds\n");
                    request = &httpRRArray[i];
                    switch(request->state)
                    {
                    case STATE_CONNECT:
                        request->readBuffer->bytesRead = 0;
                        request->readBuffer->bytesToRead = MAXPAYLOAD;
                        readRequest(request);
                        break;
                    case STATE_READ:
                        readRequest(request);
                        break;
                    }
                }
                if (httpRRArray[i].state == STATE_SEND && (httpRRArray[i].socket, &write_fds)) {
                    // printf("write fds\n");

                    sendResponse(&httpRRArray[i]);
                }
                if (httpRRArray[i].state == STATE_COMPLETE || httpRRArray[i].state == STATE_ERROR) {
                    shutdown(httpRRArray[i].socket, SHUT_RDWR);
                    close(httpRRArray[i].socket);
                    for (int k = 0; k < N; k++) {
                        if (fdArray[n][k] == httpRRArray[i].socket)
                            fdArray[n][k] = -1;
                    }
                    httpRRArray[i].socket = -1;
                }
            }
        }
    }
}

void readRequest(httpRequestResponsePtr request) {
    request->state = STATE_READ;

    char *mesg = request->readBuffer->initialBuffer +
        request->readBuffer->bytesRead;

    int rcvd = recv(request->socket, mesg, request->readBuffer->bytesToRead, 0);

    if (rcvd == -1) {
        request->errorCode = errno;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        request->state = STATE_ERROR;
        return;
    }

    if (rcvd == 0) {
        request->state = STATE_COMPLETE;
        return;
    }

    if (rcvd > 0) {
        if (request->readBuffer->bytesRead + rcvd >= MAXPAYLOAD) {
            send(request->socket, "HTTP/1.0 413 Payload Too Large\n", 31, 0);
            logMessage(LOG_WARN, "Max Payload", request->socket);
            request->state = STATE_COMPLETE;
            return;
        }

        request->readBuffer->bytesRead += rcvd;
        request->readBuffer->bytesToRead -= rcvd;
        mesg[request->readBuffer->bytesRead] = 0;

        if (strnstr(request->readBuffer->initialBuffer,
            "\r\n\r\n", request->readBuffer->bytesRead) != 0) {
            request->readBuffer->bytesToRead = 0;
            identifyResource(request);
        }
    }
}

void identifyResource(httpRequestResponsePtr request) {
    char *mesg = request->readBuffer->initialBuffer;
    struct httpRequest header;
    char *token;
    char *rest = mesg;
    char *reqline[3];
    char path[2048];
    char logBuffer[4096];
    int fd, bytesRead;

    request->state = STATE_INDENTIFY;

    while ((token = strtok_r(rest, "\n", &rest))) {
        if (strncmp(token, "Host: ", 6) == 0) {
            int i = 0;
            strcpy(header.host, token + 6);
            header.host[strlen(header.host) - 1] = '\0';
            while (header.host[i++] != '\r')
                if (header.host[i] == ':') {
                    header.host[i] = '\0';
                    break;
                }
        }
        if (strncmp(token, "User -Agent: ", 12) == 0) {
            strcpy(header.userAgent, token + 12);
        }
    }

    rest = mesg;
    reqline[0] = strtok_r(rest, " \t\n", &rest);


    if ((strncmp(reqline[0], "GET", 3) != 0) && strncmp(reqline[0], "HEAD", 4) != 0)
    {
        send_response(request, 405, "Method Not Allowed", "text/html", "<h1>405 Method Not Allowed</h1>", 31);
        return;
    }
    else
    {
        reqline[1] = strtok_r(rest, " \t", &rest);
        reqline[2] = strtok_r(rest, " \t\n", &rest);

        if (strncmp(reqline[2], "HTTP/1.0", 8) != 0 &&
            strncmp(reqline[2], "HTTP/1.1", 8) != 0) {
            send_response(request, 400, "Bad Request", "text/html", "<h1>405 Bad Request</h1>", 31);
            return;
        }

        strcpy(path, rootDir);

        if (reqline[1] && reqline[1][0] == '/' && reqline[1][1] == '\0') {
            reqline[1] = "/index.html";
            strcpy(&path[strlen(rootDir)], reqline[1]);
        } else {
            strcpy(&path[strlen(rootDir)], reqline[1]);
        }
        
        char *response[512];
        struct stat fileInfo;

        if (stat(path, &fileInfo) == -1 || S_ISDIR(fileInfo.st_mode)) {
            send_response(request, 404, "Not Found", "text/html", "<h1>404 Not Found</h1>", 22);
            return;
        }


        long filesize = fileInfo.st_size;

        if (access(path, R_OK) == -1) {
            send_response(request, 403, "Forbidden", "text/html", "<h1>403 Forbidden</h1>", 22);
            close(fd);
            return;
        }
        
        if ((fd = open(path, O_RDONLY)) == -1){
            send_response(request, 404, "Not Found", "text/html", "<h1>40? Not Found</h1>", 22);
            return;
        }

        if (filesize > MAXFILESIZE) {
            send_response(request, 413, "Payload Too Large", "text/html", "<h1>413 Payload Too Large</h1>", 22);
            return;
        }

        flock(fd, LOCK_EX);
        char *buffer = malloc(filesize);
        bytesRead = read(fd, buffer, filesize);
        close(fd);
            
        if (strncmp(reqline[0], "HEAD", 4) == 0)
        {
            char headerBuffer[512];
            snprintf(headerBuffer, sizeof(headerBuffer), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                filesize, (endswith(path, ".html") ? "text/html" : "text/plain"));

            send(request->socket, headerBuffer, strlen(headerBuffer), 0);

            sprintf(logBuffer, "HEAD %s -- %s", path, header.userAgent);
            logMessage(LOG_INFO, logBuffer, request->socket);

            free(buffer);
        }
        else if (strncmp(reqline[0], "GET", 3) == 0){
            if (endswith(path, ".html"))
            {
                char headerBuffer[512];
                snprintf(headerBuffer, sizeof(headerBuffer), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                    filesize, "text/html");

                send(request->socket, headerBuffer, strlen(headerBuffer), 0);
            }
            else 
            {
                send(request->socket, "HTTP/1.0 200 OK\n\n", 17, 0);
            }
            request->writeBuffer->data = buffer;
            request->writeBuffer->size = bytesRead;
            request->writeBuffer->bytesWritten = 0;

            sendResponse(request);
            sprintf(logBuffer, "GET %s -- %s", path, header.userAgent);
            logMessage(LOG_INFO, logBuffer, request->socket);
        }
    }
}

void sendResponse(httpRequestResponsePtr request) {
    request->state = STATE_SEND;

    int len = send(request->socket,
        request->writeBuffer->data + request->writeBuffer->bytesWritten,
        request->writeBuffer->size, 0);

    if (len == -1) {
        request->errorCode = errno;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        request->state = STATE_ERROR;
        return;
    }

    if (len > 0) {
        request->writeBuffer->bytesWritten += len;
        request->writeBuffer->size -= len;
        if (request->writeBuffer->size == 0) {
            request->state = STATE_COMPLETE;
        }
    }
}

void logMessage(int severity, char *string, int socket) {
    time_t now;
    struct tm ts;
    char timeBuf[80];
    char clientip[30];
    int showLog = 0;

    switch (severity) {
        case LOG_ERROR:
            showLog = -1;
            break;
        case LOG_WARN:
            if (logLevel == 1 || logLevel == 2) showLog = 1;
            break;
        case LOG_INFO:
            if (logLevel == 2) showLog = 1;
            break;
    }
    if (showLog == 0) return;

    if (socket) {
        struct sockaddr_in addr;
        socklen_t addr_size = sizeof(struct sockaddr_in);
        getpeername(socket, (struct sockaddr *)&addr, &addr_size);
        strcpy(clientip, inet_ntoa(addr.sin_addr));
    }
    time(&now);
    ts = *localtime(&now);
    strftime(timeBuf, sizeof(timeBuf), "%a %Y-%m-%d %H:%M:%S %Z", &ts);

    if (socket)
        fprintf(stdout, "[%s ] - %s - %s -- %s\n",
            timeBuf, severityMessage[severity],
            clientip, string);
    else
        fprintf(stdout, "[%s] - %s - %s\n",
            timeBuf, severityMessage[severity],
            string);

    if (showLog == -1) shutdownServer(0);
}

void printUsage(char *prog) {
    fprintf(stderr,
        "\n%s [-p PORT -t Threads -c Connections -l LogLevel -r RootDir]\n"
        "  -p PORT: Port number to listen on, default %s\n"
        "  -t Threads: Number of threads to run, default %ld\n"
        "  -c Connections: Number of concurrent connections, default %ld\n"
        "  -l LogLevel: 0 = off, 1 = WARN, 2 = INFO (full)\n"
        "  -r RootDir: Document root for server, default current working "
        "directory\n\n",
        prog, serverPort, maxThreads, maxConnections);
}

int main(int argc, char* argv[]) {
    struct sockaddr_in clientaddr;
    socklen_t addrlen;
    char c;

    signal(SIGINT, shutdownServer);
    sigaction(SIGPIPE, &(struct sigaction){{SIG_IGN}}, NULL);

    char port[10];
    rootDir = "./static";
    strcpy(port, serverPort);
    maxConnections = CONNMAX;

    while ((c = getopt(argc, argv, "p:r:t:l:c:")) != -1)
        switch (c) {
            case 'r':
                rootDir = malloc(strlen(optarg) + 1);
                strcpy(rootDir, optarg);
                break;
            case 'p':
                strcpy(port, optarg);
                break;
            case 't':
                maxThreads = atoi(optarg);
                break;
            case 'l':
                logLevel = atoi(optarg);
                break;
            case 'c':
                maxConnections = atoi(optarg);
                break;
            case '?':
                printUsage(argv[0]);
                exit(1);
            default:
                exit(1);
        }
    if(chroot(rootDir));
    startServer(port);
    initThreads();
    for(int i=0; i<maxConnections; i++)
        httpRRArray[i].socket = -1;

    char buffer[1024];
    sprintf(buffer,
        "Server started at port no. %s%s%s with %s%ld%s Connections, "
        "%s%ld%s Threads, rootDir directory as %s%s%s",
        "\033[92m", port, "\033[0m",
        "\033[92m", maxConnections, "\033[0m",
        "\033[92m", maxThreads, "\033[0m",
        "\033[92m", rootDir, "\033[0m");
    logMessage(LOG_INFO, buffer, 0);

    int slot = 0;
    int threadSlot = 0;
    printf("server listen on socket %d\n", listenfd);
    while (1) {
        addrlen = sizeof(clientaddr);
        httpRRArray[slot].socket = accept(listenfd, NULL, NULL);
        // httpRRArray[slot].socket = 4;

        if (httpRRArray[slot].socket < 0) {
            if (errno != EAGAIN)
                logMessage(LOG_ERROR, strerror(httpRRArray[slot].socket), 0);
        } else {
            httpRRArray[slot].state = STATE_CONNECT;
            fcntl(httpRRArray[slot].socket, F_SETFL,
                fcntl(httpRRArray[slot].socket, F_GETFL) | O_NONBLOCK);

            char abc[1024];
            // sprintf(abc, "the connection %d was accepted\n", httpRRArray[slot].socket);
            logMessage(LOG_INFO, abc, 0);    

            int i = 0;
            while (fdArray[threadSlot][i] != -1) i++;
            fdArray[threadSlot][i] = httpRRArray[slot].socket;
            // printf("in main slot = %d, i = %d , client = %d, aray = %d\n", slot,  i, httpRRArray[slot].socket, fdArray[threadSlot][i]);
            threadSlot = (threadSlot + 1) % maxThreads;

        }
        while (httpRRArray[slot].socket != -1) slot = (slot + 1) % maxConnections;
    }
    return 0;
}