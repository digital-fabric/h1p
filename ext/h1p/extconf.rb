# frozen_string_literal: true

require 'rubygems'
require 'mkmf'

$CFLAGS << " -Wno-format-security"
CONFIG['optflags'] << ' -fno-strict-aliasing' unless RUBY_PLATFORM =~ /mswin/

require_relative './limits'
H1P_LIMITS.each { |k, v| $defs << "-D#{k.upcase}=#{v}" }

dir_config 'h1p_ext'
create_makefile 'h1p_ext'
