#!/usr/bin/env lua

require "ubus"
require "uloop"

uloop.init()

print(arg[1])

local conn = ubus.connect(arg[1])
if not conn then
	error("Failed to connect to ubus")
end

local my_method = {
	broken = {
		hello = 1,
		hello1 = {
			function(req)
			end, {id = "fail"}, { 3 }
		},
	},
	test = {
		hello = {
			function(req, msg)
				conn:reply(req, {message="foo"});
				print("Call to function 'hello'")
				for k, v in pairs(msg) do
					print("key=" .. k .. " value=" .. tostring(v))
				end
			end, {id = ubus.INT32, msg = ubus.STRING}, {256}
		},
		hello1 = {
			function(req)
				conn:reply(req, {message="foo1"});
				conn:reply(req, {message="foo2"});
				print("Call to function 'hello1'")
			end, {id = ubus.INT32, msg = ubus.STRING}, {1024}
		}
	}
}

conn:add(my_method)

local my_event = {
	test = function(msg)
		print("Call to test event")
		for k, v in pairs(msg) do
			print("key=" .. k .. " value=" .. tostring(v))
		end
	end,
}

conn:listen(my_event)

uloop.run()
