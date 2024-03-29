#include <stdnoreturn.h>
#include "h1p.h"

// Security-related limits are defined in limits.rb and injected as
// defines in extconf.rb

#define INITIAL_BUFFER_SIZE     4096
#define BUFFER_TRIM_MIN_LEN     4096
#define BUFFER_TRIM_MIN_POS     2048
#define MAX_HEADERS_READ_LENGTH 4096
#define MAX_BODY_READ_LENGTH    (1 << 20) // 1MB

#define BODY_READ_MODE_UNKNOWN  -2
#define BODY_READ_MODE_CHUNKED  -1

ID ID_arity;
ID ID_backend_read;
ID ID_backend_recv;
ID ID_backend_send;
ID ID_backend_splice;
ID ID_backend_write;
ID ID_call;
ID ID_downcase;
ID ID_eof_p;
ID ID_eq;
ID ID_join;
ID ID_read_method;
ID ID_read;
ID ID_readpartial;
ID ID_to_i;
ID ID_to_s;
ID ID_upcase;
ID ID_write;
ID ID_write_method;

static VALUE cError;

VALUE eArgumentError;

VALUE NUM_max_headers_read_length;
VALUE NUM_buffer_start;
VALUE NUM_buffer_end;

VALUE STR_pseudo_method;
VALUE STR_pseudo_path;
VALUE STR_pseudo_protocol;
VALUE STR_pseudo_protocol_default;
VALUE STR_pseudo_rx;
VALUE STR_pseudo_status;
VALUE STR_pseudo_status_default;
VALUE STR_pseudo_status_message;

VALUE STR_chunked;
VALUE STR_content_length;
VALUE STR_content_length_capitalized;
VALUE STR_transfer_encoding;
VALUE STR_transfer_encoding_capitalized;

VALUE STR_CRLF;
VALUE STR_EMPTY_CHUNK;
VALUE STR_COMMA_SPACE;

VALUE SYM_backend_read;
VALUE SYM_backend_recv;
VALUE SYM_backend_send;
VALUE SYM_backend_write;
VALUE SYM_stock_readpartial;

VALUE SYM_client;
VALUE SYM_server;

enum read_method {
  RM_READPARTIAL,       // receiver.readpartial(len, buf, pos, raise_on_eof: false) (Polyphony-specific)
  RM_BACKEND_READ,      // Polyphony.backend_read (Polyphony-specific)
  RM_BACKEND_RECV,      // Polyphony.backend_recv (Polyphony-specific)
  RM_CALL,              // receiver.call(len) (Universal)
  RM_STOCK_READPARTIAL  // receiver.readpartial(len)
};

enum write_method {
  WM_BACKEND_WRITE,
  WM_BACKEND_SEND
};

enum parser_mode {
  mode_server,
  mode_client
};

typedef struct parser {
  enum  parser_mode mode;
  VALUE io;
  VALUE buffer;
  VALUE headers;
  int   current_request_rx;

  enum  read_method read_method;
  int   body_read_mode;
  int   body_left;
  int   request_completed;

  char *buf_ptr;
  int   buf_len;
  int   buf_pos;

} Parser_t;

VALUE cParser = Qnil;

static void Parser_mark(void *ptr) {
  Parser_t *parser = ptr;
  rb_gc_mark(parser->io);
  rb_gc_mark(parser->buffer);
  rb_gc_mark(parser->headers);
}

static void Parser_free(void *ptr) {
  xfree(ptr);
}

static size_t Parser_size(const void *ptr) {
  return sizeof(Parser_t);
}

static const rb_data_type_t Parser_type = {
  "Parser",
  {Parser_mark, Parser_free, Parser_size,},
  0, 0, 0
};

static VALUE Parser_allocate(VALUE klass) {
  Parser_t *parser;

  parser = ALLOC(Parser_t);
  return TypedData_Wrap_Struct(klass, &Parser_type, parser);
}

#define GetParser(obj, parser) \
  TypedData_Get_Struct((obj), Parser_t, &Parser_type, (parser))

static inline VALUE Polyphony(void) {
  static VALUE mPolyphony = Qnil;
  if (mPolyphony == Qnil) {
    mPolyphony = rb_const_get(rb_cObject, rb_intern("Polyphony"));
    rb_gc_register_mark_object(mPolyphony);
  }
  return mPolyphony;
}

static enum read_method detect_read_method(VALUE io) {
  if (rb_respond_to(io, ID_read_method)) {
    VALUE method = rb_funcall(io, ID_read_method, 0);
    if (method == SYM_stock_readpartial) return RM_STOCK_READPARTIAL;
    if (method == SYM_backend_read)      return RM_BACKEND_READ;
    if (method == SYM_backend_recv)      return RM_BACKEND_RECV;

    return RM_READPARTIAL;
  }
  else if (rb_respond_to(io, ID_call)) {
    return RM_CALL;
  }
  else
    rb_raise(rb_eRuntimeError, "Provided reader should be a callable or respond to #__read_method__");
}

static enum write_method detect_write_method(VALUE io) {
  if (rb_respond_to(io, ID_write_method)) {
    VALUE method = rb_funcall(io, ID_write_method, 0);
    if (method == SYM_backend_write)  return WM_BACKEND_WRITE;
    if (method == SYM_backend_send)   return WM_BACKEND_SEND;
  }
  rb_raise(rb_eRuntimeError, "Provided io should respond to #__write_method__");
}

enum parser_mode parse_parser_mode(VALUE mode) {
  if (mode == SYM_server) return mode_server;
  if (mode == SYM_client) return mode_client;

  rb_raise(rb_eRuntimeError, "Invalid parser mode specified");
}

/* call-seq:
 *   parser.initialize(io, mode)
 *
 * Initializes a new parser with the given IO instance and mode. Mode is either
 * `:server` or `:client`.
 */
VALUE Parser_initialize(VALUE self, VALUE io, VALUE mode) {
  Parser_t *parser;
  GetParser(self, parser);

  parser->mode = parse_parser_mode(mode);
  parser->io = io;
  parser->buffer = rb_str_new_literal("");
  parser->headers = Qnil;

  // pre-allocate the buffer
  rb_str_modify_expand(parser->buffer, INITIAL_BUFFER_SIZE);

  parser->read_method = detect_read_method(io);
  parser->body_read_mode = BODY_READ_MODE_UNKNOWN;
  parser->body_left = 0;

  RB_GC_GUARD(parser->io);
  RB_GC_GUARD(parser->buffer);

  parser->buf_ptr = 0;
  parser->buf_len = 0;
  parser->buf_pos = 0;

  return self;
}

////////////////////////////////////////////////////////////////////////////////

#define str_downcase(str) (rb_funcall((str), ID_downcase, 0))
#define str_upcase(str) (rb_funcall((str), ID_upcase, 0))

#define FILL_BUFFER_OR_GOTO_EOF(parser) { if (!fill_buffer(parser)) goto eof; }

#define BUFFER_POS(parser) ((parser)->buf_pos)
#define BUFFER_LEN(parser) ((parser)->buf_len)
#define BUFFER_CUR(parser) ((parser)->buf_ptr[(parser)->buf_pos])
#define BUFFER_AT(parser, pos) ((parser)->buf_ptr[pos])
#define BUFFER_PTR(parser, pos) ((parser)->buf_ptr + pos)
#define BUFFER_STR(parser, pos, len) (rb_obj_freeze(rb_utf8_str_new((parser)->buf_ptr + pos, len)))
#define BUFFER_STR_DOWNCASE(parser, pos, len) (rb_obj_freeze(str_downcase(rb_utf8_str_new((parser)->buf_ptr + pos, len))))
#define BUFFER_STR_UPCASE(parser, pos, len) (rb_obj_freeze(str_upcase(rb_utf8_str_new((parser)->buf_ptr + pos, len))))

#define INC_BUFFER_POS(parser) { \
  BUFFER_POS(parser)++; \
  if (BUFFER_POS(parser) == BUFFER_LEN(parser)) FILL_BUFFER_OR_GOTO_EOF(parser); \
}

#define INC_BUFFER_POS_NO_FILL(parser) BUFFER_POS(parser)++;

#define INC_BUFFER_POS_UTF8(parser, len) { \
  unsigned char c = BUFFER_CUR(parser); \
  if ((c & 0xf0) == 0xf0) { \
    while (BUFFER_LEN(parser) - BUFFER_POS(parser) < 4) FILL_BUFFER_OR_GOTO_EOF(parser); \
    BUFFER_POS(parser) += 4; \
    len += 4; \
  } \
  else if ((c & 0xe0) == 0xe0) { \
    while (BUFFER_LEN(parser) - BUFFER_POS(parser) < 3) FILL_BUFFER_OR_GOTO_EOF(parser); \
    BUFFER_POS(parser) += 3; \
    len += 3; \
  } \
  else if ((c & 0xc0) == 0xc0) { \
    while (BUFFER_LEN(parser) - BUFFER_POS(parser) < 2) FILL_BUFFER_OR_GOTO_EOF(parser); \
    BUFFER_POS(parser) += 2; \
    len += 2; \
  } \
  else { \
    BUFFER_POS(parser)++; \
    len ++; \
    if (BUFFER_POS(parser) == BUFFER_LEN(parser)) FILL_BUFFER_OR_GOTO_EOF(parser); \
  } \
}

#define INIT_PARSER_STATE(parser) { \
  (parser)->buf_len = RSTRING_LEN((parser)->buffer); \
  if (BUFFER_POS(parser) == BUFFER_LEN(parser)) \
    FILL_BUFFER_OR_GOTO_EOF(parser) \
  else \
    (parser)->buf_ptr = RSTRING_PTR((parser)->buffer); \
}

#define RAISE_BAD_REQUEST(msg) rb_raise(cError, msg)

#define SET_HEADER_VALUE_FROM_BUFFER(parser, key, pos, len) { \
  VALUE value = BUFFER_STR(parser, pos, len); \
  rb_hash_aset(parser->headers, key, value); \
  RB_GC_GUARD(value); \
}

#define SET_HEADER_DOWNCASE_VALUE_FROM_BUFFER(parser, key, pos, len) { \
  VALUE value = BUFFER_STR_DOWNCASE(parser, pos, len); \
  rb_hash_aset(parser->headers, key, value); \
  RB_GC_GUARD(value); \
}

#define SET_HEADER_UPCASE_VALUE_FROM_BUFFER(parser, key, pos, len) { \
  VALUE value = BUFFER_STR_UPCASE(parser, pos, len); \
  rb_hash_aset(parser->headers, key, value); \
  RB_GC_GUARD(value); \
}

#define SET_HEADER_VALUE_INT(parser, key, value) { \
  rb_hash_aset(parser->headers, key, INT2FIX(value)); \
}

#define CONSUME_CRLF(parser) { \
  INC_BUFFER_POS(parser); \
  if (BUFFER_CUR(parser) != '\n') goto bad_request; \
  INC_BUFFER_POS(parser); \
}

#define CONSUME_CRLF_NO_FILL(parser) { \
  INC_BUFFER_POS(parser); \
  if (BUFFER_CUR(parser) != '\n') goto bad_request; \
  INC_BUFFER_POS_NO_FILL(parser); \
}

#define GLOBAL_STR(v, s) v = rb_str_new_literal(s); rb_global_variable(&v); rb_obj_freeze(v)

// case-insensitive compare
#define CMP_CI(parser, down, up) ((BUFFER_CUR(parser) == down) || (BUFFER_CUR(parser) == up))

////////////////////////////////////////////////////////////////////////////////

static inline VALUE io_call(VALUE io, VALUE maxlen, VALUE buf, VALUE buf_pos) {
  VALUE result = rb_funcall(io, ID_call, 1, maxlen);
  if (result == Qnil) return Qnil;

  if (buf_pos == NUM_buffer_start) rb_str_set_len(buf, 0);
  rb_str_append(buf, result);
  RB_GC_GUARD(result);
  return buf;
}

static inline VALUE io_stock_readpartial(VALUE io, VALUE maxlen, VALUE buf, VALUE buf_pos) {
  VALUE eof = rb_funcall(io, ID_eof_p, 0);
  if (RTEST(eof)) return Qnil;

  VALUE result = rb_funcall(io, ID_readpartial, 1, maxlen);
  if (result == Qnil) return Qnil;
  if (buf == Qnil) return result;

  if (buf_pos == NUM_buffer_start) rb_str_set_len(buf, 0);
  rb_str_append(buf, result);
  RB_GC_GUARD(result);
  return buf;
}

static inline VALUE parser_io_read(Parser_t *parser, VALUE maxlen, VALUE buf, VALUE buf_pos) {
  switch (parser->read_method) {
    case RM_BACKEND_READ:
      return rb_funcall(Polyphony(), ID_backend_read, 5, parser->io, buf, maxlen, Qfalse, buf_pos);
    case RM_BACKEND_RECV:
      return rb_funcall(Polyphony(), ID_backend_recv, 4, parser->io, buf, maxlen, buf_pos);
    case RM_READPARTIAL:
      return rb_funcall(parser->io, ID_readpartial, 4, maxlen, buf, buf_pos, Qfalse);
    case RM_CALL:
      return io_call(parser->io, maxlen, buf, buf_pos);
    case RM_STOCK_READPARTIAL:
      return io_stock_readpartial(parser->io, maxlen, buf, buf_pos);
    default:
      return Qnil;
  }
}

static inline VALUE parser_io_write(VALUE io, VALUE buf, enum write_method method) {
  switch (method) {
    case WM_BACKEND_WRITE:
      return rb_funcall(Polyphony(), ID_backend_write, 2, io, buf);
    case WM_BACKEND_SEND:
      return rb_funcall(Polyphony(), ID_backend_send, 3, io, buf, INT2FIX(0));
    default:
      return Qnil;
  }
}

static inline int parser_io_splice(VALUE src, VALUE dest, int len) {
  VALUE ret = rb_funcall(Polyphony(), ID_backend_splice, 3, src, dest, INT2FIX(len));
  return FIX2INT(ret);
}

static inline int fill_buffer(Parser_t *parser) {
  VALUE ret = parser_io_read(parser, NUM_max_headers_read_length, parser->buffer, NUM_buffer_end);
  if (ret == Qnil) return 0;

  parser->buffer = ret;
  int len = RSTRING_LEN(parser->buffer);
  int read_bytes = len - parser->buf_len;
  if (!read_bytes) return 0;

  parser->buf_ptr = RSTRING_PTR(parser->buffer);
  parser->buf_len = len;
  return read_bytes;
}

static inline void buffer_trim(Parser_t *parser) {
  int len = RSTRING_LEN(parser->buffer);
  int pos = parser->buf_pos;
  int left = len - pos;

  // The buffer is trimmed only if length and position thresholds are passed,
  // *and* position is past the halfway point.
  if (len < BUFFER_TRIM_MIN_LEN ||
      pos < BUFFER_TRIM_MIN_POS ||
      left >= pos) return;

  if (left > 0) {
    char *ptr = RSTRING_PTR(parser->buffer);
    memcpy(ptr, ptr + pos, left);
  }
  rb_str_set_len(parser->buffer, left);
  parser->buf_pos = 0;
}

static inline void str_append_from_buffer(VALUE str, char *ptr, int len) {
  int str_len = RSTRING_LEN(str);
  rb_str_modify_expand(str, len);
  memcpy(RSTRING_PTR(str) + str_len, ptr, len);
  rb_str_set_len(str, str_len + len);
}

////////////////////////////////////////////////////////////////////////////////

static inline int parse_request_line_method(Parser_t *parser) {
  int pos = BUFFER_POS(parser);
  int len = 0;

  while (1) {
    switch (BUFFER_CUR(parser)) {
      case ' ':
        if (len < 1 || len > MAX_METHOD_LENGTH) goto bad_request;
        INC_BUFFER_POS(parser);
        goto done;
      case '\r':
      case '\n':
        goto bad_request;
      default:
        INC_BUFFER_POS(parser);
        len++;
        // INC_BUFFER_POS_UTF8(parser, len);
        if (len > MAX_METHOD_LENGTH) goto bad_request;
    }
  }
done:
  SET_HEADER_UPCASE_VALUE_FROM_BUFFER(parser, STR_pseudo_method, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid method");
eof:
  return 0;
}

static int parse_request_line_target(Parser_t *parser) {
  while (BUFFER_CUR(parser) == ' ') INC_BUFFER_POS(parser);
  int pos = BUFFER_POS(parser);
  int len = 0;
  while (1) {
    switch (BUFFER_CUR(parser)) {
      case ' ':
        if (len < 1 || len > MAX_PATH_LENGTH) goto bad_request;
        INC_BUFFER_POS(parser);
        goto done;
      case '\r':
      case '\n':
        goto bad_request;
      default:
        INC_BUFFER_POS(parser);
        len++;
        // INC_BUFFER_POS_UTF8(parser, len);
        if (len > MAX_PATH_LENGTH) goto bad_request;
    }
  }
done:
  SET_HEADER_VALUE_FROM_BUFFER(parser, STR_pseudo_path, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid request target");
eof:
  return 0;
}

static int parse_request_line_protocol(Parser_t *parser) {
  while (BUFFER_CUR(parser) == ' ') INC_BUFFER_POS(parser);
  int pos = BUFFER_POS(parser);
  int len = 0;

  if (CMP_CI(parser, 'H', 'h')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'T', 't')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'T', 't')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'P', 'p')) INC_BUFFER_POS(parser) else goto bad_request;
  if (BUFFER_CUR(parser) == '/') INC_BUFFER_POS(parser) else goto bad_request;
  if (BUFFER_CUR(parser) == '1') INC_BUFFER_POS(parser) else goto bad_request;
  len = 6;
  while (1) {
    switch (BUFFER_CUR(parser)) {
      case '\r':
        CONSUME_CRLF(parser);
        goto done;
      case '\n':
        INC_BUFFER_POS(parser);
        goto done;
      case '.':
        INC_BUFFER_POS(parser);
        char c = BUFFER_CUR(parser);
        if (c == '0' || c == '1') {
          INC_BUFFER_POS(parser);
          len += 2;
          continue;
        }
        goto bad_request;
      default:
        goto bad_request;
    }
  }
done:
  if (len < 6 || len > 8) goto bad_request;
  SET_HEADER_DOWNCASE_VALUE_FROM_BUFFER(parser, STR_pseudo_protocol, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid protocol");
eof:
  return 0;
}

static inline int parse_request_line(Parser_t *parser) {
  if (!parse_request_line_method(parser)) goto eof;
  if (!parse_request_line_target(parser)) goto eof;
  if (!parse_request_line_protocol(parser)) goto eof;

  return 1;
eof:
  return 0;
}

static inline int parse_status_line_protocol(Parser_t *parser) {
  int pos = BUFFER_POS(parser);
  int len = 0;

  if (CMP_CI(parser, 'H', 'h')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'T', 't')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'T', 't')) INC_BUFFER_POS(parser) else goto bad_request;
  if (CMP_CI(parser, 'P', 'p')) INC_BUFFER_POS(parser) else goto bad_request;
  if (BUFFER_CUR(parser) == '/') INC_BUFFER_POS(parser) else goto bad_request;
  if (BUFFER_CUR(parser) == '1') INC_BUFFER_POS(parser) else goto bad_request;
  len = 6;
  while (1) {
    switch (BUFFER_CUR(parser)) {
      case '.':
        INC_BUFFER_POS(parser);
        char c = BUFFER_CUR(parser);
        if (c == '0' || c == '1') {
          INC_BUFFER_POS(parser);
          len += 2;
          continue;
        }
        goto bad_request;
      case ' ':
        INC_BUFFER_POS(parser);
        goto done;
      default:
        goto bad_request;
    }
  }
done:
  if (len < 6 || len > 8) goto bad_request;
  SET_HEADER_DOWNCASE_VALUE_FROM_BUFFER(parser, STR_pseudo_protocol, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid protocol");
eof:
  return 0;
}

static inline int parse_status_line_status(Parser_t *parser) {
  while (BUFFER_CUR(parser) == ' ') INC_BUFFER_POS(parser);
  int len = 0;
  int status = 0;
  while (1) {
    if (len > 4) goto bad_request;
    
    char c = BUFFER_CUR(parser);
    if (c >= '0' && c <= '9') {
      status = status * 10 + (c - '0');
      len += 1;
      INC_BUFFER_POS(parser);
      continue;
    }
    switch (c) {
      case ' ':
        INC_BUFFER_POS(parser);
        goto done;
      case '\r':
      case '\n':
        goto done;
      default:
        goto bad_request;
    }
  }
done:
  SET_HEADER_VALUE_INT(parser, STR_pseudo_status, status);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid response status");
eof:
  return 0;
}

static inline int parse_status_line_status_message(Parser_t *parser) {
  while (BUFFER_CUR(parser) == ' ') INC_BUFFER_POS(parser);
  int pos = BUFFER_POS(parser);
  int len = 0;
  while (1) {
    switch (BUFFER_CUR(parser)) {
      case '\r':
        CONSUME_CRLF(parser);
        goto done;
      case '\n':
        INC_BUFFER_POS(parser);
        goto done;
      default:
        INC_BUFFER_POS(parser);
        len++;
        if (len > MAX_STATUS_MESSAGE_LENGTH) goto bad_request;
    }
  }
done:
  SET_HEADER_VALUE_FROM_BUFFER(parser, STR_pseudo_status_message, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid request target");
eof:
  return 0;
}


static inline int parse_status_line(Parser_t *parser) {
  if (!parse_status_line_protocol(parser)) goto eof;
  if (!parse_status_line_status(parser)) goto eof;
  if (!parse_status_line_status_message(parser)) goto eof;

  return 1;
eof:
  return 0;
}

static inline int parse_header_key(Parser_t *parser, VALUE *key) {
  int pos = BUFFER_POS(parser);
  int len = 0;

  while (1) {
    switch (BUFFER_CUR(parser)) {
      case ' ':
        goto bad_request;
      case ':':
        if (len < 1 || len > MAX_HEADER_KEY_LENGTH)
          goto bad_request;
        INC_BUFFER_POS(parser);
        goto done;
      case '\r':
        if (BUFFER_POS(parser) > pos) goto bad_request;
        CONSUME_CRLF_NO_FILL(parser);
        goto done;
      case '\n':
        if (BUFFER_POS(parser) > pos) goto bad_request;

        INC_BUFFER_POS_NO_FILL(parser);
        goto done;
      default:
        INC_BUFFER_POS(parser);
        len++;
        // INC_BUFFER_POS_UTF8(parser, len);
        if (len > MAX_HEADER_KEY_LENGTH) goto bad_request;
    }
  }
done:
  if (len == 0) return -1;
  (*key) = BUFFER_STR_DOWNCASE(parser, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid header key");
eof:
  return 0;
}

static inline int parse_header_value(Parser_t *parser, VALUE *value) {
  while (BUFFER_CUR(parser) == ' ') INC_BUFFER_POS(parser);

  int pos = BUFFER_POS(parser);
  int len = 0;

  while (1) {
    switch (BUFFER_CUR(parser)) {
      case '\r':
        CONSUME_CRLF(parser);
        goto done;
      case '\n':
        INC_BUFFER_POS(parser);
        goto done;
      default:
        INC_BUFFER_POS_UTF8(parser, len);
        if (len > MAX_HEADER_VALUE_LENGTH) goto bad_request;
    }
  }
done:
  if (len < 1 || len > MAX_HEADER_VALUE_LENGTH) goto bad_request;
  (*value) = BUFFER_STR(parser, pos, len);
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid header value");
eof:
  return 0;
}

static inline int parse_header(Parser_t *parser) {
  VALUE key, value;

  switch (parse_header_key(parser, &key)) {
    case -1: return -1;
    case 0: goto eof;
  }

  if (!parse_header_value(parser, &value)) goto eof;

  VALUE existing = rb_hash_aref(parser->headers, key);
  if (existing != Qnil) {
    if (TYPE(existing) != T_ARRAY) {
      existing = rb_ary_new3(2, existing, value);
      rb_hash_aset(parser->headers, key, existing);
    }
    else
      rb_ary_push(existing, value);
  }
  else
    rb_hash_aset(parser->headers, key, value);

  RB_GC_GUARD(existing);
  RB_GC_GUARD(key);
  RB_GC_GUARD(value);
  return 1;
eof:
  return 0;
}

VALUE Parser_parse_headers_safe(VALUE self) {
  Parser_t *parser;
  GetParser(self, parser);
  parser->headers = rb_hash_new();

  buffer_trim(parser);
  int initial_pos = parser->buf_pos;
  INIT_PARSER_STATE(parser);
  parser->current_request_rx = 0;

  if (parser->mode == mode_server) {
    if (!parse_request_line(parser)) goto eof;
  }
  else {
    if (!parse_status_line(parser)) goto eof;
  }

  int header_count = 0;
  while (1) {
    if (header_count > MAX_HEADER_COUNT) RAISE_BAD_REQUEST("Too many headers");
    switch (parse_header(parser)) {
      case -1: goto done; // empty header => end of headers
      case 0: goto eof;
    }
    header_count++;
  }
eof:
  parser->headers = Qnil;
done:
  parser->body_read_mode = BODY_READ_MODE_UNKNOWN;
  int read_bytes = BUFFER_POS(parser) - initial_pos;

  parser->current_request_rx += read_bytes;
  if (parser->headers != Qnil)
    rb_hash_aset(parser->headers, STR_pseudo_rx, INT2FIX(read_bytes));
  return parser->headers;
}

noreturn VALUE Parser_parse_headers_rescue(VALUE args, VALUE error) {
  RAISE_BAD_REQUEST("Invalid character sequences in method or header name");
}

/* call-seq: parser.parse_headers -> headers
 *
 * Parses headers from the associated IO instance, returning a hash mapping
 * header keys to their respective values. Header keys are downcased and dashes
 * are converted to underscores. The returned headers will also include the
 * following pseudo-headers:
 * 
 * - `':protocol'` - the protocol as specified in the query line / status line
 * - `':path'` - the query path (for HTTP requests)
 * - `':method'` - the HTTP method (for HTTP requests)
 * - `':status'` - the HTTP status (for HTTP responses)
 * - `':rx'` - the total number of bytes read by the parser
 */
VALUE Parser_parse_headers(VALUE self) {
  return rb_rescue2(
    Parser_parse_headers_safe, self,
    Parser_parse_headers_rescue, self,
    eArgumentError, (VALUE)0
  );
}

////////////////////////////////////////////////////////////////////////////////

static inline int str_to_int(VALUE value, const char *error_msg) {
  char *ptr = RSTRING_PTR(value);
  int len = RSTRING_LEN(value);
  int int_value = 0;

  while (len) {
    char c = *ptr;
    if ((c >= '0') && (c <= '9'))
      int_value = int_value * 10 + (c - '0');
    else
      RAISE_BAD_REQUEST(error_msg);
    len--;
    ptr++;
  }

  return int_value;
}

VALUE read_body_with_content_length(Parser_t *parser, int read_entire_body, int buffered_only) {
  if (parser->body_left <= 0) return Qnil;

  VALUE body = Qnil;

  int len = RSTRING_LEN(parser->buffer);
  int pos = BUFFER_POS(parser);

  if (pos < len) {
    int available = len - pos;
    if (available > parser->body_left) available = parser->body_left;
    body = rb_str_new(RSTRING_PTR(parser->buffer) + pos, available);
    BUFFER_POS(parser) += available;
    parser->current_request_rx += available;
    parser->body_left -= available;
    if (!parser->body_left) parser->request_completed = 1;
  }
  else {
    body = Qnil;
    len = 0;
  }
  if (buffered_only) return body;

  while (parser->body_left) {
    int maxlen = parser->body_left <= MAX_BODY_READ_LENGTH ? parser->body_left : MAX_BODY_READ_LENGTH;
    VALUE tmp_buf = parser_io_read(parser, INT2FIX(maxlen), Qnil, NUM_buffer_start);
    if (tmp_buf == Qnil) goto eof;
    if (body != Qnil)
      rb_str_append(body, tmp_buf);
    else
      body = tmp_buf;
    int read_bytes = RSTRING_LEN(tmp_buf);
    parser->current_request_rx += read_bytes;
    parser->body_left -= read_bytes;
    if (!parser->body_left) parser->request_completed = 1;
    RB_GC_GUARD(tmp_buf);
    if (!read_entire_body) goto done;
  }
done:
  rb_hash_aset(parser->headers, STR_pseudo_rx, INT2FIX(parser->current_request_rx));
  RB_GC_GUARD(body);
  return body;
eof:
  RAISE_BAD_REQUEST("Incomplete body");
}

int chunked_encoding_p(VALUE transfer_encoding) {
  if (transfer_encoding == Qnil) return 0;
  return rb_funcall(str_downcase(transfer_encoding), ID_eq, 1, STR_chunked) == Qtrue;
}

int parse_chunk_size(Parser_t *parser, int *chunk_size) {
  int len = 0;
  int value = 0;
  int initial_pos = BUFFER_POS(parser);

  while (1) {
    char c = BUFFER_CUR(parser);
    if ((c >= '0') && (c <= '9'))       value = (value << 4) + (c - '0');
    else if ((c >= 'a') && (c <= 'f'))  value = (value << 4) + (c - 'a' + 10);
    else if ((c >= 'A') && (c <= 'F'))  value = (value << 4) + (c - 'A' + 10);
    else switch (c) {
      case '\r':
        CONSUME_CRLF_NO_FILL(parser);
        goto done;
      case '\n':
        INC_BUFFER_POS_NO_FILL(parser);
        goto done;
      default:
        goto bad_request;
    }
    INC_BUFFER_POS(parser);
    len++;
    if (len >= MAX_CHUNKED_ENCODING_CHUNK_SIZE_LENGTH) goto bad_request;
  }
done:
  if (len == 0) goto bad_request;
  (*chunk_size) = value;
  parser->current_request_rx += BUFFER_POS(parser) - initial_pos;
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid chunk size");
eof:
  return 0;
}

int read_body_chunk_with_chunked_encoding(Parser_t *parser, VALUE *body, int chunk_size, int buffered_only) {
  int len = RSTRING_LEN(parser->buffer);
  int pos = BUFFER_POS(parser);
  int left = chunk_size;

  if (pos < len) {
    int available = len - pos;
    if (available > left) available = left;
    if (*body != Qnil)
      str_append_from_buffer(*body, RSTRING_PTR(parser->buffer) + pos, available);
    else
      *body = rb_str_new(RSTRING_PTR(parser->buffer) + pos, available);
    BUFFER_POS(parser) += available;
    parser->current_request_rx += available;
    left -= available;
  }
  if (buffered_only) return 1;

  while (left) {
    int maxlen = left <= MAX_BODY_READ_LENGTH ? left : MAX_BODY_READ_LENGTH;

    VALUE tmp_buf = parser_io_read(parser, INT2FIX(maxlen), Qnil, NUM_buffer_start);
    if (tmp_buf == Qnil) goto eof;
    if (*body != Qnil)
      rb_str_append(*body, tmp_buf);
    else
      *body = tmp_buf;
    int read_bytes = RSTRING_LEN(tmp_buf);
    parser->current_request_rx += read_bytes;
    left -= read_bytes;
    RB_GC_GUARD(tmp_buf);
  }
  return 1;
eof:
  return 0;
}

int splice_body_chunk_with_chunked_encoding(Parser_t *parser, VALUE dest, int chunk_size, enum write_method method) {
  int len = RSTRING_LEN(parser->buffer);
  int pos = BUFFER_POS(parser);
  int left = chunk_size;

  if (pos < len) {
    int available = len - pos;
    if (available > left) available = left;
    VALUE buf = rb_str_new(RSTRING_PTR(parser->buffer) + pos, available);
    BUFFER_POS(parser) += available;
    parser->current_request_rx += available;
    parser_io_write(dest, buf, method);
    RB_GC_GUARD(buf);
    left -= available;
  }

  while (left) {
    int spliced = parser_io_splice(parser->io, dest, left);
    if (!spliced) goto eof;
    parser->current_request_rx += spliced;
    left -= spliced;
  }
  return 1;
eof:
  return 0;
}

static inline int parse_chunk_postfix(Parser_t *parser) {
  int initial_pos = BUFFER_POS(parser);
  if (initial_pos == BUFFER_LEN(parser)) FILL_BUFFER_OR_GOTO_EOF(parser);
  switch (BUFFER_CUR(parser)) {
    case '\r':
      CONSUME_CRLF_NO_FILL(parser);
      goto done;
    case '\n':
      INC_BUFFER_POS_NO_FILL(parser);
      goto done;
    default:
      goto bad_request;
  }
done:
  parser->current_request_rx += BUFFER_POS(parser) - initial_pos;
  return 1;
bad_request:
  RAISE_BAD_REQUEST("Invalid chunk");
eof:
  return 0;
}

VALUE read_body_with_chunked_encoding(Parser_t *parser, int read_entire_body, int buffered_only) {
  buffer_trim(parser);
  INIT_PARSER_STATE(parser);
  VALUE body = Qnil;

  while (1) {
    int chunk_size = 0;
    if (BUFFER_POS(parser) == BUFFER_LEN(parser)) FILL_BUFFER_OR_GOTO_EOF(parser);
    if (!parse_chunk_size(parser, &chunk_size)) goto bad_request;

    if (chunk_size) {
      if (!read_body_chunk_with_chunked_encoding(parser, &body, chunk_size, buffered_only)) goto bad_request;
    }
    else parser->request_completed = 1;

    if (!parse_chunk_postfix(parser)) goto bad_request;
    if (!chunk_size || !read_entire_body) goto done;
  }
bad_request:
  RAISE_BAD_REQUEST("Malformed request body");
eof:
  RAISE_BAD_REQUEST("Incomplete request body");
done:
  rb_hash_aset(parser->headers, STR_pseudo_rx, INT2FIX(parser->current_request_rx));
  RB_GC_GUARD(body);
  return body;
}

void splice_body_with_chunked_encoding(Parser_t *parser, VALUE dest, enum write_method method) {
  buffer_trim(parser);
  INIT_PARSER_STATE(parser);

  while (1) {
    int chunk_size = 0;
    if (BUFFER_POS(parser) == BUFFER_LEN(parser)) FILL_BUFFER_OR_GOTO_EOF(parser);
    if (!parse_chunk_size(parser, &chunk_size)) goto bad_request;

    if (chunk_size) {
      if (!splice_body_chunk_with_chunked_encoding(parser, dest, chunk_size, method))
        goto bad_request;
    }
    else
      parser->request_completed = 1;

    // read post-chunk delimiter ("\r\n")
    if (!parse_chunk_postfix(parser)) goto bad_request;
    if (!chunk_size) goto done;
  }
bad_request:
  RAISE_BAD_REQUEST("Malformed request body");
eof:
  RAISE_BAD_REQUEST("Incomplete request body");
done:
  rb_hash_aset(parser->headers, STR_pseudo_rx, INT2FIX(parser->current_request_rx));
}

void splice_body_with_content_length(Parser_t *parser, VALUE dest, enum write_method method)  {
  if (parser->body_left <= 0) return;

  int len = RSTRING_LEN(parser->buffer);
  int pos = BUFFER_POS(parser);

  if (pos < len) {
    int available = len - pos;
    if (available > parser->body_left) available = parser->body_left;
    VALUE buf = rb_str_new(RSTRING_PTR(parser->buffer) + pos, available);
    BUFFER_POS(parser) += available;
    parser_io_write(dest, buf, method);
    RB_GC_GUARD(buf);
    parser->current_request_rx += available;
    parser->body_left -= available;
    if (!parser->body_left) parser->request_completed = 1;
  }

  while (parser->body_left) {
    int spliced = parser_io_splice(parser->io, dest, parser->body_left);
    if (!spliced) goto eof;
    parser->current_request_rx += spliced;
    parser->body_left -= spliced;
  }
  rb_hash_aset(parser->headers, STR_pseudo_rx, INT2FIX(parser->current_request_rx));
  return;
eof:
  RAISE_BAD_REQUEST("Incomplete body");
}

static inline void detect_body_read_mode(Parser_t *parser) {
  VALUE content_length = rb_hash_aref(parser->headers, STR_content_length);
  if (content_length != Qnil) {
    int int_content_length = str_to_int(content_length, "Invalid content length");
    if (int_content_length < 0) RAISE_BAD_REQUEST("Invalid body content length");
    parser->body_read_mode = parser->body_left = int_content_length;
    parser->request_completed = 0;
    return;
  }

  VALUE transfer_encoding = rb_hash_aref(parser->headers, STR_transfer_encoding);
  if (chunked_encoding_p(transfer_encoding)) {
    parser->body_read_mode = BODY_READ_MODE_CHUNKED;
    parser->request_completed = 0;
    return;
  }
  parser->request_completed = 1;

}

static inline VALUE read_body(VALUE self, int read_entire_body, int buffered_only) {
  Parser_t *parser;
  GetParser(self, parser);

  if (parser->body_read_mode == BODY_READ_MODE_UNKNOWN)
    detect_body_read_mode(parser);

  if (parser->body_read_mode == BODY_READ_MODE_CHUNKED)
    return read_body_with_chunked_encoding(parser, read_entire_body, buffered_only);
  else
    return read_body_with_content_length(parser, read_entire_body, buffered_only);
}

/* call-seq: parser.read_body -> body
 *
 * Reads an HTTP request/response body from the associated IO instance.
 */
VALUE Parser_read_body(VALUE self) {
  return read_body(self, 1, 0);
}

/* call-seq: parser.read_body_chunk(buffered_only) -> chunk
 *
 * Reads a single body chunk (useful for chunked transfer encoding). If
 * `buffered_only` is true, will only read from the underlying buffer, without
 * reading from the associated IO instance.
 */
VALUE Parser_read_body_chunk(VALUE self, VALUE buffered_only) {
  return read_body(self, 0, buffered_only == Qtrue);
}

/* call-seq: parser.splice_body_to(dest)
 *
 * Splices the HTTP request/response body from the associated IO instance to
 * `dest`.
 */
VALUE Parser_splice_body_to(VALUE self, VALUE dest) {
  Parser_t *parser;
  GetParser(self, parser);
  enum write_method method = detect_write_method(dest);

  if (parser->body_read_mode == BODY_READ_MODE_UNKNOWN)
    detect_body_read_mode(parser);

  if (parser->body_read_mode == BODY_READ_MODE_CHUNKED)
    splice_body_with_chunked_encoding(parser, dest, method);
  else
    splice_body_with_content_length(parser, dest, method);
  
  return self;
}

/* call-seq: parser.complete?
 *
 * Returns true if a complete HTTP request/response has been read from the
 * associated IO instance.
 */
VALUE Parser_complete_p(VALUE self) {
  Parser_t *parser;
  GetParser(self, parser);

  if (parser->body_read_mode == BODY_READ_MODE_UNKNOWN)
    detect_body_read_mode(parser);

  return parser->request_completed ? Qtrue : Qfalse;
}

typedef struct send_response_ctx {
  VALUE io;
  VALUE buffer;
  char *buffer_ptr;
  unsigned int buffer_len;
  unsigned int total_written;
} send_response_ctx;

#define MAX_RESPONSE_BUFFER_SIZE 65536

void send_response_flush_buffer(send_response_ctx *ctx) {
  if (!ctx->buffer_len) return;

  rb_str_set_len(ctx->buffer, ctx->buffer_len);
  VALUE written = rb_funcall(ctx->io, ID_write, 1, ctx->buffer);
  ctx->total_written += NUM2INT(written);
  
  rb_str_set_len(ctx->buffer, 0);
  ctx->buffer_len = 0;
}

void send_response_write_status_line(send_response_ctx *ctx, VALUE protocol, VALUE status) {
  char *ptr = ctx->buffer_ptr;

  unsigned int partlen = RSTRING_LEN(protocol);
  memcpy(ptr, RSTRING_PTR(protocol), partlen);
  ctx->buffer_len += partlen;
  ptr[ctx->buffer_len] = ' ';
  ctx->buffer_len++;

  ptr += ctx->buffer_len;
  partlen = RSTRING_LEN(status);
  memcpy(ptr, RSTRING_PTR(status), partlen);
  ptr[partlen] = '\r';
  ptr[partlen + 1] = '\n';
  ctx->buffer_len += partlen + 2;
}

inline static VALUE format_comma_separated_header_values(VALUE array) {
  return rb_funcall(array, ID_join, 1, STR_COMMA_SPACE);
}

int send_response_write_header(VALUE key, VALUE val, VALUE arg) {
  if (TYPE(key) != T_STRING) key = rb_funcall(key, ID_to_s, 0);
  char *keyptr = RSTRING_PTR(key);
  if (RSTRING_LEN(key) < 1 || keyptr[0] == ':') return 0;

  switch (TYPE(val)) {
    case T_STRING:
      break;
    case T_ARRAY:
      val = format_comma_separated_header_values(val);
      break;
    default:
      val = rb_funcall(val, ID_to_s, 0);
  }
  unsigned int keylen = RSTRING_LEN(key);
  char *valptr = RSTRING_PTR(val);
  unsigned int vallen = RSTRING_LEN(val);
  send_response_ctx *ctx = (send_response_ctx *)arg;

  if (ctx->buffer_len + keylen + vallen > (MAX_RESPONSE_BUFFER_SIZE - 8))
    send_response_flush_buffer(ctx);
  
  char *ptr = ctx->buffer_ptr + ctx->buffer_len;
  memcpy(ptr, keyptr, keylen);
  ptr[keylen] = ':';
  ptr[keylen + 1] = ' ';
  ptr += keylen + 2;

  memcpy(ptr, valptr, vallen);
  ptr[vallen] = '\r';
  ptr[vallen + 1] = '\n';
  ctx->buffer_len += keylen + vallen + 4;

  RB_GC_GUARD(key);
  RB_GC_GUARD(val);
  
  return 0; // ST_CONTINUE
}

/* call-seq: H1P.send_response(io, headers, body = nil) -> total_written
 *
 * Sends an HTTP response with the given headers and body.
 */
VALUE H1P_send_response(int argc,VALUE *argv, VALUE self) {
  if (argc < 2)
    rb_raise(eArgumentError, "(wrong number of arguments (expected 2 or more))");

  VALUE io = argv[0];
  VALUE headers = argv[1];
  VALUE body = argc >= 3 ? argv[2] : Qnil;
  VALUE buffer = rb_str_new_literal("");
  rb_str_modify_expand(buffer, MAX_RESPONSE_BUFFER_SIZE);
  send_response_ctx ctx = {io, buffer, RSTRING_PTR(buffer), 0, 0};

  char *bodyptr = 0;
  unsigned int bodylen = 0;

  VALUE protocol = rb_hash_aref(headers, STR_pseudo_protocol);
  if (protocol == Qnil) protocol = STR_pseudo_protocol_default;
  VALUE status = rb_hash_aref(headers, STR_pseudo_status);
  if (status == Qnil) status = STR_pseudo_status_default;
  send_response_write_status_line(&ctx, protocol, status);

  if (body != Qnil) {
    if (TYPE(body) != T_STRING) body = rb_funcall(body, ID_to_s, 0);

    bodyptr = RSTRING_PTR(body);
    bodylen = RSTRING_LEN(body);
    // rb_hash_aset(headers, STR_content_length_capitalized, INT2FIX(bodylen));
  }

  rb_hash_foreach(headers, send_response_write_header, (VALUE)&ctx);
  send_response_write_header(STR_content_length_capitalized, INT2FIX(bodylen), (VALUE)&ctx);

  char *endptr = ctx.buffer_ptr + ctx.buffer_len;
  endptr[0] = '\r';
  endptr[1] = '\n';
  ctx.buffer_len += 2;

  if (body != Qnil) {
    while (bodylen > 0) {
      unsigned int chunklen = bodylen;
      if (chunklen > MAX_RESPONSE_BUFFER_SIZE) chunklen = MAX_RESPONSE_BUFFER_SIZE;
      
      if (ctx.buffer_len + chunklen > MAX_RESPONSE_BUFFER_SIZE)
        send_response_flush_buffer(&ctx);

      memcpy(ctx.buffer_ptr + ctx.buffer_len, bodyptr, chunklen);
      ctx.buffer_len += chunklen;
      bodyptr += chunklen;
      bodylen -= chunklen;
    }
    RB_GC_GUARD(body);
  }

  send_response_flush_buffer(&ctx);

  RB_GC_GUARD(buffer);

  return INT2FIX(ctx.total_written);
}

/* call-seq: H1P.send_body_chunk(io, chunk) -> total_written
 *
 * Sends a body chunk using chunked transfer encoding.
 */
VALUE H1P_send_body_chunk(VALUE self, VALUE io, VALUE chunk) {
  if (chunk != Qnil) {
    if (TYPE(chunk) != T_STRING) chunk = rb_funcall(chunk, ID_to_s, 0);

    VALUE len_string = rb_str_new_literal("");
    rb_str_modify_expand(len_string, 16);
    int len_string_len = sprintf(RSTRING_PTR(len_string), "%lx\r\n", RSTRING_LEN(chunk));
    rb_str_set_len(len_string,len_string_len);

    VALUE total_written = rb_funcall(io, ID_write, 3, len_string, chunk, STR_CRLF);

    RB_GC_GUARD(len_string);
    RB_GC_GUARD(chunk);
    return total_written;
  }
  else {
    return rb_funcall(io, ID_write, 1, STR_EMPTY_CHUNK);
  }
}

/* call-seq: H1P.send_chunked_response(io, headers, body = nil) -> total_written
 *
 * Sends an HTTP response with the given headers and body using chunked transfer
 * encoding.
 */
VALUE H1P_send_chunked_response(VALUE self, VALUE io, VALUE headers) {
  VALUE buffer = rb_str_new_literal("");
  rb_str_modify_expand(buffer, MAX_RESPONSE_BUFFER_SIZE);
  send_response_ctx ctx = {io, buffer, RSTRING_PTR(buffer), 0, 0};

  VALUE protocol = rb_hash_aref(headers, STR_pseudo_protocol);
  if (protocol == Qnil) protocol = STR_pseudo_protocol_default;
  VALUE status = rb_hash_aref(headers, STR_pseudo_status);
  if (status == Qnil) status = STR_pseudo_status_default;
  send_response_write_status_line(&ctx, protocol, status);

  rb_hash_foreach(headers, send_response_write_header, (VALUE)&ctx);
  send_response_write_header(STR_transfer_encoding_capitalized, STR_chunked, (VALUE)&ctx);

  ctx.buffer_ptr[ctx.buffer_len] = '\r';
  ctx.buffer_ptr[ctx.buffer_len + 1] = '\n';
  ctx.buffer_len += 2;
  send_response_flush_buffer(&ctx);

  VALUE len_string = rb_str_new_literal("");
  rb_str_modify_expand(len_string, 16);
  while (1) {
    VALUE chunk = rb_yield(Qnil);
    if (chunk == Qnil) {
      VALUE written = rb_funcall(io, ID_write, 1, STR_EMPTY_CHUNK);
      ctx.total_written += NUM2INT(written);
      break;
    }
    else {
      if (TYPE(chunk) != T_STRING) chunk = rb_funcall(chunk, ID_to_s, 0);

      int len_string_len = sprintf(RSTRING_PTR(len_string), "%lx\r\n", RSTRING_LEN(chunk));
      rb_str_set_len(len_string,len_string_len);
      VALUE written = rb_funcall(io, ID_write, 3, len_string, chunk, STR_CRLF);
      ctx.total_written += NUM2INT(written);
    }
    RB_GC_GUARD(chunk);
  }

  RB_GC_GUARD(len_string);
  RB_GC_GUARD(buffer);

  return INT2FIX(ctx.total_written);
}

void Init_H1P(void) {
  VALUE mH1P;
  VALUE cParser;

  mH1P = rb_define_module("H1P");
  rb_gc_register_mark_object(mH1P);
  cParser = rb_define_class_under(mH1P, "Parser", rb_cObject);
  rb_define_alloc_func(cParser, Parser_allocate);

  cError = rb_define_class_under(mH1P, "Error", rb_eRuntimeError);
  rb_gc_register_mark_object(cError);

  rb_define_method(cParser, "initialize", Parser_initialize, 2);
  rb_define_method(cParser, "parse_headers", Parser_parse_headers, 0);
  rb_define_method(cParser, "read_body", Parser_read_body, 0);
  rb_define_method(cParser, "read_body_chunk", Parser_read_body_chunk, 1);
  rb_define_method(cParser, "splice_body_to", Parser_splice_body_to, 1);
  rb_define_method(cParser, "complete?", Parser_complete_p, 0);

  rb_define_singleton_method(mH1P, "send_response", H1P_send_response, -1);
  rb_define_singleton_method(mH1P, "send_body_chunk", H1P_send_body_chunk, 2);
  rb_define_singleton_method(mH1P, "send_chunked_response", H1P_send_chunked_response, 2);

  ID_arity                  = rb_intern("arity");
  ID_backend_read           = rb_intern("backend_read");
  ID_backend_recv           = rb_intern("backend_recv");
  ID_backend_send           = rb_intern("backend_send");
  ID_backend_splice         = rb_intern("backend_splice");
  ID_backend_write          = rb_intern("backend_write");
  ID_call                   = rb_intern("call");
  ID_downcase               = rb_intern("downcase");
  ID_eof_p                  = rb_intern("eof?");
  ID_eq                     = rb_intern("==");
  ID_join                   = rb_intern("join");
  ID_read_method            = rb_intern("__read_method__");
  ID_read                   = rb_intern("read");
  ID_readpartial            = rb_intern("readpartial");
  ID_to_i                   = rb_intern("to_i");
  ID_to_s                   = rb_intern("to_s");
  ID_upcase                 = rb_intern("upcase");
  ID_write                  = rb_intern("write");
  ID_write_method           = rb_intern("__write_method__");

  NUM_max_headers_read_length = INT2FIX(MAX_HEADERS_READ_LENGTH);
  NUM_buffer_start = INT2FIX(0);
  NUM_buffer_end = INT2FIX(-1);

  GLOBAL_STR(STR_pseudo_method,               ":method");
  GLOBAL_STR(STR_pseudo_path,                 ":path");
  GLOBAL_STR(STR_pseudo_protocol,             ":protocol");
  GLOBAL_STR(STR_pseudo_protocol_default,     "HTTP/1.1");
  GLOBAL_STR(STR_pseudo_rx,                   ":rx");
  GLOBAL_STR(STR_pseudo_status,               ":status");
  GLOBAL_STR(STR_pseudo_status_default,       "200 OK");
  GLOBAL_STR(STR_pseudo_status_message,       ":status_message");

  GLOBAL_STR(STR_chunked,                       "chunked");
  GLOBAL_STR(STR_content_length,                "content-length");
  GLOBAL_STR(STR_content_length_capitalized,    "Content-Length");
  GLOBAL_STR(STR_transfer_encoding,             "transfer-encoding");
  GLOBAL_STR(STR_transfer_encoding_capitalized, "Transfer-Encoding");

  GLOBAL_STR(STR_CRLF,                          "\r\n");
  GLOBAL_STR(STR_EMPTY_CHUNK,                   "0\r\n\r\n");
  GLOBAL_STR(STR_COMMA_SPACE,                   ", ");

  SYM_backend_read  = ID2SYM(ID_backend_read);
  SYM_backend_recv  = ID2SYM(ID_backend_recv);
  SYM_backend_send  = ID2SYM(ID_backend_send);
  SYM_backend_write = ID2SYM(ID_backend_write);

  SYM_stock_readpartial = ID2SYM(rb_intern("stock_readpartial"));
  
  SYM_client = ID2SYM(rb_intern("client"));
  SYM_server = ID2SYM(rb_intern("server"));

  rb_global_variable(&mH1P);

  eArgumentError = rb_const_get(rb_cObject, rb_intern("ArgumentError"));
}

void Init_h1p_ext(void) {
  Init_H1P();
}
