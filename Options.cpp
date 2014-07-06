#include "Options.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

std::vector<Option> parse_options(int argc, char *argv[], const std::vector<Option::Specifier> &specifiers) {
  std::vector<Option> result;
  result.reserve(argc-1);
  for(int i = 1; i < argc; ++i) {
    auto len = strlen(argv[i]);
    if(len == 0 || argv[i][0] != '-') {
      result.emplace_back(argv[i]);
    }
    for(const auto &spec : specifiers) {
      if(len > 1 && argv[i][1] == '-') {
        if(len == 2) {
          for(int j = i+1; j < argc; ++j) {
            result.emplace_back(argv[j]);
          }
          return result;
        }
        if(0 == strcmp(spec.long_name, argv[i]+2)) {
          goto matched;
        }
      } else if(len == 2 && argv[i][1] == spec.short_name) {
        goto matched;
      }
      continue;

    matched:
      if(spec.type == Option::Type::SIMPLE) {
        result.emplace_back(spec);
      } else if(i >= argc - 1) {
        result.emplace_back(argv[i]);
      } else {
        ++i;
        switch(spec.type) {
        case Option::Type::INTEGER:
          result.emplace_back(spec, static_cast<uint64_t>(strtoll(argv[i], nullptr, 10)));
          break;

        case Option::Type::UNSIGNED_INTEGER:
          result.emplace_back(spec, static_cast<uint64_t>(strtoull(argv[i], nullptr, 10)));
          break;

        case Option::Type::STRING:
          result.emplace_back(spec, argv[i]);
          break;

        default:
          abort();
        }
      }
    }
  }

  return result;
}

void print_options(const std::vector<Option::Specifier> &specifiers) {
  for(const auto &spec : specifiers) {
    if(spec.type == Option::Type::SIMPLE) {
      fprintf(stderr, "\t--%s | -%c - %s\n", spec.long_name, spec.short_name, spec.usage);
    } else {
      fprintf(stderr, "\t(--%s | -%c) <%s> - %s\n", spec.long_name, spec.short_name, spec.arg, spec.usage);
    }
  }
}
