-- Initialize the counter
local counter = 0
local socket = require("socket")

-- Get the current time in milliseconds

local start_time = socket.gettime()
local last_time = socket.gettime()
-- Setup the HTTP request
request = function()
    return wrk.format("GET", "/")
end

-- Track the response and increment the counter
response = function(status, headers, body)
    counter = counter + 1
    local elapsed_time = socket.gettime() - last_time
    -- if the time is longer than 100 ms. Print the number of completed requests
    if elapsed_time > 0.2 then
        print("Time:", socket.gettime() - start_time, "throughput:", counter / elapsed_time)
        counter = 0
        last_time = socket.gettime()
    end
end