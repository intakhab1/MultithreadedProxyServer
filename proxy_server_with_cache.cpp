#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctime>
#include <pthread.h>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>

#define MAX_BYTES 4096  // 4KB max allowed size of request/response
#define MAX_CLIENTS 400 // Max concurrent clients
#define MAX_CACHE_SIZE (200 * (1 << 20)) // 200MB
#define MAX_ELEMENT_SIZE (10 * (1 << 20)) // 10MB

class ParsedRequest {
public:
    std::string method;
    std::string protocol;
    std::string host;
    std::string port;
    std::string path;
    std::string version;
    std::string buf;
    
    struct Header {
        std::string key;
        std::string value;
    };
    std::vector<Header> headers;

    static std::shared_ptr<ParsedRequest> create() {
        return std::make_shared<ParsedRequest>();
    }

    static int parse(std::shared_ptr<ParsedRequest> pr, const char* buf, int buflen) {
        if (buflen < 4 || buflen > 65535) return -1;

        std::string request(buf, buflen);
        size_t end_of_headers = request.find("\r\n\r\n");
        if (end_of_headers == std::string::npos) return -1;

        std::istringstream iss(request.substr(0, end_of_headers));
        std::string line;
        
        // Parse request line
        if (!std::getline(iss, line)) return -1;
        std::istringstream request_line(line);
        if (!(request_line >> pr->method >> pr->protocol >> pr->version)) return -1;

        // Parse protocol (http://host:port/path)
        size_t proto_end = pr->protocol.find("://");
        if (proto_end == std::string::npos) return -1;
        
        std::string rest = pr->protocol.substr(proto_end + 3);
        pr->protocol = pr->protocol.substr(0, proto_end);

        size_t slash_pos = rest.find('/');
        if (slash_pos == std::string::npos) {
            pr->host = rest;
            pr->path = "/";
        } else {
            pr->host = rest.substr(0, slash_pos);
            pr->path = rest.substr(slash_pos);
        }

        // Parse port if present
        size_t colon_pos = pr->host.find(':');
        if (colon_pos != std::string::npos) {
            pr->port = pr->host.substr(colon_pos + 1);
            pr->host = pr->host.substr(0, colon_pos);
        }

        // Parse headers
        while (std::getline(iss, line) && line != "\r") {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // Trim whitespace
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](int ch) { return !std::isspace(ch); }));
            value.erase(std::find_if(value.rbegin(), value.rend(), [](int ch) { return !std::isspace(ch); }).base(), value.end());
            
            setHeader(pr, key, value);
        }

        return 0;
    }

    static int unparse_headers(std::shared_ptr<ParsedRequest> pr, char* buf, size_t buflen) {
        std::string headers_str;
        for (const auto& header : pr->headers) {
            headers_str += header.key + ": " + header.value + "\r\n";
        }
        headers_str += "\r\n";

        if (headers_str.size() > buflen) return -1;
        std::copy(headers_str.begin(), headers_str.end(), buf);
        return 0;
    }

    static Header* getHeader(std::shared_ptr<ParsedRequest> pr, const std::string& key) {
        for (auto& header : pr->headers) {
            if (header.key == key) return &header;
        }
        return nullptr;
    }

    static int setHeader(std::shared_ptr<ParsedRequest> pr, const std::string& key, const std::string& value) {
        auto header = getHeader(pr, key);
        if (header) {
            header->value = value;
        } else {
            pr->headers.push_back({key, value});
        }
        return 0;
    }
};

struct CacheEntry {
    std::string data;           // Cached response data
    std::string url;            // Original request URL
    time_t last_accessed;       // LRU timestamp
    std::shared_ptr<CacheEntry> next;   // Linked list pointer
};

// ====================== UTILITY FUNCTION ======================
std::string get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt = *std::localtime(&timer);
    
    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S") << '.' 
        << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
void log_event(const std::string& message) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "[" << get_current_timestamp() << "] " << message << "\n";
}

class LoggedMutex {
    std::mutex mtx;
    std::string name;
public:
    LoggedMutex(const std::string& name = "cache") : name(name) {}
    void lock() {
        auto wait_start = std::chrono::steady_clock::now();
        std::cout << "[Mutex] Waiting to Lock " << name << " at " << get_current_timestamp() << "\n";
        mtx.lock();
        auto acquired = std::chrono::steady_clock::now();
        auto wait_dur = std::chrono::duration_cast<std::chrono::milliseconds>(acquired - wait_start);
        std::cout << "[Mutex] Acquired Lock on " << name << " at " << get_current_timestamp() << " (" << wait_dur.count() << "ms wait)\n";
        lock_time = acquired; // Store for hold duration
    }
    void unlock() {
        auto released = std::chrono::steady_clock::now();
        auto hold_dur = std::chrono::duration_cast<std::chrono::milliseconds>(released - lock_time);
        mtx.unlock();
        std::cout << "[Mutex] Released Lock from " << name << " at " << get_current_timestamp() << " (held for " << hold_dur.count() << "ms)\n";
    }
private:
    std::chrono::steady_clock::time_point lock_time;
};

class ProxyServer {
private:
    int port;                   // Proxy listening port
    int server_fd;              // Server socket file descriptor
    std::mutex sem_mutex;          // Limits concurrent clients
    std::condition_variable sem_cond;
    LoggedMutex cache_mutex{"cache"};    // Protects cache operations
    std::shared_ptr<CacheEntry> cache_head; // LRU cache linked list head
    size_t cache_size = 0;      // Current cache size in bytes
    int sem_count = MAX_CLIENTS;

    std::atomic<int> active_connections{0};
    std::atomic<long> total_connections{0};
    
    void log_connection(const std::string& event, const std::string& client_ip, int client_port, long duration_ms = -1) {
        std::string msg = "Connection " + event + " with the Client having, Client IP address : " + client_ip + " and Client Port number : " + std::to_string(client_port);
        if (duration_ms >= 0) {
            msg += " (Duration: " + std::to_string(duration_ms) + "ms)";
        }
        msg += ", Active: " + std::to_string(active_connections.load());
        log_event(msg);
    }

    int getSemaphoreValue() {
        std::lock_guard<std::mutex> lock(sem_mutex);
        return sem_count;
    }
    
    void sem_wait() {
        std::unique_lock<std::mutex> lock(sem_mutex);
        sem_cond.wait(lock, [this]{ return sem_count > 0; });
        sem_count--;
    }
    
    void sem_post() {
        std::lock_guard<std::mutex> lock(sem_mutex);
        sem_count++;
        sem_cond.notify_one();
    }

    // Returns HTTP-formatted timestamp (e.g., Fri, 01 Jan 2023 12:00:00 GMT)
    std::string getCurrentTime() {
        char buf[50];
        time_t now = time(0);
        struct tm tm = *gmtime(&now);
        strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %Z", &tm);
        return buf;
    }

    // Sends HTTP error responses (e.g., 400 Bad Request)
    void sendError(int client_fd, int code, const std::string& message) {
        std::string response = "HTTP/1.1 " + std::to_string(code) + " " + message + "\r\n";
        response += "Content-Length: " + std::to_string(message.length() + 2) + "\r\n";
        response += "Connection: close\r\n";
        response += "Content-Type: text/html\r\n";
        response += "Date: " + getCurrentTime() + "\r\n";
        response += "Server: ProxyServer/1.0\r\n\r\n";
        response += "<h1>" + std::to_string(code) + " " + message + "</h1>";
        send(client_fd, response.c_str(), response.length(), 0);
    }

    // CACHE MISS --> Establishing TCP connection to the Original server ( ex: google.com server ).
    int connectToServer(const std::string& host, int port) {

	    // Creating Socket for remote server ---------------------------
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0){
            std::cout<<"Error in creating scocket" << std::endl;
            return -1;
        } 

    	// Get host by the name or ip address provided
        struct hostent* server = gethostbyname(host.c_str());
        if (!server){
            std::cout<<"No such host exists" << std::endl;
            return -1;
        }

	    // inserts ip address and port number of host in struct `server_addr`
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

	    // Connect to Remote server ----------------------------------------------------
        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr))) {
            std::cout<<"Error in connecting" << std::endl;
            return -1;
        }
        // free(host_addr);
        return sock;
    }

    // Checks if a URL exists in cache (LRU order). then Updates access time and moves entry to front.
    std::shared_ptr<CacheEntry> findInCache(const std::string& url) {
        std::lock_guard<LoggedMutex> lock(cache_mutex); // Auto-logs
        std::cout << "[Cache] Searching the Cache for: " << url << "\n";
        auto current = cache_head;
        auto prev = cache_head;

        while (current) {
            if (current->url == url) {
                // Update LRU time ( Update LRU: Move to front)
                current->last_accessed = time(0);
                
                // Move to front if not already
                if (current != cache_head) {
                    prev->next = current->next;
                    current->next = cache_head;
                    cache_head = current;
                }
                return current; // Cache HIT
            }
            prev = current;
            current = current->next;
        }
        return nullptr; // Cache MISS
    }

    // Adding a new entry, evicting old entries if exceeding MAX_CACHE_SIZE.
    void addToCache(const std::string& data, const std::string& url) {
        if (data.size() > MAX_ELEMENT_SIZE) return;

        std::cout << "[Cache] Attempting to add the data in Cache: " << url << " (" << data.size() << " bytes)\n";
        std::lock_guard<LoggedMutex> lock(cache_mutex); // Auto-logs

        // Logging before eviction:
        if (cache_size + data.size() > MAX_CACHE_SIZE) {
            std::cout << "Evicting old Cache entries (current size: " 
                    << cache_size << " bytes, need to free " 
                    << (cache_size + data.size() - MAX_CACHE_SIZE) 
                    << " bytes)" << std::endl;
        }
        
        // Remove old entries if needed
        while (cache_size + data.size() > MAX_CACHE_SIZE && cache_head) {
            auto to_remove = cache_head;
            cache_head = cache_head->next;
            cache_size -= to_remove->data.size();

            // Evicted cache entry
            std::cout << "Evicted: " << to_remove->url 
            << " (" << to_remove->data.size() << " bytes)" << std::endl;
        }

        auto entry = std::make_shared<CacheEntry>();
        entry->data = data;
        entry->url = url;
        entry->last_accessed = time(0);
        entry->next = cache_head;
        cache_head = entry;
        cache_size += data.size();

        // New cache size
        std::cout << "Added data to cache: " << url << " (" << data.size() 
        << " bytes). Total cache size: " << cache_size << " bytes" << std::endl;
    }

    // HandleRequest -> It Checks cache, forwards requests, and caches responses.
    int handleRequest(int client_fd, std::shared_ptr<ParsedRequest> request, const std::string& original_request) {
        // Checking the cache first
        auto cached = findInCache(original_request);

        // Logging cache hit or miss
        if (cached) {
            std::cout << "Cache HIT and Data retrieved for URL: " << request->host << request->path << std::endl;
        } else {
            std::cout << "Cache MISS, Data not found in Cache for URL: " << request->host << request->path << std::endl;
        }

        // CACHE HIT : Skip origin server ( Cache Hit --> Send Cached Data --> Close Connection )
        if (cached) {
            send(client_fd, cached->data.c_str(), cached->data.size(), 0);
            return 0;
        }

        // CACHE MISS → Forward request to remote server
        // 1 - DETERMINE Origin Server Port
        int server_port = 80; // Default port of HTTP is 80 , Overrides if URL contains explicit port (e.g., http://example.com:8080)
        if (!request->port.empty()) {
            server_port = std::stoi(request->port);
        }
        // 2 - CONNECT to Origin Server ( ex: google.com server )
        int server_fd = connectToServer(request->host, server_port);
        if (server_fd < 0) {
            sendError(client_fd, 502, "Bad Gateway");
            return -1;
        }

        // 3 - BUILD REQUEST
        // Converts GET http://example.com/ HTTP/1.1 → GET / HTTP/1.1
        std::string req_str = request->method + " " + request->path + " " + request->version + "\r\n";
        ParsedRequest::setHeader(request, "Connection", "close");
        if (!ParsedRequest::getHeader(request, "Host")) {
            ParsedRequest::setHeader(request, "Host", request->host);
        }

        char headers_buf[MAX_BYTES];
        // converts headers from vector<Header> to raw HTTP format:
            // Host: example.com
            // Connection: close
        if (ParsedRequest::unparse_headers(request, headers_buf, MAX_BYTES) < 0) {
            sendError(client_fd, 500, "Internal Server Error");  // Returns 500 (buffer overflow)
            close(server_fd);
            return -1;
        }
        req_str += headers_buf;

        // 4 - SEND REQUEST to Origin server
        if (send(server_fd, req_str.c_str(), req_str.size(), 0) < 0) {
            close(server_fd);
            return -1;
        }

        // 5 - FORWARD RESPONSE from Origin server to client
        // Stream Response : Chunked Transfer from Origin server to client
        // Memory Efficiency:
        // Processes data in 4KB chunks (MAX_BYTES)
        // Avoids loading entire response into memory
        char buffer[MAX_BYTES];
        std::string response_data;
        int bytes_read;
        
        while ((bytes_read = recv(server_fd, buffer, MAX_BYTES, 0)) > 0) {
            send(client_fd, buffer, bytes_read, 0);
            response_data.append(buffer, bytes_read);
        }

        // 6 - CACHE RESPONSE from Origin server if successful
        // Cache Criteria:
            // Only caches non-empty responses
            // Validates size against MAX_ELEMENT_SIZE (10MB)
            // Performs LRU eviction if needed
        if (!response_data.empty()) {
            addToCache(response_data, original_request);
        }

        close(server_fd);
        return 0;
    }

    //clientHandler -> Acts as the bridge between the pthread_create() call and handleClient logic
    static void* clientHandler(void* arg) {
        auto* ctx = static_cast<std::pair<ProxyServer*, int>*>(arg);
        ctx->first->handleClient(ctx->second);
        delete ctx;
        return nullptr;
    }
    
    void printSemaphoreStatus(const std::string& context = "") {
        std::cout << "[Semaphore] ";
        if (!context.empty()) {
            std::cout << context << " - ";
        }
        std::cout << "Available connections: " << getSemaphoreValue() 
                  << "/" << MAX_CLIENTS << std::endl;
    }
    
    // WORKER THREAD creation ( Limited to 400 concurrent threads by the semaphore ), Each handles one client connection, Automatically cleaned up via pthread_detach()
    void handleClient(int client_fd) {
        // Get client info
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_fd, (sockaddr*)&client_addr, &addr_len);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        // Connection start
        auto start = std::chrono::steady_clock::now();
        active_connections++;
        total_connections++;
        log_connection("opened", client_ip, client_port);

        try {
            // ... PROCESS REQUEST ...
            printSemaphoreStatus("Client connecting");
            sem_wait();   // WAIT ( Block if full i.e >400 concurrent clients )
            printSemaphoreStatus("After acquiring semaphore");
            // REQUEST READING
            // 1 - Buffer Initialization
            char buffer[MAX_BYTES] = {0};
            std::string request;
            int bytes_read;

            // 2 - Read complete request (until \r\n\r\n)
            while ((bytes_read = recv(client_fd, buffer, MAX_BYTES, 0)) > 0) {
                request.append(buffer, bytes_read);
                if (request.find("\r\n\r\n") != std::string::npos) break;
                memset(buffer, 0, MAX_BYTES);  // memset prevents data leakage between reads
            }
            // Read Failure:
            // A - Client disconnect (bytes_read == 0)
            // B - Socket error (bytes_read == -1)
            if (bytes_read <= 0) {
                close(client_fd);
                sem_post();
                return;
            }

            // HTTP Parsing
            auto parsed = ParsedRequest::create();
            // Parse HTTP
                // Example of a parsed data:
                // method="GET", host="example.com", path="/index.html"
            // Parse Failure Cases:
                // Invalid HTTP syntax
                // Missing required components
            if (ParsedRequest::parse(parsed, request.c_str(), request.size()) < 0) {
                sendError(client_fd, 400, "Bad Request");
                close(client_fd);
                sem_post();
                return;
            }
            // Proxy only supports GET requests  
            if (parsed->method != "GET") {
                sendError(client_fd, 501, "Not Implemented");
                close(client_fd);
                sem_post();
                return;
            }
            // Protocol Validation: HTTP/1.0 or HTTP/1.1
            if (parsed->host.empty() || parsed->path.empty() || 
                (parsed->version.find("HTTP/1.0") != 0 && parsed->version.find("HTTP/1.1") != 0)) { 
                sendError(client_fd, 400, "Bad Request");
                close(client_fd);
                sem_post();
                return;
            }
            handleRequest(client_fd, parsed, request);  // Process the request
            close(client_fd);

            sem_post();   // SIGNAL ( Release slot )
            printSemaphoreStatus("After releasing semaphore");
        } catch (...) {
            log_event("Error processing request from " + 
                     std::string(client_ip) + ":" + std::to_string(client_port));
        }

        // Connection end
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        close(client_fd);
        active_connections--;
        log_connection("closed", client_ip, client_port, duration.count());

    }

public:
// 1- Constructor: ProxyServer(int port)
    ProxyServer(int port) : port(port) {
        // 1- SOCKET CREATION - server_fd becomes the reference for all server operations
            // Parameter:
                // AF_INET: IPv4 address family
                // SOCK_STREAM: TCP protocol
                // 0: Default protocol (TCP)
        printSemaphoreStatus("Server initialized");
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("Socket creation failed");  // Throws exception if socket creation fails (kernel resource exhaustion)
        }

        // 2- SOCKET OPTION CONFIGURATION
        // SO_REUSEADDR : 
            // to avoid "Address already in use" errors
            // Allows immediate reuse of port after server restart
            // Bypasses TCP's TIME_WAIT state (typically 2MSL ~60-120s)
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 

        // 3- SOCKET BINDING
        // Address Structure:
            // sin_family=AF_INET: IPv4
            // INADDR_ANY: Binds to all available interfaces
            // htons(port): Converts port to network byte order
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Bind failed");
        }

        // 4- LISTENING SETUP
        // Kernel Behavior:
        // Transitions socket to passive state

        // Maintaining two queues:
            // Incomplete connections (SYN_RECEIVED)
            // Completed connections (ESTABLISHED)
        if (listen(server_fd, MAX_CLIENTS) < 0) {
            throw std::runtime_error("Listen failed");
        }
    }
// 2- Destructor: ~ProxyServer()
    //Semaphore Destruction:
    // Releases kernel resources for the semaphore
    // Undoes sem_init()
    ~ProxyServer() {
        close(server_fd);
    }

    void run() {
        std::cout << "Proxy server running on port " << port << std::endl;
        
        while (true) {  // ← Main Thread (Proxy Server) loop
            // 1- CLIENT CONNECTION ACCEPTANCE
            // Components:
                // client_addr: Stores client connection details (IP/port)
                // client_len: Size of address structure (updated by accept)
                // client_fd: New file descriptor for this connection
            // Behavior:
                // Blocks until new connection arrives
                // Creates new socket for client communication
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            
            // Common errors: EMFILE (too many open files), ECONNABORTED
            if (client_fd < 0) {
                std::cerr << "TCP Connection failed" << std::endl;
                continue;   // Skip to next iteration
            }

            // 2- CLIENT IDENTIFICATION
            // Conversion Functions:
                // inet_ntop(): Converts binary IP to string (e.g., "192.168.1.1")
                // ntohs(): Converts network byte order port to host order

            // Get client IP
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            int client_port = ntohs(client_addr.sin_port);

            // Get proxy's local IP that received this connection
            struct sockaddr_in proxy_addr;
            socklen_t proxy_len = sizeof(proxy_addr);
            if (getsockname(client_fd, (struct sockaddr*)&proxy_addr, &proxy_len) < 0) {
                log_event("getsockname failed (errno: " + std::to_string(errno));
                close(client_fd);
                continue;
            }
            char proxy_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &proxy_addr.sin_addr, proxy_ip, sizeof(proxy_ip));
            int proxy_port = ntohs(proxy_addr.sin_port);

            std::cout << "[" << get_current_timestamp() << "] TCP Connection established between - " << "Client with IP address : " << client_ip << " and Port number : " << ntohs(client_addr.sin_port) << " with the Proxy Server with Proxy IP address: " << proxy_ip << " and Proxy Port number : " << ntohs(proxy_addr.sin_port) << std::endl;


            // 3- MAIN THREAD Proxy Server) CREATION
            // Thread Management:
                // Context Creation: Packages proxy instance and client fd
                // Thread Launch: pthread_create starts new thread
                // clientHandler: Static method that processes the request
                // Detach: Allows autonomous thread cleanup
            // Memory Management:
                // ctx is dynamically allocated (deleted in clientHandler)
                // No thread joining (detached threads self-clean)
            auto* ctx = new std::pair<ProxyServer*, int>(this, client_fd);
            pthread_t thread;
            if (pthread_create(&thread, nullptr, &ProxyServer::clientHandler, ctx) != 0) {
                log_event("Main Thread (Proxy Server) creation failed for Client with IP address : " + std::string(client_ip));
                delete ctx;
                close(client_fd);
                continue;
            }
            pthread_detach(thread);
        }
    }
};

int main(int argc, char* argv[]) {
    // Exactly one argument (port number)
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;  // Usage: ./proxy 8080
        return 1;
    }

    // PROXY INITIALIZATION
        // Convert port string to integer (std::stoi)
        // Construct ProxyServer instance
        // Enter main loop via run()
    try {
        ProxyServer proxy(std::stoi(argv[1]));
        proxy.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;   // ERRORS from: Invalid port number , Socket/bind/listen failures
        return 1;
    }

    return 0;
}