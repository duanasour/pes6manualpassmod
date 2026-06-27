**A DirectInput override for PES6 to match manual pass control to modern PES games**

_This DLL allows L2+X/O to pass manually in PES6 instead of using right joystick and L1.
This should work for other old PES titles, PES5 and PES2010 tested._

To install and enable:
- Copy dinput8.dll and dinput8.cfg to the PES6 game folder
- In PES6 button configuration, select manual pass type 2 (i.e. right joystick for group pass, L1+right joystick for lob pass)

To use in gameplay:
- Press and hold L2, then press X to ground pass, O to lob pass.

How it works:
- When holding L2, manual pass direction is taken from left joystick, power is determined by how long X/O is pressed.
- The DLL intercepts L2+X/O and convert back to right joystick input for PES.

Caveats:
- L2 + X/O is used for strategy change in the game, to avoid the conflict, you need to press L2 all the way down to trigger manual pass, and not-all-the-way-down for strategy change.
- A few things can be tweaked via dinput8.cfg, but default value should work just fine.
- If it works, change LOGGING=0 in dinput8.cfg to avoid unnecessary logs.
