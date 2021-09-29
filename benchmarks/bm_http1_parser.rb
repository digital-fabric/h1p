# frozen_string_literal: true

HTTP_REQUEST = "GET /foo HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nUser-Agent: foobar\r\n\r\n"

# HTTP_REQUEST =
#   "GET /wp-content/uploads/2010/03/hello-kitty-darth-vader-pink.jpg HTTP/1.1\r\n" +
#   "Host: www.kittyhell.com\r\n" +
#   "User-Agent: Mozilla/5.0 (Macintosh; U; Intel Mac OS X 10.6; ja-JP-mac; rv:1.9.2.3) Gecko/20100401 Firefox/3.6.3 Pathtraq/0.9\r\n" +
#   "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n" +
#   "Accept-Language: ja,en-us;q=0.7,en;q=0.3\r\n" +
#   "Accept-Encoding: gzip,deflate\r\n" +
#   "Accept-Charset: Shift_JIS,utf-8;q=0.7,*;q=0.7\r\n" +
#   "Keep-Alive: 115\r\n" +
#   "Connection: keep-alive\r\n" +
#   "Cookie: wp_ozh_wsa_visits=2; wp_ozh_wsa_visit_lasttime=xxxxxxxxxx; __utma=xxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.xxxxxxxxxx.x; __utmz=xxxxxxxxx.xxxxxxxxxx.x.x.utmccn=(referral)|utmcsr=reader.livedoor.com|utmcct=/reader/|utmcmd=referral\r\n\r\n"

def measure_time_and_allocs
  4.times { GC.start }
  GC.disable

  t0 = Time.now
  a0 = object_count
  yield
  t1 = Time.now
  a1 = object_count
  [t1 - t0, a1 - a0]
ensure
  GC.enable
end

def object_count
  count = ObjectSpace.count_objects
  count[:TOTAL] - count[:FREE]
end

def benchmark_other_http1_parser(iterations)
  STDOUT << "http_parser.rb: "
  require 'http_parser.rb'

  i, o = IO.pipe
  parser = Http::Parser.new
  done = false
  headers = nil
  rx = 0
  parser.on_headers_complete = proc do |h|
    headers = h
    headers[':method'] = parser.http_method
    headers[':path'] = parser.request_url
    headers[':rx'] = rx
  end
  parser.on_message_complete = proc { done = true }

  elapsed, allocated = measure_time_and_allocs do
    iterations.times do
      o << HTTP_REQUEST
      done = false
      rx = 0
      while !done
        msg = i.readpartial(4096)
        rx += msg.bytesize
        parser << msg
      end
    end
  end
  puts(format('elapsed: %f, allocated: %d (%f/req), rate: %f ips', elapsed, allocated, allocated.to_f / iterations, iterations / elapsed))
end

def benchmark_tipi_http1_parser(iterations)
  STDOUT << "H1P parser: "
  require_relative '../lib/h1p'
  i, o = IO.pipe
  parser = H1P::Parser.new(i)

  elapsed, allocated = measure_time_and_allocs do
    iterations.times do
      o << HTTP_REQUEST
      headers = parser.parse_headers
    end
  end
  puts(format('elapsed: %f, allocated: %d (%f/req), rate: %f ips', elapsed, allocated, allocated.to_f / iterations, iterations / elapsed))
end

def fork_benchmark(method, iterations)
  pid = fork do
    send(method, iterations)
  rescue Exception => e
    p e
    p e.backtrace
    exit!
  end
  Process.wait(pid)
end

x = 100000
fork_benchmark(:benchmark_other_http1_parser, x)
fork_benchmark(:benchmark_tipi_http1_parser, x)

# benchmark_tipi_http1_parser(x)