# Linux RCP demo

This example provides a basic example on how to use RCP link protocol to
communicate between two peers.

This example provides a reference IO bus implementation over Linux sockets,
allowing two Linux applications - demo_rcp_host and demo_rcp_mcu - to exchange
data over link.

This program can be used for testing the generic protocol layer on developer
machine.

## Build

```
cmake -B build samples/demo_rcp
cmake --build build
```

## Run

1. Start demo_rcp_mcu in one terminal.

```
./build/demo_rcp_mcu
```

2. Start demo_rcp_host in a separate terminal.

```
./build/demo_rcp_host
```
