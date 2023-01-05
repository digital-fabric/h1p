# frozen_string_literal: true

require_relative 'helper'
require 'h1p'

class SendResponseTest < MiniTest::Test
  def test_send_response_status_line
    i, o = IO.pipe
    H1P.send_response(o, { ':status' => '418 I\'m a teapot' })
    o.close
    response = i.read
    assert_equal "HTTP/1.1 418 I'm a teapot\r\n\r\n", response

    i, o = IO.pipe
    count = H1P.send_response(o, { ':protocol' => 'HTTP/1.0' })
    o.close
    response = i.read
    assert_equal "HTTP/1.0 200 OK\r\n\r\n", response
    assert_equal "HTTP/1.0 200 OK\r\n\r\n".bytesize, count
  end

  def test_send_response_string_headers
    i, o = IO.pipe
    H1P.send_response(o, { 'Foo' => 'Bar', 'Content-Length' => '123' })
    o.close
    response = i.read
    assert_equal "HTTP/1.1 200 OK\r\nFoo: Bar\r\nContent-Length: 123\r\n\r\n", response
  end

  def test_send_response_non_string_headers
    i, o = IO.pipe
    H1P.send_response(o, { :Foo => 'Bar', 'Content-Length' => 123 })
    o.close
    response = i.read
    assert_equal "HTTP/1.1 200 OK\r\nFoo: Bar\r\nContent-Length: 123\r\n\r\n", response
  end

  def test_send_response_with_body
    i, o = IO.pipe
    H1P.send_response(o, {}, "foobar")
    o.close
    response = i.read
    assert_equal "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nfoobar", response
  end

  def test_send_response_with_big_body
    i, o = IO.pipe
    body = "abcdefg" * 10000
    Thread.new { H1P.send_response(o, {}, body); o.close }

    response = i.read
    assert_equal "HTTP/1.1 200 OK\r\nContent-Length: #{body.bytesize}\r\n\r\n#{body}", response
  end

  def test_send_response_with_big_body
    i, o = IO.pipe
    body = "abcdefg" * 10000
    Thread.new { H1P.send_response(o, {}, body); o.close }

    response = i.read
    assert_equal "HTTP/1.1 200 OK\r\nContent-Length: #{body.bytesize}\r\n\r\n#{body}", response
  end
end

class SendBodyChunkTest < MiniTest::Test
  def test_send_body_chunk
    i, o = IO.pipe
    len1 = H1P.send_body_chunk(o, 'foo')
    assert_equal 8, len1
    len2 = H1P.send_body_chunk(o, :barbazbarbaz)
    assert_equal 17, len2
    len3 = H1P.send_body_chunk(o, 1234)
    assert_equal 9, len3
    len4 = H1P.send_body_chunk(o, nil)
    assert_equal 5, len4
    o.close
    response = i.read
    assert_equal "3\r\nfoo\r\nc\r\nbarbazbarbaz\r\n4\r\n1234\r\n0\r\n\r\n", response
  end

  def test_send_body_chunk_big
    i, o = IO.pipe

    chunk = 'foobar1' * 20000
    len = nil

    Thread.new do
      len = H1P.send_body_chunk(o, chunk)
      o.close
    end
    
    response = i.read
    assert_equal "#{chunk.bytesize.to_s(16)}\r\n#{chunk}\r\n", response
    assert_equal chunk.bytesize + chunk.bytesize.to_s(16).bytesize + 4, len
  end
end
