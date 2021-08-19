# frozen_string_literal: true

require_relative './h1p_ext'

unless Object.const_defined?('Polyphony')
  class IO
    def __parser_read_method__
      :stock_readpartial
    end
  end

  require 'socket'

  class Socket
    def __parser_read_method__
      :stock_readpartial
    end
  end

  class TCPSocket
    def __parser_read_method__
      :stock_readpartial
    end
  end

  class UNIXSocket
    def __parser_read_method__
      :stock_readpartial
    end
  end
end
