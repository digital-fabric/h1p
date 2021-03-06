#ifndef H1P_H
#define H1P_H

#include "ruby.h"

// debugging
#define OBJ_ID(obj) (NUM2LONG(rb_funcall(obj, rb_intern("object_id"), 0)))
#define INSPECT(str, obj) { printf(str); VALUE s = rb_funcall(obj, rb_intern("inspect"), 0); printf(": %s\n", StringValueCStr(s)); }
#define TRACE_CALLER() { VALUE c = rb_funcall(rb_mKernel, rb_intern("caller"), 0); INSPECT("caller: ", c); }
#define TRACE_C_STACK() { \
  void *entries[10]; \
  size_t size = backtrace(entries, 10); \
  char **strings = backtrace_symbols(entries, size); \
  for (unsigned long i = 0; i < size; i++) printf("%s\n", strings[i]); \
  free(strings); \
}
#define PRINT_BUFFER(prefix, ptr, len) { \
  printf("%s buffer (%d): ", prefix, (int)len); \
  for (int i = 0; i < len; i++) printf("%02X ", ptr[i]); \
  printf("\n"); \
}

#endif /* H1P_H */
