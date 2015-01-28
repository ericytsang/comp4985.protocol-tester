#include "Server.h"

static char debugString[1000];

struct EventAcceptThreadParams
{
    HANDLE eventHandle;
    SOCKET serverSocket;
    SOCKET* newSocket;
    sockaddr* client;
    int* clientLength;
};

typedef struct EventAcceptThreadParams EventAcceptThreadParams;

// thread functions
static DWORD WINAPI serverThread(void*);
static DWORD WINAPI eventAcceptThread(void*);

// other functions...
static HANDLE eventAccept(HANDLE, SOCKET, SOCKET*, sockaddr*, int*);

// initializes a server structure
void serverInit(Server* server, short protocolFamily,
    unsigned short port , unsigned long remoteInetAddr)
{
    memset(&server->_server, 0, sizeof(sockaddr_in));
    server->_server.sin_family      = protocolFamily;
    server->_server.sin_port        = htons(port);
    server->_server.sin_addr.s_addr = htonl(remoteInetAddr);

    server->_onConnect      = 0;
    server->_onError        = 0;

    server->_stopEvent      = CreateEvent(NULL, TRUE, FALSE, NULL);
    server->_serverThread   = INVALID_HANDLE_VALUE;
}

// starts the server if it is not already started
int serverStart(Server* server)
{
    DWORD threadId;     // useless...

    // make sure server isn't already running
    if(server->_serverThread != INVALID_HANDLE_VALUE)
    {
        return SERVER_ALREADY_RUNNING_FAIL;
    }

    // start the server
    ResetEvent(server->_stopEvent);
    server->_serverThread =
        CreateThread(NULL, 0, serverThread, server, 0, &threadId);
    if(server->_serverThread == INVALID_HANDLE_VALUE)
    {
        server->_onError(server, THREAD_FAIL, 0, 0);
        return THREAD_FAIL;
    }

    return NORMAL_SUCCESS;
}

// requests server to close listening socket, but not accepted sockets
int serverStop(Server* server)
{
    // make sure server is already running
    if(server->_serverThread == INVALID_HANDLE_VALUE)
    {
        return SERVER_ALREADY_STOPPED_FAIL;
    }

    // signal server thread to stop
    SetEvent(server->_stopEvent);

    // forget about the server thread
    CloseHandle(server->_serverThread);
    server->_serverThread = INVALID_HANDLE_VALUE;

    return NORMAL_SUCCESS;
}

// sets the onConnect function callback of the server
void serverSetOnConnect(Server* server, void(*onConnect)(Server*, SOCKET))
{
    server->_onConnect = onConnect;
}

// sets the onError function callback of the server
void serverSetOnError(Server* server, void(*onError)(Server*, int, void*, int))
{
    server->_onError = onError;
}

// sets the onClose funcation callback of the server
void serverSetOnClose(Server* server, void(*onClose)(Server*, int, void*, int))
{
    server->_onClose = onClose;
}

// server's thread that is used to continuously accept connections
static DWORD WINAPI serverThread(void* params)
{
    Server* server;

    // threads and synchronization
    HANDLE threadHandle;
    HANDLE handles[2];
    HANDLE acceptEvent;

    // sockets and stuff
    int clientLength;
    SOCKET clientSocket;
    sockaddr_in clientAddress;

    SOCKET serverSocket;

    // initialize variables
    server = (Server*) params;
    acceptEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // create a stream socket
    serverSocket = socket(server->_server.sin_family, SOCK_STREAM, 0);
    if(serverSocket == -1)
    {
        sprintf_s(debugString, "Error @ 0: %d\n", GetLastError());
        OutputDebugString(debugString);
        server->_onError(server, SOCKET_FAIL, 0, 0);
        return SOCKET_FAIL;
    }

    // Bind an address to the socket
    if(bind(serverSocket, (sockaddr*) &server->_server, sizeof(server->_server)) == -1)
    {
        sprintf_s(debugString, "Error @ 1: %d\n", GetLastError());
        OutputDebugString(debugString);
        server->_onError(server, BIND_FAIL, 0, 0);
        return BIND_FAIL;
    }

    // listen for connections
    listen(serverSocket, 5);     // queue up to 5 connect requests

    // continuously accept connections
    while(TRUE)
    {
        clientLength = sizeof(clientAddress);
        threadHandle = eventAccept(acceptEvent, serverSocket, &clientSocket,
            (sockaddr*) &clientAddress, &clientLength);
        if(threadHandle == INVALID_HANDLE_VALUE)
        {
            sprintf_s(debugString, "Accept Error @ 2: %d\n", GetLastError());
            OutputDebugString(debugString);
            server->_onError(server, THREAD_FAIL, 0, 0);
            return THREAD_FAIL;
        }

        // wait for something to happen
        ResetEvent(acceptEvent);
        handles[0] = acceptEvent;  // signaled when accept returns
        handles[1] = server->_stopEvent;    // signaled to stop the server
        DWORD waitResult =
            WaitForMultipleObjects(sizeof(handles) / sizeof(HANDLE), handles, FALSE, INFINITE);
        switch(waitResult)
        {

            // accept event signaled; handle it
            case WAIT_OBJECT_0+0:
            if(clientSocket == -1)
            {   // handle error
                sprintf_s(debugString, "Error @ 3: %d\n", GetLastError());
                OutputDebugString(debugString);
                server->_onError(server, ACCEPT_FAIL, 0, 0);
                server->_onClose(server, ACCEPT_FAIL, 0, 0);
                closesocket(serverSocket);
                WaitForSingleObject(threadHandle, INFINITE);
                return ACCEPT_FAIL;
            }
            else
            {   // handle new connection
                server->_onConnect(server, clientSocket);
                sprintf_s(debugString, "Remote Address:  %s\n", inet_ntoa(clientAddress.sin_addr));
                OutputDebugString(debugString);
            }
            break;

            // stop event signaled; stop the server
            case WAIT_OBJECT_0+1:
            server->_onClose(server, NORMAL_SUCCESS, 0, 0);
            closesocket(serverSocket);
            WaitForSingleObject(threadHandle, INFINITE);
            return 0;

            // some sort of something; report error
            default:
            sprintf_s(debugString, "Error @ 4: %d\n", GetLastError());
            OutputDebugString(debugString);
            server->_onError(server, ACCEPT_FAIL, 0, 0);
            server->_onClose(server, ACCEPT_FAIL, 0, 0);
            closesocket(serverSocket);
            WaitForSingleObject(threadHandle, INFINITE);
            return UNKNOWN_FAIL;
        }
    }
}

// signals the event when the accept call finishes...newSocket is what accept returns
// returns -1 if the thread could not be created..0 otherwise
static HANDLE eventAccept(HANDLE eventHandle, SOCKET serverSocket,
    SOCKET* newSocket, sockaddr* client, int* clientLength)
{
    EventAcceptThreadParams* threadParams;
    DWORD threadId;
    HANDLE threadHandle;
    int ret;

    // prepare thread parameters
    threadParams = (EventAcceptThreadParams*)
        malloc(sizeof(EventAcceptThreadParams));
    threadParams->eventHandle   = eventHandle;
    threadParams->serverSocket  = serverSocket;
    threadParams->newSocket     = newSocket;
    threadParams->client        = client;
    threadParams->clientLength  = clientLength;

    // make the thread to make asynchronous call
    threadHandle =
        CreateThread(NULL, 0, eventAcceptThread, threadParams, 0, &threadId);

    return threadHandle;
}

static DWORD WINAPI eventAcceptThread(void* params)
{
    // parse thread params
    EventAcceptThreadParams* threadParams = (EventAcceptThreadParams*) params;
    HANDLE eventHandle  = threadParams->eventHandle;
    SOCKET serverSocket = threadParams->serverSocket;
    SOCKET* newSocket   = threadParams->newSocket;
    sockaddr* client    = threadParams->client;
    int* clientLength   = threadParams->clientLength;

    // make the accept call
    *newSocket = accept(serverSocket, client, clientLength);

    // trigger the signal, because we're no longer blocked by accept
    SetEvent(eventHandle);

    // clean up and return
    free(threadParams);
    return 0;
}
