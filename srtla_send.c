/*
    srtla - SRT transport proxy with link aggregation
    Copyright (C) 2020-2021 BELABOX project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common.h"

#define PKT_LOG_SZ 256
#define CONN_TIMEOUT 4
#define GLOBAL_TIMEOUT 10
#define IDLE_TIME 1

#define min(a, b) ((a < b) ? a : b)
#define max(a, b) ((a > b) ? a : b)
#define min_max(a, l, h) (max(min((a), (h)), (l)))

#define WINDOW_MIN 1
#define WINDOW_DEF 20
#define WINDOW_MAX 60
#define WINDOW_MULT 1000
#define WINDOW_DECR 100
#define WINDOW_INCR 30

#define LOG_PKT_INT 20

typedef struct conn {
  struct conn *next;
  int fd;
  time_t last_rcvd;
  time_t last_sent;
  struct sockaddr src;
  int removed;
  int in_flight_pkts;
  int window;
  int pkt_idx;
  int pkt_log[PKT_LOG_SZ];
} conn_t;

char *source_ip_file = NULL;

int do_update_conns = 0;

struct sockaddr srtla_addr, srt_addr;
const socklen_t addr_len = sizeof(srtla_addr);
conn_t *conns = NULL;
int listenfd;
int active_connections = 0;

char srtla_id[SRTLA_ID_LEN];


/*

Async I/O support

*/
fd_set active_fds;
int max_act_fd = -1;

int add_active_fd(int fd) {
  if (fd < 0) return -1;

  if (fd > max_act_fd) max_act_fd = fd;
  FD_SET(fd, &active_fds);

  return 0;
}

int remove_active_fd(int fd) {
  if (fd < 0) return -1;

  FD_CLR(fd, &active_fds);

  return 0;
}


/*

Misc helper functions

*/
void print_help() {
  fprintf(stderr,
          "Syntax: srtla_send SRT_LISTEN_PORT SRTLA_HOST SRTLA_PORT BIND_IPS_FILE\n\n"
          "-v      Print the version and exit\n");
}


/*

srtla registration helpers

*/
int try_reg1(conn_t *c) {
  if (c->fd < 0) return -1;

  char buf[MTU];
  uint16_t packet_type = htobe16(SRTLA_TYPE_REG1);
  memcpy(buf, &packet_type, sizeof(packet_type));
  memcpy(buf + sizeof(packet_type), srtla_id, SRTLA_ID_LEN);

  int ret = sendto(c->fd, buf, SRTLA_TYPE_REG1_LEN, 0, &srtla_addr, addr_len);
  if (ret != SRTLA_TYPE_REG1_LEN) return -1;

  int timeout = 5;
  while(timeout) {
    int n = recvfrom(c->fd, buf, MTU, 0, NULL, NULL);
    if (is_srtla_reg2(buf, n)) {
      char *id = &buf[2];
      if (memcmp(id, srtla_id, SRTLA_ID_LEN/2) != 0) {
        err("%s (%p): got a mismatching ID in SRTLA_REG2\n",
            print_addr(&c->src), c);
        return -1;
      }
      memcpy(srtla_id, id, SRTLA_ID_LEN);
      return 0;
    }
    timeout--;
  }

  return -1;
}

int send_reg2(conn_t *c) {
  if (c->fd < 0) return -1;

  char buf[SRTLA_TYPE_REG2_LEN];
  uint16_t packet_type = htobe16(SRTLA_TYPE_REG2);
  memcpy(buf, &packet_type, sizeof(packet_type));
  memcpy(buf + sizeof(packet_type), srtla_id, SRTLA_ID_LEN);

  int ret = sendto(c->fd, buf, SRTLA_TYPE_REG2_LEN, 0, &srtla_addr, addr_len);
  return (ret == SRTLA_TYPE_REG2_LEN) ? 0 : -1;
}


/*

Handling code for packets coming from the SRT caller

*/
void reg_pkt(conn_t *c, int32_t packet) {
  debug("%s (%p): register packet %d at idx %d\n",
        print_addr(&c->src), c, packet, c->pkt_idx);
  c->pkt_log[c->pkt_idx] = packet;
  c->pkt_idx++;
  c->pkt_idx %= PKT_LOG_SZ;

  c->in_flight_pkts++;
}

conn_t *select_conn() {
  conn_t *min_c = NULL;
  int max_score = -1;
  int max_window = 0;

  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (c->window > max_window) {
      max_window = c->window;
    }
  }

  time_t t;
  assert(get_seconds(&t) == 0);

  for (conn_t *c = conns; c != NULL; c = c->next) {
    /* If we have some very slow links, we may be better off ignoring them
       However, we'd probably need to periodically re-probe them, otherwise
       a link disabled due to a momentary glitch might not ever get enabled
       again unless all the remaining links suffered from high packet loss
       at some point. */
    /*if (c->window < max_window / 5) {
      c->window++;
      continue;
    }*/

    if (c->last_rcvd < (t-CONN_TIMEOUT)) {
      debug("%s (%p): is timed out, ignoring it\n", print_addr(&c->src), c);
      continue;
    }

    int score = c->window / (c->in_flight_pkts + 1);
    if (score > max_score) {
      min_c = c;
      max_score = score;
    }
  }

  if (min_c) {
    min_c->last_sent = t;
  }

  return min_c;
}

void handle_srt_data(int fd) {
  char buf[MTU];
  socklen_t len = sizeof(srt_addr);
  int n = recvfrom(fd, &buf, MTU, 0, &srt_addr, &len);

  conn_t *c = select_conn();
  if (c) {
    int32_t sn = get_srt_sn(buf, n);
    int ret = sendto(c->fd, &buf, n, 0, &srtla_addr, addr_len);
    if (ret == n) {
      if (sn >= 0) {
        reg_pkt(c, sn);
      }
    } else {
      /* If sending the packet fails, adjust the timestamp to disable the link until a
         reconnection is confirmed. 1 so connection_housekeeping() prints its message */
      c->last_rcvd = 1;
      err("%s (%p): sendto() failed, disabling the connection\n",
          print_addr(&c->src), c);
    }
  }
}


/*

Handling code for packets coming from the receiver

*/
int get_pkt_idx(int idx, int increment) {
  idx = idx + increment;
  if (idx < 0) idx += PKT_LOG_SZ;
  idx %= PKT_LOG_SZ;
  assert(idx >= 0 && idx < PKT_LOG_SZ);
  return idx;
}

void register_nak(int32_t packet) {
  for (conn_t *c = conns; c != NULL; c = c->next) {
    int idx = get_pkt_idx(c->pkt_idx, -1);
    for (int i = idx; i != c->pkt_idx; i = get_pkt_idx(i, -1)) {
      if (c->pkt_log[i] == packet) {
        c->pkt_log[i] = -1;
        // It might be better to use exponential decay like this
        //c->window = c->window * 998 / 1000;
        c->window -= WINDOW_DECR;
        c->window = max(c->window, WINDOW_MIN*WINDOW_MULT);
        debug("%s (%p): found NAKed packet %d in the log\n",
              print_addr(&c->src), c, packet);
        return;
      }
    }
  }

  debug("Didn't find NAKed packet %d in our logs\n", packet);
}

void register_srtla_ack(int32_t ack) {
  int found = 0;

  for (conn_t *c = conns; c != NULL; c = c->next) {
    int idx = get_pkt_idx(c->pkt_idx, -1);
    for (int i = idx; i != c->pkt_idx && !found; i = get_pkt_idx(i, -1)) {
      if (c->pkt_log[i] == ack) {
        found = 1;
        if (c->in_flight_pkts > 0) {
          c->in_flight_pkts--;
        }
        c->pkt_log[i] = -1;

        if (c->in_flight_pkts*WINDOW_MULT > c->window) {
          c->window += WINDOW_INCR - 1;
        }

        break;
      }
    }

    if (c->last_rcvd != 0) {
      c->window += 1;
      c->window = min(c->window, WINDOW_MAX*WINDOW_MULT);
    }
  }
}

/*
  TODO after the sequence number overflows, we should probably also mark high
  sn packets as received. However, this shouldn't normally be an issue as SRTLA
  ACKs acknowledge each packet individually. Also, if the SRTLA ACK is lost,
  stale entries will be overwritten soon enough as pkt_log is a circular buffer
*/
void conn_register_srt_ack(conn_t *c, int32_t ack) {
  int count = 0;
  int idx = get_pkt_idx(c->pkt_idx, -1);
  for (int i = idx; i != c->pkt_idx; i = get_pkt_idx(i, -1)) {
    if (c->pkt_log[i] < ack) {
      c->pkt_log[i] = -1;
    } else {
      count++;
    }
  }
  c->in_flight_pkts = count;
}

void register_srt_ack(int32_t ack) {
  for (conn_t *c = conns; c != NULL; c = c->next) {
    conn_register_srt_ack(c, ack);
  }
}

void handle_srtla_data(conn_t *c) {
  char buf[MTU];

  int n = recvfrom(c->fd, &buf, MTU, 0, NULL, NULL);
  if (n <= 0) return;

  uint16_t packet_type = get_srt_type(buf, n);

  /* Handling NGPs separately because we don't want them to update last_rcvd
     Otherwise they could be keeping failed connections marked active */
  if (packet_type == SRTLA_TYPE_REG_NGP) {
    if (active_connections == 0) {
      err("The connection group has failed, restarting all connections\n");
      if (try_reg1(c) == 0) {
        // Send reg2 on all the connections
        for (conn_t *i = conns; i != NULL; i = i->next) {
          send_reg2(i);
        }
        /* Increment active_connections directly, otherwise we might handle other NGPs
           before send_keepalive_all() has a chance to update it. */
        active_connections++;
        get_seconds(&c->last_rcvd);
      }
    }
    return;
  }

  get_seconds(&c->last_rcvd);

  switch(packet_type) {
    case SRT_TYPE_ACK: {
      uint32_t last_ack = *((uint32_t *)&buf[16]);
      last_ack = be32toh(last_ack);
      register_srt_ack(last_ack);
      break;
    }

    case SRT_TYPE_NAK: {
      uint32_t *ids = (uint32_t *)buf;
      for (int i = 4; i < n/4; i++) {
        uint32_t id = be32toh(ids[i]);
        if (id & (1 << 31)) {
          id = id & 0x7FFFFFFF;
          uint32_t last_id = be32toh(ids[i+1]);
          for (int32_t lost = id; lost <= last_id; lost++) {
            register_nak(lost);
          }
          i++;
        } else {
          register_nak(id);
        }
      }
      break;
    }

    // srtla packets below, don't send to SRT
    case SRTLA_TYPE_ACK: {
      uint32_t *acks = (uint32_t *)buf;
      for (int i = 1; i < n/4; i++) {
        uint32_t id = be32toh(acks[i]);
        debug("%s (%p): ack %d\n", print_addr(&c->src), c, id);
        register_srtla_ack(id);
      }
      return;
    }
    case SRTLA_TYPE_KEEPALIVE:
      debug("%s (%p): got a keepalive\n", print_addr(&c->src), c);
      return; // don't send to SRT

    case SRTLA_TYPE_REG3:
      info("%s (%p): connection established\n", print_addr(&c->src), c);
      return;
  } // switch

  sendto(listenfd, &buf, n, 0, &srt_addr, addr_len);
}


/*

Connection and socket management

*/
conn_t *conn_find_by_src(struct sockaddr *src) {
  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (memcmp(src, &c->src, sizeof(*src)) == 0) {
      return c;
    }
  }

  return NULL;
}

int setup_conns(char *source_ip_file) {
  FILE *config = fopen(source_ip_file, "r");
  if (config == NULL) {
    perror("Failed to open the source ip list file: ");
    exit_help();
  }

  int count = 0;
  char *line = NULL;
  size_t line_len = 0;
  while(getline(&line, &line_len, config) >= 0) {
    char *nl;
    if ((nl = strchr(line, '\n'))) {
      *nl = '\0';
    }

    struct sockaddr src;

    int ret = parse_ip((struct sockaddr_in *)&src, line);
    if (ret == 0) {
      conn_t *c = conn_find_by_src(&src);
      if (c == NULL) {
        conn_t *c = calloc(1, sizeof(conn_t));
        assert(c != NULL);

        c->src = src;
        c->fd = -1;
        c->window = WINDOW_DEF * WINDOW_MULT;

        c->next = conns;
        conns = c;

        count++;

        printf("Added connection via %s (%p)\n", print_addr(&c->src), c);
      } else {
        c->removed = 0;
      }
    }
  }
  if (line) free(line);

  fclose(config);

  return count;
}

void update_conns(char *source_ip_file) {
  for (conn_t *c = conns; c != NULL; c = c->next) {
    c->removed = 1;
  }

  setup_conns(source_ip_file);

  conn_t **prev = &conns;
  conn_t *next;
  for (conn_t *c = conns; c != NULL; c = next) {
    next = c->next;
    if (c->removed) {
      printf("Removed connection via %s (%p)\n", print_addr(&c->src), c);

      remove_active_fd(c->fd);
      close(c->fd);
      *prev = c->next;
      free(c);
    } else {
      prev = &c->next;
    }
  }
}

void schedule_update_conns(int signal) {
  do_update_conns = 1;
}

int open_socket(conn_t *c, int quiet) {
  if (c->fd >= 0) {
    remove_active_fd(c->fd);
    close(c->fd);
    c->fd = -1;
  }

  // Set up the socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    err("Failed to open a socket");
    return -1;
  }
  struct timeval to;
  to.tv_sec = 1;
  to.tv_usec = 0;
  int ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  if (ret != 0) {
    err("Failed to set receive timeout");
    goto err;
  }
  ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  if (ret != 0) {
    err("Failed to set send timeout");
    goto err;
  }

  // Bind it to the source address
  ret = bind(fd, &c->src, sizeof(c->src));
  if (ret != 0) {
    if (!quiet) {
      err("Failed to bind to the source address %s: ", print_addr(&c->src));
    }
    goto err;
  }

  add_active_fd(fd);
  c->fd = fd;

  return 0;

err:
  close(fd);
  return -1;
}

int open_conns(char *host, char *port) {
  // Check that we can actually open & bind at least one socket
  int opened = 0;
  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (open_socket(c, 0) == 0) {
      opened++;
    }
  }
  if (opened == 0) return 0;

  // Resolve the address of the receiver
  struct addrinfo hints;
  struct addrinfo *addrs;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  int ret = getaddrinfo(host, port, &hints, &addrs);
  if (ret != 0) return 0;

  // See if we can do a SRTLA REG1 + REG2 exchange to any of the resolved addresses
  int connected = 0;
  for (struct addrinfo *addr = addrs; addr != NULL; addr = addr->ai_next) {
    memcpy(&srtla_addr, addr->ai_addr, addr->ai_addrlen);
    info("Trying to connect to %s...\n", print_addr(addr->ai_addr));

    for (conn_t *c = conns; c != NULL; c = c->next) {
      if (try_reg1(c) == 0) {
        connected = 1;
        break;
      }
    }
  }
  freeaddrinfo(addrs);
  return connected;
}


/*

Connection housekeeping

*/
void send_keepalive(conn_t *c) {
  debug("%s (%p): sending keepalive\n", print_addr(&c->src), c);
  uint16_t pkt = htobe16(SRTLA_TYPE_KEEPALIVE);
  // ignoring the result on purpose
  sendto(c->fd, &pkt, sizeof(pkt), 0, &srtla_addr, addr_len);
}

#define HOUSEKEEPING_INT 1000 // ms
void connection_housekeeping() {
  static uint64_t all_failed_at = 0;
  /* We use milliseconds here because with a seconds timer we may be
     resending a second REG2 very soon after the first one, depending
     on when the first execution happens within the seconds interval */
  static uint64_t last_ran = 0;
  uint64_t ms;
  assert(get_ms(&ms) == 0);
  if ((last_ran + HOUSEKEEPING_INT) > ms) return;

  time_t time = (time_t)(ms / 1000);

  active_connections = 0;

  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (c->fd < 0) {
      open_socket(c, 1);
      continue;
    }

    if (c->last_rcvd < (time-CONN_TIMEOUT)) {
      /* When we first detect the connection having failed,
         we reset its status and print a message */
      if (c->last_rcvd > 0) {
        info("%s (%p): connection failed, attempting to reconnect\n",
             print_addr(&c->src), c);
        c->last_rcvd = 0;
        c->last_sent = 0;
        c->window = WINDOW_MIN * WINDOW_MULT;
        c->in_flight_pkts = 0;
        for (int i = 0; i < PKT_LOG_SZ; i++) {
          c->pkt_log[i] = -1;
        }
      }
      /* As the connection has timed out on our end, the receiver might have garbage
         collected it. Try to re-establish it rather than send a keepalive */
      send_reg2(c);
      continue;
    }

    /* If a connection has received data in the last CONN_TIMEOUT seconds,
       then it's active */
    active_connections++;

    if ((c->last_sent + IDLE_TIME) < time) {
      send_keepalive(c);
    }
  }

  if (last_ran > 0 && active_connections == 0) {
    err("warning: no available connections\n");
    if (all_failed_at == 0) {
      all_failed_at = ms;
    } else if (ms > (all_failed_at + (GLOBAL_TIMEOUT * 1000))) {
      err("Failed to re-establish any connections. Giving up...\n");
      exit(EXIT_FAILURE);
    }
  } else {
    all_failed_at = 0;
  }

  last_ran = ms;
}

#define ARG_LISTEN_PORT (argv[1])
#define ARG_SRTLA_HOST  (argv[2])
#define ARG_SRTLA_PORT  (argv[3])
#define ARG_IPS_FILE    (argv[4])
int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "-v") == 0) {
    printf(VERSION "\n");
    exit(0);
  }
  if (argc != 5) exit_help();

  source_ip_file = ARG_IPS_FILE;
  int conn_count = setup_conns(source_ip_file);
  if (conn_count <= 0) {
    fprintf(stderr, "Failed to parse any IP addresses in %s\n", source_ip_file);
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in listen_addr;

  int port = parse_port(ARG_LISTEN_PORT);
  if (port < 0) exit_help();

  // Read a random connection group id for this session
  FILE *fd = fopen("/dev/urandom", "rb");
  assert(fd != NULL);
  assert(fread(srtla_id, 1, SRTLA_ID_LEN, fd) == SRTLA_ID_LEN);
  fclose(fd);

  FD_ZERO(&active_fds);

  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);
  listenfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (listenfd < 0) { 
    perror("socket creation failed"); 
    exit(EXIT_FAILURE); 
  }

  int ret = bind(listenfd, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
  if (ret < 0) { 
    perror("bind failed"); 
    exit(EXIT_FAILURE); 
  }
  add_active_fd(listenfd);

  int connected = open_conns(ARG_SRTLA_HOST, ARG_SRTLA_PORT);
  if (connected < 1) {
    fprintf(stderr, "Failed to establish any initial connections to %s:%s, aborting\n",
                    ARG_SRTLA_HOST, ARG_SRTLA_PORT);
    exit(EXIT_FAILURE);
  }

  signal(SIGHUP, schedule_update_conns);

  int info_int = LOG_PKT_INT;

  while(1) {
    if (do_update_conns) {
      update_conns(source_ip_file);
      do_update_conns = 0;
    }

    connection_housekeeping();

    fd_set read_fds = active_fds;
    struct timeval to = {.tv_sec = 0, .tv_usec = 200*1000};
    ret = select(FD_SETSIZE, &read_fds, NULL, NULL, &to);

    if (ret > 0) {
      if (FD_ISSET(listenfd, &read_fds)) {
        handle_srt_data(listenfd);
      }

      for (conn_t *c = conns; c != NULL; c = c->next) {
        if (c->fd >= 0 && FD_ISSET(c->fd, &read_fds)) {
          handle_srtla_data(c);
        }
      }
    } // ret > 0

    info_int--;
    if (info_int == 0) {
      for (conn_t *c = conns; c != NULL; c = c->next) {
        debug("%s (%p): in flight: %d, window: %d, last_rcvd %ld\n",
              print_addr(&c->src), c, c->in_flight_pkts, c->window, c->last_rcvd);
      }
      info_int = LOG_PKT_INT;
    }
  } // while(1);
}
