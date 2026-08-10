# rigOsc

![preview](examples/osc/img/preview.png)

UDP OSC transport plus network identity / show-bus PODs for installs.

- Bind / listen / send (`UdpSocket` + `OscCodec`)
- `CNetworkIdentity`, `COscEndpoint`, `COscShowBus` — data on the pack
- Bind failures surface in `lastError()` (no fake open)
- Shared CLI for any app: `applyCommandLine` + `commandLineHelp`

## CLI

After pack register / `setup()`:

```cpp
m_osc->applyCommandLine(args);
```

| Flag | POD |
|------|-----|
| `--network-id` / `--id` | `CNetworkIdentity::networkId` |
| `--bind-address` | `CNetworkIdentity::bindAddress` |
| `--listen-port` | `COscEndpoint::listenPort` |
| `--send-host` | `COscEndpoint::sendHost` |
| `--send-port` | `COscEndpoint::sendPort` |

Accepts `--key=value` or `--key value`. Logs `id=… listen … send … listening=…`.

Paste `rigOsc::commandLineHelp()` into your app’s `--help`.

Addresses: `/rigkit/<networkId>/master|blackout|color|status|heartbeat` (directed) and `/rigkit/…` without id (broadcast). Bus sends append the sender `networkId` as a trailing string arg. `color` is three floats then optional sender id.

Host reference with author/show + multi-instance: RigKit `examples/oscHost`.

[API/docs](https://rigkid.github.io/rigOsc/)
