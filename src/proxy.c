#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <ctype.h>

#define PORT 8081
#define BACKLOG 15  
#define BUFFER_SIZE 8192
#define MAX_EVENTS 200

int packet_id = 1;

int epoll_fd;

struct url_parts{
    char hostname[256];
    char relative_path[224];
    int port;
};

struct descriptor_info {
    int fd; 
    struct descriptor_info *peer;
    struct url_parts target_url_info;
    char *type;
    char *buffer;
    char *request;
};

struct http_request_parts{
    char method[16];
    char url[256];
    char version[16]; 
};

int set_server_sock(){
    int v = 1;
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    
    if (server_socket < 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) < 0) {
        perror("setsockopt failed");
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr)); 
    server_addr.sin_family = AF_INET;               
    server_addr.sin_addr.s_addr = INADDR_ANY;     
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, BACKLOG) < 0) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    return server_socket;
}

int accept_client_socket(int server_socket){
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));
    
    int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

    if (client_socket < 0) {
        perror("Accept failed");
        return -1;
    }

    printf("Accepted connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    return client_socket;
}

// Modify http request to send it to the target server
char *prepare_request(char *buffer, struct http_request_parts request_info, char *relative_path){
    char *request = malloc(BUFFER_SIZE);

    if (request == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    char request_line[BUFFER_SIZE];
    char *request_headers = strstr(buffer, "\r\n"); 
    if (request_headers == NULL) {
        return NULL;
    }

    request_headers += 2;

    snprintf(request_line, sizeof(request_line), "%s %s %s\r\n", request_info.method, relative_path, request_info.version); 
    strcpy(request, request_line);
    strcat(request_headers, "\r\n");
    strcat(request, request_headers);

    return request;
}

struct url_parts split_url(char *dist_url){

    struct url_parts url_parts;
    if (sscanf(dist_url, "http://%255[^:/]:%d/%255[^\n]", url_parts.hostname, &url_parts.port, url_parts.relative_path) < 3) {
        
        sscanf(dist_url, "http://%255[^:/]%255[^\n]", url_parts.hostname, url_parts.relative_path);
        url_parts.port = 80;
    }

    if (url_parts.relative_path[0] == '\0') {
        strcpy(url_parts.relative_path, "/");
    }
    
    return url_parts;
}

int set_target_socket(int client_socket, struct url_parts target_url_info){

    struct hostent *host = gethostbyname(target_url_info.hostname);
    struct sockaddr_in target_addr;

    if (host == NULL) {
        fprintf(stderr, "Could not resolve hostname: %s\n", target_url_info.hostname);
        return -1;
    }
    
    int target_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (target_socket < 0) {
        perror("Unable to create target socket!");
        return -1;
    }

    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(target_url_info.port);
    memcpy(&target_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
    
    fcntl(target_socket, F_SETFL, fcntl(target_socket, F_GETFL) | O_NONBLOCK);
    if (fcntl(target_socket, F_GETFL) == -1) {
        perror("Failed to set non-blocking mode");
        close(target_socket);
        return -1;
    }
    
    if (connect(target_socket, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("Error connecting to target server");
            close(target_socket);

            return -1;
        }
    }

    return target_socket;
}

// Send request to the target server
int send_to_target(int target_socket, char *request){
    int err;
    socklen_t len = sizeof(err);
    if (getsockopt(target_socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        perror("getsockopt error");
        close(target_socket);

        return -1;
    }
    
    ssize_t server_bytes_sent = send(target_socket, request, strlen(request), 0);
    if(server_bytes_sent < 0 ){
        perror("Error sending data to target server");
        close(target_socket);

        return -1;
    }

    return 1;
}

void send_response_to_client(int client_socket, char* response, int response_size, char* target_addr){
    ssize_t client_bytes_received = send(client_socket, response, response_size, 0);

    if (client_bytes_received > 0) {
        packet_id++;

        if (target_addr) {
            printf("%d) %ld bytes sent to client from: %s\n", packet_id, client_bytes_received, target_addr);      
        }

    } else if (client_bytes_received < 0) {
        perror("Error while sending data response to client socket!");
    } else {
        puts("End of transfer to the client!");
    }
}

void delete_descriptor(struct descriptor_info* descriptor){
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, descriptor->fd, NULL) == -1) {
        perror("Deleting of epoll event failed!");
    }

    close(descriptor->fd);
    free(descriptor);
}

int process_get(struct descriptor_info* active_descriptor){
    struct descriptor_info* target_descriptor = malloc(sizeof(struct descriptor_info));
    int target_socket_fd = set_target_socket(active_descriptor->fd, active_descriptor->target_url_info);

    if (target_socket_fd < 0) {
        perror("Failed to connect to target server");
        free(target_descriptor);

        return -1;
    }

    target_descriptor->fd = target_socket_fd;
    target_descriptor->type = "target";
    target_descriptor->peer = active_descriptor;
    target_descriptor->target_url_info = active_descriptor->target_url_info;
    target_descriptor->request = active_descriptor->request;

    struct epoll_event nevent = {0};
    nevent.events = EPOLLOUT;
    nevent.data.ptr = target_descriptor;
                
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target_socket_fd, &nevent) == -1) {
        delete_descriptor(target_descriptor);

        return -1;
    }

    return 1;
}

char* to_lowercase(const char* str) {
   
    size_t len = strlen(str);

    char* lower_str = (char*)malloc(len + 1);
    if (lower_str == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        lower_str[i] = tolower((unsigned char)str[i]);
    }
    
    lower_str[len] = '\0';

    return lower_str; 
}

int main() {
    
    int server_socket = set_server_sock();
    epoll_fd = epoll_create1(0);
    fcntl(server_socket, F_SETFL, fcntl(server_socket, F_GETFL) | O_NONBLOCK);

    if (fcntl(server_socket, F_GETFL) == -1) {
        perror("Failed to set non-blocking mode");
        close(server_socket);
    }

    struct epoll_event event;

    struct descriptor_info* server_fd_info = malloc(sizeof(struct descriptor_info));
    server_fd_info->fd = server_socket;
    server_fd_info->type = "server";
    event.data.ptr = server_fd_info;
    event.events = EPOLLIN;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }
    
    while (1) {
        struct epoll_event events[MAX_EVENTS];    
        memset(events, 0, sizeof(events)); 

        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 
        
        for (int i = 0; i < n; i++) {
            
            struct descriptor_info* active_descriptor = (struct descriptor_info*)events[i].data.ptr;

            if (active_descriptor->fd == server_socket) {

                int client_socket = accept_client_socket(server_socket);    

                fcntl(client_socket, F_SETFL, fcntl(client_socket, F_GETFL) | O_NONBLOCK);
                if (fcntl(client_socket, F_GETFL) == -1) {
                    perror("Failed to set non-blocking mode");
                    close(client_socket);
                }

                if (client_socket < 0) {
                    perror("Accept_client_socket ERROR");
                    exit(0);
                }

                struct descriptor_info* client_descriptor = malloc(sizeof(struct descriptor_info));
                client_descriptor->fd = client_socket;
                client_descriptor->type = "client";
                client_descriptor->buffer = (char *) malloc(BUFFER_SIZE);

                struct epoll_event nevent;
                nevent.events = EPOLLIN;
                nevent.data.ptr = client_descriptor;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_descriptor->fd, &nevent) == -1) {
                    perror("Failed to add client socket to epoll");
                    close(client_descriptor->fd); 
                }
                
            } else if (active_descriptor->type == "client" && events[i].events & EPOLLIN) {

                memset(active_descriptor->buffer, 0, sizeof(active_descriptor->buffer));
                ssize_t client_bytes_received = recv(active_descriptor->fd, active_descriptor->buffer, BUFFER_SIZE, 0);

                if (client_bytes_received > 0) {

                    struct http_request_parts request_info; 
                
                    sscanf(active_descriptor->buffer, "%s %s %s", request_info.method, request_info.url, request_info.version); 

                    active_descriptor->buffer[client_bytes_received] = '\0';
                    active_descriptor->target_url_info = split_url(request_info.url);
                    active_descriptor->request = prepare_request(active_descriptor->buffer, request_info, active_descriptor->target_url_info.relative_path);
                
                    if (strcasecmp(request_info.method, "GET") == 0) {
                        if(process_get(active_descriptor) < 0){
                            continue;
                        }
                    } else {    
                        printf("Unsupported HTTP method: %s from %s\n", request_info.method, request_info.url);
                        delete_descriptor(active_descriptor);
                    }         

                } else if (client_bytes_received == 0) {

                    printf("Сlient %d has completed data transfer!\n", active_descriptor->fd);
                    delete_descriptor(active_descriptor);

                } else {

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        puts("EAGAIN!");
                    } else {
                        perror("Error while receiving data!");
                        delete_descriptor(active_descriptor);
                    }
                }

            } else if (active_descriptor->type == "target" && events[i].events & EPOLLOUT) {

                if (send_to_target(active_descriptor->fd, active_descriptor->request) < 0) {
                    puts("Error! Unable to send a request to the target server!");
                    continue;
                }
                        
                struct epoll_event nevent;
                nevent.events = EPOLLIN;
                nevent.data.ptr = active_descriptor;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, active_descriptor->fd, &nevent);

            } else if (active_descriptor->type == "target" && events[i].events & EPOLLIN) {

                char response[BUFFER_SIZE];
                char fullPath[256];

                memset(response, 0, sizeof(response));    
                memset(fullPath, 0, sizeof(fullPath));

                strcat(fullPath, active_descriptor->target_url_info.hostname);
                strcat(fullPath, active_descriptor->target_url_info.relative_path);

                ssize_t server_bytes_received = recv(active_descriptor->fd, response, sizeof(response)-1, 0);
                response[server_bytes_received] = '\0';

                printf("%ld bytes recieved from %s\n", server_bytes_received, fullPath);
                
                if (server_bytes_received > 0) {                  
                    send_response_to_client(active_descriptor->peer->fd, response, server_bytes_received, fullPath);
                } else if(server_bytes_received == 0) {
                    puts("Request was completed successfully! Closing...");
                    delete_descriptor(active_descriptor);
                } else {
                    if (errno == EAGAIN) {
                        puts("EAGAIN!");
                    } else {
                        perror("Error receiving data from target server");
                        delete_descriptor(active_descriptor);
                    }
                }  

            } else {
                puts("Unexpected epoll method!");
            }           
        }
    }
    
    close(server_socket);
    close(epoll_fd);

    return EXIT_SUCCESS; 
}

