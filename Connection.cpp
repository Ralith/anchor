#include "Connection.h"

#include <sstream>

#include <cstring>
#include <cstdio>

#include "Client.h"
#include "Util.h"

namespace {
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
    connection.close();
    return 0;
  }

  connection.state = Connection::State::IDLE;

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
        connection.close();
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
  connection.stats.start_time = uv_now(&connection.client.loop);
  connection.stats.bytes = 0;
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
  connection.stats.bytes += length;
  connection.client.progress(length);

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
    connection.close();
    return;
  }

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
  auto http_errno = HTTP_PARSER_ERRNO(&connection.parser);
  if(http_errno == HPE_CB_message_complete) {
    connection.status = "";
    http_parser_init(&connection.parser, HTTP_RESPONSE);
    connection.client.schedule_work();
  }

  if(parsed != static_cast<size_t>(nread)) {
    switch(http_errno) {
    case HPE_CB_message_complete:
      assert(false);

    default:
      if(nread == UV__EOF) {
        assert(parsed == 0);
        connection.state = Connection::State::COMPLETE;
      } else {
        fprintf(stderr, "WARN: HTTP parse error: %s: %s\n", http_errno_name(http_errno), http_errno_description(http_errno));
        connection.state = Connection::State::FAILED;
      }
      connection.close();
    }
  }
}

void write_cb(uv_write_t* req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);
  if(status < 0) {
    fprintf(stderr, "WARN: Failed to send HTTP request to %s: %s\n", connection.host_port.c_str(), uv_strerror(status));
    connection.state = Connection::State::FAILED;
    connection.close();
    return;
  }
}

void connect_cb(uv_connect_t *req, int status) {
  auto &connection = *reinterpret_cast<Connection *>(req->data);

  if(status < 0) {
    fprintf(stderr, "WARN: Connection to %s failed: %s\n", connection.host_port.c_str(), uv_strerror(status));
    connection.state = Connection::State::FAILED;
    connection.close();
    return;
  }

  connection.state = Connection::State::HEAD;

  uv_buf_t bufs[7];
  bufs[0].base = const_cast<char *>("HEAD ");
  bufs[0].len = strlen(bufs[0].base);
  bufs[1].base = const_cast<char *>(connection.path.data());
  bufs[1].len = connection.path.size();
  bufs[2].base = const_cast<char *>(" HTTP/1.1\r\nHost: ");
  bufs[2].len = strlen(bufs[2].base);
  bufs[3].base = const_cast<char *>(connection.host_port.data());
  bufs[3].len = connection.host_port.size();
  bufs[4].base = const_cast<char *>("\r\nUser-Agent: ");
  bufs[4].len = strlen(bufs[4].base);
  bufs[5].base = const_cast<char *>(connection.client.user_agent);
  bufs[5].len = strlen(bufs[5].base);
  bufs[6].base = const_cast<char *>("\r\nConnection: keep-alive\r\n\r\n");
  bufs[6].len = strlen(bufs[6].base);

  uv_write(&connection.write_req, reinterpret_cast<uv_stream_t *>(&connection.handle), bufs, elementsof(bufs), write_cb);
  uv_read_start(reinterpret_cast<uv_stream_t *>(&connection.handle), alloc_cb, read_cb);
}
}

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

void Connection::connect(in_addr ip, in_port_t port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = ip;
  uv_tcp_connect(&connect_req, &handle, reinterpret_cast<struct sockaddr *>(&addr), connect_cb);
}

void Connection::connect(in6_addr ip, in_port_t port) {
  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = ip;
  uv_tcp_connect(&connect_req, &handle, reinterpret_cast<struct sockaddr *>(&addr), connect_cb);
}

void Connection::close() {
  uv_close(reinterpret_cast<uv_handle_t *>(&handle), nullptr);
  if(begin != nullptr && begin != end) {
    client.chunks.push_back(Chunk{static_cast<size_t>(begin - client.file_data), static_cast<size_t>(end - begin)});
  }
  client.balance_chunks();
}

void Connection::get(Chunk chunk) {
  assert(state == Connection::State::IDLE);
  state = Connection::State::GET_HEADERS;

  begin = client.file_data + chunk.off;
  end = begin + chunk.len;

  std::ostringstream builder;
  builder << "GET " << path << " HTTP/1.1\r\n"
          << "Host: " << host_port << "\r\n"
          << "Range: bytes=" << begin - client.file_data
          << "-" << end - client.file_data - 1 << "\r\n"
          << "User-Agent: " << client.user_agent << "\r\n"
          << "Connection: keep-alive\r\n"
          << "\r\n";
  get_req = builder.str();

  uv_buf_t buf;
  buf.base = const_cast<char *>(get_req.data());
  buf.len = get_req.size();

  uv_write(&write_req, reinterpret_cast<uv_stream_t *>(&handle), &buf, 1, write_cb);
}
