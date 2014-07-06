#ifndef ANCHOR_OPTIONS_H_
#define ANCHOR_OPTIONS_H_

#include <cinttypes>
#include <vector>

struct Option {
  enum class Type { SIMPLE, INTEGER, UNSIGNED_INTEGER, STRING };

  struct Specifier {
    int id;
    Type type;
    char short_name;
    const char *long_name;
    const char *usage;
    const char *arg;

    Specifier(int id, const char *ln, char sn, const char *us)
        : id(id), type(Type::SIMPLE), short_name(sn), long_name(ln), usage(us), arg(nullptr) {}
    Specifier(int id, const char *ln, char sn, const char *an, Type ty, const char *us)
        : id(id), type(ty), short_name(sn), long_name(ln), usage(us), arg(an) {}
  };

  int id;
  union {
    int64_t integer;
    uint64_t unsigned_integer;
    const char *string;
  } parameter;

  Option(const char *s) : id(-1) {parameter.string = s;}

  Option(const Specifier &spec) : id(spec.id) {}
  Option(const Specifier &spec, int64_t i) : id(spec.id) {parameter.integer = i;}
  Option(const Specifier &spec, uint64_t u) : id(spec.id) {parameter.unsigned_integer = u;}
  Option(const Specifier &spec, const char *s) : id(spec.id) {parameter.string = s;}
};

std::vector<Option> parse_options(int argc, char *argv[], const std::vector<Option::Specifier> &specifiers);
void print_options(const std::vector<Option::Specifier> &specifiers);

#endif
