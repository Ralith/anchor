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
#include "Options.h"

#if UV_VERSION_MAJOR != 0 || UV_VERSION_MINOR != 11
#error unsupported libuv version
#endif

enum OptionId {
  OUTPUT,
  USER_AGENT
};

const std::vector<Option::Specifier> options({
    {OUTPUT, "output", 'o', "path", Option::Type::STRING, "file to write"},
    {USER_AGENT, "user-agent", 'u', "user agent", Option::Type::STRING, "user-agent to transmit to the server"},
  });

void usage(const char *name) {
  fprintf(stderr, "Usage: %s [options] <url>*\nOptions:\n", name);
  print_options(options);
}

int main(int argc, char **argv) {
  if(argc < 2) {
    usage(argv[0]);
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

  for(const auto &param : parse_options(argc, argv, options)) {
    switch(param.id) {
    case OUTPUT:
      client.file_name = param.parameter.string;
      break;

    case USER_AGENT:
      client.user_agent = param.parameter.string;
      break;

    default: {
      const char *url_str = param.parameter.string;
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
      client.open(std::string(url.host.base, url.host.len) + (url.port.base ? ":" + std::string(url.port.base, url.port.len) : ""),
                  std::string(url.host.base, url.host.len), port, std::move(path));

      if(client.file_name == nullptr && url.path.base != nullptr && url.path.len != 0) {
        client.file_name = url.path.base;
        for(const char *ch = url.path.base; ch != url.path.base + url.path.len - 1; ++ch) {
          if(*ch == '/') client.file_name = ch+1;
        }
      }
      break;
    }
    }
  }

  if(client.file_name == nullptr) {
    fprintf(stderr, "Output filename must be specified!\n");
    usage(argv[0]);
    return 4;
  }

  client.ares_stage();

  uv_run(&client.loop, UV_RUN_DEFAULT);
  puts("");

  return 0;
}
