# H1P - HTTP/1 tools for Ruby

[![Gem Version](https://badge.fury.io/rb/h1p.svg)](http://rubygems.org/gems/h1p)
[![H1P Test](https://github.com/digital-fabric/h1p/workflows/Tests/badge.svg)](https://github.com/digital-fabric/h1p/actions?query=workflow%3ATests)
[![MIT licensed](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/digital-fabric/h1p/blob/master/LICENSE)

H1P is a blocking/synchronous HTTP/1 parser for Ruby with a simple and intuitive
API. Its design lends itself to writing HTTP servers in a sequential style. As
such, it might prove useful in conjunction with the new fiber scheduler
introduced in Ruby 3.0, but is also useful with a normal thread-based server
(see
[example](https://github.com/digital-fabric/h1p/blob/main/examples/http_server.rb).)
The H1P was originally written as part of
[Tipi](https://github.com/digital-fabric/tipi), a web server running on top of
[Polyphony](https://github.com/digital-fabric/polyphony).

In addition to parsing, H1P offers APIs for formatting and writing HTTP/1
requests and responses.

## Features

- Simple, blocking/synchronous API
- Zero dependencies
- Transport-agnostic
- Parses both HTTP request and HTTP response
- Support for chunked encoding
- Support for both `LF` and `CRLF` line breaks
- Support for **splicing** request/response bodies (when used with
  [Polyphony](https://github.com/digital-fabric/polyphony))
- Track total incoming traffic
- Write HTTP requests and responses to any IO instance, with support for chunked
  transfer encoding.

## Installing

If you're using bundler just add it to your `Gemfile`:

```ruby
source 'https://rubygems.org'

gem 'h1p'
```

You can then run `bundle install` to install it. Otherwise, just run `gem install h1p`.

## Usage

Start by creating an instance of `H1P::Parser`, passing a connection instance and the parsing mode:

```ruby
require 'h1p'

parser = H1P::Parser.new(conn, :server)
```

In order to parse HTTP responses, change the mode to `:client`:

```ruby
parser = H1P::Parser.new(conn, :client)
```

To read the next message from the connection, call `#parse_headers`:

```ruby
loop do
  headers = parser.parse_headers
  break unless headers

  handle_request(headers)
end
```

The `#parse_headers` method returns a single hash containing the different HTTP
headers. In case the client has closed the connection, `#parse_headers` will
return `nil` (see the guard clause above).

In addition to the header keys and values, the resulting hash also contains the
following "pseudo-headers" (in server mode):

- `:method`: the HTTP method (in upper case)
- `:path`: the request target
- `:protocol`: the protocol used (either `'http/1.0'` or `'http/1.1'`)
- `:rx`: the total bytes read by the parser

In client mode, the following pseudo-headers will be present:

- `:protocol`: the protocol used (either `'http/1.0'` or `'http/1.1'`)
- `:status': the HTTP status as an integer
- `:status_message`: the HTTP status message
- `:rx`: the total bytes read by the parser


The header keys are always lower-cased. Consider the following HTTP request:

```
GET /foo HTTP/1.1
Host: example.com
User-Agent: curl/7.74.0
Accept: */*

```

The request will be parsed into the following Ruby hash:

```ruby
{
  ":method"     => "get",
  ":path"       => "/foo",
  ":protocol"   => "http/1.1",
  "host"        => "example.com",
  "user-agent"  => "curl/7.74.0",
  "accept"      => "*/*",
  ":rx"         => 78
}
```

Multiple headers with the same key will be coalesced into a single key-value
where the value is an array containing the corresponding values. For example,
multiple `Cookie` headers will appear in the hash as a single `"cookie"` entry,
e.g. `{ "cookie" => ['a=1', 'b=2'] }`

### Handling of invalid message

When an invalid message is encountered, the parser will raise a `H1P::Error`
exception. An incoming message may be considered invalid if an invalid character
has been encountered at any point in parsing the message, or if any of the
tokens have an invalid length. You can consult the limits used by the parser
[here](https://github.com/digital-fabric/h1p/blob/main/ext/h1p/limits.rb).

### Reading the message body

To read the message body use `#read_body`:

```ruby
# read entire body
body = parser.read_body
```

The H1P parser knows how to read both message bodies with a specified
`Content-Length` and request bodies in chunked encoding. The method call will
return when the entire body has been read. If the body is incomplete or has
invalid formatting, the parser will raise a `H1P::Error` exception.

You can also read a single chunk of the body by calling `#read_body_chunk`:

```ruby
# read a body chunk
chunk = parser.read_body_chunk(false)

# read chunk only from buffer:
chunk = parser.read_body_chunk(true)
```

If no more chunks are availble, `#read_body_chunk` will return nil. To test
whether the request is complete, you can call `#complete?`:

```ruby
headers = parser.parse_headers
unless parser.complete?
  body = parser.read_body
end
```

The `#read_body` and `#read_body_chunk` methods will return `nil` if no body is
expected (based on the received headers).

## Splicing request/response bodies

> Splicing of request/response bodies is available only on Linux, and works only
> with [Polyphony](https://github.com/digital-fabric/polyphony).

H1P also lets you [splice](https://man7.org/linux/man-pages/man2/splice.2.html)
request or response bodies directly to a pipe. This is particularly useful for
uploading or downloading large files, as the data does not need to be loaded
into Ruby strings. In fact, the data will stay almost entirely in kernel
buffers, which means any data copying is reduced to the absolute minimum.

The following example sends a request, then splices the response body to a file:

```ruby
require 'polyphony'
require 'h1p'

socket = TCPSocket.new('example.com', 80)
socket << "GET /bigfile HTTP/1.1\r\nHost: example.com\r\n\r\n"

parser = H1P::Parser.new(socket, :client)
headers = parser.parse_headers

pipe = Polyphony.pipe
File.open('bigfile', 'w+') do |f|
  spin { parser.splice_body_to(pipe) }
  f.splice_from(pipe)
end
```

## Parsing from arbitrary transports

The H1P parser was built to read from any arbitrary transport or source, as long
as they conform to one of two alternative interfaces:

- An object implementing a `__read_method__` method, which returns any of
  the following values:

  - `:stock_readpartial` - to be used for instances of `IO`, `Socket`,
    `TCPSocket`, `SSLSocket` etc.
  - `:backend_read` - for use in Polyphony-based servers.
  - `:backend_recv` - for use in Polyphony-based servers.
  - `:readpartial` - for use in Polyphony-based servers.

- An object implementing a `call` method, such as a `Proc` or any other. The
  call is given a single argument signifying the maximum number of bytes to
  read, and is expected to return either a string with the read data, or `nil`
  if no more data is available. The callable can be passed as an argument or as
  a block. Here's an example for parsing from a callable:

  ```ruby
  data = ['GET ', '/foo', " HTTP/1.1\r\n", "\r\n"]
  data = ['GET ', '/foo', " HTTP/1.1\r\n", "\r\n"]
  parser = H1P::Parser.new { data.shift }
  parser.parse_headers
  #=> {":method"=>"get", ":path"=>"/foo", ":protocol"=>"http/1.1", ":rx"=>21}
  ```

## Writing HTTP requests and responses

H1P implements optimized methods for writing HTTP requests and responses to
arbitrary IO instances. To write a response with or without a body, use
`H1P.send_response(io, headers, body = nil)`:

```ruby
H1P.send_response(socket, { 'Some-Header' => 'header value'}, 'foobar')
# HTTP/1.1 200 OK
# Some-Header: header value
# 
# foobar

# The :protocol pseudo header sets the protocol in the status line:
H1P.send_response(socket, { ':protocol' => 'HTTP/0.9' })
# HTTP/0.9 200 OK
#
#

# The :status pseudo header sets the response status:
H1P.send_response(socket, { ':status' => '418 I\'m a teapot' })
# HTTP/1.1 418 I'm a teapot
#
#
```

To send responses using chunked transfer encoding use
`H1P.send_chunked_response(io, header, body = nil)`:

```ruby
H1P.send_chunked_response(socket, {}, "foobar")
# HTTP/1.1 200 OK
# Transfer-Encoding: chunked
# 6
# foobar
# 0
#
#
```

You can also call `H1P.send_chunked_response` with a block that provides the
next chunk to send. The last chunk is signalled by returning `nil` from the
block:

```ruby
IO.open('/path/to/file') do |f|
  H1P.send_chunked_response(socket, {}) { f.read(CHUNK_SIZE) }
end
```

To send individual chunks use `H1P.send_body_chunk`:

```ruby
H1P.send_body_chunk(socket, 'foo')
# 3
# foo
#

H1P.send_body_chunk(socket, nil)
# 0
#
#
```

## Parser Design

The H1P parser design is based on the following principles:

- Implement a blocking API for use with a sequential programming style.
- Minimize copying of data between buffers.
- Parse each piece of data only once.
- Minimize object and buffer allocations.
- Minimize the API surface area.

One of the unique aspects of H1P is that instead of the server needing to feed
data to the parser, the parser itself reads data from its source whenever it
needs more of it. If no data is yet available, the parser blocks until more data
is received.

The different parts of the request are parsed one byte at a time, and once each
token is considered complete, it is copied from the buffer into a new string, to
be stored in the headers hash.

## Performance

The included benchmark (against
[http_parser.rb](https://github.com/tmm1/http_parser.rb), based on the *old*
[node.js HTTP parser](https://github.com/nodejs/http-parser)) shows the H1P
parser to be about 10-20% slower than http_parser.rb.

However, in a fiber-based environment such as
[Polyphony](https://github.com/digital-fabric/polyphony), H1P is slightly
faster, as the overhead of dealing with pipelined requests (which will cause
`http_parser.rb` to emit callbacks multiple times) significantly affects its
performance.

## Roadmap

Here are some of the features and enhancements planned for H1P:

- Add conformance and security tests
- Add ability to splice the message body into an arbitrary fd
  (Polyphony-specific)
- Improve performance

## Contributing

Issues and pull requests will be gladly accepted. If you have found this gem
useful, please let me know.