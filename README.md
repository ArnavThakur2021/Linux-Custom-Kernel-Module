This project contains code and steps to initialize a custom kernel module for linux to monitor network throughput.

You can you linux like ubuntu or arch and you can also use AWS Linux for this just make sure you store all the files in same dir, I have stored in
 user/develop/kernel/netspeed and stored these files using nano 
Now enter the following lines to compile and run the code:

make

sudo insmod netspeed.ko

chmod +x netspeed_top.py

watch -n1 cat /proc/netspeed

sudo python3 netspeed_top.py

#You can open other terminal using tmux (then ctrl+b and %) .To check the network monitor simultaneously you can run the below curl in other tab (ctrl+b and <- or ->).
 curl "https://speed.edgecompute.app/down?bytes=1000000000000000000" > /dev/null

To unload the module after you are done sudo rmmod netspeed
