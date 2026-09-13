#!/bin/sh
# netcheck: only sewn may make a network call. Every other package's sources are
# grepped for what one needs — libcurl, libwebsockets, an INET socket family, a
# resolver, an https:// URL — and any hit fails the build. Comments count too:
# a package that has to explain itself with a URL should do so in its README.
#   sh testing/netcheck.sh <pkg>...
set -u
status=0
pattern='curl_[a-z_]*(|lws_[a-z_]*(|getaddrinfo|gethostbyname|AF_INET|PF_INET|SOCK_DGRAM|inet_pton|inet_addr|https://'
for pkg in "$@"; do
    [ -d "$pkg" ] || continue
    hits=$(grep -rEn "$pattern" "$pkg" --include='*.c' --include='*.h' 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "netcheck: $pkg reaches for the network:"
        echo "$hits" | sed 's/^/  /'
        status=1
    fi
done
[ $status = 0 ] && echo "netcheck: only sewn talks to the network ($#: packages checked)"
exit $status
