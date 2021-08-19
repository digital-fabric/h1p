# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/clean"

require "rake/extensiontask"
Rake::ExtensionTask.new("h1p_ext") do |ext|
  ext.ext_dir = "ext/h1p"
end

task :recompile => [:clean, :compile]
task :default => [:compile, :test]

task :test do
  exec 'ruby test/test_h1p.rb'
end
