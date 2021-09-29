# frozen_string_literal: true

require 'bundler/setup'
require 'h1p'

puts "pid: #{Process.pid}"
puts 'Listening on port 1234...'

trap('SIGINT') { exit! }

def handle_client(conn)
  Thread.new do
    parser = H1P::Parser.new(conn)
    loop do
      headers = parser.parse_headers
      break unless headers

      req_body = parser.read_body

      p headers: headers
      p body: req_body

      resp = 'Hello, world!'
      conn << "HTTP/1.1 200 OK\r\nContent-Length: #{resp.bytesize}\r\n\r\n#{resp}"
    rescue H1P::Error => e
      puts "Invalid request: #{e.message}"
    ensure
      conn.close
      break
    end
  end
end

require 'socket'
server = TCPServer.new('0.0.0.0', 1234)
loop do
  conn = server.accept
  handle_client(conn)
end
