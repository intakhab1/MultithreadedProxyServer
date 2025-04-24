# Multi Threaded Proxy Web Server

A high-performance web proxy server implementation using c++ with two variants:
1. Basic proxy server (forwarding only)
2. Enhanced proxy with LRU caching capabilities

The proxy server demonstrates core networking concepts, operating system concepts, and caching algorithms.

## Table of Contents
- [Features](#features)
- [Architecture](#architecture)
- [Implementation Details](#implementation-details)
  - [Request Processing](#request-processing)
  - [Thread Management](#thread-management)
  - [LRU Cache Implementation](#LRU-Cache-Implementation)
  - [Connection Handling](#connection-handling)
  - [Concurrency Control](#concurrency-control)
  - [Socket Management](#socket-management)
- [Performance Comparison](#performance-comparison)
- [Build & Run](#Build-Run)
- [Conclusion](#Conclusion)
- [Future Enhancements](#future-enhancements)

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
![System Architecture](UML_Class_Diagram.png)
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
![Thread Flow](Request_Processing.png)

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

![Cache](Cache_Hit_and_Miss.jpg)

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
![cache vs no-cache](Request_Time_comparision.jpg)

**Key Observations**:
  - First Request	715ms(cached)	716ms(non-cached)
  - Repeat Request	23ms (31x faster)(cached)	729ms(non-cached)
  - CPU Utilization	69%(cached)	1%(non-cached)

**Key Findings**:
1. Cold Start: Both implementations perform similarly
2. Warm Cache: Dramatic 31x speed improvement 
3. Resource Usage:
  - Cached version uses more CPU (local processing)
  - Uncached version is I/O bound (network latency)

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