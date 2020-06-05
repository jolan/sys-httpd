// This file is under the terms of the unlicense (https://github.com/DavidBuchanan314/ftpd/blob/master/LICENSE)

#define ENABLE_LOGGING 1
/* This FTP server implementation is based on RFC 959,
 * (https://tools.ietf.org/html/rfc959), RFC 3659
 * (https://tools.ietf.org/html/rfc3659) and suggested implementation details
 * from https://cr.yp.to/ftp/filesystem.html
 */
#include "http.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#ifdef _3DS
#    include <3ds.h>
#    define lstat stat
#elif defined(__SWITCH__)
#    include <switch.h>
#    define lstat stat
#else
#    include <stdbool.h>
#    define BIT(x) (1 << (x))
#endif
#include "console.h"
#include "led.h"
#include "util.h"

#define POLL_UNKNOWN (~(POLLIN | POLLPRI | POLLOUT))

int LISTEN_PORT;
//#define LISTEN_PORT 5000
#ifdef _3DS
#    define DATA_PORT (LISTEN_PORT + 1)
#else
#    define DATA_PORT 0 /* ephemeral port */
#endif

#include "minIni.h"
#include <assert.h>

int Callback(const char* section, const char* key, const char* value, void* userdata)
{
    (void)userdata; /* this parameter is not used in this example */
    printf("    [%s]\t%s=%s\n", section, key, value);
    return 1;
}

static void update_free_space(void);

/*! appletHook cookie */
static AppletHookCookie cookie;

/*! server listen address */
static struct sockaddr_in serv_addr;
/*! listen file descriptor */
static int listenfd = -1;

/*! server start time */
static time_t start_time = 0;

struct MetaData
{
    char TID[10];
    int version;
    char displayVersion[0x10];
    char name[0x201];
};
typedef struct Entry
{
    struct MetaData Data;
    struct Entry* next;
    struct Entry* prev;
} Entry;

void freeList(Entry* head)
{
    Entry* currEntry;
    currEntry = head;

    /* Free List */
    while (currEntry != NULL)
    {
        head = head->next;
        free(currEntry);
        currEntry = head;
    }
    return;
}

void initLists(NsApplicationRecord** titleRecords, int* recordsLength, NsApplicationContentMetaStatus*** metaStatusList, int** metaLength)
{
    /* Construct Installed Titles List */
    NsApplicationRecord* wipRecords;
    wipRecords = malloc(sizeof(NsApplicationRecord) * 1000);
    nsListApplicationRecord(wipRecords, 1000, 0, recordsLength);

    /* Construct Meta Status List */
    int tmpRecordsLength = *recordsLength;
    int* wipMetaLength;
    NsApplicationContentMetaStatus** wipMetaStatusList;
    wipMetaLength = malloc(sizeof(int) * tmpRecordsLength);
    wipMetaStatusList = malloc(sizeof(NsApplicationContentMetaStatus*) * tmpRecordsLength);
    for (int i = 0; i < tmpRecordsLength; i++)
    {
        *(wipMetaLength + i) = 0;
        *(wipMetaStatusList + i) = malloc(sizeof(NsApplicationContentMetaStatus) * 30);
        nsListApplicationContentMetaStatus(wipRecords[i].application_id, 0, *(wipMetaStatusList + i), 10, wipMetaLength + i);
    }
    nsExit();

    *metaStatusList = wipMetaStatusList;
    *metaLength = wipMetaLength;
    *titleRecords = wipRecords;

    return;
}

void updateMeta(char** namePtr, char** dispVerPtr, u64 TID)
{
    char* name;
    char* dispVer;
    name = calloc(0x201, sizeof(char));
    dispVer = calloc(0x10, sizeof(char));
    NacpLanguageEntry* nacpName;
    NsApplicationControlData* titleControl;
    titleControl = malloc(sizeof(NsApplicationControlData));
    if (titleControl == NULL)
    {
        console_print(RED "Failed to alloc mem\n" RESET);
    }
    u64 length = 0;
    int rc1 = nsGetApplicationControlData(NsApplicationControlSource_Storage, TID, titleControl, sizeof(NsApplicationControlData), &length);
    console_print(RED "nsGetApplicationControlData() result: 0x%x\n", rc1);
    console_print(RED "before nacpGetLanguageEntry\n" RESET);
    int rc2 = nacpGetLanguageEntry(&titleControl->nacp, &nacpName);
    console_print(RED "nacpGetLanguageEntry() result: 0x%x\n", rc2);
    console_print(RED "after nacpGetLanguageEntry\n" RESET);
    console_print(RED "before strncpy 1\n" RESET);
    console_print(RED "TID=%ld name=%s dispVer=%s\n" RESET, TID, nacpName->name, titleControl->nacp.display_version);
    strncpy(name, nacpName->name, 0x201);
    console_print(RED "after strncpy 1\n" RESET);
    strncpy(dispVer, titleControl->nacp.display_version, 0x10);
    console_print(RED "after strncpy 2\n" RESET);

    console_print(RED "before pointers\n" RESET);
    *(namePtr) = name;
    *(dispVerPtr) = dispVer;
    console_print(RED "after pointers\n" RESET);
    return;
}

Entry* initLocalVerList()
{
    console_print(RED "initLocalVerList() reached\n" RESET);
    /* Linked list of Entries to return */
    Entry *localVerList, *currEntry, *tmp;
    currEntry = calloc(1, sizeof(Entry));
    localVerList = currEntry;

    /* List of Installed Titles */
    int recordsEntry = 0;
    int recordsLength = 0;
    NsApplicationRecord* titleRecords;

    /* Meta Status Lists for Installed Titles */
    int* metaLength;
    NsApplicationContentMetaStatus** metaStatusList;

    /* Init titleRecords and metaStatusList */
    initLists(&titleRecords, &recordsLength, &metaStatusList, &metaLength);

    /* Meta Status List of Current Title */
    NsApplicationContentMetaStatus* currMeta;

    /* Name of Current Title */
    char* titleName;

    /* Display Version of Current Title */
    char* titleDispVersion;

    console_print(RED "recordsLength: %d metaLength %d\n" RESET,
                  recordsLength, *metaLength);
    while (recordsEntry < recordsLength)
    {
        currMeta = *(metaStatusList + recordsEntry);
        console_print(RED "updateMeta started\n" RESET);
        updateMeta(&titleName, &titleDispVersion, titleRecords[recordsEntry].application_id);
        console_print(RED "updateMeta ended\n" RESET);

        tmp = calloc(1, sizeof(Entry));
        currEntry->next = tmp;
        tmp->prev = currEntry;
        tmp = NULL;
        currEntry = currEntry->next;

        /* Parse TID into currEntry */
        char tmpTID[17];
        console_print(RED "parse TID\n" RESET);
        for (int i = 0; i < 16; i++)
        {
            char currDigit;
            int currInt = titleRecords[recordsEntry].application_id % 16;
            titleRecords[recordsEntry].application_id /= 16;
            if (currInt > 9)
            {
                currInt -= 10;
                currDigit = currInt + 'a';
            }
            else
            {
                currDigit = currInt + '0';
            }
            tmpTID[i] = currDigit;
        }
        console_print(RED "flip\n" RESET);
        /* Need to flip what we parsed */
        for (int i = 4; i < 8; i++)
        {
            currEntry->Data.TID[i - 4] = tmpTID[15 - i];
            currEntry->Data.TID[12 - i] = tmpTID[i - 1];
        }
        currEntry->Data.TID[4] = tmpTID[7];

        console_print(RED "check for update ended\n" RESET);
        /* Parse Version into currEntry */
        bool foundUpdate = false;
        for (int i = 0; i < *(metaLength + recordsEntry); i++)
        {
            if (currMeta[i].meta_type == NcmContentMetaType_Patch)
            {
                currEntry->Data.version = currMeta[i].version;
                i = *(metaLength + recordsEntry);
                foundUpdate = true;
            }
        }
        if (!foundUpdate)
        {
            currEntry->Data.version = 0;
        }

        /* Parse Name into currEntry */
        strncpy(currEntry->Data.name, titleName, 0x200);
        console_print(RED "%d/%d titleName: %s\n" RESET, recordsEntry, recordsLength, titleName);

        /* Parse DispVersion into currEntry */
        strncpy(currEntry->Data.displayVersion, titleDispVersion, 0x0F);

        /* Move to next record */
        recordsEntry++;
        free(tmp);
    }

    free(titleRecords);
    free(metaLength);
    free(metaStatusList);
    return localVerList;
}

/*! close a socket
 *
 *  @param[in] fd        socket to close
 *  @param[in] connected whether this socket is connected
 */
static void
http_closesocket(int fd,
                 bool connected)
{
    int rc;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    struct pollfd pollinfo;

    //  console_print("0x%X\n", socketGetLastBsdResult());

    if (connected)
    {
        /* get peer address and print */
        rc = getpeername(fd, (struct sockaddr*)&addr, &addrlen);
        if (rc != 0)
        {
            console_print(RED "getpeername: %d %s\n" RESET, errno, strerror(errno));
            console_print(YELLOW "closing connection to fd=%d\n" RESET, fd);
        }
        else
            console_print(YELLOW "closing connection to %s:%u\n" RESET,
                          inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        /* shutdown connection */
        rc = shutdown(fd, SHUT_WR);
        if (rc != 0)
            console_print(RED "shutdown: %d %s\n" RESET, errno, strerror(errno));

        /* wait for client to close connection */
        pollinfo.fd = fd;
        pollinfo.events = POLLIN;
        pollinfo.revents = 0;
        rc = poll(&pollinfo, 1, 250);
        if (rc < 0)
            console_print(RED "poll: %d %s\n" RESET, errno, strerror(errno));
    }

    /* set linger to 0 */
    struct linger linger;
    linger.l_onoff = 1;
    linger.l_linger = 0;
    rc = setsockopt(fd, SOL_SOCKET, SO_LINGER,
                    &linger, sizeof(linger));
    if (rc != 0)
        console_print(RED "setsockopt: SO_LINGER %d %s\n" RESET,
                      errno, strerror(errno));

    /* close socket */
    rc = close(fd);
    if (rc != 0)
        console_print(RED "close: %d %s\n" RESET, errno, strerror(errno));
}

char* concat(const char* s1, const char* s2)
{
    char* r = malloc(strlen(s1) + strlen(s2) + 1);
    strcpy(r, s1);
    strcat(r, s2);
    return r;
}

char* strappend(const char* s1, const char* s2)
{
    char* r = malloc(strlen(s1) + strlen(s2) + 1);
    strcpy(r, s1);
    strcat(r, s2);
    assert(strlen(r) == strlen(s1) + strlen(s2));
    return r;
}

char* read_text_from_socket(int sockfd)
{
    const int BUF_SIZE = 256;
    char* buffer = malloc(BUF_SIZE);

    char* result = malloc(1);
    result[0] = '\0';

    while (1)
    {
        int n = read(sockfd, buffer, BUF_SIZE - 1);
        if (n < 0)
        {
            break;
        }
        buffer[n] = '\0';
        char* last_result = result;
        result = strappend(last_result, buffer);
        free(last_result);
        if (n < BUF_SIZE - 1)
        {
            break;
        }
    }

    free(buffer);

    return result;
}

void http_write_line(int sockfd, const char* message)
{
    write(sockfd, message, strlen(message));
    write(sockfd, "\r\n", 2);
}

void http_write_content(int sockfd, const char* content)
{
    char length_str[20];
    sprintf(length_str, "%d", (int)strlen(content) + 2);

    char* content_length_str = concat("Content-Length: ", length_str);
    http_write_line(sockfd, "Server: sys-httpd/0.1.0");
    http_write_line(sockfd, "Content-Type: text/plain");
    http_write_line(sockfd, "Connection: close");
    http_write_line(sockfd, content_length_str);
    http_write_line(sockfd, "");
    http_write_line(sockfd, content);

    free(content_length_str);
}

/*! allocate new http request
 *
 *  @param[in] listen_fd socket to accept connection from
 */
static int
http_request_new(int listen_fd)
{
#ifdef DISABLEFTP
    ssize_t rc;
#endif
    int new_fd;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    const char* GET_STATUS = "GET /status";
    const char* GET_CMD_LIST = "GET /cmd/list";
    const char* GET_CMD_REBOOT = "GET /cmd/reboot";
    const char* GET_CMD_TITLES = "GET /cmd/titles";
    char resp[2048];

    /* accept connection */
    new_fd = accept(listen_fd, (struct sockaddr*)&addr, &addrlen);
    if (new_fd < 0)
    {
        console_print(RED "accept: %d %s\n" RESET, errno, strerror(errno));
        return -1;
    }
    flash_led_connect();

    // Read request
    char* buf = read_text_from_socket(new_fd);

    if (buf[0] != '\0')
    {
        console_print(CYAN "received req %s from %s:%u\n" RESET,
                      buf, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }
    else
    {
        console_print(RED "failed to receive from %s:%u\n" RESET,
                      inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        return -1;
    }

    if (strncmp(buf, GET_STATUS, strlen(GET_STATUS)) == 0)
    {
        http_write_line(new_fd, "HTTP/1.1 200 OK");
        http_write_content(new_fd, "ready for more requests!");
    }
    else if (strncmp(buf, GET_CMD_LIST, strlen(GET_CMD_LIST)) == 0)
    {
        http_write_line(new_fd, "HTTP/1.1 200 OK");
        sprintf(resp, "Command list:\n%s\n%s\n%s\n%s\n", GET_STATUS,
                GET_CMD_LIST, GET_CMD_REBOOT, GET_CMD_TITLES);
        http_write_content(new_fd, resp);
    }
    else if (strncmp(buf, GET_CMD_REBOOT, strlen(GET_CMD_REBOOT)) == 0)
    {

        http_write_line(new_fd, "HTTP/1.1 202 Accepted");
        http_write_content(new_fd, "rebooting...");
        bpcRebootSystem();
    }
    else if (strncmp(buf, GET_CMD_TITLES, strlen(GET_CMD_TITLES)) == 0)
    {
        Entry* localVerList;
        console_print(CYAN "starting populating version list" RESET);
        localVerList = initLocalVerList();
        console_print(CYAN "ended populating version list" RESET);
        Entry* currLocalEntry = localVerList->next;
        while (currLocalEntry != NULL)
        {
            console_print(CYAN "%s [0100%s000][%s][v%d]\n" RESET,
                          currLocalEntry->Data.name, currLocalEntry->Data.TID,
                          currLocalEntry->Data.displayVersion,
                          currLocalEntry->Data.version);
            sprintf(resp, "%s [0100%s000][%s][v%d]\n",
                    currLocalEntry->Data.name, currLocalEntry->Data.TID,
                    currLocalEntry->Data.displayVersion,
                    currLocalEntry->Data.version);
            currLocalEntry = currLocalEntry->next;
        }
        freeList(localVerList);
        http_write_line(new_fd, "HTTP/1.1 200 OK");
        http_write_content(new_fd, resp);
    }
    else
    {
        http_write_line(new_fd, "HTTP/1.1 404 Not Found");
        sprintf(resp, "Unknown command, use %s to get a list of valid commands\n", GET_CMD_LIST);
        http_write_content(new_fd, resp);
    }

    /* shutdown connection */
    int rc = shutdown(new_fd, SHUT_WR);
    if (rc != 0)
    {
        console_print(RED "shutdown: %d %s\n" RESET, errno, strerror(errno));
    }
    flash_led_disconnect();

    return 0;
}

/* Update free space in status bar */
static void
update_free_space(void)
{
#if defined(_3DS) || defined(__SWITCH__)
#    define KiB (1024.0)
#    define MiB (1024.0 * KiB)
#    define GiB (1024.0 * MiB)
    char buffer[16];
    struct statvfs st;
    double bytes_free;
    int rc, len;

    rc = statvfs("sdmc:/", &st);
    if (rc != 0)
        console_print(RED "statvfs: %d %s\n" RESET, errno, strerror(errno));
    else
    {
        bytes_free = (double)st.f_bsize * st.f_bfree;

        if (bytes_free < 1000.0)
            len = snprintf(buffer, sizeof(buffer), "%.0lfB", bytes_free);
        else if (bytes_free < 10.0 * KiB)
            len = snprintf(buffer, sizeof(buffer), "%.2lfKiB", floor((bytes_free * 100.0) / KiB) / 100.0);
        else if (bytes_free < 100.0 * KiB)
            len = snprintf(buffer, sizeof(buffer), "%.1lfKiB", floor((bytes_free * 10.0) / KiB) / 10.0);
        else if (bytes_free < 1000.0 * KiB)
            len = snprintf(buffer, sizeof(buffer), "%.0lfKiB", floor(bytes_free / KiB));
        else if (bytes_free < 10.0 * MiB)
            len = snprintf(buffer, sizeof(buffer), "%.2lfMiB", floor((bytes_free * 100.0) / MiB) / 100.0);
        else if (bytes_free < 100.0 * MiB)
            len = snprintf(buffer, sizeof(buffer), "%.1lfMiB", floor((bytes_free * 10.0) / MiB) / 10.0);
        else if (bytes_free < 1000.0 * MiB)
            len = snprintf(buffer, sizeof(buffer), "%.0lfMiB", floor(bytes_free / MiB));
        else if (bytes_free < 10.0 * GiB)
            len = snprintf(buffer, sizeof(buffer), "%.2lfGiB", floor((bytes_free * 100.0) / GiB) / 100.0);
        else if (bytes_free < 100.0 * GiB)
            len = snprintf(buffer, sizeof(buffer), "%.1lfGiB", floor((bytes_free * 10.0) / GiB) / 10.0);
        else
            len = snprintf(buffer, sizeof(buffer), "%.0lfGiB", floor(bytes_free / GiB));

        console_set_status("\x1b[0;%dH" GREEN "%s", 50 - len, buffer);
    }
#endif
}

/*! Update status bar */
static int
update_status(void)
{
#if defined(_3DS) || defined(__SWITCH__)
//  console_set_status("\n" GREEN STATUS_STRING " "
#    ifdef ENABLE_LOGGING
//                     "DEBUG "
#    endif
    //                    CYAN "%s:%u" RESET,
    //                  inet_ntoa(serv_addr.sin_addr),
    //                ntohs(serv_addr.sin_port));
    update_free_space();
#elif 0 //defined(__SWITCH__)
    char hostname[128];
    socklen_t addrlen = sizeof(serv_addr);
    int rc;
    rc = gethostname(hostname, sizeof(hostname));
    if (rc != 0)
    {
        console_print(RED "gethostname: %d %s\n" RESET, errno, strerror(errno));
        return -1;
    }
    console_set_status("\n" GREEN STATUS_STRING " test "
#    ifdef ENABLE_LOGGING
                       "DEBUG "
#    endif
                       CYAN "%s:%u" RESET,
                       hostname,
                       ntohs(serv_addr.sin_port));
    update_free_space();
#else
    char hostname[128];
    socklen_t addrlen = sizeof(serv_addr);
    int rc;

    rc = getsockname(listenfd, (struct sockaddr*)&serv_addr, &addrlen);
    if (rc != 0)
    {
        console_print(RED "getsockname: %d %s\n" RESET, errno, strerror(errno));
        return -1;
    }

    rc = gethostname(hostname, sizeof(hostname));
    if (rc != 0)
    {
        console_print(RED "gethostname: %d %s\n" RESET, errno, strerror(errno));
        return -1;
    }

    console_set_status(GREEN STATUS_STRING " "
#    ifdef ENABLE_LOGGING
                                           "DEBUG "
#    endif
                       YELLOW "IP:" CYAN "%s " YELLOW "Port:" CYAN "%u" RESET,
                       hostname,
                       ntohs(serv_addr.sin_port));
#endif

    return 0;
}

#ifdef _3DS
/*! Handle apt events
 *
 *  @param[in] type    Event type
 *  @param[in] closure Callback closure
 */
static void
apt_hook(APT_HookType type,
         void* closure)
{
    switch (type)
    {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
        /* turn on backlight, or you can't see the home menu! */
        if (R_SUCCEEDED(gspLcdInit()))
        {
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
            gspLcdExit();
        }
        break;

    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        /* restore backlight power state */
        if (R_SUCCEEDED(gspLcdInit()))
        {
            (lcd_power ? GSPLCD_PowerOnBacklight : GSPLCD_PowerOffBacklight)(GSPLCD_SCREEN_BOTH);
            gspLcdExit();
        }
        break;

    default:
        break;
    }
}
#elif defined(__SWITCH__)
/*! Handle applet events
 *
 *  @param[in] type    Event type
 *  @param[in] closure Callback closure
 */
static void
applet_hook(AppletHookType type,
            void* closure)
{
    (void)closure;
    (void)type;
    /* stubbed for now */
    switch (type)
    {
    default:
        break;
    }
}
#endif

void http_pre_init(void)
{
    start_time = time(NULL);

    /* register applet hook */
    appletHook(&cookie, applet_hook, NULL);
}

/*! initialize ftp subsystem */
int http_init(void)
{
    int rc = 0;

    /* allocate socket to listen for clients */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
    {
        console_print(RED "socket: %d %s\n" RESET, errno, strerror(errno));
        http_exit();
        return -1;
    }

    /* get address to listen on */
    serv_addr.sin_family = AF_INET;

    serv_addr.sin_addr.s_addr = INADDR_ANY;
    char str_port[100];
    ini_gets("Port", "port:", "dummy", str_port, sizearray(str_port), CONFIGPATH);
    LISTEN_PORT = atoi(str_port);
    serv_addr.sin_port = htons(LISTEN_PORT);

    /* reuse address */
    {
        int yes = 1;
        rc = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (rc != 0)
        {
            console_print(RED "setsockopt: %d %s\n" RESET, errno, strerror(errno));
            http_exit();
            return -1;
        }
    }

    /* bind socket to listen address */
    rc = bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if (rc != 0)
    {
        console_print(RED "bind: %d %s\n" RESET, errno, strerror(errno));
        http_exit();
        return -1;
    }

    /* listen on socket */
    rc = listen(listenfd, 5);
    if (rc != 0)
    {
        console_print(RED "listen: %d %s\n" RESET, errno, strerror(errno));
        http_exit();
        return -1;
    }

    /* print server address */
    rc = update_status();
    if (rc != 0)
    {
        http_exit();
        return -1;
    }

    return 0;
}

/*! deinitialize http subsystem */
void http_exit(void)
{
    debug_print("exiting http server\n");

    /* stop listening for new clients */
    if (listenfd >= 0)
        http_closesocket(listenfd, false);

    /* deinitialize socket driver */
    console_render();
    console_print(CYAN "Waiting for socketExit()...\n" RESET);
}

void http_post_exit(void)
{
}

/*! http loop
 *
 *  @returns whether to keep looping
 */
loop_status_t
http_loop(void)
{
    int rc;
    struct pollfd pollinfo;

    /* we will poll for new client connections */
    pollinfo.fd = listenfd;
    pollinfo.events = POLLIN;
    pollinfo.revents = 0;

    /* poll for a new client */
    rc = poll(&pollinfo, 1, 0);
    if (rc < 0)
    {
        /* wifi got disabled */
        console_print(RED "poll: FAILED!\n" RESET);

        if (errno == ENETDOWN)
            return LOOP_RESTART;

        console_print(RED "poll: %d %s\n" RESET, errno, strerror(errno));
        return LOOP_EXIT;
    }
    else if (rc > 0)
    {
        if (pollinfo.revents & POLLIN)
        {
            /* we got a new client */
            if (http_request_new(listenfd) != 0)
            {
                return LOOP_RESTART;
            }
        }
        else
        {
            console_print(YELLOW "listenfd: revents=0x%08X\n" RESET, pollinfo.revents);
        }
    }

#ifdef DISABLEFTP
    /* poll each session */
    session = sessions;
    while (session != NULL)
        session = ftp_session_poll(session);
#endif

#ifdef _3DS
    /* check if the user wants to exit */
    hidScanInput();
    u32 down = hidKeysDown();

    if (down & KEY_B)
        return LOOP_EXIT;

    /* check if the user wants to toggle the LCD power */
    if (down & KEY_START)
    {
        lcd_power = !lcd_power;
        apt_hook(APTHOOK_ONRESTORE, NULL);
    }
#elif defined(__SWITCH__)
    /* check if the user wants to exit */
#endif

    return LOOP_CONTINUE;
}