#!/usr/bin/env ruby

require "webrick"

host = `route -qn get 0.0.0.0 | grep 'address:' | sed 's/.*: //'`.strip
port = 8000
file = ARGV[0]

s = WEBrick::HTTPServer.new(
  :Host => host,
  :Port => port,
  :DocumentRoot => "/dev/null"
)
s.mount_proc "/ota.txt" do |req,res|
  ver = File.read("wifippp.h").
    scan(/_VERSION.*/)[0].
    gsub(/"$/, "").
    gsub(/.*"/, "")
  size = File.size(file)
  md5 = `md5 -q #{file}`.strip

  res.body = "#{ver}\n" +
    "#{size}\n" +
    "#{md5}\n" +
    "http://#{host}:#{port}/update.bin\n"
end

s.mount_proc "/update.bin" do |req,res|
  res.body = File.binread(ARGV[0])
end

puts "",
  "Issue update command:",
  "",
  "AT$UPDATE! http://#{host}:#{port}/ota.txt",
  ""

s.start
