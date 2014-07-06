#include <string>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

#include <uv.h>

#include "Url.h"
#include "Connection.h"
#include "Client.h"
#include "Util.h"

#if UV_VERSION_MAJOR != 0 || UV_VERSION_MINOR != 11
#error unsupported libuv version
#endif

int main(int argc, char **argv) {
  if(argc < 3) {
    fprintf(stderr, "Usage: %s <output file> <url>*\n", argv[0]);
    return 1;
  }

  Client client;
  if(int err = client.ares.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 2;
  }

  if(int err = client.dns.start()) {
    fprintf(stderr, "FATAL: c-ares: %s\n", ares_strerror(err));
    return 3;
  }

  client.file_name = argv[1];

  for(int i = 2; i < argc; ++i) {
    const char *url_str = argv[i];
    Url url(url_str);

    if(url.scheme.base != nullptr &&
       url.scheme.len != 4 &&
       0 != strncmp(url.scheme.base, "http", url.scheme.len)) {
      fprintf(stderr, "WARN: Skipping non-http URL %s\n", url_str);
      continue;
    }

    if(url.host.base == nullptr || url.host.len == 0) {
      fprintf(stderr, "WARN: Skipping URL missing host %s\n(did you forget the leading \"//\"?)\n", url_str);
      continue;
    }

    const in_port_t port = url.port.base == nullptr ? 80 : strtol(url.port.base, nullptr, 10);
    if(port == 0) {
      fprintf(stderr, "WARN: Skipping URL with invalid port: %s\n", url_str);
      continue;
    }
    std::string path = url.path.base != nullptr ? std::string(url.path.base, url.path.len) : "/";
    client.open(std::string(url.host.base, url.host.len), port, std::move(path));
  }

  client.ares_stage();

  uv_run(&client.loop, UV_RUN_DEFAULT);
  puts("");

  return 0;
}
