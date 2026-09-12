# Menu keyboard capture regression

`build.bat` compiles and runs this test on Windows. It uses the production
input gate with simulated overlay-style COM devices, then creates real ANSI
and Unicode DirectInput keyboards through its patched executable import.

The fixtures verify separate device tables, preserved object identity and
original calls, joystick pass-through, buffered key releases and peek behavior,
input loss, held closing keys, table restoration, and balanced references.
They do not inject keyboard events or require a VR runtime.
