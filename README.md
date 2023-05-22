# httpc
This is a httpc dynamic library wrapped in C++.
If you don’t care about the content of the http response, and just want to know if the request was sent successfully, then you can try using it.
get and post are supported.

# Usage
```lua
local httpc = require "httpc"

-- Start a libcurl client managed by epoll.
-- Success will return 0.
local result = httpc.start("epoll")
print("start :", result)

-- Send an https request.
local url = "https://example.com/"
-- Discard the response content.
local lose_content = true
result = httpc.request(url, lose_content)
print("request : ", result)

while true do
	-- Timed refresh
	httpc.flush()
	-- Get the error messages of all failed requests.
	local list = httpc.failed()
	-- sleep 100 ms
	httpc.sleep(100)
end
```

