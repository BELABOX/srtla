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

#define MTU 1500

#define SRT_TYPE_ACK         0x8002
#define SRT_TYPE_NAK         0x8003
#define SRTLA_TYPE_KEEPALIVE 0x9000
#define SRTLA_TYPE_ACK       0x9100

void print_help();
void exit_help();
int parse_ip(struct sockaddr_in *addr, char *ip_str);
int parse_port(char *port_str);
int parse_ip_port(struct sockaddr_in *addr, char *ip_str, char *port_str);
int add_active_fd(int fd);
int32_t get_srt_sn(void *pkt);
uint16_t get_srt_type(void *pkt);
int is_srt_ack(void *pkt);
int is_srtla_keepalive(void *pkt);
