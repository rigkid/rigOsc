# rigOsc

![preview](examples/osc/img/preview.png)

UDP OSC transport. Show-bus addresses are `/<appName>/<networkId>/<leaf>` and broadcast `/<appName>/<leaf>`. The app registers each leaf.

- Bind / listen / send (`UdpSocket` + `OscCodec`)
- OSC 1.0 messages and bundles; typed args `i` / `f` / `s` / `T`/`F` / blob `b`
- Address-pattern listeners (`?` `*` `[]` `{}`, plus pack `**` across `/`)
- `CNetworkIdentity`, `COscEndpoint` - data on the pack
- Bind failures surface in `lastError()` (no fake open)
- Shared CLI for any app: `applyCommandLine` + `commandLineHelp`
- `addBusListener` routes a leaf the app names. The pack does not ship leaf names.

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

Accepts `--key=value` or `--key value`. Logs `id=... listen ... send ... listening=...`.

Paste `rigOsc::commandLineHelp()` into your app’s `--help`.

Addresses: `/<appName>/<networkId>/<leaf>` directed, `/<appName>/<leaf>` broadcast. `oscHost` and this example register `master`, `blackout`, `color`, `status`, and send `heartbeat`. Other apps register their own leaves, or only use `addListener`.

Host reference with author/show + multi-instance: RigKit `examples/oscHost`.

[API/docs](https://rigkid.github.io/rigOsc/)
