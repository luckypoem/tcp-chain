#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <linux/netfilter_ipv4.h>
#include <ev.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>

#include "main.h"

#define PORT_NO 3033
#define PLUGIN_DIR "./plugins"

int total_clients = 0;  // Total number of connected clients

struct plugin_hooks loaded_plugins[MAX_PLUGINS];
int plugin_count;

struct relay_info relays[MAX_RELAYS];

struct ev_loop* loop;

int setnonblocking(int fd) {
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
    flags = 0;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int relay_send_func(struct sock_info* identifier, const void *buffer, size_t length, int flags) {
  struct relay_info* relay = &(relays[identifier->relay_id]);

  //Apply on_send() on all plugins
  for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
    (*(loaded_plugins[plugin_index].on_send))(&(relay->plugin_socks[plugin_index]), buffer, &length);
  }


  size_t ret;

  if (relay->pending_send_data_len > 0) {
    ret = 0;
  } else {
    ret = send(relay->sock_fd, buffer, length, flags);
    if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
      relay_close_func(identifier);
      return ret;
    }
  }

  if (ret == -1) {
    ret = 0;
  }

  if (ret < length) {
    if (relay->pending_send_data_len + (length-ret) > relay->pending_send_data_buf_len) {
      relay->pending_send_data = (char*)realloc(relay->pending_send_data, relay->pending_send_data_len + (length-ret));
      relay->pending_send_data_buf_len = relay->pending_send_data_len + (length-ret);
    }

    memcpy(relay->pending_send_data + relay->pending_send_data_len, buffer + ret, length-ret);

    relay->pending_send_data_len += (length - ret);

    ev_io_start(loop, &((relay->write_io_wrap).io));

    //Apply pause_remote_recv() on all plugins
    for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
      (*(loaded_plugins[plugin_index].pause_remote_recv))(&((relay->plugin_socks)[plugin_index]), 1);
    }
  }

  return length;
}

int relay_close_func(struct sock_info* identifier) {
  struct relay_info* relay = &(relays[identifier->relay_id]);

  if (!(relay->active)) {
    return -1;
  }

  //Apply on_close() on all plugins
  for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
    (*(loaded_plugins[plugin_index].on_close))(&(relay->plugin_socks[plugin_index]));

  }

  ev_io_stop(loop, &((relay->read_io_wrap).io));
  ev_io_stop(loop, &((relay->write_io_wrap).io));
  int ret = close(relay->sock_fd);
  
  total_clients --;
  close_relay(identifier->relay_id);

  printf("main: Closing client.\n");
  printf("main: %d client(s) connected.\n", total_clients);

  return ret;
}

void relay_pause_recv_func(struct sock_info* identifier, int pause) {
  struct relay_info* relay = &(relays[identifier->relay_id]);

  if (pause) {
    ev_io_stop(loop, &((relay->read_io_wrap).io));
  } else {
    ev_io_start(loop, &((relay->read_io_wrap).io));
  }
}

int init_relay(int sock_fd, struct sockaddr* src_addr, struct sockaddr* dst_addr) {
  int free_relay_founded = 0;
  int relay_index = -1;
  for (int i=0; i<MAX_RELAYS; i++) {
    if (!relays[i].active) {
      free_relay_founded = 1;
      relay_index = i;
      break;
    }
  }
  if (!free_relay_founded) {
    return -1;
  }

  struct relay_info* relay = &(relays[relay_index]);
  relay->active = 1;
  relay->sock_fd = sock_fd;
  relay->shared_data = (void*)malloc(BUFFER_SIZE);
  relay->takeovered = 0;
  memcpy(&(relay->src_addr), src_addr, sizeof(struct sockaddr));
  memcpy(&(relay->dst_addr), dst_addr, sizeof(struct sockaddr));
  for (int i=0; i<plugin_count; i++) {
    (relay->plugin_socks)[i].plugin_id = i;
    (relay->plugin_socks)[i].relay_id = relay_index;
    (relay->plugin_socks)[i].data = NULL;
    (relay->plugin_socks)[i].shared_data = relay->shared_data;
    (relay->plugin_socks)[i].src_addr = (struct sockaddr*) &(relay->src_addr);
    (relay->plugin_socks)[i].dst_addr = (struct sockaddr*) &(relay->dst_addr);
    (relay->plugin_socks)[i].takeovered = &(relay->takeovered);
  }
  (relay->read_io_wrap).relay = relay;
  (relay->write_io_wrap).relay = relay;

  relay->pending_send_data = (char*)malloc(BUFFER_SIZE);
  relay->pending_send_data_len = 0;
  relay->pending_send_data_buf_len = BUFFER_SIZE;

  return relay_index;
}

void close_relay(int relay_id) {
  relays[relay_id].active = 0;
  free(relays[relay_id].shared_data);
  free(relays[relay_id].pending_send_data);
}

void load_plugins() {
  DIR *plugin_d = opendir(PLUGIN_DIR);
  char full_filename[128];
  bzero(full_filename, 128);
  memcpy(full_filename, PLUGIN_DIR, strlen(PLUGIN_DIR));
  full_filename[strlen(PLUGIN_DIR)] = '/';

  struct plugin_hooks plugin;
  char* error;

  struct dirent* plugin_dir;
  if (!plugin_d) {
    printf("Plugin directory not found.\n");
    exit(-1);
    return;
  }

  plugin_count = 0;
  while((plugin_dir = readdir(plugin_d)) != NULL) {
    char* filename = plugin_dir->d_name;
    error = NULL;
    if (strlen(filename) <= 3) {
      continue;
    }
    if (!strcmp(filename + (strlen(filename) - 3), ".so")) {
      memcpy(full_filename + strlen(PLUGIN_DIR) + 1, filename, strlen(filename));

      printf("Loading plugin: %s\n", full_filename);
      void* module = dlopen(full_filename, RTLD_LAZY);
      if (!module) {
        printf("Failed to load plugin: %s\n", full_filename);
        continue;
      }
      dlerror();
      plugin.on_connect = dlsym(module, "on_connect");
      plugin.on_send = dlsym(module, "on_send");
      plugin.on_recv = dlsym(module, "on_recv");
      plugin.on_close = dlsym(module, "on_close");
      plugin.on_init = dlsym(module, "on_init");
      plugin.pause_remote_recv = dlsym(module, "pause_remote_recv");
      if (error = dlerror()) {
        printf("Plugin symbol not found. %s\n", error);
        continue;
      }
      loaded_plugins[plugin_count] = plugin;
      plugin_count ++;
    }
  }
  closedir(plugin_d);
}

int init_server_socket() {
  // Create server socket
  int sd;
  struct sockaddr_in addr;

  if( (sd = socket(PF_INET, SOCK_STREAM, 0)) < 0 ) {
    perror("socket error");
    exit(-1);
    return -1;
  }

  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT_NO);
  addr.sin_addr.s_addr = INADDR_ANY;

  // Bind socket to address
  if (bind(sd, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
    perror("bind error");
    exit(-1);
    return -1;
  }

  // Start listing on the socket
  if (listen(sd, 2) < 0) {
    perror("listen error");
    exit(-1);
    return -1;
  }

  setnonblocking(sd);

  return sd;
}


int main() {

  load_plugins();

  for (int i=0; i<MAX_RELAYS; i++) {
    relays[i].active = 0;
  }

  loop = ev_default_loop(0);
  int sd;
  struct ev_io w_accept;
  
  for (int i=0; i<plugin_count; i++) {
    struct init_info* hook_init_info = (struct init_info*)malloc(sizeof(struct init_info));
    hook_init_info->default_loop = loop;
    hook_init_info->plugin_id = i;
    hook_init_info->relay_send = relay_send_func;
    hook_init_info->relay_close = relay_close_func;
    hook_init_info->relay_pause_recv = relay_pause_recv_func;
    (*((loaded_plugins[i]).on_init))(hook_init_info);
  }

  sd = init_server_socket();

  // Initialize and start a watcher to accepts client requests
  ev_io_init(&w_accept, accept_cb, sd, EV_READ);
  ev_io_start(loop, &w_accept);

  // Start infinite loop
  ev_run(loop, 0);

  return 0;
}

/* Accept client requests */
void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
  struct sockaddr_in client_addr, original_dst_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd;

  if(EV_ERROR & revents) {
    return;
  }

  // Accept client request
  client_fd = accept(watcher->fd, (struct sockaddr *)&client_addr, &client_len);

  setnonblocking(client_fd);
  setnonblocking(watcher->fd);

  if (client_fd < 0) {
    perror("accept error");
    return;
  }

  total_clients ++; // Increment total_clients count
  printf("main: Client connected.\n");
  printf("main: %d client(s) connected.\n", total_clients);

  //Get original destination address from accepted socket
  int sockaddr_len = sizeof(original_dst_addr);
  if (getsockopt(client_fd, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr*)&original_dst_addr, &sockaddr_len)) {
    printf("Get original destination failed. Dropping.\n");
    total_clients --;
    close(client_fd);
    return;
  }

  //Create relay info
  int relay_id = init_relay(client_fd, (struct sockaddr*)&client_addr, (struct sockaddr*)&original_dst_addr);
  if (relay_id < 0) {
    printf("Relay queue full. Dropping.\n");
    total_clients --;
    close(client_fd);
    return;
  }

  struct relay_info* relay = &(relays[relay_id]);

  //Apply on_connect() on all plugins
  for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
    (*(loaded_plugins[plugin_index].on_connect))(&(relay->plugin_socks[plugin_index]));
  }

  struct ev_io *w_client_read = &((relay->read_io_wrap).io);
  struct ev_io *w_client_write = &((relay->write_io_wrap).io);
  ev_io_init(w_client_read, read_cb, client_fd, EV_READ);
  ev_io_init(w_client_write, write_cb, client_fd, EV_WRITE);
  ev_io_start(EV_A_ w_client_read);
  ev_io_start(EV_A_ w_client_write);
}

void write_cb(struct ev_loop *loop, struct ev_io *w_, int revents) {
  ssize_t read;
  struct relay_wrap* io_wrap = (struct relay_wrap*)w_;
  struct ev_io* watcher = &(io_wrap->io);
  struct relay_info* relay = io_wrap->relay;

  if(EV_ERROR & revents) {
    return;
  }

  if (relay->pending_send_data_len <= 0) {
    ev_io_stop(loop, watcher);
    return;
  }

  size_t bytes_sent = send(watcher->fd, relay->pending_send_data, relay->pending_send_data_len, 0);

  if (bytes_sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    //ev_io_stop(loop, watcher);
    printf("main: Sock send error.\n");
    relay_close_func(&(((io_wrap->relay)->plugin_socks)[0])); 
    return;
  }

  if (bytes_sent == -1) {
    return;
  }

  if (bytes_sent < relay->pending_send_data_len) {
    memmove(relay->pending_send_data, relay->pending_send_data + bytes_sent, relay->pending_send_data_len - bytes_sent);
    relay->pending_send_data_len -= bytes_sent;
  } else {
    relay->pending_send_data_len = 0;
    //Apply pause_remote_recv() on all plugins
    for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
      (*(loaded_plugins[plugin_index].pause_remote_recv))(&(((io_wrap->relay)->plugin_socks)[plugin_index]), 0);
    }
    ev_io_stop(loop, watcher);
  }

}

void read_cb(struct ev_loop *loop, struct ev_io *w_, int revents){
  char buffer[BUFFER_SIZE];
  ssize_t read;
  struct relay_wrap* io_wrap = (struct relay_wrap*)w_;
  struct ev_io* watcher = &(io_wrap->io);

  if(EV_ERROR & revents) {
    return;
  }

  bzero(buffer, BUFFER_SIZE);

  // Receive message from client socket
  read = recv(watcher->fd, buffer, BUFFER_SIZE, 0);

  if((read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) || read == 0) {
    printf("main: Client closed.\n");
    ev_io_stop(loop, watcher);
    relay_close_func(&(((io_wrap->relay)->plugin_socks)[0])); 
    return;
  }

  if (read == -1) {
    return;
  }

  //Apply on_recv() on all plugins
  for (int plugin_index=0; plugin_index<plugin_count; plugin_index++) {
    (*(loaded_plugins[plugin_index].on_recv))(&(((io_wrap->relay)->plugin_socks)[plugin_index]), buffer, &read);
  }

}
