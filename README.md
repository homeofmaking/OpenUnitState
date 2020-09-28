# OpenUnitState
<img src="doc/openunitstate_demo1.jpg?raw=true" alt="OpenUnitState Demo" width="250"/>

An approach to "unit" status displays and authentication in open workshops and 
makerspaces. A unit can be anything from a tool found in a workshop, a 3D printer,
even a conventional printer or a kitchen stove. 

OpenUnitState aims to be "stupid by default" and therefor relies on logic 
implemented in the backend. Communication is handled through "unencrypted" MQTT
(feel free to post a pull request to enhance this). The reason for this is that we
compare OpenUnitState to a code lock on a device - if someone wants to go around
it, they will find a way to. The same holds true for OpenUnitState. It will most
likely be easier to rip out the wires of the unlocking mechanism and short them...

OpenUnitState itself consists of a hardware and a software part. The hardware 
consists mainly of an ESP8266 µC, a RC522 based card reader, a liquid crystal 
display and a button. Depending on the unit you want to control you can either 
use the 5V/VCC mosfet or a relais. The mosfet is always required, the relais is
an optional component.

OpenUnitState does indeed depend on WiFi availability. A stable WiFi and network 
setup is absolutely recommended when using OpenUnitState, however network is 
only used where necessary, e.g. updating information to the device or e.g. 
authentication requests. This is the drawback of heavily relying on off-device 
logic.

## Features
*  5 different modes to support a majority of use-cases in shared environments
    - ID to unlock (authentication required) 
    - Push to unlock
    - Permanently unlocked
    - Maintenance
    - OTA
* Information on a liquid crystal display dependant on current mode
    - Support long messages for maintenance mode /w scrolling
    - Spinner shows device is not hanging & works as intended
    - Easy to read status symbol (check for unlocked, button for push to unlock, padlock for id to unlock and skull for maintenance)
* When unlock time is close to running out, the display will start blinking
* Ability to unlock machines during maintenance mode (backend should check for permission)
* Report "unit broken" by holding the button
* WiFi manager for joining networks for easy setup and device relocation
* Stable WiFi and MQTT re-connect implemented

## Example implementations
<details>
    <summary>Demo Device</summary>
    <img src="doc/openunitstate_demo1.jpg?raw=true" alt="OpenUnitState Demo" width="200"/>
    <strong>Description:</strong> Example device used for demonstrations and development. 
    <strong>Backend:</strong> Node-Red /w Node-Red dashboard
</details>

<details>
    <summary>Check-In & Check-Out Terminal</summary>
    <img src="doc/openunitstate_checkin-out1.jpg?raw=true" alt="OpenUnitState Check-In & Check-Out terminal" width="250"/>
    <strong>Description:</strong> OpenUnitState used as a Check-In and Check-Out terminal. Part of a hygiene concept that was required by local authorities to re-open a makerspace in munich.
    <strong>Backend:</strong> Node-Red, MySQL
</details>


## MQTT connection
Right now MQTT connection needs to be configured before building the project.
`#define MQTT_TOPIC "iot/openmakerstate/"` defines the first part of the MQTT 
topic. The device will listen and write to `%MQTT_TOPIC%/%CHIP_ID%/#`

If you use `#define MQTT_TOPIC "iot/openmakerstate/"` and your ESP8266 chip id 
is c64c95 the full path would be `iot/openmakerstate/c64c95/#` (replace # with
commands found below)

## Commands MQTT > OUS
| Topic endpoint | Payload | Description |
| ------ | ------ | ------ |
| /config_name | unitName | sets the displayed unit name |
| /config_maintenance_long_reason | maintenanceReason |  sets the long reason that is displayed when unit is in maintenance mode | 
| /config_status | 5,2,0,-1,-2,-3 |  5 = ID to unlock, 2 = push to unlock, 0 = permanently unlocked, -1 = maintenance, -2 = OTA, -3 = Check-In/Out Mode | 
| /unlocked_time | secondsToUnlock |  unlocks the machine for the given time, can be called during unlocked state to set timer again  | 
| /quick_display_msg | messageToDisplay |  Briefly displays a message on line 2 of the display |        
| /reset | - |  calls esp.restart() to hard-reset the device | 

## Commands OUS > MQTT
| Topic endpoint | Payload | Description |
| ------ | ------ | ------ |
| /connected | (always true) | called as soon as MQTT connection is established |
| /localip | ip address | called as soon as the MQTT connection is established |
| /started | firmwareVersion | called during setup but NOT upon MQTT re-connect to indicate ESP was (re-)started |
| /ready_for_ota | chipId | as soon as device is set to mode -2 (OTA) and OTA is ready |
| /button_reported_broken | chipId | called when someone reports the device as broken using the pushbutton |
| /push_to_unlock | chipId | called on single button press when device is in push to unlock mode |
| /state_relocked | chipId | called to indicate that the device timer has run out and was re-locked |
| /card_read | hex of card | called whenever a card is presented to the reader |

## Backend design recommendations
The device boots without any configuration other than the pre-configured MQTT server. 
Therefor it is recommended to listen for the `/started` signal from OUS and 
reply with `/config_name` and `/config_status`. This initializes the device and
it is ready to use. 

If you intend to use the ID to unlock mode you should listen for `/card_read` 
and reply with an appropriate `/unlocked_time` from the backend. 

It is recommended to implement listening for `/card_read` in maintenance mode as 
well and then sending `/unlocked_time` for cards that are owned by people which
usually perform maintenance on machines.

`/card_read` could also be processed in push to unlock mode to just perform the
same action as a button press would. 

`/push_to_unlock` should be replied with `/unlocked_time` in order to unlock
the machine upon request.

## Security concerns
Please see security.md for further information on how to report vulnerabilities.

