# Multi Threaded Proxy Web Server

A high-performance web proxy server implementation using c++ with two variants:
1. Basic proxy server (forwarding only)
2. Enhanced proxy with LRU caching capabilities

The proxy server demonstrates core networking concepts, operating system concepts, and caching algorithms.

## Table of Contents
- [Features](https://github.com/intakhab1/MultithreadedProxyServer#Features)
- [Architecture](https://github.com/intakhab1/MultithreadedProxyServer#Architecture)
- [Implementation Details](https://github.com/intakhab1/MultithreadedProxyServer#implementation-details)
  - [Request Processing](https://github.com/intakhab1/MultithreadedProxyServer#request-processing)
  - [Thread Management](https://github.com/intakhab1/MultithreadedProxyServer#thread-management)
  - [LRU Cache Implementation](https://github.com/intakhab1/MultithreadedProxyServer#LRU-Cache-Implementation)
  - [Concurrency Control](https://github.com/intakhab1/MultithreadedProxyServer#concurrency-control)
  - [Socket Management](https://github.com/intakhab1/MultithreadedProxyServer#socket-management)
- [Performance Comparison](https://github.com/intakhab1/MultithreadedProxyServer#performance-comparison)
- [Build & Run](https://github.com/intakhab1/MultithreadedProxyServer#Build-Run)
- [Conclusion](https://github.com/intakhab1/MultithreadedProxyServer#Conclusion)
- [Future Enhancements](https://github.com/intakhab1/MultithreadedProxyServer#future-enhancements)

## Features

### Core Functionality
- Multi-threaded architecture (400+ concurrent connections)
- HTTP/1.1 compliant request handling
- GET method support with Host header validation
- Connection keep-alive support

### Caching Proxy Features
- LRU (Least Recently Used) cache implementation
- Configurable cache limits (200MB total, 10MB per item)
- Automatic cache eviction when full
- Cache hit/miss logging

### Performance Features
- Semaphore-based connection limiting
- Mutex-protected cache operations
- Zero-copy data forwarding
- Efficient memory management

## Architecture

### UML Class Diagram
![UML Class Diagram](https://github.com/intakhab1/MultithreadedProxyServer/blob/main/Images/UML_Class_Diagram.png)
The architecture consists of three core components:

1. **ProxyServer Class**:
   - Manages the main server operations
   - Contains thread pool (400 max threads)
   - Controls access via semaphores (`sem_t`)
   - Maintains LRU cache with mutex protection (`cache_mutex`)

2. **ParsedRequest Class**:
   - Handles HTTP request parsing
   - Manages headers and protocol information
   - Provides methods for request serialization

3. **CacheEntry Structure**:
   - Stores cached responses
   - Implements LRU linked list (`shared_ptr<CacheEntry> next`)
   - Tracks last access time (`time_t last_accessed`)

### Key Components:
1. **ProxyServer**: Main server class handling connections and request routing
2. **ParsedRequest**: HTTP request parser and formatter
3. **CacheEntry**: LRU cache node structure
4. **ThreadContext**: Worker thread context wrapper

## Implementation Details

### Multithreading
- Uses pthreads for concurrent request handling
- Main thread accepts connections and spawns worker threads
- Maximum 400 concurrent threads (configurable)
- Threads are detached for automatic cleanup

## Request Processing

### Request Processing Flow
![Request Processing](https://github.com/intakhab1/MultithreadedProxyServer/blob/main/Images/Request_Processing.png)

1. Client establishes TCP connection
2. Semaphore slot acquisition (sem_wait())
3. Main thread accepts connection and creates worker thread
4. Worker thread:
  - Checks cache (mutex locked)
  - On cache hit: Returns cached response
  - On cache miss: Forwards to origin server
5. Responses are cached if cacheable
6. Thread cleans up and releases semaphore slot

## Thread Management

### Concurrency Model
1. **Main Thread**:
   - Listens on specified port (8080/8081)
   - Accepts incoming connections
   - Enforces 400-connection limit via semaphore

2. **Worker Threads**:
   - Each thread handles one client connection
   - Implements full request/response cycle
   - Automatically cleans up after completion

3. **Semaphore Control**:
   - Blocks when all 400 threads are active
   - Implements full request/response cycle
   - Automatically cleans up after completion

## LRU Cache Implementation

![Cache Hit/Miss](https://github.com/intakhab1/MultithreadedProxyServer/blob/main/Images/Cache_Hit_and_Miss.jpg)

1. **Cache Structure**:
  - Maximum size: 200MB
  - Maximum element size: 10MB
  - Linked list organization (MRU at head)

2. **Cache Miss**:
  - Worker thread acquires mutex
  - Searches cache (no entry found)
  - Forwards request to origin server
  - Stores response in cache:

3. **Cache Hit**:
  - Worker thread finds cached entry
  - Immediately returns cached response
  - Updates LRU position (moves to head)

4. **Eviction**:
  - Removes least recently used items when cache full
  - Thread-safe eviction with mutex protection

## Concurrency Control

1. Semaphore: Limits active threads to 400
2. Mutex: Protects cache operations
3. Atomic Counters: Track active connections

## Socket Management

1. Non-blocking socket operations
2. Proper connection termination handling
3. Socket reuse with SO_REUSEADDR
4. Timeout handling for stalled connections

## Performance Comparison

### Cache vs No-Cache Comparison
![cache vs no-cache](https://github.com/intakhab1/MultithreadedProxyServer/blob/main/Images/Request_Time_comparision.jpg)

**Key Observations**:
  - First Request	715ms(cached)	716ms(non-cached)
  - Repeat Request	23ms (31x faster)(cached)	729ms(non-cached)
  - CPU Utilization	69%(cached)	1%(non-cached)

![cache vs no-cache](https://github.com/intakhab1/MultithreadedProxyServer/blob/main/Images/With_and_Without_Caching.jpg)

**Key Findings**:
1. Cached Proxy Performance:
  - Avg. latency: 0.33ms (near-instant, using cache)
  - Throughput: 3,023 req/s (consistent with in-memory operations)
  - 100% of requests completed in 2ms max
  - The 454x performance difference (3,023 vs 6.66 req/s) ~99.8% cache hit ratio


2. Uncached Proxy Performance:
  - Avg. latency: 150ms 
  - Throughput: 6.66 req/s (origin server limits)

## Build & Run

### Requirements
C++17 compatible compiler
pthread library
Linux/macOS environment

### Build Instructions
```bash
$ # Caching Proxy Server
$ g++ -std=c++17 -pthread -Wno-deprecated-declarations proxy_parse.cpp proxy_server_with_cache.cpp -o proxy_server_with_cache
$ ./proxy_server_with_cache 8080
$
$ # Basic Proxy Server
$ g++ -std=c++17 -pthread -Wno-deprecated-declarations proxy_server_without_cache.cpp -o proxy_server_without_cache
$ ./proxy_server_with_cache 8081
```

### Testing
**Test using curl**:
```bash
$ # First request (cache miss)
$ time curl -x localhost:8080 http://example.com
$
$ # Repeat request (cache hit)
$ time curl -x localhost:8080 http://example.com
```
## Conclusion

### This implementation demonstrates:
  - Efficient multi-threaded request handling
  - Effective LRU cache acceleration
  - Proper resource management via semaphores
  - Thread-safe cache operations
  - Significant real-world performance gains

The architectural decisions and performance characteristics make this suitable for production-grade proxy deployments with configurable caching behavior.

## Future Enhancements

1. HTTPS/TLS support
2. Cache cluster synchronization
3. Adaptive caching policies
4. HTTP/2 protocol support
5. Administrative dashboard