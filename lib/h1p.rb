# frozen_string_literal: true

require_relative './h1p_ext'

class ::IO
  if !method_defined?(:__read_method__)
    def __read_method__
      :stock_readpartial
    end
  end
end

require 'socket'

class Socket
  if !method_defined?(:__read_method__)
    def __read_method__
      :stock_readpartial
    end
  end
end

class TCPSocket
  if !method_defined?(:__read_method__)
    def __read_method__
      :stock_readpartial
    end
  end
end

class UNIXSocket
  if !method_defined?(:__read_method__)
    def __read_method__
      :stock_readpartial
    end
  end
end
