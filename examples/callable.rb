# frozen_string_literal: true

require 'bundler/setup'
require 'h1p'

data = ['GET ', '/foo', " HTTP/1.1\r\n", "\r\n"]
parser = H1P::Parser.new(proc { data.shift })

headers = parser.parse_headers
p headers
