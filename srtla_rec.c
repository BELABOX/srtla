/*
    srtla - SRT transport proxy with link aggregation
    Copyright (C) 2020 BELABOX project

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
#include <endian.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common.h"

#define RECV_ACK_INT 10
typedef struct srtla_conn {
  struct srtla_conn *next;
  struct sockaddr_in addr;
  int recv_idx;
  uint32_t recv_log[RECV_ACK_INT];
} srtla_conn_t;

typedef struct {
  uint32_t type;
  uint32_t acks[RECV_ACK_INT];
} srtla_ack_pkt;

int strla_fd, srt_fd;
struct sockaddr_in srtla_addr, srt_addr;
srtla_conn_t *srtla_conns = NULL;
fd_set active_fds;
int max_act_fd = -1;

void print_help() {
  fprintf(stderr, "Syntax: srtla_rec SRTLA_LISTEN_PORT SRT_HOST SRT_PORT\n");
}

void handle_srt_data(int fd) {
  char buf[MTU];

  int n = recvfrom(fd, &buf, MTU, 0, NULL, NULL);
  socklen_t addr_len = sizeof(srtla_addr);

  // ACK
  if (is_srt_ack(buf)) {
    // Broadcast SRT ACKs over all connections for timely delivery
    for (srtla_conn_t *c = srtla_conns; c != NULL; c = c->next) {
      sendto(strla_fd, &buf, n, 0, (struct sockaddr *)&(c->addr), addr_len);
    }
  } else {
    // send other packets over the most recently used SRTLA connection
    int ret = sendto(strla_fd, &buf, n, 0, (struct sockaddr *) &srtla_addr, addr_len);
    assert(ret == n);
  }
}

srtla_conn_t *get_conn(struct sockaddr_in *addr, size_t len) {
  for (srtla_conn_t *i = srtla_conns; i != NULL; i = i->next) {
    int ret = memcmp(&i->addr, addr, len);
    if (ret == 0) return i;
  }

  srtla_conn_t *n = malloc(sizeof(srtla_conn_t));
  n->addr = *addr;
  n->next = srtla_conns;
  srtla_conns = n;

  return n;
}

void register_packet(srtla_conn_t *c, int32_t sn) {
  // store the sequence numbers in BE, as they're transmitted over the network
  c->recv_log[c->recv_idx++] = htobe32(sn);

  if (c->recv_idx == RECV_ACK_INT) {
    srtla_ack_pkt ack;
    ack.type = htobe32(SRTLA_TYPE_ACK << 16);
    memcpy(&ack.acks, &c->recv_log, sizeof(c->recv_log));

    int ret = sendto(strla_fd, &ack, sizeof(ack), 0, (struct sockaddr *) &c->addr, sizeof(c->addr));
    assert(ret == sizeof(ack));

    c->recv_idx = 0;
  }
}

void handle_srtla_data(int fd) {
  char buf[MTU];

  socklen_t addr_len = sizeof(srtla_addr);
  int n = recvfrom(fd, &buf, MTU, 0, (struct sockaddr *) &srtla_addr, &addr_len);

  // Resend SRTLA keep-alive packets to the sender
  if (is_srtla_keepalive(buf)) {
    addr_len = sizeof(srtla_addr);
    sendto(fd, &buf, n, 0, (struct sockaddr *) &srtla_addr, addr_len);
    return;
  }

  // Keep track of the received data packets to send SRTLA ACKs
  int32_t sn = get_srt_sn(buf);
  if (sn >= 0) {
    srtla_conn_t *c = get_conn(&srtla_addr, addr_len);
    register_packet(c, sn);
  }

  // Forward the packet to SRT
  int ret = sendto(srt_fd, &buf, n, 0, (struct sockaddr *) &srt_addr, addr_len);
  assert(ret == n);
}

int main(int argc, char **argv) {
  if (argc != 4) exit_help();

  struct sockaddr_in listen_addr;

  int port = parse_port(argv[1]);
  if (port < 0) exit_help();

  FD_ZERO(&active_fds);

  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(port);
  if ((strla_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("socket creation failed");
    exit(EXIT_FAILURE);
  }

  if (bind(strla_fd, (const struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
  add_active_fd(strla_fd);

  int ret = parse_ip_port(&srt_addr, argv[2], argv[3]);
  if (ret != 0) {
    fprintf(stderr, "Failed to parse the SRT IP and/or port\n");
    exit_help();
  }

  if ((srt_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("socket creation failed");
    exit(EXIT_FAILURE);
  }
  add_active_fd(srt_fd);

  while(1) {
    fd_set read_fds = active_fds;
    select(FD_SETSIZE, &read_fds, NULL, NULL, NULL);

    for (int i = 0; i <= max_act_fd; i++) {
      if (FD_ISSET(i, &read_fds)) {
        if (i == strla_fd) {
          handle_srtla_data(i);
        } else {
          handle_srt_data(i);
        }
      } // FD_ISSET
    } // for
  } // while(1);
}

