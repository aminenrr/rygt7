# Telemetry-driven trigger feedback

Apply renderer/shadGT-patches/0019-telemetry-driven-adaptive-triggers.patch to shadGT ryty-gpu afc9e36 (patches 0001-0018 already applied). This patch is independent of the logging and RPC branches. Rebuild libSceVideoOut.prx with SDL3; no SDL fork or new platform-specific headers are required.

GT Sport CUSA03220 v1.69 continuously builds a plaintext 0x128-byte Simulator Interface packet, including when there are no UDP subscribers. A guarded hook wraps its virtual update method and publishes a validated copy to the input thread. Both the original virtual pointer and the packet-builder instruction bytes must match; unsupported builds receive no telemetry hook. The original update always runs. No heap address from a diagnostic session is hardcoded.

The left trigger responds to estimated braking wheel slip. The right responds to excess wheel speed under throttle or the game's TCS-active bit. The model does not claim to read an ABS-active flag, which is absent from the documented packet. Body rumble and light-bar feedback remain independent. DualSense receives SDL_SendGamepadEffect trigger-only reports; Xbox controllers advertising SDL's trigger-rumble capability receive SDL_RumbleGamepadTriggers, not resistance effects. No effect is sent to unsupported controllers.

Telemetry feedback is for player 1 only. Explicit game trigger commands take priority on each player's controller, including release commands. Telemetry expires after 250 ms without a new packet; menus, pause/loading flags and speeds below 3 m/s release the effects. Xbox rumble uses a 500 ms lease, refreshed every 250 ms. DualSense effects persist in hardware until released. Disconnect releases trigger effects and reconnect resets the output cache. Failed sends retry at most once per second.

Set RYGT_ADAPTIVE_TRIGGERS=0 in the game's launch environment to disable the feature. Steam Input or another controller mapper may hide native DualSense effects or Xbox trigger-rumble capabilities.

## Validation

Compile src/input/trigger_feedback_tests.cpp as a standalone C++20 executable with src on the include path. It covers packet decoding, invalid data, rolling/lock/spin/TCS behavior, pause/menu/loading/standstill gating, stale data and packet ordering, and DualSense report isolation and release encoding.

A read-only capture from the running game produced 600 samples / 581 distinct packet IDs, speed 18.54-44.44 m/s, throttle 0-255, brake 0-245 and 107 TCS-active samples. Measured wheel-speed deficits during braking reached 20.6 percent. This establishes real input signals, not physical controller behavior. Trigger feel and thresholds still require hands-on testing.

References:
- https://github.com/Nenkai/PDTools/blob/master/PDTools.SimulatorInterface/SimulatorPacket.cs (packet fields and flags)
- https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c (SDL DualSense effect layout)
- https://wiki.libsdl.org/SDL3/SDL_RumbleGamepadTriggers (Xbox trigger-rumble API)

## Strength and pedal resistance

The launcher's Controls section includes a 0-200 percent Trigger strength slider (default 100). Zero disables feedback. The Input.trigger_strength value is saved in user/custom_configs/CUSA03220.json and reloaded within one second while playing. Strength scales both Xbox trigger motors and DualSense effect amplitudes, clamped to hardware limits. DualSense uses progressive resistance with a firmer brake and lighter accelerator; event vibration temporarily replaces resistance on the affected trigger, then restores it. Xbox controllers receive event vibration only. Normal window close sends releases before the runtime terminates.

The initial event-feedback build was confirmed working by the user on DualSense USB; SDL acceptance and live telemetry were independently verified. The subsequent slider and progressive resistance build requires hands-on testing. The source patch remains independent of logging/RPC changes; the local test launcher retains the existing logging UI.

## HD rumble and per-effect settings (FEAT-GTS-265, shadgt7 FEAT-GT7-179)

The Trigger strength slider moved into Controls > Controller > HD rumble & triggers. That window has a 0-200 percent slider per effect, where 0 turns that effect off, all saved in the same Input object and read again every second while playing:

- HD rumble on the DualSense's body (`haptics_strength`, `haptics_engine`, `haptics_road`, `haptics_slip`, `haptics_shift`, `haptics_limiter`, `haptics_impact`): played on a USB DualSense's voice-coil actuators through its sound card, on top of its motor rumble. Kerbs and bumps on the side of the car they happen, wheel lock and wheelspin per side, gear changes, the rev limiter, impacts, and an engine hum whose pitch follows the revs. Bluetooth has no sound card, so nothing plays there.
- Adaptive triggers (`trigger_strength`, `trigger_brake`, `trigger_throttle`, `trigger_dynamic`, `trigger_lock`, `trigger_spin`): the resistance follows the car. The brake is soft at a standstill, firmer when moving and firmest under hard braking. The accelerator stiffens with the revs, lets go at gear changes and pulses on the limiter. With lock or spin at 0, the pedal keeps its resistance instead of vibrating.

Each effect's Test button runs `tools\ryty-feel-test.exe` from the renderer build, which plays it from a short scripted drive through the same code as the game. `RYGT_HAPTICS_LOG=1` logs what the haptics see while driving. The suspension, RPM, gear and velocity offsets the haptics add follow GT Sport's documented layout and are not yet confirmed in GT7.
