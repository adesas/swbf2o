# Playing from another device on the LAN

The page needs three things no plain `http://<ip>` URL can give a device that is not this machine:
a **Secure Context** for WebGPU, `SharedArrayBuffer` (COOP/COEP) and OPFS. So "open it on the laptop"
is not just a matter of the right address, and the failure looks identical whether the cause is the
server, the certificate, or the router. Hence the tools below, which measure rather than assert.

```sh
tools/lan_serve.sh start      # durable server (http 8123 + https 8443 + multiplayer 8133)
tools/lan_check.py            # proves whether this machine really serves the LAN; exits nonzero
tools/lan_serve.sh status     # who is listening right now
tools/lan_serve.sh logs       # tail the server log
tools/lan_serve.sh stop       # takes the multiplayer child with it, not just the parent
```

Then give a neighbour `https://<this machine's LAN ip>:8443/`. The certificate is self-signed, so
expect one "Advanced → proceed" prompt per device; that warning is the tool working, not broken.
`http://<ip>:8123/` still works from this machine and from any device that cannot be given the
certificate, but on a non-local address it now answers with a 302 to the HTTPS port, because the
game would otherwise load and then silently have no GPU, no threads and no saved profiles.

## What a "cannot reach it from another computer" report actually is, in order

Every occurrence of this in this project's history was the first of these, and every time it was
assumed to be the last two:

1. **Nothing is listening.** The server was started as a background job of an interactive shell, and
   died with that shell when the session ended. `ss -ltn | grep 81` shows no 8123/8443/8133, and curl
   says *connection refused* — which reads exactly like a firewall. This is why `lan_serve.sh` exists:
   it starts with `setsid nohup`, and then refuses to report success until the ports actually accept.
2. **The device is on plain http.** `window.isSecureContext` is false for `http://192.168.x.x`, so
   `navigator.gpu` is undefined, `SharedArrayBuffer` is unavailable and OPFS is missing. The page loads
   and does nothing useful. `tools/serve.py` 302s a LAN `Host` to `:8443`; `browser/play.mjs` also
   redirects client-side, which covers the case where someone is already halfway in.
3. **The wrong address.** A machine with Docker has several routable IPv4s. The addresses printed at
   startup are ordered so the interface carrying the default route comes first, and the docker/VM
   bridges are labelled `(not the default route)`. `172.17.0.1` is never what a neighbour wants.
4. **This machine's firewall.** Checked first here, because it is cheap: `iptables -L INPUT -n` on this
   box is policy `ACCEPT` with no rules, so it was never the cause. If a future distro enables one,
   open 8123 and 8443.
5. **The router.** Client isolation / AP isolation / a guest SSID on a different VLAN. This is the only
   cause that cannot be diagnosed from this machine — and the only one `lan_check.py` cannot rule out.

## Why `lan_check.py` does not stop at `curl`

A request from this machine to its own LAN address is delivered over loopback: it never traverses the
ingress path a neighbour's packets take, so it cannot catch an ingress filter, a wrong-interface bind
or an asymmetric-route drop. When passwordless sudo is available, `lan_check.py` builds a network
namespace on a different subnet (`10.99.0.2` behind a `veth` pair) and repeats the request from there,
arriving over a real interface — the closest thing available to testing from the next desk. It skips
that check rather than pretending, and prints how to enable it.

The checks are: bind is not loopback-only; `http` 302s for a LAN `Host` but leaves `localhost` alone;
`https /` carries COOP/COEP; the certificate SAN actually contains the LAN address (read with openssl,
because `ssl.getpeercert()` returns an empty dict on an unverified connection — that bug made this
check pass on a completely empty SAN until it was caught); `/api/health` and a real `Upgrade: websocket`
handshake are proxied to the signaling server; a byte-range request inside a `.lvl` container returns
206.

## Multiplayer behind the one port

`tools/serve.py` proxies `/api/*` and `/ws` on the same origin to the signaling server on
`127.0.0.1:8133`, so a neighbour needs one URL and one certificate prompt. Every peer therefore
arrives from the proxy, not from its own address — that is fine here because the signaling server hands
out a synthetic in-game address per room slot (`10.200.<subnet>.<slot>`) rather than deriving identity
from `remoteAddress`, so proxied peers cannot collide. If that ever changes, the proxy needs to add
`X-Forwarded-For` and the server needs to trust it.

## Two things a certificate will not fix

* If this machine's DHCP address changes, the URL changes and the certificate is regenerated for the
  new address, so every device prompts again. Reserve the lease on the router if this bothers people.
* Everything on the network can read the whole GameData dump over `:8443`. Keep this off untrusted
  networks; it is a LAN tool, not a public service.
