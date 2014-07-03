#include <vector>
#include <deque>
#include <sstream>
#include <string>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

#include <arpa/nameser.h>

#include <unistd.h>
#include <sys/mman.h>

#include <uv.h>

#include <ares.h>

#include "http-parser/http_parser.h"

#if UV_VERSION_MAJOR != 0 || UV_VERSION_MINOR != 11
#error unsupported libuv version
#endif

#define elementsof(array) (sizeof(array) / sizeof((array)[0]))

class Ares {
public:
  ~Ares() { if(started_) ares_library_cleanup(); }

  int start() {
    int result = ares_library_init(ARES_LIB_INIT_ALL);
    assert(!started_);
    started_ = result == 0;
    return result;
  }

  class Channel {
  public:
    ares_channel channel;

    ~Channel() { if(started_) ares_destroy(channel); }

    int start() {
      assert(!started_);
      int result = ares_init(&channel);
      started_ = result == 0;
      return result;
    }

  private:
    bool started_ = false;
  };

private:
  bool started_ = false;
};

struct AresPoll {
  uv_poll_t handle;
  int fd;
  ares_channel channel;
};

struct Client;

struct Connection {
  enum class State { CONNECT, HEAD, GET_HEADERS, GET_COPY, GET_DIRECT, FAILED, COMPLETE };

  Connection(Client &s, std::string h, std::string p) : client(s), host_port(std::move(h)), path(std::move(p)) {
    connect_req.data = this;
    write_req.data = this;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = this;
  }

  bool head(uint64_t size);

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

struct Resolution {
  const std::string host;
  const in_port_t port;
  const std::string path;
  Client &client;
};

struct Client {
  Client() {
    uv_loop_init(&loop);
    uv_timer_init(&loop, &ares_timer);
    ares_timer.data = this;
  }

  ~Client() {
    uv_loop_close(&loop);
  }

  void ares_stage();

  void init_file(uint64_t size);

  uv_loop_t loop;

  Ares ares;
  Ares::Channel dns;
  uv_timer_t ares_timer;
  std::vector<AresPoll> ares_polls;

  std::deque<Resolution> resolutions;
  std::deque<Connection> connections;

  std::string file_name;
  uint64_t file_size = ~0;
  int fd = -1;
  uint8_t *file_data = nullptr;
};

bool Connection::head(uint64_t size) {
  if(client.file_size == ~0ULL) {
    client.init_file(size);
    return false;
  }

  if(client.file_size == size) {
    return false;
  }

  return true;
}

void Client::init_file(uint64_t size) {
  file_size = size;
  fd = open(file_name.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if(fd == -1) {
    fprintf(stderr, "FATAL: Failed to open file %s for writing: %s\n", file_name.c_str(), strerror(errno));
    exit(1);
  }

  {
    int result = posix_fallocate(fd, 0, size);
    if(result != 0) {
      fprintf(stderr, "FATAL: Couldn't allocate %lu bytes of file space for output: %s\n", size, strerror(result));
    }
  }

  file_data = static_cast<uint8_t *>(mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0));
  if(file_data == MAP_FAILED) {
    fprintf(stderr, "FATAL: mmap: %s\n", strerror(errno));
    abort();
  }

  size_t live_connections = 0;
  for(auto &conn : connections) {
    if(conn.state != Connection::State::FAILED)
      ++live_connections;
  }
  size_t per_connection = size / live_connections;
  size_t accum = 0;
  for(auto &conn : connections) {
    if(conn.state != Connection::State::FAILED) {
      conn.begin = file_data + accum;
      accum += per_connection;
      conn.end = file_data + accum;
    }
  }
  for(auto it = connections.rbegin(); it != connections.rend(); --it) {
    if(it->state != Connection::State::FAILED) {
      it->end = file_data + size;
      break;
    }
  }
}

void alloc_cb(uv_handle_t* handle,
              size_t suggested_size,
              uv_buf_t* buf) {
  (void)suggested_size;
  auto &connection = *reinterpret_cast<Connection *>(handle);
  if(connection.state == Connection::State::GET_COPY)
    connection.state = Connection::State::GET_DIRECT;

  if(connection.state == Connection::State::GET_DIRECT) {
    buf->base = reinterpret_cast<char *>(connection.begin);
    buf->len = connection.end - connection.begin;
  } else {
    static char buffer[1024 * 1024];
    buf->base = buffer;
    buf->len = elementsof(buffer);
  }
}

void write_cb(uv_write_t* req, int status);

int message_complete_cb(http_parser *parser) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  if((connection.state == Connection::State::HEAD && parser->status_code != 200) ||
     ((connection.state == Connection::State::GET_HEADERS ||
       connection.state == Connection::State::GET_COPY ||
       connection.state == Connection::State::GET_DIRECT)
      && parser->status_code != 206)) {
    fprintf(stderr, "WARN: Abandoning connection to %s due to HTTP %u %s\n", connection.host_port.c_str(), parser->status_code, connection.status.c_str());
    connection.state = Connection::State::FAILED;
    uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
    return 0;
  }

  if(connection.state == Connection::State::HEAD) {
    connection.state = Connection::State::GET_HEADERS;

    std::ostringstream builder;
    builder << "GET " << connection.path << " HTTP/1.1\r\n"
            << "Host: " << connection.host_port << "\r\n"
            << "Range: bytes=" << connection.begin - connection.client.file_data
            << "-" << connection.end - connection.client.file_data - 1 << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";
    connection.get_req = builder.str();

    uv_buf_t buf;
    buf.base = const_cast<char *>(connection.get_req.data());
    buf.len = connection.get_req.size();

    uv_write(&connection.write_req, reinterpret_cast<uv_stream_t *>(&connection.handle), &buf, 1, write_cb);
  } else {
    connection.state = Connection::State::COMPLETE;
  }

  return 1;
}

int header_complete(http_parser *parser) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  if(connection.header_name == "Content-Length") {
    char *endptr;
    uint64_t size = strtoll(connection.header_data.c_str(), &endptr, 10);
    if(endptr == connection.header_data.c_str() + connection.header_data.size()) {
      if(connection.head(size)) {
        fprintf(stderr, "WARN: %s served file of %lu bytes, expected %lu bytes\n", connection.host_port.c_str(), size,
                connection.client.file_size);
        connection.state = Connection::State::FAILED;
        uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
      }
    } else {
      fprintf(stderr, "WARN: Failed to parse Content-Length header\n");
    }
  }
  connection.header_name.clear();
  connection.header_data.clear();

  return 1;
}

int header_field_cb(http_parser *parser, const char *at, size_t length) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  if(!connection.header_data.empty()) {
    header_complete(parser);
  }
  connection.header_name += std::string(at, length);
  return 0;
}

int header_value_cb(http_parser *parser, const char *at, size_t length) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  connection.header_data += std::string(at, length);
  return 0;
}

int headers_complete_cb(http_parser *parser) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  if(connection.state == Connection::State::GET_HEADERS) {
    connection.state = Connection::State::GET_COPY;
  }
  return connection.state == Connection::State::HEAD ? header_complete(parser) : 0;
}

int status_cb(http_parser *parser, const char *at, size_t length) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);
  connection.status += std::string(at, length);
  return 0;
}

int body_cb(http_parser *parser, const char *at, size_t length) {
  auto &connection = *reinterpret_cast<Connection *>(parser->data);

  if(connection.begin + length > connection.end) {
    fprintf(stderr, "WARN: Server tried to overflow output\n");
    return 1;
  }

  if(connection.state == Connection::State::GET_COPY) {
    memcpy(connection.begin, at, length);
  }
  connection.begin += length;

  return 0;
}

void read_cb(uv_stream_t* stream,
             ssize_t nread,
             const uv_buf_t* buf) {
  auto &connection = *reinterpret_cast<Connection *>(stream);
  if(nread < 0 && nread != UV__EOF) {
    fprintf(stderr, "WARN: Closing connection to %s due to read error: %s\n", connection.host_port.c_str(),
            uv_strerror(nread));
    connection.state = Connection::State::FAILED;
    uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
    return;
  }

parse:
  http_parser_settings settings{};
  settings.on_status = status_cb;
  settings.on_message_complete = message_complete_cb;
  settings.on_headers_complete = headers_complete_cb;
  if(connection.state == Connection::State::HEAD) {
    settings.on_header_field = header_field_cb;
    settings.on_header_value = header_value_cb;
  }
  settings.on_body = body_cb;
  auto parsed = http_parser_execute(&connection.parser, &settings, buf->base, nread == UV__EOF ? 0 : nread);
  if(parsed != static_cast<size_t>(nread)) {
    auto http_errno = HTTP_PARSER_ERRNO(&connection.parser);
    switch(http_errno) {
    case HPE_CB_message_complete:
      assert(parsed == 0);
      connection.status = "";
      http_parser_init(&connection.parser, HTTP_RESPONSE);
      goto parse;

    default:
      if(nread == UV__EOF) {
        assert(parsed == 0);
        connection.state = Connection::State::COMPLETE;
      } else {
        fprintf(stderr, "WARN: HTTP parse error: %s: %s\n", http_errno_name(http_errno), http_errno_description(http_errno));
        connection.state = Connection::State::FAILED;
      }
      uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
    }
  }
}

void write_cb(uv_write_t* req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);
  if(status < 0) {
    fprintf(stderr, "WARN: Failed to send HTTP request to %s: %s\n", connection.host_port.c_str(), uv_strerror(status));
    connection.state = Connection::State::FAILED;
    uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
    return;
  }
}

void connect_cb(uv_connect_t *req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);

  if(status < 0) {
    fprintf(stderr, "WARN: Connection to %s failed: %s\n", connection.host_port.c_str(), uv_strerror(status));
    connection.state = Connection::State::FAILED;
    uv_close(reinterpret_cast<uv_handle_t *>(&connection.handle), nullptr);
    return;
  }

  connection.state = Connection::State::HEAD;

  uv_buf_t bufs[5];
  bufs[0].base = const_cast<char *>("HEAD ");
  bufs[0].len = strlen(bufs[0].base);
  bufs[1].base = const_cast<char *>(connection.path.data());
  bufs[1].len = connection.path.size();
  bufs[2].base = const_cast<char *>(" HTTP/1.1\r\nHost: ");
  bufs[2].len = strlen(bufs[2].base);
  bufs[3].base = const_cast<char *>(connection.host_port.data());
  bufs[3].len = connection.host_port.size();
  bufs[4].base = const_cast<char *>("\r\nConnection: keep-alive\r\n\r\n");
  bufs[4].len = strlen(bufs[4].base);

  uv_write(&connection.write_req, reinterpret_cast<uv_stream_t *>(&connection.handle), bufs, elementsof(bufs), write_cb);
  uv_read_start(reinterpret_cast<uv_stream_t *>(&connection.handle), alloc_cb, read_cb);
}

void query4_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  auto &res = *reinterpret_cast<Resolution *>(arg);
  if(status != ARES_SUCCESS) {
    fprintf(stderr, "WARN: DNS resolution failed: %s\n", ares_strerror(status));
    return;
  }
  assert(abuf != nullptr && alen != 0);
  struct ares_addrttl addrs[1];
  int naddrs = elementsof(addrs);
  auto result = ares_parse_a_reply(abuf, alen, nullptr, addrs, &naddrs);
  if(result != ARES_SUCCESS) {
    fprintf(stderr, "WARN: Couldn't parse reply from DNS server: %s\n", ares_strerror(status));
    return;
  }

  res.client.connections.emplace_back(res.client, res.host + ":" + std::to_string(res.port), res.path);
  uv_tcp_init(&res.client.loop, &res.client.connections.back().handle);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(res.port);
  addr.sin_addr = addrs[0].ipaddr;
  uv_tcp_connect(&res.client.connections.back().connect_req, &res.client.connections.back().handle,
                 reinterpret_cast<struct sockaddr *>(&addr), connect_cb);
}

void ares_process_cb(uv_poll_t *handle, int status, int events) {
  auto &ares_poll = *reinterpret_cast<AresPoll *>(handle);
  if(status < 0) {
    // FIXME
    fprintf(stderr, "FATAL: %s\n", uv_strerror(status));
    abort();
  }
  ares_process_fd(ares_poll.channel,
                  events & UV_READABLE ? ares_poll.fd : ARES_SOCKET_BAD,
                  events & UV_WRITABLE ? ares_poll.fd : ARES_SOCKET_BAD);
  auto &client = *reinterpret_cast<Client *>(handle->data);
  client.ares_stage();
}

void ares_timer_cb(uv_timer_t *timer) {
  auto &client = *reinterpret_cast<Client *>(timer->data);
  ares_process_fd(client.dns.channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  client.ares_stage();
}

void Client::ares_stage() {
  uv_timer_stop(&ares_timer);
  for(auto &poll : ares_polls) {
    uv_poll_stop(&poll.handle);
  }
  ares_polls.clear();

  struct timeval tv;
  if(nullptr != ares_timeout(dns.channel, nullptr, &tv)) {
    uint64_t timeout = tv.tv_usec / 1000 + tv.tv_sec * 1000;
    uv_timer_start(&ares_timer, ares_timer_cb, timeout, 0);
  }

  fd_set read_fds, write_fds;
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);
  int nfds = ares_fds(dns.channel, &read_fds, &write_fds);

  ares_polls.reserve(nfds);
  for(int fd = 0; fd < nfds; ++fd) {
    if(FD_ISSET(fd, &read_fds)) {
      ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(&loop, &ares_polls.back().handle, fd);
      ares_polls.back().handle.data = this;
      uv_poll_start(&ares_polls.back().handle, UV_READABLE, ares_process_cb);
    }

    if(FD_ISSET(fd, &write_fds)) {
      ares_polls.emplace_back(AresPoll{{}, fd, dns.channel});
      uv_poll_init(&loop, &ares_polls.back().handle, fd);
      ares_polls.back().handle.data = this;
      uv_poll_start(&ares_polls.back().handle, UV_WRITABLE, ares_process_cb);
    }
  }
}

int main(int argc, char **argv) {
  if(argc < 3) {
    fprintf(stderr, "FAIL: Insufficient arguments\n");
  }

  Client client;
  if(int err = client.ares.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 1;
  }

  if(int err = client.dns.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 2;
  }

  client.file_name = argv[1];

  for(int i = 2; i < argc; ++i) {
    const char *url = argv[i];
    // TODO: Custom URL parser
    http_parser_url result {};
    if(0 != http_parser_parse_url(url, strlen(url), 0, &result)) {
      fprintf(stderr, "WARN: Skipping unparsable URL %s\n", url);
      continue;
    }

    if(result.field_set & UF_SCHEMA &&
       0 != strncmp(url + result.field_data[UF_SCHEMA].off, "http", result.field_data[UF_SCHEMA].len)) {
      fprintf(stderr, "WARN: Skipping non-http URL %s\n", url);
      continue;
    }

    if(!(result.field_set & UF_HOST)) {
      fprintf(stderr, "WARN: Skipping URL missing host?! %s\n", url);
      continue;
    }

    char hostname[NS_MAXDNAME + 1];
    if(result.field_data[UF_HOST].len > sizeof(hostname)) {
      fprintf(stderr, "WARN: Skipping excessively long domain name %s\n", url);
      continue;
    }
    memcpy(hostname, url + result.field_data[UF_HOST].off, result.field_data[UF_HOST].len);
    hostname[result.field_data[UF_HOST].len] = '\0';
    const in_port_t port = result.port ? result.port : 80;
    std::string path = result.field_set & UF_PATH
        ? std::string(url + result.field_data[UF_PATH].off, result.field_data[UF_PATH].len)
        : "/";
    client.resolutions.emplace_back(
      Resolution{std::string(url + result.field_data[UF_HOST].off, result.field_data[UF_HOST].len), port,
            std::move(path), client});
    ares_query(client.dns.channel, hostname, ns_c_in, ns_t_a, query4_cb, &client.resolutions.back());
    //ares_query(client.dns.channel, hostname, ns_c_in, ns_t_aaaa, query6_cb, nullptr);
  }

  client.ares_stage();

  uv_run(&client.loop, UV_RUN_DEFAULT);

  if(client.fd != -1) {
    munmap(client.file_data, client.file_size);
    close(client.fd);
  }

  return 0;
}
