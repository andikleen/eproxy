eproxy is a simple epoll based efficient TCP proxy for Linux
It connects two TCP ports to each other and communicates zero copy.

It is not trying to compete with the "big boy" load balancers, 
but is very easy to adapt for experiment. It should be fairly
efficient however.

Simple port forwarder

	proxy inport outip outport

Uses pipes to splice two sockets together. This should give something
approaching zero copy, if the NIC driver is capable. 

This method is rather file descriptor intensive (4 fds/conn), so make sure you 
have enough. 


Andi Kleen
