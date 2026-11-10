## Documentation for the vscpl2drv-ikea-dirigera driver

**Document version:** ${/var/document-version} - ${/var/creation-time}
[HISTORY](./history.md)

The IKEA Dirigera driver is a VSCP level II driver that lets the
[VSCP daemon](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_daemon)
and other VSCP host applications interact with Matter/Zigbee devices
connected to an IKEA Dirigera hub.

The driver talks to the hub's REST API (`https://<hub>:8443/v1`) and gets
status updates either by polling or by subscribing to the hub's websocket
event stream. Device attribute changes - on/off state, motion, open/close,
temperature, humidity, air quality, illuminance, dim level - are translated
to VSCP events, and incoming CLASS1.CONTROL events are translated to hub
commands so lights and outlets can be switched and dimmed from the VSCP side.

On startup the complete home state (devices, rooms, scenes, users, device
sets and hub info) is fetched from the hub and stored in a SQLite database.
Every hub device is simulated as a VSCP node: status and functionality are
exposed through registers, behaviour can be programmed through a per-device
decision matrix, and an MDF is generated internally for each device type.
Active, reachable devices send periodic VSCP node heartbeats.

The level II driver interface is
[described here](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_driver_interfaces).
With the simple interface API the VSCP level II driver uses it is also easy
to use the driver as a component in other software.

* [Repository for the module](https://github.com/grodansparadis/${/var/driver-name})
* This manual is available [here](https://grodansparadis.github.io/${/var/driver-name})


## VSCP - the Very Simple Control Protocol (framework)

![VSCP logo](./images/logo_100.png)

VSCP is a free and open automation protocol for IoT and m2m devices. Visit
[the VSCP site](https://www.vscp.org) for more information.

**VSCP is free.** Placed in the **public domain**. Free to use. Free to
change. Free to do whatever you want to do with it. VSCP is not owned by
anyone. VSCP will stay free and gratis forever.

The specification for the VSCP protocol is
[here](https://grodansparadis.github.io/vscp-doc-spec/#/)

VSCP documentation for various parts of the protocol/framework can be found
[here](https://docs.vscp.org/).

If you use VSCP please consider contributing resources or time to the
project ([https://github.com/sponsors/grodansparadis](https://github.com/sponsors/grodansparadis)).


## Document license

This document is licensed under
[Creative Commons BY 4.0](https://creativecommons.org/licenses/by/4.0/) and
can be freely copied, redistributed, remixed, transformed, built upon as
long as you give credits to the author.


[filename](./bottom-copyright.md ':include')
