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
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common.h"

void exit_help() {
  print_help();
  exit(EXIT_FAILURE);
}

int parse_ip(struct sockaddr_in *addr, char *ip_str) {
  in_addr_t ip = inet_addr(ip_str);
  if (ip == -1) return -1;

  memset(addr, 0, sizeof(addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = ip;

  return 0;
}

int parse_port(char *port_str) {
  int port = strtol(port_str, NULL, 10);
  if (port <= 0 || port > 65535) return -2;
  return port;
}

int parse_ip_port(struct sockaddr_in *addr, char *ip_str, char *port_str) {
  if (parse_ip(addr, ip_str) != 0) return -1;

  int port = parse_port(port_str);
  if (port < 0) return -2;
  addr->sin_port = htons(port);

  return 0;
}

extern fd_set active_fds;
extern int max_act_fd;
int add_active_fd(int fd) {
  if (fd < 0) return -1;

  if (fd > max_act_fd) max_act_fd = fd;
  FD_SET(fd, &active_fds);

  return 0;
}

int32_t get_srt_sn(void *pkt) {
  uint32_t sn = be32toh(*((uint32_t *)pkt));
  if ((sn & (1 << 31)) == 0) {
    return (int32_t)sn;
  }
  return -1;
}

uint16_t get_srt_type(void *pkt) {
  return be16toh(*((uint16_t *)pkt));
}

int is_srt_ack(void *pkt) {
  return get_srt_type(pkt) == SRT_TYPE_ACK;
}

int is_srtla_keepalive(void *pkt) {
  return get_srt_type(pkt) == SRTLA_TYPE_KEEPALIVE;
}
