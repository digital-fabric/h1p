require_relative './lib/h1p/version'

Gem::Specification.new do |s|
  s.name        = 'h1p'
  s.version     = H1P::VERSION
  s.licenses    = ['MIT']
  s.summary     = 'H1P is a blocking HTTP/1 parser for Ruby'
  s.author      = 'Sharon Rosner'
  s.email       = 'sharon@noteflakes.com'
  s.files       = `git ls-files`.split
  s.homepage    = 'http://github.com/digital-fabric/h1p'
  s.metadata    = {
    "source_code_uri" => "https://github.com/digital-fabric/h1p"
  }
  s.rdoc_options = ["--title", "h1p", "--main", "README.md"]
  s.extra_rdoc_files = ["README.md"]
  s.extensions = ["ext/h1p/extconf.rb"]
  s.require_paths = ["lib"]
  s.required_ruby_version = '>= 2.7'

  s.add_development_dependency  'rake-compiler',        '1.2.3'
  s.add_development_dependency  'rake',               '~>13.0.6'
  s.add_development_dependency  'minitest',           '~>5.18.0'
end
