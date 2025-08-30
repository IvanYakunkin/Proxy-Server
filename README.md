# Proxy-Server

A simple proxy server written in C using EPOLL to handle multi-user connections. This project is under development and only supports the HTTP GET method.

## Description

This proxy server is designed to demonstrate the capabilities of asynchronous I/O using EPOLL, which allows handling multiple connections with high performance. The current version only supports the HTTP GET method, which provides basic functionality for testing and studying the principles of proxy servers. Can only work on Unix-like systems.

## Functions

- Asynchronous connection handling using EPOLL
- Support for HTTP GET requests
- Error handling

## Installation

To build the project you will need:

- C compiler (eg GCC)

```bash
git clone https://github.com/IvanYakunkin/Proxy-Server.git
cd http-proxy-server/
sudo gcc /src/proxy.c -o proxy
sudo ./proxy
```
## Current status
<b>The project is currently under development.</b> Plans include implementing support for other HTTP methods, support for https tunneling, and improving performance.
