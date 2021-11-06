/*
 * multi-server.c
 */
#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset(), strcmp() */
#include <fcntl.h>      /* for open() */
#include <semaphore.h>  /* for sem_open(), sem_unlink() */
#include <unistd.h>     /* for close() and fork() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <errno.h>      /* for errno */
#include <sys/stat.h>   /* for stat() */
#include <sys/wait.h>   /* for waitpid() */
#include <sys/mman.h>   /* for mmap() */

#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

#define STATISTIC_SIZE sizeof(int) * 5

#define MAXLINE 5000

#define FILE_PATH_SIZE 512

#define FILE_NAME_SIZE 250

#define FILE_ENTRY_HTML_SIZE 1000

#define CHILD_NUMBER 100

pid_t serverStatpid;

typedef void Sigfunc (int signo);

const char *semName = "sem_newbee";
// const char *acceptSemName = "sem_accept";


static void *sharedMemo;

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");
      
    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 * 
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else 
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}

/*
 * Update statistics.
 */
static void updateStatistics(int statusCode)
{
    sem_t *sharedMemoSem = sem_open(semName, O_CREAT, 0660, 1);
    if (sharedMemoSem == SEM_FAILED)
        die("sem_open error");

    waitAgain:
    // Handle interrputed sem_wait
    if (sem_wait(sharedMemoSem) < 0) {
        if (errno == EINTR)
            goto waitAgain;
        die("sem_wait error");
    }

    int *stat = (int *) sharedMemo;
    stat[0] += 1;
    stat[statusCode/100 - 1] += 1;

    if (sem_post(sharedMemoSem) < 0)
        die("sem_post error");
    if (sem_close(sharedMemoSem) < 0)
        die("sem_close error");
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // send the buffer to the browser
    Send(clntSock, buf);

    // update statistics
    updateStatistics(statusCode);
}

static void getPathAndFile(char *src, char* path, char* file) {
    int length = strlen(src);
    int i = length - 1;
    while (i > -1 && src[i] != ' ')
        --i;
    memcpy(path, src, i + 1);
    memcpy(file, &src[i+1], length - i - 1);
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    // Compose the file path from webRoot and requestURI.
    // If requestURI equals to '/', append "index.html".
    
    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (strcmp(requestURI, "/") == 0) {
        strcat(file, "index.html");
    }

    // See if the requested file is statistics
    if (strcmp(requestURI, "/statistics") == 0) {
        statusCode = 200;
        updateStatistics(statusCode);
        char buf[1000] = "HTTP/1.0 200 OK\r\n\r\n";
        int *stat = (int *) sharedMemo;

        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>Server Statistics</h1>\n"
                "Requests: %d</br>\n"
                "2xx: %d</br>\n"
                "3xx: %d</br>\n"
                "4xx: %d</br>\n"
                "5xx: %d</br>\n"
                "</body></html>\n",
                stat[0], stat[1], stat[2], stat[3], stat[4]);
        strcat(buf, body);

        // send the buffer to the browser
        Send(clntSock, buf);
        goto func_end;
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        int n;
        int fd_stdout[2];
        int fd_stderr[2];
        pid_t pid;

        if (pipe(fd_stdout) < 0)
            die("pipe error");

        if (pipe(fd_stderr) < 0)
            die("pipe error");

        if ((pid = fork()) < 0) {
            die("pipe error");
        }
        else if (pid == 0) { /* child */
            close(fd_stdout[0]);
            close(fd_stderr[0]);
            if (dup2(fd_stdout[1], STDOUT_FILENO) != STDOUT_FILENO)
                die("dup2 error");
            if (dup2(fd_stderr[1], STDERR_FILENO) != STDERR_FILENO) 
                die("dup2 error");
            execl("/bin/ls", "ls", "-a", "-l", file, (char *)0);
            die("execl error");
        }
        else {              /* parent */
            close(fd_stdout[1]);
            close(fd_stderr[1]);
            char lsRes[MAXLINE] = {'\0'};
            if ((n = read(fd_stdout[0], lsRes, MAXLINE)) < 0 ||
                (n == 0 && (read(fd_stderr[0], lsRes, MAXLINE)) < 0))
                die("read error");

            // Convert raw ls result into html format
            char fancyLsRes[MAXLINE] = {'\0'};
            char *token_separators = "\n"; // tab, space, new line
            strcpy(fancyLsRes, strtok(lsRes, token_separators));

            char *tmp;
            while ((tmp = strtok(NULL, token_separators)) != NULL) {
                char fileName[FILE_NAME_SIZE] = {'\0'};
                char path[FILE_PATH_SIZE] = {'\0'};
                char fileHTML[FILE_ENTRY_HTML_SIZE + 50] = {'\0'};
                
                getPathAndFile(tmp, path, fileName);
                if (strcmp(fileName, ".") == 0)
                    sprintf(fileHTML, "</br>%s<a href=\"%s\">%s</a>", path, requestURI, fileName);
                else
                    sprintf(fileHTML, "</br>%s<a href=\"%s/%s\">%s</a>", path, requestURI, fileName, fileName);
                strcat(fancyLsRes, fileHTML);
            }

            statusCode = 200;
            char buf[MAXLINE] = "HTTP/1.0 200 OK\r\n\r\n";
            char body[MAXLINE + 30] = {'\0'};
            sprintf(body, 
                "<html><body>\n"
                "%s\n"
                "</body></html>\n",
                fancyLsRes);
            strcat(buf, body);

            Send(clntSock, buf);
            goto func_end;
        }
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sendStatusLine(clntSock, statusCode);

    // send the file 
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}

static void SIGCHLDHandler(int signo)
{
    int origErrno = errno;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
        fprintf(stderr, "waitpid (%d)\n", pid);
    errno = origErrno;
}

static void freeResources(int signo)
{
    if (sem_unlink(semName) < 0)
        die("sem_unlink error");
    exit(0);
}

/* Reference: http://www.strudel.org.uk/itoa/ */
static char *itoa(int val, char *ptr){
    
    char buf[32] = {0};
    strcpy(ptr, &buf[1]);
    
    int i = 30;
    
    if (val == 0) {
        buf[i--] = '0';
    } else {
        for(; val && i ; --i, val /= 10)
            buf[i] = "0123456789abcdef"[val % 10];
    }
    
    strcpy(ptr, &buf[i+1]);
    return ptr;
}

static void showStatistics()
{
    sem_t *sharedMemoSem = sem_open(semName, O_CREAT, 0660, 1);
    if (sharedMemoSem == SEM_FAILED)
        die("sem_open error");

    waitAgain:
    // Handle interrputed sem_wait
    if (sem_wait(sharedMemoSem) < 0) {
        if (errno == EINTR)
            goto waitAgain;
        die("sem_wait error");
    }

    int *stat = (int *) sharedMemo;

    char num[31];
    char statisticsRes[400];
    strcpy(statisticsRes, "\n====================\nRequests: ");
    strcat(statisticsRes, itoa(stat[0], num));
    strcat(statisticsRes, "\n\n2xx: ");
    strcat(statisticsRes, itoa(stat[1], num));
    strcat(statisticsRes, "\n3xx: ");
    strcat(statisticsRes, itoa(stat[2], num));
    strcat(statisticsRes, "\n4xx: ");
    strcat(statisticsRes, itoa(stat[3], num));
    strcat(statisticsRes, "\n5xx: ");
    strcat(statisticsRes, itoa(stat[4], num));
    strcat(statisticsRes, "\n====================\n");

    write(STDERR_FILENO, statisticsRes, strlen(statisticsRes));

    if (sem_post(sharedMemoSem) < 0)
        die("sem_post error");
    if (sem_close(sharedMemoSem) < 0)
        die("sem_close error");
}

static void handleHTTPRequest(FILE * clntFp, int clntSock, const char * webRoot, struct sockaddr_in clntAddr)
{
    /*
    * Let's parse the request line.
    */

    char *method      = "";
    char *requestURI  = "";
    char *httpVersion = "";

    char line[1000];
    char requestLine[1000];
    int statusCode;

    if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
        // socket closed - there isn't much we can do
        statusCode = 400; // "Bad Request"
        goto loop_end;
    }

    char *token_separators = "\t \r\n"; // tab, space, new line
    method = strtok(requestLine, token_separators);
    requestURI = strtok(NULL, token_separators);
    httpVersion = strtok(NULL, token_separators);
    char *extraThingsOnRequestLine = strtok(NULL, token_separators);

    // check if we have 3 (and only 3) things in the request line
    if (!method || !requestURI || !httpVersion || 
            extraThingsOnRequestLine) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // we only support GET method 
    if (strcmp(method, "GET") != 0) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // we only support HTTP/1.0 and HTTP/1.1
    if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
        strcmp(httpVersion, "HTTP/1.1") != 0) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }
    
    // requestURI must begin with "/"
    if (!requestURI || *requestURI != '/') {
        statusCode = 400; // "Bad Request"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // make sure that the requestURI does not contain "/../" and 
    // does not end with "/..", which would be a big security hole!
    int len = strlen(requestURI);
    if (len >= 3) {
        char *tail = requestURI + (len - 3);
        if (strcmp(tail, "/..") == 0 || 
                strstr(requestURI, "/../") != NULL)
        {
            statusCode = 400; // "Bad Request"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
    }

    /*
    * Now let's skip all headers.
    */

    while (1) {
        if (fgets(line, sizeof(line), clntFp) == NULL) {
            // socket closed prematurely - there isn't much we can do
            statusCode = 400; // "Bad Request"
            goto loop_end;
        }
        if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
            // This marks the end of headers.  
            // Break out of the while loop.
            break;
        }
    }

    /*
    * At this point, we have a well-formed HTTP GET request.
    * Let's handle it.
    */

    statusCode = handleFileRequest(webRoot, requestURI, clntSock);

    loop_end:

    /*
    * Done with client request.
    * Log it, close the client socket, and go back to accepting
    * connection.
    */

    fprintf(stderr, "%s (%d) \"%s %s %s\" %d %s\n",
            inet_ntoa(clntAddr.sin_addr),
            getpid(),
            method,
            requestURI,
            httpVersion,
            statusCode,
            getReasonPhrase(statusCode));

    // close the client socket.
    fclose(clntFp);
}

static void worker(int servSock, const char * webRoot)
{

    for (;;) {

        /*
        * wait for a client to connect
        */

        // initialize the in-out parameter
        struct sockaddr_in clntAddr;
        unsigned int clntLen = sizeof(clntAddr);
        int clntSock;

        acceptAgain:
        // Handle interrputed accept.
        if ((clntSock = accept(servSock, (struct sockaddr *)&clntAddr, &clntLen)) < 0) {
            if (errno == EINTR)
                goto acceptAgain; /* just an interrupted system call */
            /* handle other errors */
            die("accept() failed");
        }

        // sem_post(acceptSem);
        // sem_close(acceptSem);

        FILE *clntFp = fdopen(clntSock, "r");
        if (clntFp == NULL)
            die("fdopen failed");

        handleHTTPRequest(clntFp, clntSock, webRoot, clntAddr);

    } // for (;;)
}

/* Reliable version of signal(), using POSIX sigaction().  */
Sigfunc * signal(int signo, Sigfunc *func)
{
    struct sigaction    act, oact;

    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (signo == SIGALRM) {
#ifdef  SA_INTERRUPT
    act.sa_flags |= SA_INTERRUPT;
#endif
    } else {
        act.sa_flags |= SA_RESTART;
    }
    if (sigaction(signo, &act, &oact) < 0)
        return(SIG_ERR);
    return(oact.sa_handler);
}

static void SIGUSR1Handler(int signo) 
{
    kill(serverStatpid, SIGUSR2);
}
static void SIGUSR2Handler(int signo) 
{
    return; 
}

int main(int argc, char *argv[])
{
    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");
    // When child terminates, parent will handle it immediately.
    if (signal(SIGCHLD, SIGCHLDHandler) == SIG_ERR)
        die("signal() failed");
    // When web server terminates, free resources.
    if (signal(SIGTERM, freeResources) == SIG_ERR)
        die("signal() failed");
    // Set handler for SIGUSR1
    if (signal(SIGUSR1, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    // Create a block of shared memory.
    int fd;
    if ((fd = open("/dev/zero", O_RDWR)) < 0)
        die("open /dev/zero failed");
    if ((sharedMemo = mmap(0, STATISTIC_SIZE, PROT_READ | PROT_WRITE, 
                                        MAP_SHARED, fd, 0)) == MAP_FAILED)
        die("mmap error");

    close(fd);


    if (argc != 3) {
        fprintf(stderr, "usage: %s <server_port> <web_root>\n", argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);
    const char *webRoot = argv[2];

    int servSock = createServerSocket(servPort);
    pid_t childpid;

    fprintf(stderr, "parent pid %i", getpid());

    for (int i = 0; i < CHILD_NUMBER; i++) {

        if ((childpid = fork()) < 0) {
            die("fork() failed");

        } else if (childpid == 0) { /* child */

            fprintf(stderr, "child pid %i \n", getpid());
            worker(servSock, webRoot);
        } 
    }

    if (signal(SIGUSR1, SIGUSR1Handler) == SIG_ERR) 
        die("signal() failed");

    pid_t statChildpid = fork();
    if (statChildpid < 0) {
        die("fork() failed");
    } else if (statChildpid == 0) {  /* child */
        // Catch SIGUSR2 that is sent when there is a request for statistics 
        // helps to break out of pause
        if (signal(SIGUSR2, SIGUSR2Handler) == SIG_ERR)
            die("signal() failed");

        while (1) {
            pause(); // waiting for SIGUSR2 
            showStatistics();
        }
    }
    serverStatpid = statChildpid;

    while (1) {
        waitagain:
        if (waitpid(-1, NULL, 0) < 0) {
            if (errno == EINTR)
                goto waitagain;
            else
                die("waitpid() failed");
        }
    }
    return 0; 
}
