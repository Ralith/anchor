#ifndef ANCHOR_CONNECTION_H_
#define ANCHOR_CONNECTION_H_

#include <string>
#include <cinttypes>

#include <arpa/inet.h>

#include <uv.h>

#include "http-parser/http_parser.h"

struct Client;

struct Chunk {
  size_t off, len;
};

struct Connection {
  // Short-term operations <= IDLE for scheduling convenience
  enum class State { CONNECT, HEAD, IDLE, GET_HEADERS, GET_COPY, GET_DIRECT, FAILED, COMPLETE };

  Connection(Client &s, std::string h, std::string p) : client(s), host_port(std::move(h)), path(std::move(p)) {
    connect_req.data = this;
    write_req.data = this;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = this;
  }

  bool head(uint64_t size);
  void connect(in_addr ip, in_port_t port);
  void connect(in6_addr ip, in_port_t port);
  void close();
  void get(Chunk chunk);

  uv_tcp_t handle;
  uv_connect_t connect_req;
  uv_write_t write_req;
  std::string get_req;

  State state = State::CONNECT;
  http_parser parser;
  std::string status;
  std::string header_name;
  std::string header_data;
  uint8_t *begin = nullptr;
  uint8_t *end = nullptr;

  Client &client;
  const std::string host_port;
  const std::string path;
};

#endif
