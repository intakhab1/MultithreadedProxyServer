#include <iostream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctime>
#include <pthread.h>
#include <semaphore.h>
#include <cstring>
#include <sstream>
#include <algorithm>

#define MAX_BYTES 4096
#define MAX_CLIENTS 400

struct ParsedRequest {
    std::string method;
    std::string protocol;
    std::string host;
    std::string port;
    std::string path;
    std::string version;
    
    struct Header {
        std::string key;
        std::string value;
    };
    std::vector<Header> headers;
};

// Semaphore wrapper for macOS
class Semaphore {
    sem_t sem;
public:
    Semaphore(int value) { sem_init(&sem, 0, value); }
    ~Semaphore() { sem_destroy(&sem); }
    void wait() { sem_wait(&sem); }
    void post() { sem_post(&sem); }
    int getvalue() { 
        int val; 
        sem_getvalue(&sem, &val); 
        return val;
    }
};

int port_number = 8081;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
Semaphore seamaphore(MAX_CLIENTS);
int Connected_socketId[MAX_CLIENTS];

int sendErrorMessage(int socket, int status_code) {
    std::string str;
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code) {
        case 400: 
            str = "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " + 
                  std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>";
            std::cout << "400 Bad Request" << std::endl;
            send(socket, str.c_str(), str.length(), 0);
            break;

        case 403:
            str = "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: " + 
            std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>";
            std::cout << "403 Forbidden" << std::endl;
            send(socket, str.c_str(), str.length(), 0);
            break;

        case 404:
            str = "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: " + 
            std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>";
            std::cout << "404 Not Found" << std::endl;
            send(socket, str.c_str(), str.length(), 0);
            break;

        case 500:
            str = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " + 
            std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>";
            send(socket, str.c_str(), str.length(), 0);
            break;

        case 501:
            str = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " + 
            std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>";
            std::cout << "501 Not Implemented" << std::endl;
            send(socket, str.c_str(), str.length(), 0);
            break;

        case 505:
            str = "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " + 
            std::string(currentTime) + "\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>";
            std::cout << "505 HTTP Version Not Supported" << std::endl;
            send(socket, str.c_str(), str.length(), 0);
            break;

        default:  
            return -1;
    }
    return 1;
}

int connectRemoteServer(const std::string& host_addr, int port_num) {
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket < 0) {
        perror("socket creation failed");
        return -1;
    }

    struct hostent* host = gethostbyname(host_addr.c_str());
    if (!host) {
        std::cerr << "No such host exists: " << host_addr << std::endl;
        herror("gethostbyname");
        close(remoteSocket);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    memcpy(&server_addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (connect(remoteSocket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        close(remoteSocket);
        return -1;
    }
    return remoteSocket;
}

int ParsedRequest_parse(ParsedRequest* pr, const char* buf, int buflen) {
    std::string request(buf, buflen);
    std::istringstream iss(request);
    std::string line;
    
    // Parse request line
    if (!std::getline(iss, line)) return -1;
    std::istringstream request_line(line);
    if (!(request_line >> pr->method >> pr->protocol >> pr->version)) return -1;

    // Handle absolute URI (e.g., "http://google.com/")
    if (pr->protocol.find("http://") == 0) {
        size_t proto_end = 7; // length of "http://"
        std::string rest = pr->protocol.substr(proto_end);
        
        size_t slash_pos = rest.find('/');
        if (slash_pos == std::string::npos) {
            pr->host = rest;
            pr->path = "/";
        } else {
            pr->host = rest.substr(0, slash_pos);
            pr->path = rest.substr(slash_pos);
        }
        
        // Handle port if specified (e.g., "http://google.com:8080/")
        size_t colon_pos = pr->host.find(':');
        if (colon_pos != std::string::npos) {
            pr->port = pr->host.substr(colon_pos + 1);
            pr->host = pr->host.substr(0, colon_pos);
        }
    }

    // Parse headers
    while (std::getline(iss, line)) {
        if (line.empty() || line == "\r") break;
        
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim whitespace
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](int ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](int ch) { return !std::isspace(ch); }).base(), value.end());
        
        // Special handling for Host header
        if (key == "Host" && pr->host.empty()) {
            pr->host = value;
            // Handle port in Host header (e.g., "Host: google.com:8080")
            size_t colon_pos = pr->host.find(':');
            if (colon_pos != std::string::npos) {
                pr->port = pr->host.substr(colon_pos + 1);
                pr->host = pr->host.substr(0, colon_pos);
            }
        }
        
        pr->headers.push_back({key, value});
    }
    return 0;
}

int ParsedRequest_unparse_headers(ParsedRequest* pr, char* buf, size_t buflen) {
    std::string headers;
    for (const auto& h : pr->headers) {
        headers += h.key + ": " + h.value + "\r\n";
    }
    headers += "\r\n";
    
    if (headers.size() > buflen) return -1;
    memcpy(buf, headers.c_str(), headers.size());
    return 0;
}

int ParsedHeader_set(ParsedRequest* pr, const char* key, const char* value) {
    for (auto& h : pr->headers) {
        if (h.key == key) {
            h.value = value;
            return 0;
        }
    }
    pr->headers.push_back({key, value});
    return 0;
}

ParsedRequest::Header* ParsedHeader_get(ParsedRequest* pr, const char* key) {
    for (auto& h : pr->headers) {
        if (h.key == key) return &h;
    }
    return nullptr;
}

ParsedRequest* ParsedRequest_create() {
    return new ParsedRequest();
}

void ParsedRequest_destroy(ParsedRequest* pr) {
    delete pr;
}

int handle_request(int clientSocket, ParsedRequest* request, char* buf) {
    bzero(buf, MAX_BYTES);
    
    // Use only the path for the forwarded request (remove protocol/host)
    strcpy(buf, "GET ");
    strcat(buf, request->path.c_str());
    strcat(buf, " ");
    strcat(buf, "HTTP/1.1\r\n"); // Always use HTTP/1.1 for forwarded requests
    
    size_t len = strlen(buf);

    // Set required headers
    if (ParsedHeader_set(request, "Connection", "close") < 0) {
        std::cerr << "Failed to set Connection header" << std::endl;
    }

    // Ensure Host header is set
    if (!ParsedHeader_get(request, "Host") && !request->host.empty()) {
        std::string host_header = request->host;
        if (!request->port.empty() && request->port != "80") {
            host_header += ":" + request->port;
        }
        if (ParsedHeader_set(request, "Host", host_header.c_str()) < 0) {
            std::cerr << "Failed to set Host header" << std::endl;
        }
    }

    // Copy other headers
    if (ParsedRequest_unparse_headers(request, buf + len, MAX_BYTES - len) < 0) {
        std::cerr << "Failed to unparse headers" << std::endl;
        return -1;
    }

    // Determine port
    int server_port = 80;
    if (!request->port.empty()) {
        try {
            server_port = std::stoi(request->port);
        } catch (...) {
            std::cerr << "Invalid port number: " << request->port << std::endl;
            return -1;
        }
    }

    // Connect to remote server
    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0) {
        std::cerr << "Failed to connect to remote server" << std::endl;
        return -1;
    }

    // Send request
    int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);
    if (bytes_send < 0) {
        std::cerr << "Failed to send request to remote server" << std::endl;
        close(remoteSocketID);
        return -1;
    }

    // Forward response
    bzero(buf, MAX_BYTES);
    bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    while (bytes_send > 0) {
        int sent = send(clientSocket, buf, bytes_send, 0);
        if (sent < 0) {
            std::cerr << "Failed to send data to client" << std::endl;
            close(remoteSocketID);
            return -1;
        }
        bzero(buf, MAX_BYTES);
        bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    }

    close(remoteSocketID);
    return 0;
}

void* thread_fn(void* socketNew) {
    int* t = (int*)(socketNew);
    int socket = *t;
    seamaphore.wait();
    
    char buffer[MAX_BYTES] = {0};
    int bytes_recv = recv(socket, buffer, MAX_BYTES, 0);
    
    std::cout << "Received " << bytes_recv << " bytes from client" << std::endl;
    if (bytes_recv > 0) {
        std::cout << "Request data:\n" << std::string(buffer, bytes_recv) << std::endl;
    }

    while (bytes_recv > 0) {
        size_t len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL) {
            bytes_recv = recv(socket, buffer + len, MAX_BYTES - len, 0);
            std::cout << "Received additional " << bytes_recv << " bytes" << std::endl;
        } else {
            break;
        }
    }

    if (bytes_recv > 0) {
        std::cout << "Complete request received" << std::endl;
        ParsedRequest* request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, strlen(buffer)) < 0) {
            std::cerr << "Parsing failed" << std::endl;
            sendErrorMessage(socket, 400);
        } else {
            std::cout << "Parsed request - Method: " << request->method 
                      << ", Host: " << request->host 
                      << ", Path: " << request->path << std::endl;
            if (request->method == "GET") {
                if (!request->host.empty() && !request->path.empty()) {
                    if (handle_request(socket, request, buffer) < 0) {
                        sendErrorMessage(socket, 502);
                    }
                } else {
                    sendErrorMessage(socket, 400);
                }
            } else {
                sendErrorMessage(socket, 501);
            }
        }
        ParsedRequest_destroy(request);
    } else {
        std::cerr << "No data received or connection closed" << std::endl;
    }

    close(socket);
    seamaphore.post();
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    port_number = std::stoi(argv[1]);

    std::cout << "Starting proxy server..." << std::endl;
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0) {
        perror("Socket creation failed");
        std::cerr << "ERR: Cannot create socket (errno: " << errno << ")" << std::endl;
        exit(EXIT_FAILURE);
    }
    std::cout << "Socket created successfully" << std::endl;

    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt failed");
        std::cerr << "WARN: Cannot set SO_REUSEADDR (continuing anyway)" << std::endl;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    std::cout << "Attempting to bind to port " << port_number << "..." << std::endl;
    if (bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        std::cerr << "ERR: Cannot bind to port " << port_number 
                << " (errno: " << errno << ")" << std::endl;
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }
    std::cout << "Successfully bound to port " << port_number << std::endl;

    std::cout << "Starting to listen..." << std::endl;
    if (listen(proxy_socketId, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        std::cerr << "ERR: Cannot listen on socket (errno: " << errno << ")" << std::endl;
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }
    std::cout << "Server is now listening on port " << port_number << std::endl;

    int i = 0;
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socketId < 0) {
            std::cout << "Accept failed" << std::endl;
            continue;
        }

        Connected_socketId[i] = client_socketId;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, str, INET_ADDRSTRLEN);
        std::cout << "Client connected: " << str << ":" << ntohs(client_addr.sin_port) << std::endl;

        pthread_create(&tid[i], NULL, thread_fn, &Connected_socketId[i]);
        i = (i + 1) % MAX_CLIENTS;
    }

    close(proxy_socketId);
    return 0;
}