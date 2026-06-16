# Qualcomm RCP application

RCP (Remote Co-Processor) application, communicating with a host over
Qualcomm RCP protocol.

# Build

## QCC744M EVK

```
cd rcp_zephyr/
west build -p -b qcc744m_evk samples/rcp_app
```

### WiFi co-processor application

```
cd rcp_zephyr/
west build -p -b qcc744m_evk samples/rcp_app -- -DEXTRA_CONF_FILE=prj_wifi.conf
```

### BT co-processor application

```
cd rcp_zephyr/
west build -p -b qcc744m_evk samples/rcp_app -- -DEXTRA_CONF_FILE=prj_bt.conf
```

## Flash

```
west flash
```
