# Example OSC

![preview](img/preview.png)

Lamp follows Master / Blackout / Color. This example registers those leaves, plus status and heartbeat, on `/<appName>/<networkId>/`. Window title shows `[network-id]`. **Broadcast Color / Master** send `/<appName>/color` and `/<appName>/master` so every peer on the send target picks them up. **Last from** is the network id in a directed address.

```bash
cmake -S examples/osc -B examples/osc/build
cmake --build examples/osc/build --target osc
./examples/osc/build/bin/osc/osc
./examples/osc/build/bin/osc/osc --smoke-osc

# Two instances (directed heartbeat + broadcast buttons)
./examples/osc/build/bin/osc/osc --network-id=a --listen-port=8000 --send-port=8001
./examples/osc/build/bin/osc/osc --network-id=b --listen-port=8001 --send-port=8000
```

Multi-instance author/show shell: RigKit `examples/oscHost`.
