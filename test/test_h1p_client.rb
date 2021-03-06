# frozen_string_literal: true

require_relative 'helper'
require 'h1p'
require 'socket'
require_relative '../ext/h1p/limits'

class H1PClientTest < MiniTest::Test
  Error = H1P::Error

  def setup
    super
    @i, @o = IO.pipe
    @parser = H1P::Parser.new(@i, :client)
  end
  alias_method :reset_parser, :setup

  def test_status_line
    msg = "HTTP/1.1 200 OK\r\n\r\n"
    @o << msg
    headers = @parser.parse_headers

    assert_equal(
      {
        ':status' => 200,
        ':status_message' => 'OK',
        ':protocol' => 'http/1.1',
        ':rx' => msg.bytesize
      },
      headers
    )
  end

  def test_status_line_whitespace
    msg = "HTTP/1.1               404 Not found\r\n\r\n"
    @o << msg
    headers = @parser.parse_headers

    assert_equal(
      {
        ':protocol' => 'http/1.1',
        ':status' => 404,
        ':status_message' => 'Not found',
        ':rx' => msg.bytesize
      },
      headers
    )
  end

  def test_eof
    @o << "HTTP/1.1 200 OK"
    @o.close

    assert_nil @parser.parse_headers
  end

  def test_protocol_case
    @o << "HTTP/1.1 200\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'http/1.1', headers[':protocol']

    reset_parser
    @o << "http/1.1 200\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'http/1.1', headers[':protocol']
  end

  def test_bad_status_line
    @o << "    HTTP/1.1 200\r\n\r\n"
    @o.close

    assert_raises(Error) { @parser.parse_headers }

    max_length = H1P_LIMITS[:max_status_message_length]

    reset_parser
    @o << "HTTP/1.1 200 #{'a' * max_length}\r\n\r\n"
    assert_equal 'a' * max_length, @parser.parse_headers[':status_message']

    reset_parser
    @o << "HTTP/1.1 200 #{'a' * (max_length + 1)}\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }
  end

  def test_path_characters
    @o << "HTTP/1.1 200 äBçDé¤23~{@€\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'äBçDé¤23~{@€', headers[':status_message']
  end

  def test_bad_status
    @o << "HTTP/1.1 abc\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    reset_parser
    @o << "HTTP/1.1 1111111111111111111\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    reset_parser
    @o << "HTTP/1.1 200a\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }
  end

  def test_headers_eof
    @o << "HTTP/1.1 200 OK\r\na"
    @o.close

    assert_nil @parser.parse_headers

    reset_parser
    @o << "HTTP/1.1 200 OK\r\na:"
    @o.close

    assert_nil @parser.parse_headers

    reset_parser
    @o << "HTTP/1.1 200 OK\r\na:      "
    @o.close

    assert_nil @parser.parse_headers
  end

  def test_headers
    @o << "HTTP/1.1 200 OK\r\nFoo: Bar\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal [':protocol', ':status', ':status_message', 'foo', ':rx'], headers.keys
    assert_equal 'Bar', headers['foo']

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nFOO:    baR\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'baR', headers['foo']

    reset_parser
    @o << "HTTP/1.1 200 OK\r\na: bbb\r\nc: ddd\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'bbb', headers['a']
    assert_equal 'ddd', headers['c']
  end

  def test_headers_multiple_values
    @o << "HTTP/1.1 200 OK\r\nFoo: Bar\r\nfoo: baz\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal ['Bar', 'baz'], headers['foo']
  end

  def test_bad_headers
    @o << "HTTP/1.1 200 OK\r\n   a: b\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    reset_parser
    @o << "HTTP/1.1 200 OK\r\na b\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    max_key_length = H1P_LIMITS[:max_header_key_length]

    reset_parser
    @o << "HTTP/1.1 200 OK\r\n#{'a' * max_key_length}: b\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'b', headers['a' * max_key_length]

    reset_parser
    @o << "HTTP/1.1 200 OK\r\n#{'a' * (max_key_length + 1)}: b\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    max_value_length = H1P_LIMITS[:max_header_value_length]

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nfoo: #{'a' * max_value_length}\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal 'a' * max_value_length, headers['foo']

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nfoo: #{'a' * (max_value_length + 1)}\r\n\r\n"
    assert_raises(Error) { @parser.parse_headers }

    max_header_count = H1P_LIMITS[:max_header_count]

    reset_parser
    hdrs = (1..max_header_count).map { |i| "foo#{i}: bar\r\n" }.join
    @o << "HTTP/1.1 200 OK\r\n#{hdrs}\r\n"
    headers = @parser.parse_headers
    assert_equal (max_header_count + 4), headers.size

    reset_parser
    hdrs = (1..(max_header_count + 1)).map { |i| "foo#{i}: bar\r\n" }.join
    @o << "HTTP/1.1 200 OK\r\n#{hdrs}\r\n"
    assert_raises(Error) { @parser.parse_headers }
  end

  def test_request_without_cr
    msg = "HTTP/1.1 200 OK\nBar: baz\n\n"
    @o << msg
    headers = @parser.parse_headers
    assert_equal({
      ':protocol' => 'http/1.1',
      ':status'   => 200,
      ':status_message' => 'OK',
      'bar'       => 'baz',
      ':rx'       => msg.bytesize
    }, headers)
  end

  def test_read_body_with_content_length
    10.times do
      data = ' ' * rand(20..60000)
      msg = "HTTP/1.1 200 OK\r\nContent-Length: #{data.bytesize}\r\n\r\n#{data}"
      Thread.new { @o << msg }

      headers = @parser.parse_headers
      assert_equal data.bytesize.to_s, headers['content-length']

      body = @parser.read_body
      assert_equal data, body
      assert_equal msg.bytesize, headers[':rx']
    end
  end

  def test_read_body_chunk_with_content_length
    data = 'abc' * 20000
    msg = "HTTP/1.1 200 OK\r\nContent-Length: #{data.bytesize}\r\n\r\n#{data}"
    Thread.new { @o << msg }
    headers = @parser.parse_headers
    assert_equal data.bytesize.to_s, headers['content-length']

    buf = +''
    count = 0
    while (chunk = @parser.read_body_chunk(false))
      count += 1
      buf += chunk
    end
    assert_equal data.bytesize, data.bytesize
    assert_equal data, buf
    assert_in_range 1..20, count
    assert_equal msg.bytesize, headers[':rx']
  end

  def test_read_body_with_content_length_incomplete
    data = ' ' * rand(20..60000)
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nContent-Length: #{data.bytesize + 1}\r\n\r\n#{data}"
      @o.close # !!! otherwise the parser will keep waiting
    end
    headers = @parser.parse_headers

    assert_raises(H1P::Error) { @parser.read_body }
  end

  def test_read_body_chunk_with_content_length_incomplete
    data = 'abc' * 50
    @o << "HTTP/1.1 200 OK\r\nContent-Length: #{data.bytesize + 1}\r\n\r\n#{data}"
    @o.close
    headers = @parser.parse_headers

    assert_raises(H1P::Error) { @parser.read_body_chunk(false) }
  end

  def test_read_body_with_chunked_encoding
    chunks = []
    total_sent = 0
    Thread.new do
      msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      @o << msg
      total_sent += msg.bytesize
      rand(8..16).times do |i|
        chunk = i.to_s * rand(200..360000)
        msg = "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
        @o << msg
        chunks << chunk
        total_sent += msg.bytesize
      end
      msg = "0\r\n\r\n"
      @o << msg
      total_sent += msg.bytesize
    end
    headers = @parser.parse_headers
    assert_equal 'chunked', headers['transfer-encoding']

    body = @parser.read_body
    assert_equal chunks.join, body
    assert_equal total_sent, headers[':rx']
  end

  def test_read_body_chunk_with_chunked_encoding
    chunks = []
    total_sent = 0
    Thread.new do
      msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      @o << msg
      total_sent += msg.bytesize
      rand(8..16).times do |i|
        chunk = i.to_s * rand(40000..360000)
        msg = "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
        @o << msg
        total_sent += msg.bytesize
        chunks << chunk
      end
      msg = "0\r\n\r\n"
      @o << msg
      total_sent += msg.bytesize
    end
    headers = @parser.parse_headers
    assert_equal 'chunked', headers['transfer-encoding']

    received = []
    while (chunk = @parser.read_body_chunk(false))
      received << chunk
    end
    assert_equal chunks, received
    assert_equal total_sent, headers[':rx']
  end

  def test_read_body_with_chunked_encoding_malformed
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' '.to_s * rand(40000..360000)
      @o << "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n3"
      @o << "0\r\n\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    assert_raises(H1P::Error) { @parser.read_body }

    reset_parser
    # missing last empty chunk
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' '.to_s * rand(40000..360000)
      @o << "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    assert_raises(H1P::Error) { @parser.read_body }

    reset_parser
    # bad chunk size
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' '.to_s * rand(40000..360000)
      @o << "-#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    assert_raises(H1P::Error) { @parser.read_body }
  end

  def test_read_body_chunk_with_chunked_encoding_malformed
    chunk = nil
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' ' * rand(40000..360000)
      @o << "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n3"
      @o << "0\r\n\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    read = @parser.read_body_chunk(false)
    assert_equal chunk, read
    assert_raises(H1P::Error) { @parser.read_body_chunk(false) }

    reset_parser

    # missing last empty chunk
    chunk = nil
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' '.to_s * rand(20..1600)
      @o << "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    read = @parser.read_body_chunk(false)
    assert_equal chunk, read
    assert_raises(H1P::Error) { @parser.read_body_chunk(false) }

    reset_parser

    # bad chunk size
    Thread.new do
      @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      chunk = ' '.to_s * rand(20..1600)
      @o << "-#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n"
      @o.close
    end
    headers = @parser.parse_headers
    assert_raises(H1P::Error) { @parser.read_body_chunk(false) }

    reset_parser

    # missing body
    @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    @o.close
    headers = @parser.parse_headers
    assert_raises(H1P::Error) { @parser.read_body_chunk(false) }
  end

  def test_complete?
    @o << "HTTP/1.1 200 OK\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal true, @parser.complete?

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal false, @parser.complete?
    @o << 'foo'
    body = @parser.read_body
    assert_equal 'foo', body
    assert_equal true, @parser.complete?

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    headers = @parser.parse_headers
    assert_equal false, @parser.complete?
    @o << "3\r\nfoo\r\n"
    chunk = @parser.read_body_chunk(false)
    assert_equal 'foo', chunk
    assert_equal false, @parser.complete?
    @o << "0\r\n\r\n"
    chunk = @parser.read_body_chunk(false)
    assert_nil chunk
    assert_equal true, @parser.complete?
  end

  def test_buffered_body_chunk
    @o << "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nfoo"
    headers = @parser.parse_headers
    assert_equal false, @parser.complete?

    chunk = @parser.read_body_chunk(true)
    assert_equal 'foo', chunk
    assert_equal true, @parser.complete?
    chunk = @parser.read_body_chunk(false)
    assert_nil chunk
    assert_equal true, @parser.complete?

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nfoo"
    headers = @parser.parse_headers
    assert_equal false, @parser.complete?

    chunk = @parser.read_body_chunk(true)
    assert_equal 'foo', chunk
    assert_equal false, @parser.complete?
    @o << 'bar'
    chunk = @parser.read_body_chunk(false)
    assert_equal 'bar', chunk
    assert_equal true, @parser.complete?

    reset_parser
    @o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nfoo\r\n"
    headers = @parser.parse_headers
    assert_equal false, @parser.complete?

    chunk = @parser.read_body_chunk(true)
    assert_equal 'foo', chunk
    assert_equal false, @parser.complete?
    @o << "0\r\n\r\n"
    chunk = @parser.read_body_chunk(true)
    assert_nil chunk
    assert_equal true, @parser.complete?
  end

  def test_parser_with_tcp_socket
    port = rand(1234..5678)
    server = TCPServer.new('127.0.0.1', port)
    server_thread = Thread.new do
      while (socket = server.accept)
        Thread.new do
          parser = H1P::Parser.new(socket, :client)
          headers = parser.parse_headers
          socket << headers.inspect
          socket.shutdown
          socket.close
        end
      end
    end

    sleep 0.001
    client = TCPSocket.new('127.0.0.1', port)
    msg = "HTTP/1.1 418 I'm a teapot\r\nCookie: abc=def\r\n\r\n"
    client << msg
    reply = client.read
    assert_equal({
      ':protocol' => 'http/1.1',
      ':status' => 418,
      ':status_message' => "I'm a teapot",
      'cookie' => 'abc=def',
      ':rx' => msg.bytesize,
    }, eval(reply))
  ensure
    client.shutdown rescue nil
    client&.close
    server_thread&.kill
    server_thread&.join
    server&.close
  end

  def test_parser_with_callable
    buf = []
    request = +"HTTP/1.1 200 OK\r\nHost: bar\r\n\r\n"
    callable = proc do |len|
      buf << {len: len}
      request
    end

    parser = H1P::Parser.new(callable, :client)

    headers = parser.parse_headers
    assert_equal({
      ':protocol' => 'http/1.1',
      ':status' => 200,
      ':status_message' => 'OK',
      'host' => 'bar',
      ':rx' => request.bytesize,

    }, headers)
    assert_equal [{len: 4096}], buf

    request = +"HTTP/1.1 404 Not found\r\nHost: baz\r\n\r\n"
    headers = parser.parse_headers
    assert_equal({
      ':protocol' => 'http/1.1',
      ':status' => 404,
      ':status_message' => 'Not found',
      'host' => 'baz',
      ':rx' => request.bytesize,

    }, headers)
    assert_equal [{len: 4096}, {len: 4096}], buf
  end
end
