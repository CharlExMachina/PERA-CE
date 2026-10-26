/* The shared Winsock adapter. Its Darwin paths (__APPLE__) translate the
game's two-byte address family to and from sa_len, make each socket with
SO_NOSIGPIPE, take arc4random for getrandom and send to a connected peer
that Darwin refuses as a destination. */
#include "port/linux/src/posix_net.c"
