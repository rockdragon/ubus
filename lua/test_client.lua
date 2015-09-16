#!/usr/bin/env lua

require "ubus"
require "uloop"

uloop.init()

print(arg[1])
local conn = ubus.connect(arg[1])
if not conn then
	error("Failed to connect to ubusd")
end

local namespaces = conn:objects()
for i, n in ipairs(namespaces) do
	print("namespace=" .. n)
	local signatures = conn:signatures(n)
	for p, s in pairs(signatures) do
		print("\tprocedure=" .. p)
		for k, v in pairs(s) do
			print("\t\tattribute=" .. k .. " type=" .. v)
		end
	end
end

--local st = os.time()
--for i = 1, 100000 do

	--local tt = os.time()
	local status = conn:call("test", "hello", { msg = "eth0" })

	for k, v in pairs(status) do
		print("key=" .. k .. " value=" .. tostring(v))
	end

  --print("[curr]:", i)
  --print("[total]:", os.time() - st, "seconds")
	--print("[aver]:", (os.time() - st) / i, "second")

	local status = {conn:call("test", "hello1", { msg = "eth0" })}

	for a = 1, #status do
		for k, v in pairs(status[a]) do
			print("key=" .. k .. " value=" .. tostring(v))
		end
	end

	conn:send("test", { foo = "bar"})

--end

--print("[total]:", os.time() - st, "seconds")

collectgarbage("collect")

uloop.run()
