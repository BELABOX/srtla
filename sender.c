/*
    srtla - SRT transport proxy with link aggregation
    Copyright (C) 2020 BELAbox project

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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common.h"

#define PKT_LOG_SZ 256
#define TIMEOUT 3

#define min(a, b) ((a < b) ? a : b)
#define max(a, b) ((a > b) ? a : b)
#define min_max(a, l, h) (max(min((a), (h)), (l)))

#define WINDOW_MIN 1
#define WINDOW_DEF 20
#define WINDOW_MAX 60
#define WINDOW_MULT 1000
#define WINDOW_DECR 100
#define WINDOW_INCR 30

#define PKT_INT 20

//#define DEBUG 1
#ifdef DEBUG
  #define debug(...) fprintf (stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

typedef struct conn {
  struct conn *next;
  int fd;
  time_t last_rcvd;
  struct sockaddr_in src;
  int in_flight_pkts;
  int window;
  int pkt_idx;
  int pkt_log[PKT_LOG_SZ];
} conn_t;

int listenfd;
struct sockaddr_in remote_addr, local_peer;
conn_t *conns = NULL;
fd_set active_fds;
int max_act_fd = -1;
int do_send_keepalive = 0;

void alarm_handler() {
  do_send_keepalive = 1;
}

void print_help() {
  fprintf(stderr, "Syntax: sender SRT_LISTEN_PORT SRTLA_HOST SRTLA_PORT\n");
}

int get_sec(time_t *ts) {
  struct timespec time;
  int ret = clock_gettime(CLOCK_MONOTONIC_COARSE, &time);
  if (ret == 0) {
    *ts = time.tv_sec;
    return 0;
  }
  return -1;
}

int get_pkt_idx(int idx, int increment) {
  idx = idx + increment;
  if (idx < 0) idx += PKT_LOG_SZ;
  idx %= PKT_LOG_SZ;
  assert(idx >= 0 && idx < PKT_LOG_SZ);
  return idx;
}

void register_nak(int32_t packet) {
  debug("nak %d ", packet);

  for (conn_t *c = conns; c != NULL; c = c->next) {
    int idx = get_pkt_idx(c->pkt_idx, -1);
    for (int i = idx; i != c->pkt_idx; i = get_pkt_idx(i, -1)) {
      if (c->pkt_log[i] == packet) {
        c->pkt_log[i] = -1;
        // It might be better to use exponential decay like this
        //c->window = c->window * 998 / 1000;
        c->window -= WINDOW_DECR;
        c->window = max(c->window, WINDOW_MIN*WINDOW_MULT);
        debug("found %p\n", c);
        return;
      }
    }
  }

  debug("not found\n");
}

void reg_pkt(conn_t *c, int32_t packet) {
  debug("Register %d at idx %d for %p\n", packet, c->pkt_idx, c);
  c->pkt_log[c->pkt_idx] = packet;
  c->pkt_idx++;
  c->pkt_idx %= PKT_LOG_SZ;

  c->in_flight_pkts++;
}

void send_keepalive(conn_t *c) {
  debug("sending keepalive via %p\n", c);
  uint16_t pkt = htobe16(SRTLA_TYPE_KEEPALIVE);
  // ignoring the result on purpose
  socklen_t addr_len = sizeof(remote_addr);
  sendto(c->fd, &pkt, sizeof(pkt), MSG_CONFIRM, (struct sockaddr *) &remote_addr, addr_len);
}

void send_keepalive_all() {
  time_t time;
  assert(get_sec(&time) == 0);

  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (c->fd >= 0 && c->last_rcvd < (time-1)) {
      send_keepalive(c);
    }
  }
}


int open_conn(conn_t *c) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("Failed to open socket");
    return -1;
  }

  struct timeval to;
  to.tv_sec = 1;
  to.tv_usec = 0;
  int ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  if (ret != 0) {
    perror("Failed to set receive timeout");
    close(fd);
    return -1;
  }
  ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  if (ret != 0) {
    perror("Failed to set send timeout");
    close(fd);
    return -1;
  }

  ret = bind(fd, (struct sockaddr*)&c->src, sizeof(c->src));
  if (ret != 0) {
    perror("Failed to bind to the source address");
    close(fd);
    return -1;
  }

  return fd;
}

int open_conns(char *source_ip_file) {
  FILE *config = fopen(source_ip_file, "r");
  if (config == NULL) {
    perror("Failed to open the source ip list file: ");
    exit_help();
  }

  int total_connected = 0;
  char *line = NULL;
  size_t line_len = 0;
  while(getline(&line, &line_len, config) >= 0) {
    char *nl;
    if ((nl = strchr(line, '\n'))) {
      *nl = '\0';
    }

    conn_t *c = calloc(1, sizeof(conn_t));
    assert(c != NULL);

    c->fd = -1;
    c->next = conns;

    c->window = WINDOW_DEF * WINDOW_MULT;

    int ret = parse_ip(&c->src, line);
    if (ret != 0) {
      free(c);
    } else {
      fprintf(stderr, "Connecting from %s: ", line);

      int fd = open_conn(c);
      if (fd >= 0) {
        fprintf(stderr, "done\n");
        c->fd = fd;
        total_connected++;
        add_active_fd(fd);
        send_keepalive(c);
      }

      conns = c;
    }
  }
  if (line) free(line);
  
  return total_connected;
}

conn_t *select_conn() {
  conn_t *min_c = NULL;
  int max_score = 0;
  int max_window = 0;

  for (conn_t *c = conns; c != NULL; c = c->next) {
    if (c->window > max_window) {
      max_window = c->window;
    }
  }

  time_t t;
  assert(get_sec(&t) == 0);

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

    if (c->last_rcvd < (t-TIMEOUT)) {
      debug("%p timed out, will ignore it\n", c);
      continue;
    }

    int score = c->window / (c->in_flight_pkts + 1);
    if (score > max_score) {
      min_c = c;
      max_score = score;
    }
  }

  if (min_c == NULL) {
    fprintf(stderr, "warning: no available connections\n");
  }
  return min_c;
}

void handle_srt_data(int fd) {
  char buf[MTU];
  socklen_t addr_len = sizeof(local_peer);
  int n = recvfrom(fd, &buf, MTU, MSG_WAITALL, (struct sockaddr *) &local_peer, &addr_len);

  conn_t *c = select_conn();
  if (c) {
    int32_t sn = get_srt_sn(buf);
    int ret = sendto(c->fd, &buf, n, MSG_CONFIRM, (struct sockaddr *) &remote_addr, addr_len);
    if (ret == n) {
      if (sn >= 0) {
        reg_pkt(c, sn);
      }
    } else {
      /* If sending the packet fails, adjust the timestamp to disable
         the link until it starts returning keep alive packet */
      c->last_rcvd = 0;
    }
  } else {
    fprintf(stderr, "no SRTLA link\n");
  }
}

void register_srtla_ack(int32_t ack) {
  int found = 0;

  for (conn_t *c = conns; c != NULL; c = c->next) {
    int idx = get_pkt_idx(c->pkt_idx, -1);
    for (int i = idx; i != c->pkt_idx && !found; i = get_pkt_idx(i, -1)) {
      if (c->pkt_log[i] == ack) {
        found = 1;
        c->in_flight_pkts--;
        c->pkt_log[i] = -1;

        if (c->in_flight_pkts*WINDOW_MULT > c->window) {
          c->window += WINDOW_INCR - 1;
        }

        break;
      }
    }
    c->window += 1;
    c->window = min(c->window, WINDOW_MAX*WINDOW_MULT);
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

  int n = recvfrom(c->fd, &buf, MTU, MSG_WAITALL, NULL, NULL);
  get_sec(&c->last_rcvd);

  uint16_t packet_type = get_srt_type(buf);
  
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
    case SRTLA_TYPE_ACK: {
      debug("got a srtla ack from %p\n", c);
      uint32_t *acks = (uint32_t *)buf;
      for (int i = 1; i < n/4; i++) {
        uint32_t id = be32toh(acks[i]);
        debug("  ack %d\n", id);
        register_srtla_ack(id);
      }
      return; // don't send to SRT
    }
    case SRTLA_TYPE_KEEPALIVE:
      debug("got keepalive from %p\n", c);
      return; // don't send to SRT
  } // switch

  socklen_t addr_len = sizeof(local_peer);
  int ret = sendto(listenfd, &buf, n, MSG_CONFIRM, (struct sockaddr *) &local_peer, addr_len);
  assert(ret == n);
}

int main(int argc, char **argv) {
  if (argc != 5) exit_help();

  struct sockaddr_in listen_addr;

  int port = parse_port(argv[1]);
  if (port < 0) exit_help();

  FD_ZERO(&active_fds);

  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);
  if ((listenfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { 
    perror("socket creation failed"); 
    exit(EXIT_FAILURE); 
  }

  if (bind(listenfd, (const struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) { 
    perror("bind failed"); 
    exit(EXIT_FAILURE); 
  }
  add_active_fd(listenfd);

  signal(SIGALRM, alarm_handler);
  alarm(1);

  int ret = parse_ip_port(&remote_addr, argv[2], argv[3]);
  if (ret != 0) {
    fprintf(stderr, "Failed to parse the target IP and/or port\n\n");
    exit_help();
  }

  int connected = open_conns(argv[4]);
  if (connected < 1) {
    fprintf(stderr, "Failed to establish any initial connections, aborting\n");
    exit(EXIT_FAILURE);
  }

  int info_int = PKT_INT;

  while(1) {
    fd_set read_fds = active_fds;

    ret = select(FD_SETSIZE, &read_fds, NULL, NULL, NULL);

    if (do_send_keepalive) {
      send_keepalive_all();
      do_send_keepalive = 0;
      alarm(1);
    }

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
        debug("%p: in flight: %d, window: %d\n", c, c->in_flight_pkts, c->window);
      }
      info_int = PKT_INT;
    }
  } // while(1);
}
