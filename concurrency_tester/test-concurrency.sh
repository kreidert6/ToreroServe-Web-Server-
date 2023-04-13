#!/bin/bash

# Usage: test-concurrency.sh [HOSTNAME] [PORT_NUM]

server_hostname=$1
port_num=$2 

if [ "$#" -ne 2 ]; then
	echo "Usage: test-concurrency.sh [HOSTNAME] [PORT_NUM]"
	exit
fi

# Test for concurrency support
echo "Testing support for multithreading by opening 3 simultaneous connections"

# do a GET request for index.html
bash client3-sim.sh | socat - TCP:$server_hostname:$port_num,crnl > index-download.txt

# run the three 3 clients...
echo "Starting Client 1. It will wait 3 seconds after connecting before sending its request."
bash client1-sim.sh | socat - TCP:$server_hostname:$port_num,crnl > client1-output.txt &
CLIENT1_PID=$!

echo "Starting Client 2. It will wait 2 seconds after connecting before sending its request."
bash client2-sim.sh | socat - TCP:$server_hostname:$port_num,crnl > client2-output.txt &
CLIENT2_PID=$!

echo "Starting Client 3. It will immediately send its request."
bash client3-sim.sh | socat - TCP:$server_hostname:$port_num,crnl > client3-output.txt &
CLIENT3_PID=$!

echo "Giving time for clients to finish..."
sleep 5

echo "Sending SIGTERM to clients in case they got stuck"
kill $CLIENT1_PID
kill $CLIENT2_PID
kill $CLIENT3_PID

# compare the 3 client's outputs to the "gold" version

if cmp --silent index-download.txt client1-output.txt \
	&& cmp --silent index-download.txt client2-output.txt \
	&& cmp --silent index-download.txt client3-output.txt
then
	echo "Concurrency test passed!"
else
	echo "Concurrency test failed! Check the contents of the clientX-output.txt files."
fi
