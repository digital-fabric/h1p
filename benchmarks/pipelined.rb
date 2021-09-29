# frozen_string_literal: true

HTTP_REQUEST = "GET /foo HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nUser-Agent: foobar\r\n\r\n" +
               "GET /bar HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nUser-Agent: foobar\r\n\r\n"

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
  queue = nil
  rx = 0
  req_count = 0
  parser.on_headers_complete = proc do |h|
    h[':method'] = parser.http_method
    h[':path'] = parser.request_url
    h[':rx'] = rx
    queue << h
  end
  parser.on_message_complete = proc { done = true }

  writer = Thread.new do
    iterations.times { o << HTTP_REQUEST }
    o.close
  end

  elapsed, allocated = measure_time_and_allocs do
    queue = []
    done = false
    rx = 0
    loop do
      data = i.readpartial(4096) rescue nil
      break unless data

      rx += data.bytesize
      parser << data
      while (req = queue.shift)
        req_count += 1
      end
    end
  end
  puts(format('count: %d, elapsed: %f, allocated: %d (%f/req), rate: %f ips', req_count, elapsed, allocated, allocated.to_f / iterations, iterations / elapsed))
end

def benchmark_h1p_parser(iterations)
  STDOUT << "H1P parser: "
  require_relative '../lib/h1p'
  i, o = IO.pipe
  parser = H1P::Parser.new(i)
  req_count = 0

  writer = Thread.new do
    iterations.times { o << HTTP_REQUEST }
    o.close
  end

  elapsed, allocated = measure_time_and_allocs do
    while (headers = parser.parse_headers)
      req_count += 1
    end
  end
  puts(format('count: %d, elapsed: %f, allocated: %d (%f/req), rate: %f ips', req_count, elapsed, allocated, allocated.to_f / iterations, iterations / elapsed))
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
fork_benchmark(:benchmark_h1p_parser, x)

# benchmark_h1p_parser(x)